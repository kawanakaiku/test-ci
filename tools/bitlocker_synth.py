#!/usr/bin/env python3
"""合成 BitLocker (Windows 7+ FVE) ボリュームの書き出し — 実 Windows が作れない構成の検証用 (実 Windows 製ではない)。docs/crypto-volumes.md

用途: Windows 11 の BitLocker は AES-CBC + Elephant diffuser (Windows 7 / 8 方式) を作れないため、その復号の正しさを
独立実装 libbde (bdeinfo / bdemount) に判定させる。ここで作ったイメージを bdemount が正しく復号できる
= このライターの暗号化規則が libbde の復号規則と一致 → C++ 側の復号 (同じ規則を実装) も libbde と一致、と推論する。
同じライターで、回復パスワード / スタートアップキー (.BEK) / クリアキー / TPM のみ / 暗号化途中 の各構成も作る。

使い方: bitlocker_synth.py <平文ボリュームイメージ> <出力イメージ> --method {8000,8001,8002,8003,8004,8005}
          [--protectors pw,rec,bek,clear,tpm] [--state 4,4] [--enc-fraction 1.0] [--password P] [--meta 出力メタ(回復キー等)]
出力イメージ = 平文 + 4 * 64 KiB (ボリュームヘッダ退避領域 + FVE メタデータ 3 コピー)。BEK は <出力>.bek に書く。
"""
import argparse
import hashlib
import os
import random
import struct
import sys
import uuid

import numpy as np
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.ciphers.aead import AESCCM

SS = 512
NSEC = 16  # ボリュームヘッダ退避セクタ数 (Windows と同じ)
RA = (9, 0, 13, 0)
RB = (0, 10, 0, 25)

KEYLEN = {0x8000: 64, 0x8001: 64, 0x8002: 16, 0x8003: 32, 0x8004: 32, 0x8005: 64}


def ecb(key, data):
    return Cipher(algorithms.AES(key), modes.ECB()).encryptor().update(data)


def rol(x, n):  # numpy uint32 配列の左回転
    if n == 0:
        return x
    return ((x << np.uint32(n)) | (x >> np.uint32(32 - n))).astype(np.uint32)


def diffuser_a_encrypt(d):  # d: (nsec, nwords) uint32。復号 (昇順で +) の逆 = 降順で -
    n = d.shape[1]
    for _ in range(5):
        for i in range(n - 1, -1, -1):
            d[:, i] = (d[:, i] - (d[:, (i - 2) % n] ^ rol(d[:, (i - 5) % n], RA[i % 4]))).astype(np.uint32)
    return d


def diffuser_b_encrypt(d):
    n = d.shape[1]
    for _ in range(3):
        for i in range(n - 1, -1, -1):
            d[:, i] = (d[:, i] - (d[:, (i + 2) % n] ^ rol(d[:, (i + 5) % n], RB[i % 4]))).astype(np.uint32)
    return d


