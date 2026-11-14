#!/usr/bin/env python3
"""暗号ボリューム書き込みの独立検証用ヘルパ (docs/crypto-volumes.md「書き込み」)。自前の C++ とは別の実装 (cryptsetup + Python cryptography) だけを使う。

  crypt_indep.py make-luks IMG --plain PLAINFILE --pwfile PW [--ver 1|2] [--cipher aes-xts-plain64] [--bits 512] [--hash sha256]
                           [--sector 512] [--pbkdf pbkdf2|argon2id] [--extra MB]
      本物の `cryptsetup luksFormat` (root 不要・ファイル上) でヘッダを作り、PLAINFILE をマスターキーで暗号化して書く。
      標準出力: "data_offset=<バイト> sector=<N> size=<平文バイト>"。マスターキーは cryptsetup luksDump --dump-master-key で取り直して照合する。
  crypt_indep.py decrypt-luks IMG --pwfile PW --out OUT
      IMG のデータ領域を、cryptsetup が出したマスターキーと Python cryptography だけで復号して OUT に書く (自前 C++ を使わない)。
      書き込み後のイメージの復号結果を「期待する平文 (モデル)」と cmp するために使う。実カーネルの dm-crypt での照合は CI (crypt-write.yml)。
  crypt_indep.py luks-layout IMG --pwfile PW
      標準出力: "data_offset=<バイト> sector=<N> cipher=<..> bits=<N>"
"""
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

os.environ["PATH"] = os.environ.get("PATH", "") + ":/sbin:/usr/sbin"
CS = shutil.which("cryptsetup") or "/sbin/cryptsetup"


def run(cmd):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if r.returncode != 0:
        raise RuntimeError("%s failed: %s" % (" ".join(cmd), r.stderr.decode(errors="replace")))
    return r.stdout.decode(errors="replace")


def sector_cipher(mode, key, iv_sector, data, encrypt):
    if mode == "xts-plain64":
        c = Cipher(algorithms.AES(key), modes.XTS(struct.pack("<QQ", iv_sector, 0)))
    elif mode == "cbc-plain64":
        c = Cipher(algorithms.AES(key), modes.CBC(struct.pack("<QQ", iv_sector, 0)))
    elif mode == "cbc-essiv:sha256":
        ek = hashlib.sha256(key).digest()
        iv = Cipher(algorithms.AES(ek), modes.ECB()).encryptor().update(struct.pack("<QQ", iv_sector, 0))
        c = Cipher(algorithms.AES(key), modes.CBC(iv))
    else:
        raise ValueError(mode)
    x = c.encryptor() if encrypt else c.decryptor()
    return x.update(data) + x.finalize()


