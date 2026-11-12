#!/usr/bin/env python3
"""LUKS1 / LUKS2 のテスト用イメージを作る (core/compat/tests/test_cryptovol.cpp と tools/run_crypto_volume_tests.sh 用)。
使い方: tools/gen_luks_fixtures.py [出力ディレクトリ(既定 /dev/shm/usbmem_crypt_$USER/luks)]

ヘッダは **本物の `cryptsetup luksFormat`** (ルート権限なし・ファイル上) で作り、`cryptsetup open --test-passphrase` と
`luksDump --dump-master-key` (マスターキーが渡したものと一致) で cryptsetup 自身に検証させる。
データ領域は、既知のファイルシステムイメージ (mke2fs -d で作った ext2 / ext4、または既存の NTFS イメージ) を、そのマスターキーで
dm-crypt と同じ規則 (xts-plain64 / cbc-essiv:sha256 / cbc-plain64。IV は 512 バイト単位のセクタ番号) で暗号化して書く
(`cryptsetup open` は dm が要るためルート権限が必要で使えない)。LUKS1 の分は nbdkit の luks フィルタ (独立実装) でも復号して平文と一致を確認する。
異常系 (再暗号化途中・dm-integrity・serpent・ヘッダ破損) は LUKS2 ヘッダの JSON を書き換えてチェックサムを計算し直して作る。

出力: <out>/<name>.img, <name>.pw (パスフレーズ。使い捨て), <out>/cases.txt (タブ区切り: name, 資格情報, 平文イメージ, 期待する結果, 説明)
"""
import hashlib
import json
import os
import random
import shutil
import struct
import subprocess
import sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

os.environ["PATH"] = os.environ.get("PATH", "") + ":/sbin:/usr/sbin"
CS = shutil.which("cryptsetup") or "/sbin/cryptsetup"
OUT = sys.argv[1] if len(sys.argv) > 1 else "/dev/shm/usbmem_crypt_%s/luks" % os.environ.get("USER", "u")
os.makedirs(OUT, exist_ok=True)


def run(cmd, **kw):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kw)
    if r.returncode != 0:
        raise RuntimeError("%s failed: %s" % (" ".join(cmd), r.stderr.decode(errors="replace")))
    return r.stdout.decode(errors="replace")


def pattern(seed, off, n):
    return bytes((((off + i) * 31 + seed * 7 + ((off + i) >> 8)) & 0xFF) for i in range(n))


def make_plain(kind, size_mb=24):
    path = os.path.join(OUT, "plain_%s.img" % kind)
    tree = os.path.join(OUT, "tree_%s" % kind)
    shutil.rmtree(tree, ignore_errors=True)
    os.makedirs(os.path.join(tree, "dir/sub/deeper"))
    sizes = [0, 1, 511, 512, 513, 4095, 4096, 4097, 65536, 700001, 3 * 1024 * 1024 + 17]
    for i, sz in enumerate(sizes):
        with open(os.path.join(tree, "f_%d.bin" % sz), "wb") as f:
            f.write(pattern(i + 1, 0, sz))
    for i in range(60):
        with open(os.path.join(tree, "dir/sub/file_%02d.txt" % i), "wb") as f:
            f.write(pattern(100 + i, 0, 100 + i * 37))
    with open(os.path.join(tree, "dir/sub/deeper/日本語.txt"), "wb") as f:
        f.write(b"unicode name\n")
    with open(os.path.join(tree, "sparse.bin"), "wb") as f:
        f.seek(5 * 1024 * 1024)
        f.write(b"tail")
    os.symlink("f_513.bin", os.path.join(tree, "link_to_f"))
    for p in (os.path.join(tree, "f_513.bin"),):
        os.utime(p, (1700000000, 1700000000))
    if os.path.exists(path):
        os.unlink(path)
    with open(path, "wb") as f:
        f.truncate(size_mb << 20)
    run(["mke2fs", "-q", "-t", kind, "-F", "-L", "luksplain", "-E", "root_owner=0:0", "-d", tree, path])
    shutil.rmtree(tree, ignore_errors=True)
    return path


