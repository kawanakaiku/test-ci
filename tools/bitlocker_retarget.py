#!/usr/bin/env python3
"""実 Windows が作った BitLocker イメージを、レイアウトはそのまま (ブートセクタ・メタデータ位置・ボリュームヘッダ退避・VMK エントリ) に
暗号方式だけ別のものへ作り替える。実 Windows 11 では作れない AES-CBC + Elephant diffuser (Windows 7/8 方式) などを、
本物のレイアウトの上で作って libbde (bdeinfo / bdemount、独立実装) に判定させるために使う。実 Windows 製ではなく「実イメージからの変換」。docs/crypto-volumes.md

使い方: bitlocker_retarget.py <実イメージ> <ボリューム先頭オフセット> <パスワード> <出力> --method {8000..8005}
          [--add rec,clear,tpm] [--state 4,4] [--enc-fraction 1.0]
パスワード保護の VMK はそのまま使い、FVEK だけ新しい乱数にして FVEK エントリを VMK で包み直し、全データを新方式で暗号化し直す。
--add で VMK エントリを足す (rec=回復パスワード / clear=クリアキー / tpm=TPM のみ (ダミー))。メタ情報は <出力>.meta に書く。
"""
import argparse
import hashlib
import importlib.util
import os
import random
import struct
import sys

import numpy as np
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.ciphers.aead import AESCCM

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("bl_synth", os.path.join(here, "bitlocker_synth.py"))
bl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bl)
SS = 512