def crypt_payload(data, mode, key, sector, encrypt, iv_tweak=0):
    out = bytearray(len(data))
    step = sector // 512
    for i in range(len(data) // sector):
        o = i * sector
        out[o:o + sector] = sector_cipher(mode, key, iv_tweak + i * step, data[o:o + sector], encrypt)
    return bytes(out)


def layout(img, pwfile):
    """(data_offset, sector, mode, master_key, iv_tweak)"""
    dump = run([CS, "luksDump", "--dump-master-key", "--batch-mode", "--disable-locks", "--key-file", pwfile, img])
    lines = dump.splitlines()
    i0 = [i for i, l in enumerate(lines) if l.startswith("MK dump")][0]
    mkhex = lines[i0].split(":", 1)[1]
    for l in lines[i0 + 1:]:
        if not l.startswith("\t"):
            break
        mkhex += l
    mk = bytes.fromhex("".join(mkhex.split()))
    with open(img, "rb") as f:
        hdr = f.read(8)
    if hdr[6:8] == b"\x00\x02":  # LUKS2 (バージョン 2。先頭の "LUKS\xba\xbe" の次の BE 16 ビット)
        js = json.loads(run([CS, "luksDump", "--dump-json-metadata", "--disable-locks", img]))
        seg = js["segments"]["0"]
        off, ss = int(seg["offset"]), int(seg.get("sector_size", 512))
        enc = seg["encryption"]
        mode = enc.split("-", 1)[1]
        iv_tweak = int(seg.get("iv_tweak", "0"))
    else:
        d = run([CS, "luksDump", "--disable-locks", img])
        off = int([l for l in d.splitlines() if l.startswith("Payload offset")][0].split(":", 1)[1]) * 512
        cname = [l for l in d.splitlines() if l.startswith("Cipher name")][0].split(":", 1)[1].strip()
        cmode = [l for l in d.splitlines() if l.startswith("Cipher mode")][0].split(":", 1)[1].strip()
        mode, ss, iv_tweak = cmode, 512, 0
        assert cname == "aes"
    return off, ss, mode, mk, iv_tweak


def cmd_make(a):
    plain = open(a.plain, "rb").read()
    assert len(plain) % a.sector == 0
    mkf = a.img + ".mk.tmp"
    import random
    rnd = random.Random(a.img)
    mk = bytes(rnd.randrange(256) for _ in range(a.bits // 8))
    open(mkf, "wb").write(mk)
    if os.path.exists(a.img):
        os.unlink(a.img)
    with open(a.img, "wb") as f:
        f.truncate(len(plain) + (20 << 20) + a.extra * (1 << 20))
    cmd = [CS, "luksFormat", "--type", "luks%d" % a.ver, "--batch-mode", "--disable-locks", "--cipher", a.cipher, "--key-size", str(a.bits), "--hash", a.hash,
           "--key-file", a.pwfile, "--volume-key-file", mkf]
    if a.ver == 2:
        cmd += ["--sector-size", str(a.sector)]
        if a.pbkdf.startswith("argon2"):
            cmd += ["--pbkdf", a.pbkdf, "--pbkdf-memory", "32768", "--pbkdf-parallel", "2", "--pbkdf-force-iterations", "4"]
        else:
            cmd += ["--pbkdf", "pbkdf2", "--pbkdf-force-iterations", "1000"]
    else:
        cmd += ["--pbkdf-force-iterations", "1000"]
    run(cmd + [a.img])
    off, ss, mode, mk2, iv_tweak = layout(a.img, a.pwfile)
    assert mk2 == mk, "master key mismatch"
    assert ss == a.sector, (ss, a.sector)
    os.truncate(a.img, off + len(plain))
    with open(a.img, "r+b") as f:
        f.seek(off)
        f.write(crypt_payload(plain, mode, mk, ss, True, iv_tweak))
    os.unlink(mkf)
    print("data_offset=%d sector=%d size=%d" % (off, ss, len(plain)))


def cmd_decrypt(a):
    off, ss, mode, mk, iv_tweak = layout(a.img, a.pwfile)
    data = open(a.img, "rb").read()[off:]
    data = data[:len(data) // ss * ss]
    open(a.out, "wb").write(crypt_payload(data, mode, mk, ss, False, iv_tweak))
    print("decrypted %d bytes (mode %s, sector %d)" % (len(data), mode, ss))


def cmd_layout(a):
    off, ss, mode, mk, iv_tweak = layout(a.img, a.pwfile)
    print("data_offset=%d sector=%d cipher=%s bits=%d" % (off, ss, mode, len(mk) * 8))


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("make-luks")
    m.add_argument("img")
    m.add_argument("--plain", required=True)
    m.add_argument("--pwfile", required=True)
    m.add_argument("--ver", type=int, default=2)
    m.add_argument("--cipher", default="aes-xts-plain64")
    m.add_argument("--bits", type=int, default=512)
    m.add_argument("--hash", default="sha256")
    m.add_argument("--sector", type=int, default=512)
    m.add_argument("--pbkdf", default="pbkdf2")
    m.add_argument("--extra", type=int, default=0)
    d = sub.add_parser("decrypt-luks")
    d.add_argument("img")
    d.add_argument("--pwfile", required=True)
    d.add_argument("--out", required=True)
    l = sub.add_parser("luks-layout")
    l.add_argument("img")
    l.add_argument("--pwfile", required=True)
    a = ap.parse_args()
    {"make-luks": cmd_make, "decrypt-luks": cmd_decrypt, "luks-layout": cmd_layout}[a.cmd](a)


main()