def encrypt_sectors(method, key, data, first_phys_off):
    """data (SS の倍数) を、先頭の物理バイト位置 first_phys_off から連続した暗号化セクタとして暗号化する。"""
    n = len(data) // SS
    out = bytearray(len(data))
    if method in (0x8004, 0x8005):
        h = len(key) // 2
        k1, k2 = key[:h], key[h:]
        for i in range(n):
            tw = struct.pack("<QQ", first_phys_off // SS + i, 0)
            out[i * SS:(i + 1) * SS] = Cipher(algorithms.AES(key), modes.XTS(tw)).encryptor().update(data[i * SS:(i + 1) * SS])
        return bytes(out)
    diff = method in (0x8000, 0x8001)
    kd = {0x8000: 16, 0x8001: 32, 0x8002: 16, 0x8003: 32}[method]
    fvek = key[:kd]
    tweak = key[32:32 + kd] if diff else None  # 0x8000: 鍵は 64 バイト (FVEK 16 + 0 詰め 16 + tweak 16 + 0 詰め 16。libbde の期待)
    offs = [first_phys_off + i * SS for i in range(n)]
    ivs = ecb(fvek, b"".join(struct.pack("<QQ", o, 0) for o in offs))
    buf = np.frombuffer(bytes(data), dtype="<u4").reshape(n, SS // 4).copy() if diff else None
    if diff:
        bks = []
        for o in offs:
            bks.append(struct.pack("<QQ", o, 0))
            bks.append(struct.pack("<Q", o) + b"\0" * 7 + b"\x80")
        sk = ecb(tweak, b"".join(bks))  # 各セクタ 32 バイトの sector key
        skeys = np.frombuffer(sk, dtype="<u1").reshape(n, 32)
        sec = np.frombuffer(bytes(data), dtype="u1").reshape(n, SS) ^ np.tile(skeys, (1, SS // 32))
        buf = np.ascontiguousarray(sec).view("<u4").reshape(n, SS // 4).copy()
        buf = diffuser_a_encrypt(buf)
        buf = diffuser_b_encrypt(buf)
        data = buf.astype("<u4").tobytes()
    for i in range(n):
        out[i * SS:(i + 1) * SS] = Cipher(algorithms.AES(fvek), modes.CBC(ivs[i * 16:(i + 1) * 16])).encryptor().update(data[i * SS:(i + 1) * SS])
    return bytes(out)


def entry(etype, vtype, data, ver=1):
    return struct.pack("<HHHH", 8 + len(data), etype, vtype, ver) + data


def key_blob(method, key):
    d = struct.pack("<I", method) + key
    return struct.pack("<HHHH", 8 + len(d), 0, 1, 0) + d  # ver = 0 (実 Windows の鍵エントリと同じ)


def ccm_wrap(key, blob, rnd):
    nonce = bytes(rnd.randrange(256) for _ in range(12))
    ct = AESCCM(key, tag_length=16).encrypt(nonce, blob, None)
    return nonce + ct[-16:] + ct[:-16]  # nonce | tag | 暗号文 (Windows の並び)


def stretch(initial, salt):
    last = b"\0" * 32
    for i in range(0x100000):
        last = hashlib.sha256(last + initial + salt + struct.pack("<Q", i)).digest()
    return last


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("plain")
    ap.add_argument("out")
    ap.add_argument("--method", default="8004")
    ap.add_argument("--protectors", default="pw")
    ap.add_argument("--state", default="4,4")
    ap.add_argument("--enc-fraction", type=float, default=1.0)
    ap.add_argument("--password", default="synth-Password-1")
    ap.add_argument("--meta", default="")
    ap.add_argument("--seed", default="synth")
    a = ap.parse_args()
    method = int(a.method, 16)
    rnd = random.Random(a.seed + a.method)
    rb = lambda n: bytes(rnd.randrange(256) for _ in range(n))
    plain = open(a.plain, "rb").read()
    assert len(plain) % SS == 0 and len(plain) >= NSEC * SS
    fvek_key = rb(KEYLEN[method])
    vmk = rb(32)
    vol_guid = uuid.UUID(bytes=rb(16)).bytes_le
    cur, nxt = [int(x) for x in a.state.split(",")]
    plen = len(plain)
    # 配置は実 Windows 11 の BitLocker と同じ並び: メタデータ 1 → ボリュームヘッダ退避 (64 KiB 領域) → メタデータ 2 → メタデータ 3
    meta_offs = [plen, plen + 0x20000, plen + 0x30000]
    vh_off = plen + 0x10000
    total = plen + 0x40000
    # 「暗号化済みボリュームのサイズ」(メタデータのブロックヘッダ +16) は、完了時は total (メタデータとボリュームヘッダ退避領域を含む全長) にする。
    # 実 Windows のイメージでは退避領域が暗号化済みサイズの内側にあり、libbde も自前のリーダーも「退避領域が範囲内」を検査する。plen だと範囲外になって解錠に失敗した
    # (実機 C の p5_bitlocker で「ボリュームヘッダの退避位置が不正です」。docs/crypto-volumes.md 6.1)。
    enc_limit = (int(plen * a.enc_fraction) // SS) * SS  # 変換済み領域の終端 (暗号化途中の再現)

    # ---- データ領域の暗号化
    img = bytearray(total)
    body = plain[NSEC * SS:enc_limit] if enc_limit > NSEC * SS else b""
    if body:
        img[NSEC * SS:NSEC * SS + len(body)] = encrypt_sectors(method, fvek_key, body, NSEC * SS)
    img[NSEC * SS + len(body):plen] = plain[NSEC * SS + len(body):]  # 未変換部分は平文のまま
    img[vh_off:vh_off + NSEC * SS] = encrypt_sectors(method, fvek_key, plain[:NSEC * SS], vh_off)  # 退避された元の先頭セクタ
    # FVE ブートセクタ
    boot = bytearray(plain[:SS])
    boot[0:3] = b"\xeb\x58\x90"
    boot[3:11] = b"-FVE-FS-"
    boot[0x0B:0x0E] = bytes([0x00, 0x02, 0x08])  # bytes/sector 512, sectors/cluster 8 (libbde は BPB を軽く検査する)
    boot[0x1FE:0x200] = b"\x55\xaa"
    boot[0xA0:0xB0] = bytes.fromhex("3bd6674929 2ed84a8399f6a339e3d001".replace(" ", ""))
    boot[0xB0:0xC8] = struct.pack("<QQQ", *meta_offs)
    img[:SS] = boot

    # ---- 保護 (VMK エントリ)
    protos = a.protectors.split(",") if a.protectors else []
    entries = [entry(7, 2, "SYNTH-BL".encode("utf-16le"))]
    meta_lines = []
    ts = 0x01DD71B2FAB8E03A
    for p in protos:
        pg = rb(16)
        hdr = pg + struct.pack("<Q", ts) + struct.pack("<H", 0)
        if p == "pw":
            salt = rb(16)
            init = hashlib.sha256(hashlib.sha256(a.password.encode("utf-16le")).digest()).digest()
            key = stretch(init, salt)
            nested = entry(0, 3, struct.pack("<I", 0x1001) + salt) + entry(0, 5, ccm_wrap(key, key_blob(0x2003, vmk), rnd))
            entries.append(entry(2, 8, hdr + struct.pack("<H", 0x2000) + nested))
            meta_lines.append("password=" + a.password)
        elif p == "rec":
            raw = rb(16)
            groups = [struct.unpack("<H", raw[2 * i:2 * i + 2])[0] * 11 for i in range(8)]
            text = "-".join("%06d" % g for g in groups)
            salt = rb(16)
            key = stretch(hashlib.sha256(raw).digest(), salt)
            nested = entry(0, 3, struct.pack("<I", 0x1001) + salt) + entry(0, 5, ccm_wrap(key, key_blob(0x2003, vmk), rnd))
            entries.append(entry(2, 8, hdr + struct.pack("<H", 0x0800) + nested))
            meta_lines.append("recovery=" + text)
        elif p == "bek":
            bk = rb(32)
            nested = entry(0, 5, ccm_wrap(bk, key_blob(0x2003, vmk), rnd))
            entries.append(entry(2, 8, hdr + struct.pack("<H", 0x0200) + nested))
            ext = entry(6, 9, pg + struct.pack("<Q", ts) + entry(0, 1, struct.pack("<I", 0x2000) + bk))
            bek = struct.pack("<IIII", 48 + len(ext), 1, 48, 48 + len(ext)) + vol_guid + struct.pack("<IHH", 1, 0x2000, 0x2000) + struct.pack("<Q", ts) + ext
            open(a.out + ".bek", "wb").write(bek)
            meta_lines.append("bek=" + a.out + ".bek")
        elif p == "clear":
            ck = rb(32)
            nested = entry(0, 1, struct.pack("<I", 0x2000) + ck) + entry(0, 5, ccm_wrap(ck, key_blob(0x2003, vmk), rnd))
            entries.append(entry(2, 8, hdr + struct.pack("<H", 0x0000) + nested))
            meta_lines.append("clear=1")
        elif p == "tpm":
            nested = entry(0, 6, rb(80)) + entry(0, 5, ccm_wrap(rb(32), key_blob(0x2003, vmk), rnd))
            entries.append(entry(2, 8, hdr + struct.pack("<H", 0x0100) + nested))
            meta_lines.append("tpm=1")
        else:
            raise SystemExit("unknown protector " + p)
    entries.append(entry(3, 5, ccm_wrap(vmk, key_blob(method, fvek_key), rnd)))
    # 実 Windows 11 のボリュームヘッダブロック (type 0xf) と同じ 92 バイトの形。先頭 16 バイトが offset/size、残りは Windows が付ける追加情報
    tail = bytes.fromhex("0500 4c00 0000 0000 0b00 0000 0000 0000 0000 0000 0000 0000 0000 0000 0002 0000 0002 0000 0a00 0000 0000 0000 5866 0000 0200 0000 0000 0000".replace(" ", ""))
    tail += struct.pack("<HHQ", 0x0110, 0x0201, vh_off + NSEC * SS) + struct.pack("<II", 0x00010000, 0)
    entries.append(entry(0xF, 0xF, struct.pack("<QQ", vh_off, NSEC * SS) + tail))
    body_e = b"".join(entries)
    msize = 48 + len(body_e)
    meta_hdr = struct.pack("<IIII", msize, 1, 48, msize) + vol_guid + struct.pack("<I", 8) + struct.pack("<HH", method, method) + struct.pack("<Q", ts)
    blk = b"-FVE-FS-" + struct.pack("<HHHH", (64 + msize + 15) // 16, 2, cur, nxt) + struct.pack("<QIIQQQQ", enc_limit if a.enc_fraction < 1.0 else total, 0, NSEC, *meta_offs, vh_off)
    # ブロックヘッダは 64 バイト: sig8 + 4*u16 + u64 + u32 + u32 + 3*u64 + u64
    assert len(blk) == 64, len(blk)
    block = (blk + meta_hdr + body_e).ljust(0x10000, b"\0")
    for o in meta_offs:
        img[o:o + 0x10000] = block
    open(a.out, "wb").write(img)
    if a.meta:
        open(a.meta, "w").write("\n".join(meta_lines) + "\n")
    print("wrote %s (%d bytes, method %04x, protectors %s)" % (a.out, total, method, a.protectors))


if __name__ == "__main__":
    main()