def enc_sector(mode, key, iv_sector, data):
    if mode == "xts-plain64":
        c = Cipher(algorithms.AES(key), modes.XTS(struct.pack("<QQ", iv_sector, 0)))
        return c.encryptor().update(data)
    if mode in ("cbc-plain64", "cbc-essiv:sha256"):
        if mode == "cbc-plain64":
            iv = struct.pack("<QQ", iv_sector, 0)
        else:
            ek = hashlib.sha256(key).digest()
            iv = Cipher(algorithms.AES(ek), modes.ECB()).encryptor().update(struct.pack("<QQ", iv_sector, 0))
        return Cipher(algorithms.AES(key), modes.CBC(iv)).encryptor().update(data)
    raise ValueError(mode)


def enc_payload(plain, mode, key, sector_size):
    out = bytearray(len(plain))
    step = sector_size // 512
    for i in range(len(plain) // sector_size):
        o = i * sector_size
        out[o:o + sector_size] = enc_sector(mode, key, i * step, plain[o:o + sector_size])
    return bytes(out)


CASES = []


def case(name, ver, cipher, bits, hsh, pbkdf="pbkdf2", sector=512, plain="ext2", extra_slot=False, keyfile=False, argon=None):
    CASES.append(dict(name=name, ver=ver, cipher=cipher, bits=bits, hash=hsh, pbkdf=pbkdf, sector=sector, plain=plain, extra_slot=extra_slot, keyfile=keyfile, argon=argon))


case("luks1_xts512_sha256", 1, "aes-xts-plain64", 512, "sha256")
case("luks1_xts256_sha1", 1, "aes-xts-plain64", 256, "sha1")
case("luks1_cbc_essiv256_sha256", 1, "aes-cbc-essiv:sha256", 256, "sha256")
case("luks1_cbc_plain64_128_sha512", 1, "aes-cbc-plain64", 128, "sha512")
case("luks1_multislot_keyfile", 1, "aes-xts-plain64", 512, "sha256", extra_slot=True, keyfile=True)
case("luks2_xts512_pbkdf2", 2, "aes-xts-plain64", 512, "sha256", plain="ext4")
case("luks2_xts256_pbkdf2_sha512", 2, "aes-xts-plain64", 256, "sha512")
case("luks2_xts512_argon2id", 2, "aes-xts-plain64", 512, "sha256", pbkdf="argon2id", plain="ext4", argon=(65536, 2, 4))
case("luks2_xts256_argon2i", 2, "aes-xts-plain64", 256, "sha256", pbkdf="argon2i", argon=(32768, 1, 4))
case("luks2_xts512_sector4096", 2, "aes-xts-plain64", 512, "sha256", pbkdf="argon2id", sector=4096, plain="ext4", argon=(65536, 2, 4))
case("luks2_cbc_essiv256", 2, "aes-cbc-essiv:sha256", 256, "sha256")
case("luks2_cbc_plain64_128", 2, "aes-cbc-plain64", 128, "sha256", sector=512)
case("luks2_multislot_keyfile", 2, "aes-xts-plain64", 512, "sha256", extra_slot=True, keyfile=True)


def build(c, plains):
    name = c["name"]
    img = os.path.join(OUT, name + ".img")
    pw = os.path.join(OUT, name + ".pw")
    mkf = os.path.join(OUT, name + ".mk")
    plain = plains[c["plain"]]
    pdata = open(plain, "rb").read()
    open(pw, "wb").write(("passphrase for %s" % name).encode())
    rnd = random.Random(name)
    mk = bytes(rnd.randrange(256) for _ in range(c["bits"] // 8))
    open(mkf, "wb").write(mk)
    with open(img, "wb") as f:
        f.truncate(len(pdata) + (20 << 20))
    cmd = [CS, "luksFormat", "--type", "luks%d" % c["ver"], "--batch-mode", "--disable-locks", "--cipher", c["cipher"], "--key-size", str(c["bits"]), "--hash", c["hash"],
           "--key-file", pw, "--volume-key-file", mkf]
    if c["ver"] == 2:
        cmd += ["--sector-size", str(c["sector"])]
    if c["pbkdf"].startswith("argon2"):
        mem, par, it = c["argon"]
        pb = ["--pbkdf", c["pbkdf"], "--pbkdf-memory", str(mem), "--pbkdf-parallel", str(par), "--pbkdf-force-iterations", str(it)]
    else:
        pb = ["--pbkdf", "pbkdf2", "--pbkdf-force-iterations", "1000"] if c["ver"] == 2 else ["--pbkdf-force-iterations", "1000"]
    run(cmd + pb + [img])
    if c["extra_slot"]:
        pw2 = os.path.join(OUT, name + ".pw2")
        open(pw2, "wb").write(bytes(rnd.randrange(256) for _ in range(300)))  # キーファイル (任意バイト列)
        run([CS, "luksAddKey", "--batch-mode", "--disable-locks", "--key-file", pw] + pb + [img, pw2])
    # cryptsetup 自身にパスフレーズとマスターキーを確認させる
    run([CS, "open", "--test-passphrase", "--disable-locks", "--key-file", pw, img])
    if c["extra_slot"]:
        run([CS, "open", "--test-passphrase", "--disable-locks", "--key-file", os.path.join(OUT, name + ".pw2"), img])
    dump = run([CS, "luksDump", "--dump-master-key", "--batch-mode", "--disable-locks", "--key-file", pw, img])
    lines = dump.splitlines()
    i0 = [i for i, l in enumerate(lines) if l.startswith("MK dump")][0]
    mkhex = lines[i0].split(":", 1)[1]
    for l in lines[i0 + 1:]:
        if not l.startswith("\t"):
            break
        mkhex += l
    mkhex = "".join(mkhex.split())
    assert mkhex == mk.hex(), "master key mismatch for " + name
    # データ領域の位置・セクタサイズ
    if c["ver"] == 2:
        js = json.loads(run([CS, "luksDump", "--dump-json-metadata", "--disable-locks", img]))
        seg = js["segments"]["0"]
        off, ss = int(seg["offset"]), int(seg.get("sector_size", 512))
        assert ss == c["sector"], (ss, c["sector"])
    else:
        d = run([CS, "luksDump", "--disable-locks", img])
        off = int([l for l in d.splitlines() if l.startswith("Payload offset")][0].split(":")[1]) * 512
        ss = 512
    mode = c["cipher"].split("-", 1)[1]
    os.truncate(img, off + len(pdata))
    with open(img, "r+b") as f:
        f.seek(off)
        f.write(enc_payload(pdata, mode, mk, ss))
    # LUKS1: 独立実装 (nbdkit の luks フィルタ。essiv は非対応なので qemu-img の luks ドライバ) で復号して平文と一致を確認
    if c["ver"] == 1:
        outraw = os.path.join(OUT, name + ".oracle.raw")
        oracle = None
        try:
            run(["nbdkit", "-U", "-", "--filter=luks", "file", img, "passphrase=+" + pw, "--run", 'nbdcopy "$uri" ' + outraw])
            oracle = "nbdkit"
        except RuntimeError:
            run(["qemu-img", "convert", "-O", "raw", "--object", "secret,id=sec0,file=" + pw, "--image-opts", "driver=luks,file.filename=%s,key-secret=sec0" % img, outraw])
            oracle = "qemu-img"
        assert open(outraw, "rb").read() == pdata, "%s decrypt differs for %s" % (oracle, name)
        os.unlink(outraw)
        print("  %s: payload verified by independent decryptor (%s)" % (name, oracle))
    os.unlink(mkf)  # マスターキーは残さない
    return off


# ---- LUKS2 ヘッダの JSON 書き換え (チェックサム再計算) による異常系
def rewrite_luks2(src, dst, mutate, which="both"):
    shutil.copyfile(src, dst)
    with open(dst, "r+b") as f:
        f.seek(0)
        head = f.read(4096)
        hsz = struct.unpack(">Q", head[8:16])[0]
        for at in (0, hsz):
            f.seek(at)
            buf = bytearray(f.read(hsz))
            js = bytes(buf[4096:]).split(b"\0", 1)[0]
            obj = json.loads(js)
            mutate(obj)
            nj = json.dumps(obj, separators=(",", ":")).encode()
            assert len(nj) < hsz - 4096
            buf[4096:] = nj + b"\0" * (hsz - 4096 - len(nj))
            buf[448:512] = b"\0" * 64
            alg = bytes(buf[72:104]).split(b"\0", 1)[0].decode()
            buf[448:448 + hashlib.new(alg).digest_size] = hashlib.new(alg, bytes(buf)).digest()
            f.seek(at)
            f.write(bytes(buf))


def main():
    plains = {"ext2": make_plain("ext2"), "ext4": make_plain("ext4")}
    cases = []
    for c in CASES:
        off = build(c, plains)
        name = c["name"]
        pw = os.path.join(OUT, name + ".pw")
        cases.append((name, "pwfile:" + pw, plains[c["plain"]], "ok", "%s %s %d-bit %s sector=%d data@%d" % (c["pbkdf"], c["cipher"], c["bits"], c["hash"], c["sector"], off)))
        cases.append((name + "#wrong", "pw:definitely-wrong-passphrase", plains[c["plain"]], "auth-failed", "wrong passphrase"))
        if c["keyfile"]:
            cases.append((name + "#keyfile", "key:" + os.path.join(OUT, name + ".pw2"), plains[c["plain"]], "ok", "second keyslot via key file"))
    # 異常系 (LUKS2 JSON 書き換え)
    base = os.path.join(OUT, "luks2_xts512_pbkdf2.img")
    pwb = "pwfile:" + os.path.join(OUT, "luks2_xts512_pbkdf2.pw")

    def add_req(o):
        o["config"]["requirements"] = {"mandatory": ["online-reencrypt-v2"]}

    def add_integrity(o):
        o["segments"]["0"]["integrity"] = {"type": "hmac(sha256)", "journal_encryption": "none", "journal_integrity": "none"}

    def serpent(o):
        o["segments"]["0"]["encryption"] = "serpent-xts-plain64"

    def two_segments(o):
        o["segments"]["1"] = dict(o["segments"]["0"])

    for nm, fn, why in (("luks2_reencrypt", add_req, "再暗号化途中 (requirements)"), ("luks2_integrity", add_integrity, "dm-integrity"),
                        ("luks2_serpent", serpent, "serpent (非対応の暗号)"), ("luks2_twosegments", two_segments, "複数 segment")):
        dst = os.path.join(OUT, nm + ".img")
        rewrite_luks2(base, dst, fn)
        cases.append((nm, pwb, plains["ext4"], "unsupported", why))
    # 先頭ヘッダだけ壊す: 副ヘッダで開ける。両方壊す: corrupt
    dst = os.path.join(OUT, "luks2_primary_corrupt.img")
    shutil.copyfile(base, dst)
    with open(dst, "r+b") as f:
        f.seek(5000)
        f.write(b"X")
    cases.append(("luks2_primary_corrupt", pwb, plains["ext4"], "ok", "先頭ヘッダのチェックサム不一致 → 副ヘッダで開く"))
    dst = os.path.join(OUT, "luks2_both_corrupt.img")
    shutil.copyfile(dst.replace("both", "primary"), dst)
    with open(dst, "r+b") as f:
        f.seek(16384 + 5000)
        f.write(b"X")
    cases.append(("luks2_both_corrupt", pwb, plains["ext4"], "corrupt", "2 コピーとも不一致"))
    # LUKS1: ダイジェストを壊す → 正しいパスフレーズでも AuthFailed (キー候補の検証が効いている)
    b1 = os.path.join(OUT, "luks1_xts512_sha256.img")
    dst = os.path.join(OUT, "luks1_digest_corrupt.img")
    shutil.copyfile(b1, dst)
    with open(dst, "r+b") as f:
        f.seek(112)
        x = f.read(1)
        f.seek(112)
        f.write(bytes([x[0] ^ 1]))
    cases.append(("luks1_digest_corrupt", "pwfile:" + os.path.join(OUT, "luks1_xts512_sha256.pw"), plains["ext2"], "auth-failed", "mk-digest 破損"))
    with open(os.path.join(OUT, "cases.txt"), "w") as f:
        for c in cases:
            f.write("\t".join(c) + "\n")
    print("wrote %d cases in %s" % (len(cases), OUT))


main()
