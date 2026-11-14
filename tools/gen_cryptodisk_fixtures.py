#!/usr/bin/env python3
"""暗号ボリュームの「ディスク全体」の構成のテスト用イメージを作る (core/compat/tests/test_cryptodisk.cpp と tools/run_cryptodisk_tests.sh 用)。docs/crypto-volumes.md 6.5
使い方: tools/gen_cryptodisk_fixtures.py [出力ディレクトリ(既定 /dev/shm/usbmem_cryptodisk_$USER)]
        tools/gen_cryptodisk_fixtures.py --high-offset <既存の大きな疎ファイル> <出力ディレクトリ>   (nbdfuse の DIR/nbd など。4 GiB を超えるオフセットに暗号パーティションを置く)

作るもの (すべて root 不要。パーティション表は本物の sgdisk / sfdisk、LUKS ヘッダは本物の cryptsetup luksFormat、BitLocker は tools/bitlocker_synth.py の合成):
  - パーティション表 (GPT / MBR+拡張) の中の暗号パーティション: LUKS2 (argon2id / pbkdf2)、LUKS1、BitLocker (合成)。中は ext4 / NTFS
  - ディスク全体が 1 つの暗号ボリューム (表なし): LUKS1 / LUKS2 / BitLocker
  - ディスク全体が暗号ボリュームで、その中に GPT (中に ext4 / NTFS。さらに中に LUKS1 の入れ子も)
  - 上記から作る異常系 (間違ったパスワード・資格情報なし・切り詰め・先頭の破損) は cases.txt の引数 (--cut / --poke) だけで表す (イメージは増やさない)
出力: <out>/*.img、<out>/manifests/*.manifest (f/d サイズ sha256 パス)、<out>/cases.txt (タブ区切り: group, name, image, usbmem_cryptodisk_tests の引数)。
パスワードはすべて使い捨て (cases.txt に平文で入る)。環境変数 USBMEM_WINBL_DIR に実 Windows 製 BitLocker イメージ (bl_*.img と .manifest) があれば、それを GPT の中に入れた構成も足す。
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
HERE = os.path.dirname(os.path.abspath(__file__))
CS = shutil.which("cryptsetup") or "/sbin/cryptsetup"
MIB = 1 << 20
SECTOR = 512
INNER_MB = 12  # 中の FS (または暗号ボリュームの平文) の大きさ


def run(cmd, **kw):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kw)
    if r.returncode != 0:
        raise RuntimeError("%s failed: %s" % (" ".join(cmd), (r.stdout + r.stderr).decode(errors="replace")[-600:]))
    return r.stdout.decode(errors="replace")


def align_up(v, a):
    return (v + a - 1) // a * a


# ---- ファイルツリーと manifest ------------------------------------------------------------------------------------------
def pattern(seed, off, n):
    return bytes((((off + i) * 31 + seed * 7 + ((off + i) >> 8)) & 0xFF) for i in range(n))


def make_tree(tree, seed):
    """seed ごとに内容が違うツリー (別ボリュームの内容と取り違えてもハッシュで分かる)。シンボリックリンクは作らない (NTFS / manifest の対象外)。"""
    shutil.rmtree(tree, ignore_errors=True)
    os.makedirs(os.path.join(tree, "dir/sub/deeper"))
    os.makedirs(os.path.join(tree, "empty_dir"))
    sizes = [0, 1, 511, 512, 513, 4095, 4096, 4097, 65536, 700001, 1 * MIB + 17]
    for i, sz in enumerate(sizes):
        with open(os.path.join(tree, "f_%d.bin" % sz), "wb") as f:
            f.write(pattern(seed * 13 + i + 1, 0, sz))
    for i in range(40):
        with open(os.path.join(tree, "dir/sub/file_%02d.txt" % i), "wb") as f:
            f.write(pattern(seed * 17 + 100 + i, 0, 100 + i * 37))
    with open(os.path.join(tree, "dir/sub/deeper/日本語 ファイル.txt"), "wb") as f:
        f.write(("unicode name %d\n" % seed).encode())
    with open(os.path.join(tree, "seed.txt"), "w") as f:
        f.write("volume seed %d\n" % seed)


def write_manifest(tree, path):
    lines = []
    for root, dirs, files in os.walk(tree):
        dirs.sort()
        for d in dirs:
            lines.append("d 0 - %s" % os.path.relpath(os.path.join(root, d), tree))
        for fn in sorted(files):
            p = os.path.join(root, fn)
            h = hashlib.sha256(open(p, "rb").read()).hexdigest()
            lines.append("f %d %s %s" % (os.path.getsize(p), h, os.path.relpath(p, tree)))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


# ---- FS イメージ (平文) ------------------------------------------------------------------------------------------------
def make_ext4(path, size, tree, label):
    with open(path, "wb") as f:
        f.truncate(size)
    run(["mke2fs", "-q", "-t", "ext4", "-F", "-L", label, "-E", "root_owner=0:0", "-d", tree, path])


def make_ntfs(path, size, tree, label, work):
    with open(path, "wb") as f:
        f.truncate(size)
    run(["mkntfs", "-F", "-Q", "-q", "-s", "512", "-L", label, path])
    mnt = os.path.join(work, "ntfs_mnt")
    os.makedirs(mnt, exist_ok=True)
    run(["ntfs-3g", "-o", "rw", path, mnt])
    try:
        for name in sorted(os.listdir(tree)):
            src = os.path.join(tree, name)
            if os.path.isdir(src):
                shutil.copytree(src, os.path.join(mnt, name))
            else:
                shutil.copyfile(src, os.path.join(mnt, name))
    finally:
        run(["fusermount3", "-u", mnt])
        os.rmdir(mnt)


# ---- 暗号化 (dm-crypt と同じ規則) -------------------------------------------------------------------------------------
def enc_sector(mode, key, iv_sector, data):
    if mode == "xts-plain64":
        return Cipher(algorithms.AES(key), modes.XTS(struct.pack("<QQ", iv_sector, 0))).encryptor().update(data)
    if mode == "cbc-essiv:sha256":
        ek = hashlib.sha256(key).digest()
        iv = Cipher(algorithms.AES(ek), modes.ECB()).encryptor().update(struct.pack("<QQ", iv_sector, 0))
        return Cipher(algorithms.AES(key), modes.CBC(iv)).encryptor().update(data)
    raise ValueError(mode)


def luks_encrypt_payload(src, dst, off, mode, key):
    with open(src, "rb") as f, open(dst, "r+b") as g:
        g.seek(off)
        i = 0
        while True:
            blk = f.read(SECTOR * 256)
            if not blk:
                break
            out = bytearray()
            for k in range(len(blk) // SECTOR):
                out += enc_sector(mode, key, i, blk[k * SECTOR:(k + 1) * SECTOR])
                i += 1
            g.write(bytes(out))


def make_luks(plain, out, ver, pbkdf, cipher, bits, pw, seed):
    """cryptsetup luksFormat (本物) でヘッダを作り、ペイロードをマスターキーで暗号化して書く。"""
    plen = os.path.getsize(plain)
    pwf, mkf = out + ".pw", out + ".mk"
    open(pwf, "wb").write(pw.encode())
    rnd = random.Random("cryptodisk-%s" % seed)
    mk = bytes(rnd.randrange(256) for _ in range(bits // 8))
    open(mkf, "wb").write(mk)
    with open(out, "wb") as f:
        f.truncate(plen + 20 * MIB)
    cmd = [CS, "luksFormat", "--type", "luks%d" % ver, "--batch-mode", "--disable-locks", "--cipher", cipher, "--key-size", str(bits), "--hash", "sha256",
           "--key-file", pwf, "--volume-key-file", mkf]
    if ver == 2:
        cmd += ["--sector-size", "512"]
        if pbkdf == "argon2id":
            cmd += ["--pbkdf", "argon2id", "--pbkdf-memory", "65536", "--pbkdf-parallel", "4", "--pbkdf-force-iterations", "4"]
        else:
            cmd += ["--pbkdf", "pbkdf2", "--pbkdf-force-iterations", "1000"]
    else:
        cmd += ["--pbkdf-force-iterations", "1000"]
    run(cmd + [out])
    run([CS, "open", "--test-passphrase", "--disable-locks", "--key-file", pwf, out])  # cryptsetup 自身にパスフレーズを確認させる
    if ver == 2:
        off = int(json.loads(run([CS, "luksDump", "--dump-json-metadata", "--disable-locks", out]))["segments"]["0"]["offset"])
    else:
        d = run([CS, "luksDump", "--disable-locks", out])
        off = int([l for l in d.splitlines() if l.startswith("Payload offset")][0].split(":")[1]) * SECTOR
    os.truncate(out, off + plen)
    luks_encrypt_payload(plain, out, off, cipher.split("-", 1)[1], mk)
    os.unlink(mkf)
    os.unlink(pwf)


def make_bitlocker(plain, out, pw, seed):
    run([sys.executable, os.path.join(HERE, "bitlocker_synth.py"), plain, out, "--method", "8004", "--password", pw, "--seed", seed])


# ---- ノード (パーティションの中身 / ディスク) ---------------------------------------------------------------------------
class Node:
    """path: ペイロードのファイル (None = 空 = 疎の何もない領域)。size: バイト。kind: パーティションタイプの選択用。
    expects: 相対キー ('' = このノード自身、'.dec' = 復号後、'.p2' = 2 番パーティション、'#table' = 表) → 期待。creds: (相対キー, 資格情報)。"""

    def __init__(self, path, size, kind, expects, creds=(), layout=None, meta=None):
        self.path, self.size, self.kind = path, size, kind
        self.expects, self.creds = dict(expects), list(creds)
        self.layout = layout or {}  # ディスクのとき: パーティション番号 -> (開始バイト, 長さ)
        self.meta = meta or {}


class Builder:
    def __init__(self, out):
        self.out = out
        self.work = os.path.join(out, "_work")
        self.man = os.path.join(out, "manifests")
        os.makedirs(self.work, exist_ok=True)
        os.makedirs(self.man, exist_ok=True)
        self.n = 0
        self.cases = []

    def tmp(self, suffix):
        self.n += 1
        return os.path.join(self.work, "t%d%s" % (self.n, suffix))

    def fs(self, fs, seed, label=None, size_mb=INNER_MB):
        tree = self.tmp("_tree")
        make_tree(tree, seed)
        man = os.path.join(self.man, "v%d.manifest" % seed)
        write_manifest(tree, man)
        path = self.tmp(".%s" % fs)
        label = label or ("CD%d" % seed)
        if fs == "ext4":
            make_ext4(path, size_mb * MIB, tree, label)
        else:
            make_ntfs(path, size_mb * MIB, tree, label, self.work)
        shutil.rmtree(tree)
        return Node(path, size_mb * MIB, fs, {"": "verified:" + man})

    def enc(self, inner, kind, pw, seed):
        """inner を暗号ボリュームに入れる。kind: luks1 / luks1-essiv / luks2-argon2id / luks2-pbkdf2 / bitlocker"""
        path = self.tmp(".enc")
        if kind == "bitlocker":
            make_bitlocker(inner.path, path, pw, "cd%d" % seed)
            nk = "bitlocker"
        else:
            ver = 1 if kind.startswith("luks1") else 2
            cipher, bits = ("aes-cbc-essiv:sha256", 256) if kind == "luks1-essiv" else ("aes-xts-plain64", 512)
            make_luks(inner.path, path, ver, kind.split("-", 1)[1] if ver == 2 else "pbkdf2", cipher, bits, pw, "%s-%d" % (kind, seed))
            nk = "luks"
        os.unlink(inner.path)
        exp = {"": "unlocked"}
        exp.update({".dec" + k: v for k, v in inner.expects.items()})
        creds = [("", "pw:" + pw)] + [(".dec" + k, c) for k, c in inner.creds]
        return Node(path, os.path.getsize(path), nk, exp, creds, meta=dict(kind=kind, pw=pw, inner_size=inner.size))

    def disk(self, table, entries, existing=None, disk_mb=None):
        """entries: [Node | ('logical', Node) | ('gap', size_bytes)]。GPT は 1.. の番号、MBR は主 1-3 + 拡張 (4) + 論理 5..。"""
        path = existing or self.tmp(".disk")
        parts = []  # (index, node, logical)
        if table == "gpt":
            for i, e in enumerate(entries):
                parts.append((i + 1, e))
        else:
            prim = [e for e in entries if not isinstance(e, tuple) or e[0] != "logical"]
            logi = [e[1] for e in entries if isinstance(e, tuple) and e[0] == "logical"]
            for i, e in enumerate(prim):
                parts.append((i + 1, e))
            for i, e in enumerate(logi):
                parts.append((5 + i, ("logical", e)))
        nodes = {}
        for idx, e in parts:
            node = e[1] if isinstance(e, tuple) and e[0] == "logical" else e
            if isinstance(node, tuple) and node[0] == "gap":
                node = Node(None, node[1], "gap", {"": "unknown-fs"})
            nodes[idx] = (node, isinstance(e, tuple) and e[0] == "logical")
        ptype = {"ext4": ("8300", "83"), "ntfs": ("0700", "07"), "luks": ("8309", "83"), "bitlocker": ("0700", "07"), "disk": ("8300", "83"), "gap": ("8300", "83")}
        total_sectors = 0
        if table == "gpt":
            cur = 2048
            lay = {}
            for idx in sorted(nodes):
                n = nodes[idx][0]
                sec = align_up(n.size, SECTOR) // SECTOR
                lay[idx] = (cur, sec)
                cur = align_up(cur + sec, 2048)
            total_sectors = cur + 2048
        else:
            # 主パーティション → 拡張 (論理を全部含む) の順。論理の前に EBR 用の隙間 (2048 セクタ) を置く
            cur = 2048
            lay = {}
            for idx in sorted(i for i in nodes if not nodes[i][1]):
                sec = align_up(nodes[idx][0].size, SECTOR) // SECTOR
                lay[idx] = (cur, sec)
                cur = align_up(cur + sec, 2048)
            ext_start = cur
            for idx in sorted(i for i in nodes if nodes[i][1]):
                cur += 2048
                sec = align_up(nodes[idx][0].size, SECTOR) // SECTOR
                lay[idx] = (cur, sec)
                cur = align_up(cur + sec, 2048)
            ext = (ext_start, cur - ext_start) if any(v[1] for v in nodes.values()) else None
            total_sectors = cur + 2048
        if existing is None:
            with open(path, "wb") as f:
                f.truncate(total_sectors * SECTOR)
        else:
            assert os.path.getsize(path) >= total_sectors * SECTOR, "disk file too small"
        # パーティション表 (本物のツール)
        if table == "gpt":
            run(["sgdisk", "-o", path])
            for idx in sorted(nodes):
                s, n = lay[idx]
                run(["sgdisk", "-a", "1", "-n", "%d:%d:%d" % (idx, s, s + n - 1), "-t", "%d:%s" % (idx, ptype[nodes[idx][0].kind][0]), "-c", "%d:cd-part-%d" % (idx, idx), path])
        else:
            script = ["label: dos", "label-id: 0x%08x" % (0xCD000000 + len(self.cases)), "unit: sectors", ""]
            for idx in sorted(i for i in nodes if not nodes[i][1]):
                script.append("start=%d, size=%d, type=%s" % (lay[idx][0], lay[idx][1], ptype[nodes[idx][0].kind][1]))
            if ext:
                script.append("start=%d, size=%d, type=5" % ext)
            for idx in sorted(i for i in nodes if nodes[i][1]):
                script.append("start=%d, size=%d, type=%s" % (lay[idx][0], lay[idx][1], ptype[nodes[idx][0].kind][1]))
            run(["sfdisk", "--no-reread", "--no-tell-kernel", path], input=("\n".join(script) + "\n").encode())
        # 読み戻して、置く位置を sfdisk の出力で決める (自分の計算と一致を確認)
        tab = json.loads(run(["sfdisk", "-J", path]))["partitiontable"]["partitions"]
        got = {}
        for p in tab:
            idx = int("".join(ch for ch in p["node"][len(path):] if ch.isdigit()))
            got[idx] = (p["start"], p["size"])
        for idx in nodes:
            assert got.get(idx) == lay[idx], "partition %d: sfdisk says %s, expected %s" % (idx, got.get(idx), lay[idx])
        with open(path, "r+b") as g:
            for idx in sorted(nodes):
                n = nodes[idx][0]
                if n.path is None:
                    continue
                g.seek(lay[idx][0] * SECTOR)
                with open(n.path, "rb") as f:
                    shutil.copyfileobj(f, g, 1 << 20)
                os.unlink(n.path)
        exp = {"#table": "gpt" if table == "gpt" else "mbr"}
        creds = []
        for idx in nodes:
            n = nodes[idx][0]
            exp.update({".p%d" % idx + k: v for k, v in n.expects.items()})
            creds += [(".p%d" % idx + k, c) for k, c in n.creds]
        layout = {idx: (lay[idx][0] * SECTOR, nodes[idx][0].size) for idx in nodes}
        return Node(path, total_sectors * SECTOR, "disk", exp, creds, layout=layout)

    # ---- cases.txt
    def case(self, group, name, image, node, extra=(), exp_override=None, drop=(), only_creds=None, lenient=False, creds=None):
        args = []
        creds = node.creds if creds is None else creds
        for k, c in creds:
            args += ["--cred", "d%s=%s" % (k, c)]
        args += list(extra)
        if lenient:
            args.append("--lenient")
        exp = dict(node.expects)
        for k in drop:
            exp.pop(k, None)
        if exp_override:
            exp.update(exp_override)
        for k, v in sorted(exp.items()):
            args.append("d%s=%s" % (k, v))
        self.cases.append((group, name, image, args))

    def write_cases(self):
        with open(os.path.join(self.out, "cases.txt"), "w") as f:
            for g, n, im, a in self.cases:
                f.write("%s\t%s\t%s\t%s\n" % (g, n, im, " ".join(a)))


def hexle(b):
    return b.hex()


def main():
    args = sys.argv[1:]
    high = None
    if args and args[0] == "--high-offset":
        high, args = args[1], args[2:]
    out = os.path.abspath(args[0] if args else "/dev/shm/usbmem_cryptodisk_%s" % os.environ.get("USER", "u"))
    shutil.rmtree(out, ignore_errors=True) if high is None else None
    os.makedirs(out, exist_ok=True)
    b = Builder(out)
    if high:
        build_high_offset(b, high)
    else:
        build_all(b)
    b.write_cases()
    shutil.rmtree(b.work, ignore_errors=True)
    print("wrote %d cases in %s" % (len(b.cases), out))


def build_all(b):
    out = b.out

    def img(name):
        return os.path.join(out, name + ".img")

    # ===== 1. パーティション表の中の暗号パーティション =====
    # GPT: p1 平文 ext4 (対照) / p2 LUKS2 argon2id→ext4 / p3 LUKS2 pbkdf2→ext4 / p4 LUKS1→ext4 / p5 BitLocker(合成)→NTFS / p6 平文 NTFS (対照)
    gpt = b.disk("gpt", [b.fs("ext4", 1001, "PLAIN_EXT4"),
                         b.enc(b.fs("ext4", 1002), "luks2-argon2id", "cdpw-gpt-p2", 1002),
                         b.enc(b.fs("ext4", 1003), "luks2-pbkdf2", "cdpw-gpt-p3", 1003),
                         b.enc(b.fs("ext4", 1004), "luks1", "cdpw-gpt-p4", 1004),
                         b.enc(b.fs("ntfs", 1005), "bitlocker", "cdpw-gpt-p5", 1005),
                         b.fs("ntfs", 1006, "PLAIN_NTFS")])
    os.replace(gpt.path, img("gpt_all"))
    gpt.path = img("gpt_all")
    b.case("part", "gpt_all", gpt.path, gpt)
    # MBR: p1 LUKS2 argon2id / p2 LUKS1 (cbc-essiv) / p3 BitLocker / 拡張 p4 { 論理 p5 LUKS2 pbkdf2 / 論理 p6 平文 ext4 }
    mbr = b.disk("mbr", [b.enc(b.fs("ext4", 2001), "luks2-argon2id", "cdpw-mbr-p1", 2001),
                         b.enc(b.fs("ext4", 2002), "luks1-essiv", "cdpw-mbr-p2", 2002),
                         b.enc(b.fs("ntfs", 2003), "bitlocker", "cdpw-mbr-p3", 2003),
                         ("logical", b.enc(b.fs("ext4", 2005), "luks2-pbkdf2", "cdpw-mbr-p5", 2005)),
                         ("logical", b.fs("ext4", 2006, "PLAIN_LOGICAL"))])
    os.replace(mbr.path, img("mbr_all"))
    mbr.path = img("mbr_all")
    b.case("part", "mbr_all", mbr.path, mbr)
    # 資格情報を全ノード共通のリストにして、AuthFailed なら次を試す (アプリが 1 つずつ入力を受ける流れ)
    allpw = [c for _, c in gpt.creds]
    b.case("part", "gpt_all_shared_credentials", gpt.path, gpt, creds=[], extra=sum((["--cred", c] for c in reversed(allpw)), []))
    # 実 Windows 製 BitLocker (あれば)
    winbl = os.environ.get("USBMEM_WINBL_DIR", "")
    for nm in ("bl_xts256full", "bl_cbc128", "win_bitlocker"):
        p = os.path.join(winbl, nm + ".img") if winbl else ""
        if p and os.path.isfile(p):
            real_bitlocker(b, p, nm)

    # ===== 2. ディスク全体が 1 つの暗号ボリューム (パーティション表なし) =====
    whole = {}
    for name, kind, fs, seed in (("w_luks1", "luks1", "ext4", 3001), ("w_luks2_argon2id", "luks2-argon2id", "ext4", 3002),
                                 ("w_luks2_pbkdf2_ntfs", "luks2-pbkdf2", "ntfs", 3003), ("w_bitlocker", "bitlocker", "ntfs", 3004)):
        n = b.enc(b.fs(fs, seed), kind, "cdpw-" + name, seed)
        os.replace(n.path, img(name))
        n.path = img(name)
        whole[name] = n
        b.case("whole", name, n.path, n)
        b.case("whole", name + "#wrong-password", n.path, n, creds=[("", "pw:definitely-wrong")], exp_override={"": "auth-failed"}, drop=[".dec"])
    # LUKS1 ヘッダの未使用 keyslot の salt の 2 バイトがたまたま 55 AA になると、先頭セクタが MBR の署名に見える (1/65536)。salt の値は機能に影響しないので、そうしたイメージを作る
    n = whole["w_luks1"]
    b.case("whole", "w_luks1_mbr_signature_lookalike", n.path, n, extra=["--poke", "510=55aa"])

    # ===== 3. ディスク全体が暗号ボリュームで、その中にパーティション表 (GPT) =====
    for name, kind, seed in (("wg_luks2", "luks2-argon2id", 4001), ("wg_luks1", "luks1", 4002), ("wg_bitlocker", "bitlocker", 4003)):
        inner = b.disk("gpt", [b.fs("ext4", seed * 10 + 1), b.fs("ntfs", seed * 10 + 2)])
        n = b.enc(inner, kind, "cdpw-" + name, seed)
        os.replace(n.path, img(name))
        n.path = img(name)
        b.case("nested", name, n.path, n)
    # 入れ子: LUKS2 (全体) の中の GPT の p2 がさらに LUKS1
    inner = b.disk("gpt", [b.fs("ext4", 4101), b.enc(b.fs("ext4", 4102), "luks1", "cdpw-inner-luks1", 4102)])
    n = b.enc(inner, "luks2-pbkdf2", "cdpw-wg-nested", 4100)
    os.replace(n.path, img("wg_luks2_luks1"))
    n.path = img("wg_luks2_luks1")
    b.case("nested", "wg_luks2_luks1", n.path, n)
    # 全体が暗号で、中が GPT: パスワード違い → 中は見えない
    wg = os.path.join(out, "wg_luks2.img")
    # (wg_luks2 の expects を取り直さずに済むよう、期待は固定で書く)
    b.cases.append(("nested", "wg_luks2#wrong-password", wg, ["--cred", "d=pw:definitely-wrong", "d=auth-failed"]))

    # ===== 4. 異常系 (gpt_all / mbr_all に対する引数だけ) =====
    errors(b, "gpt", gpt, mbr)


def errors(b, _name, gpt, mbr):
    gi, mi = gpt.path, mbr.path
    g = gpt.layout
    m = mbr.layout
    ok_all = dict(gpt.expects)
    # (a) 暗号パーティションが複数あるうち 1 つだけパスワードが違う / 全部違う / 資格情報なし
    wrong_p3 = [(k, c) if k != ".p3" else (k, "pw:wrong-for-p3") for k, c in gpt.creds]
    b.case("errors", "gpt_one_wrong_password", gi, gpt, creds=wrong_p3, exp_override={".p3": "auth-failed"}, drop=[".p3.dec"])
    b.case("errors", "gpt_all_wrong_passwords", gi, gpt, creds=[(k, "pw:wrong-%s" % k.strip(".")) if k.endswith(("p2", "p3", "p4", "p5")) else (k, c) for k, c in gpt.creds],
           exp_override={".p2": "auth-failed", ".p3": "auth-failed", ".p4": "auth-failed", ".p5": "auth-failed"}, drop=[".p2.dec", ".p3.dec", ".p4.dec", ".p5.dec"])
    b.case("errors", "gpt_no_credentials", gi, gpt, creds=[], exp_override={".p2": "no-credentials", ".p3": "no-credentials", ".p4": "no-credentials", ".p5": "no-credentials"},
           drop=[".p2.dec", ".p3.dec", ".p4.dec", ".p5.dec"])
    wrong_m = [(k, c) if k != ".p3" else (k, "pw:wrong-for-mbr-p3") for k, c in mbr.creds]
    b.case("errors", "mbr_one_wrong_password", mi, mbr, creds=wrong_m, exp_override={".p3": "auth-failed"}, drop=[".p3.dec"])
    # (b) 先頭だけ壊れたヘッダ
    s2, s3, s4, s5 = g[2][0], g[3][0], g[4][0], g[5][0]
    # LUKS2: 先頭ヘッダの JSON の 1 バイト (チェックサム不一致) → 副ヘッダ (16 KiB 位置) で開く
    b.case("errors", "gpt_luks2_primary_json_corrupt", gi, gpt, extra=["--poke", "%d=58" % (s2 + 4096 + 20)])
    # LUKS2: 先頭のマジックを消す (副ヘッダは無傷)。結果は実装の挙動 (副ヘッダで開く or 未知) のどちらでもよいが、誤った内容を見せない・他のパーティションに影響しない
    b.case("errors", "gpt_luks2_primary_magic_wiped", gi, gpt, extra=["--poke", "%d=000000000000" % s3], lenient=True,
           exp_override={".p3": "safe", ".p3.dec": "safe"})
    # LUKS1: マジックを消す (LUKS1 に予備ヘッダは無い) → 暗号とは認識されず、FS でもない
    b.case("errors", "gpt_luks1_magic_wiped", gi, gpt, extra=["--poke", "%d=000000000000" % s4], exp_override={".p4": "unknown-fs"}, drop=[".p4.dec"])
    # LUKS1: ヘッダの mk-digest を 1 バイト壊す → 正しいパスフレーズでも AuthFailed
    b.case("errors", "gpt_luks1_digest_corrupt", gi, gpt, extra=["--poke", "%d=ff" % (s4 + 112)], exp_override={".p4": "auth-failed"}, drop=[".p4.dec"])
    # BitLocker: ブートセクタの署名 -FVE-FS- を消す → 未知
    b.case("errors", "gpt_bitlocker_signature_wiped", gi, gpt, extra=["--poke", "%d=0000000000000000" % (s5 + 3)], exp_override={".p5": "unknown-fs"}, drop=[".p5.dec"])
    # BitLocker: 3 つのメタデータのうち 1 つ目を壊す → 残りで開く
    plen5 = g[5][1] - 0x40000
    b.case("errors", "gpt_bitlocker_metadata_copy1_corrupt", gi, gpt, extra=["--poke", "%d=%s" % (s5 + plen5 + 0x10, "ff" * 32)])
    # 複数の暗号パーティションを同時に壊す (LUKS1 のマジック + BitLocker の署名)。他は無傷で通る
    b.case("errors", "gpt_two_headers_wiped", gi, gpt, extra=["--poke", "%d=000000000000" % s4, "--poke", "%d=0000000000000000" % (s5 + 3)],
           exp_override={".p4": "unknown-fs", ".p5": "unknown-fs"}, drop=[".p4.dec", ".p5.dec"])
    # パーティション表の破損: GPT 先頭ヘッダのシグネチャ / パーティションエントリ配列 → 予備の GPT から復元して全部通る
    b.case("errors", "gpt_primary_header_wiped", gi, gpt, extra=["--poke", "512=0000000000000000"], exp_override={"#table": "gpt-backup"})
    b.case("errors", "gpt_primary_entries_corrupt", gi, gpt, extra=["--poke", "1024=ffffffffffffffff"], exp_override={"#table": "gpt-backup"})
    # (c) 暗号パーティションの途中までの切り詰め。クラッシュ・ハングなし、誤った内容 (mismatch) を見せない。
    # (c-1) パーティションだけが途中までしかない (パーティション表は無傷。途中まで書いたイメージ・パーティションのコピー失敗など): --clip。他のパーティションは影響を受けない
    def clip_case(name, img, node, idx, n, exp=None):
        ov = {k: "safe" for k in node.expects if k == ".p%d" % idx or k.startswith(".p%d." % idx)}
        ov.update(exp or {})
        b.case("errors", name, img, node, extra=["--clip", "d.p%d=%d" % (idx, n)], exp_override=ov)

    clip_case("gpt_clip_p2_luks2_argon2id_mid", gi, gpt, 2, g[2][1] // 2)
    clip_case("gpt_clip_p2_luks2_in_header", gi, gpt, 2, 8192)
    clip_case("gpt_clip_p3_luks2_pbkdf2_mid", gi, gpt, 3, g[3][1] // 2)
    # ヘッダ (LUKS2 は 16 MiB・LUKS1 は 2 MiB) は無傷でペイロードだけ 4 MiB 足りない: 解錠はでき、中身は誤った内容を見せない (読めない分は読み取りエラー)
    clip_case("gpt_clip_p2_luks2_payload_short", gi, gpt, 2, g[2][1] - 4 * MIB, exp={".p2": "unlocked"})
    clip_case("gpt_clip_p3_luks2_payload_short", gi, gpt, 3, g[3][1] - 4 * MIB, exp={".p3": "unlocked"})
    clip_case("gpt_clip_p4_luks1_payload_short", gi, gpt, 4, g[4][1] - 4 * MIB, exp={".p4": "unlocked"})
    clip_case("gpt_clip_p4_luks1_mid", gi, gpt, 4, g[4][1] // 2)
    clip_case("gpt_clip_p4_luks1_in_header", gi, gpt, 4, 1024)
    clip_case("gpt_clip_p5_bitlocker_mid", gi, gpt, 5, g[5][1] // 2)
    # BitLocker のメタデータは末尾。2 つ目のコピーの途中で切れていても、1 つ目 + ボリュームヘッダ退避領域が無傷なので開けて、内容も全部正しい
    clip_case("gpt_clip_p5_bitlocker_in_metadata_copy2", gi, gpt, 5, g[5][1] - 0x40000 + 0x20000 + 0x8000, exp={".p5": "unlocked", ".p5.dec": gpt.expects[".p5.dec"]})
    clip_case("mbr_clip_p1_luks2_argon2id_mid", mi, mbr, 1, m[1][1] // 2)
    clip_case("mbr_clip_p2_luks1_essiv_mid", mi, mbr, 2, m[2][1] // 2)
    clip_case("mbr_clip_p3_bitlocker_mid", mi, mbr, 3, m[3][1] // 2)
    clip_case("mbr_clip_logical_p5_luks2_pbkdf2_mid", mi, mbr, 5, m[5][1] // 2)
    # (c-2) ディスク全体をその位置で切り詰める。GPT は予備ヘッダが無くなり、先頭ヘッダの last usable LBA がディスクの外になるのでテーブル全体が使えない (何も見せない)。MBR は切り詰め位置より前に収まる分だけ残る
    def cut_case(name, img, node, cut, gpt_table):
        ov = {"#table": "safe"}
        for k in node.expects:
            if not k.startswith(".p"):
                continue
            idx = int(k[2:].split(".")[0])
            st, sz = node.layout[idx]
            if gpt_table or st + sz > cut:
                ov[k] = "safe"
        b.case("errors", name, img, node, extra=["--cut", str(cut)], exp_override=ov, lenient=True)

    mid = lambda i: g[i][0] + g[i][1] // 2
    for nm, cut in (("mid_p2_luks2_argon2id", mid(2)), ("mid_p5_bitlocker", mid(5)), ("in_gpt_entries", 1024 + 200)):
        cut_case("gpt_cut_" + nm, gi, gpt, cut, True)
    mid = lambda i: m[i][0] + m[i][1] // 2
    for nm, cut in (("mid_p1_luks2_argon2id", mid(1)), ("mid_p2_luks1", mid(2)), ("mid_p3_bitlocker", mid(3)), ("mid_logical_p5_luks2", mid(5)), ("in_mbr", 300)):
        cut_case("mbr_cut_" + nm, mi, mbr, cut, False)
    # MBR: 先頭だけ壊れたヘッダ (LUKS2 主ヘッダの JSON、LUKS1 マジック、BitLocker 署名)
    b.case("errors", "mbr_luks2_primary_json_corrupt", mi, mbr, extra=["--poke", "%d=58" % (m[1][0] + 4096 + 20)])
    b.case("errors", "mbr_luks1_magic_wiped", mi, mbr, extra=["--poke", "%d=000000000000" % m[2][0]], exp_override={".p2": "unknown-fs"}, drop=[".p2.dec"])
    b.case("errors", "mbr_bitlocker_signature_wiped", mi, mbr, extra=["--poke", "%d=0000000000000000" % (m[3][0] + 3)], exp_override={".p3": "unknown-fs"}, drop=[".p3.dec"])
    # MBR のパーティション表 (ブートセクタの 55AA) が壊れる
    b.case("errors", "mbr_signature_wiped", mi, mbr, extra=["--poke", "510=0000"], lenient=True, exp_override={k: "safe" for k in mbr.expects})


def real_bitlocker(b, p, nm):
    """実 Windows 製 BitLocker イメージ (MBR + 1 パーティション) のパーティション部分を GPT の p2 に入れる。"""
    with open(p, "rb") as f:
        mbr = f.read(512)
        s, l = struct.unpack("<II", mbr[454:462])
        f.seek(s * SECTOR)
        data = f.read(l * SECTOR)
    path = b.tmp(".real")
    open(path, "wb").write(data)
    base = p[:-4]
    node = Node(path, len(data), "bitlocker", {"": "unlocked"})
    if os.path.isfile(base + ".manifest"):
        node.expects[".dec"] = "verified:" + base + ".manifest"
    node.creds = [("", "pw:usbmem-Test-pw-1")]
    d = b.disk("gpt", [b.fs("ext4", 5001, "PLAIN_EXT4"), node])
    img = os.path.join(b.out, "gpt_real_%s.img" % nm)
    os.replace(d.path, img)
    d.path = img
    b.case("part", "gpt_real_" + nm, img, d)


def build_high_offset(b, path):
    """4 GiB を超える位置に暗号パーティションを置く (32 ビットのバイト数/オフセットの桁あふれ検査)。path は nbdfuse の疎ファイル。"""
    gap = 4 * 1024 * MIB + 256 * MIB  # 約 4.25 GiB の何もない領域 (疎のまま)
    d = b.disk("gpt", [b.fs("ext4", 6001, "PLAIN_EXT4"), ("gap", gap), b.enc(b.fs("ext4", 6003), "luks2-argon2id", "cdpw-high-p3", 6003),
                       b.enc(b.fs("ntfs", 6004), "bitlocker", "cdpw-high-p4", 6004)], existing=path)
    assert d.layout[3][0] > (1 << 32), d.layout
    b.case("high", "gpt_high_offset", path, d)
    print("layout:", {k: (v[0], v[1]) for k, v in d.layout.items()})


main()