def parse_entries(buf, p, end):
    out = []
    while p + 8 <= end:
        sz, et, vt, ver = struct.unpack("<HHHH", buf[p:p + 8])
        if sz < 8:
            break
        out.append((et, vt, buf[p:p + sz]))
        p += sz
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("offset", type=int)
    ap.add_argument("password")
    ap.add_argument("out")
    ap.add_argument("--method", default="8000")
    ap.add_argument("--add", default="")
    ap.add_argument("--state", default="4,4")
    ap.add_argument("--enc-fraction", type=float, default=1.0)
    ap.add_argument("--drop-password", action="store_true", help="元のパスワード保護を捨てる (--add tpm と組み合わせて TPM のみのボリュームを作る。FVEK は誰にも開けない)")
    a = ap.parse_args()
    method = int(a.method, 16)
    rnd = random.Random("retarget" + a.method)
    rb = lambda n: bytes(rnd.randrange(256) for _ in range(n))
    f = open(a.src, "rb")
    f.seek(a.offset)
    boot = f.read(SS)
    moffs = struct.unpack("<QQQ", boot[0xB0:0xC8])
    f.seek(a.offset + moffs[0])
    blk = bytearray(f.read(0x10000))
    msize = struct.unpack("<I", blk[64:68])[0]
    vh_off = struct.unpack("<Q", blk[56:64])[0]
    nsec = struct.unpack("<I", blk[28:32])[0]
    enc_size = struct.unpack("<Q", blk[16:24])[0]
    ents = parse_entries(bytes(blk), 112, 64 + msize)
    real = {(et, vt): raw for et, vt, raw in ents}
    # パスワード → VMK → 旧 FVEK
    vmk_e = real[(2, 8)]
    nstart = 8 + 28
    salt = vmk_e[nstart + 12:nstart + 28]
    init = hashlib.sha256(hashlib.sha256(a.password.encode("utf-16le")).digest()).digest()
    key = bl.stretch(init, salt)
    ccm_off = nstart + struct.unpack("<H", vmk_e[nstart:nstart + 2])[0]
    ccm = vmk_e[ccm_off + 8:ccm_off + struct.unpack("<H", vmk_e[ccm_off:ccm_off + 2])[0]]
    vmk = AESCCM(key, tag_length=16).decrypt(ccm[:12], ccm[28:] + ccm[12:28], None)[12:44]
    fv = real[(3, 5)][8:]
    old_blob = AESCCM(vmk, tag_length=16).decrypt(fv[:12], fv[28:] + fv[12:28], None)
    old_key = old_blob[12:44]
    # 全体を復号 → 新方式で暗号化
    f.seek(a.offset)
    total = os.path.getsize(a.src) - a.offset
    vol = (total // SS) * SS
    new_key = rb(bl.KEYLEN[method])
    out = bytearray(vol)
    chunk = 8192 * SS
    enc_limit = (int(enc_size * a.enc_fraction) // SS) * SS
    hidden = [(o, o + 0x10000) for o in moffs] + [(vh_off, vh_off + nsec * SS)]
    for pos in range(0, vol, chunk):
        f.seek(a.offset + pos)
        raw = f.read(min(chunk, vol - pos))
        buf = bytearray(raw)
        for s in range(0, len(raw), SS):
            phys = pos + s
            if any(lo <= phys < hi for lo, hi in hidden) or phys < nsec * SS:
                continue
            tw = struct.pack("<QQ", phys // SS, 0)
            buf[s:s + SS] = Cipher(algorithms.AES(old_key), modes.XTS(tw)).decryptor().update(bytes(raw[s:s + SS]))
        # buf = 平文 (隠し領域と先頭 nsec セクタ以外)。範囲を連続区間に分けて暗号化する
        s = 0
        while s < len(raw):
            phys = pos + s
            if any(lo <= phys < hi for lo, hi in hidden) or phys < nsec * SS or phys >= enc_limit:
                s += SS
                continue
            e = s
            while e < len(raw) and not (any(lo <= pos + e < hi for lo, hi in hidden) or pos + e >= enc_limit):
                e += SS
            buf[s:e] = bl.encrypt_sectors(method, new_key, bytes(buf[s:e]), pos + s)
            s = e
        out[pos:pos + len(raw)] = buf
    # 先頭 nsec セクタ: 退避領域の旧暗号文 → 平文 → 新方式で退避位置に暗号化
    f.seek(a.offset + vh_off)
    rawh = f.read(nsec * SS)
    plain_h = b"".join(Cipher(algorithms.AES(old_key), modes.XTS(struct.pack("<QQ", vh_off // SS + i, 0))).decryptor().update(rawh[i * SS:(i + 1) * SS]) for i in range(nsec))
    out[vh_off:vh_off + nsec * SS] = bl.encrypt_sectors(method, new_key, plain_h, vh_off)
    out[:SS] = boot
    # 新しいメタデータ
    new_ents = [real[(7, 2)]] + ([] if a.drop_password else [real[(2, 8)]])
    meta = []
    ts = 0x01DD71B2FAB8E03A
    for p in [x for x in a.add.split(",") if x]:
        pg = rb(16)
        hdr = pg + struct.pack("<Q", ts) + struct.pack("<H", 0)
        if p == "rec":
            raw = rb(16)
            groups = [struct.unpack("<H", raw[2 * i:2 * i + 2])[0] * 11 for i in range(8)]
            text = "-".join("%06d" % g for g in groups)
            s2 = rb(16)
            k2 = bl.stretch(hashlib.sha256(raw).digest(), s2)
            nested = bl.entry(0, 3, struct.pack("<I", 0x1001) + s2) + bl.entry(0, 5, bl.ccm_wrap(k2, bl.key_blob(0x2003, vmk), rnd))
            new_ents.append(bl.entry(2, 8, hdr + struct.pack("<H", 0x0800) + nested))
            meta.append("recovery=" + text)
        elif p == "clear":
            ck = rb(32)
            nested = bl.entry(0, 1, struct.pack("<I", 0x2000) + ck) + bl.entry(0, 5, bl.ccm_wrap(ck, bl.key_blob(0x2003, vmk), rnd))
            new_ents.append(bl.entry(2, 8, hdr + struct.pack("<H", 0x0000) + nested))
            meta.append("clear=1")
        elif p == "tpm":
            nested = bl.entry(0, 6, rb(80)) + bl.entry(0, 5, bl.ccm_wrap(rb(32), bl.key_blob(0x2003, vmk), rnd))
            new_ents.append(bl.entry(2, 8, hdr + struct.pack("<H", 0x0100) + nested))
            meta.append("tpm=1")
        else:
            raise SystemExit("unknown --add " + p)
    new_ents.append(bl.entry(3, 5, bl.ccm_wrap(vmk, bl.key_blob(method, new_key), rnd)))
    new_ents.append(real[(15, 15)])
    body = b"".join(new_ents)
    ms = 48 + len(body)
    nb = bytearray(blk)
    nb[64:68] = struct.pack("<I", ms)
    nb[76:80] = struct.pack("<I", ms)
    nb[100:102] = struct.pack("<H", method)
    nb[102:104] = struct.pack("<H", method)
    cur, nxt = [int(x) for x in a.state.split(",")]
    nb[12:14] = struct.pack("<H", cur)
    nb[14:16] = struct.pack("<H", nxt)
    if a.enc_fraction < 1.0:
        nb[16:24] = struct.pack("<Q", enc_limit)
    nb[112:] = body + b"\0" * (len(nb) - 112 - len(body))
    nb[8:10] = struct.pack("<H", (64 + ms + 15) // 16)
    for o in moffs:
        out[o:o + 0x10000] = bytes(nb)
    open(a.out, "wb").write(bytes(out))
    open(a.out + ".meta", "w").write("\n".join(["password=" + a.password] + meta) + "\n")
    print("wrote %s (method %04x, %d bytes)" % (a.out, method, len(out)))


if __name__ == "__main__":
    main()
