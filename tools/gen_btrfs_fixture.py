#!/usr/bin/env python3
"""Btrfs のテスト用イメージを作る (--shrink で最小のサイズに縮める) (core/compat/tests/test_btrfs.cpp 用)。
使い方: tools/gen_btrfs_fixture.py <outdir> [variant...]   (PATH に mkfs.btrfs / btrfs が必要。/sbin を自動で足す)
ツリーは tools/gen_squashfs_fixture.py (= gen_ext2_fixture.py の build() + ブロック境界のファイル + 乱数/繰り返しデータ + FIFO) と同じ。
mkfs.btrfs は チェックサム (crc32c / xxhash / sha256 / blake2) × 圧縮 (なし / zlib / lzo / zstd) × ノードサイズ × セクタサイズ × 機能 (no-holes / mixed / block-group-tree …) × プロファイル (single / dup) の組み合わせで呼ぶ。
出力: <outdir>/<name>.img と <name>.manifest (gen_squashfs_fixture.py と同じ書式: D / F / L / S / R)。
独立した検証 (ドライバの出力ではなく参照実装との比較): 作った各イメージを `btrfs check --readonly` で検査し、`btrfs restore -S` で展開し直した結果が元のツリーとバイト一致することを確認する。
サブボリューム (--subvol) の入ったイメージは別途 subvols.* に作る (manifest は 2 種: 既定サブボリュームを開いたとき / トップレベル (ID 5) を開いたとき)。"""
import hashlib, importlib.util, os, shutil, socket, subprocess, sys, tempfile

os.environ["PATH"] = os.environ.get("PATH", "") + ":/sbin:/usr/sbin"
HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("gen_squashfs_fixture", os.path.join(HERE, "gen_squashfs_fixture.py"))
sq = importlib.util.module_from_spec(spec); spec.loader.exec_module(sq)
ext2 = sq.ext2

def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw)

# name, mkfs.btrfs の追加引数, 圧縮するか (フラグ)
VARIANTS = [
    ("crc32c",          [],                                                       False),
    ("xxhash",          ["--csum", "xxhash"],                                     False),
    ("sha256",          ["--csum", "sha256"],                                     False),
    ("blake2",          ["--csum", "blake2"],                                     False),
    ("zlib",            ["--compress", "zlib"],                                   True),
    ("zlib9",           ["--compress", "zlib:9"],                                 True),
    ("lzo",             ["--compress", "lzo"],                                    True),
    ("zstd",            ["--compress", "zstd"],                                   True),
    ("zstd15",          ["--compress", "zstd:15"],                                True),
    ("node4k",          ["-n", "4096"],                                           False),
    ("node8k",          ["-n", "8192"],                                           False),
    ("node32k",         ["-n", "32768"],                                          False),
    ("node64k",         ["-n", "65536", "--compress", "zstd"],                    True),
    # セクタサイズが 4 KiB 以外のものは mkfs.btrfs が大きな疎ファイルを扱えない (File exists) ので、小さなツリー (make_small) で作る
    ("sector8k",        ["-s", "8192", "-n", "16384"],                            False),
    ("sector16k_lzo",   ["-s", "16384", "-n", "16384", "--compress", "lzo"],      True),
    ("sector16k_zstd",  ["-s", "16384", "-n", "32768", "--compress", "zstd"],     True),
    ("sector64k_zlib",  ["-s", "65536", "-n", "65536", "--compress", "zlib"],     True),
    ("noholes",         ["-O", "no-holes"],                                       False),
    ("holes",           ["-O", "^no-holes"],                                      False),
    ("extref",          ["-O", "extref"],                                         False),
    ("bgtree",          ["-O", "block-group-tree"],                               False),
    ("nofst",           ["-O", "^free-space-tree"],                               False),
    ("single_single",   ["-m", "single", "-d", "single"],                         False),
    ("dup_dup",         ["-m", "dup", "-d", "dup"],                               False),
    ("mixed",           ["-M"],                                                   False),
]

def make_tree_manifest(tree, sqb):
    man = ext2.build(tree, 1024, extra=sq.extra_for(sqb))
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    cwd = os.getcwd()
    os.chdir(tree)
    try: s.bind("special_sock")
    finally: os.chdir(cwd); s.close()
    man.append("S\tspecial_sock\ts")
    return man

def check_and_restore(img, tree, td, name, restore=True):
    """btrfs check --readonly でクリーン、btrfs restore で展開し直した結果が元ツリーと一致 (参照実装との比較)。
    restore=False: セクタサイズが CPU のページサイズと違うイメージは btrfs restore (btrfs-progs 7.1) が abort する。check だけで、内容はテスト側の manifest との突き合わせで見る。"""
    chk = subprocess.run(["btrfs", "check", "--readonly", img], capture_output=True, text=True)
    if chk.returncode != 0:
        sys.exit("btrfs check NOT clean for %s:\n%s%s" % (name, chk.stdout[-1500:], chk.stderr[-500:]))
    if not restore: return
    ex = os.path.join(td, "ex"); os.makedirs(ex)
    r = subprocess.run(["btrfs", "restore", "-S", "-i", img, ex], capture_output=True, text=True)
    a, b = sq.tree_digest(tree), sq.tree_digest(ex)
    if a != b:
        sa, sb = set(a), set(b)
        diff = sorted(sa ^ sb)[:6]
        sys.exit("btrfs restore differs from source tree for %s: %d vs %d entries (rc %d), e.g. %s" % (name, len(a), len(b), r.returncode, diff))

def make_small(outdir, name, args, size="320M"):
    """小さなツリー (sector8k など用)。manifest は make() と同じ書式。大きい疎ファイルを含まない。"""
    sq.REFDIR[0] = outdir; sq.REFNAME[0] = name
    FM = ext2.FIXED_MTIME
    with tempfile.TemporaryDirectory(dir=outdir) as td:
        tree = os.path.join(td, "tree"); os.makedirs(tree + "/dir/sub")
        man = []
        n = [0]
        def stamp(p, is_link=False):
            n[0] += 1; os.utime(p, (FM + n[0], FM + n[0]), follow_symlinks=not is_link); return FM + n[0]
        seed = 1
        for sz in (0, 1, 100, 4095, 4096, 4097, 8191, 8192, 8193, 16383, 16384, 16385, 65535, 65536, 65537, 200000, 1000001):
            seed += 1
            p = os.path.join(tree, "f_%d.bin" % sz)
            with open(p, "wb") as f: f.write(ext2.pattern(seed, 0, sz))
            mt = stamp(p)
            man.append("F\tf_%d.bin\t%d\t%d\t1\t%s" % (sz, sz, mt, ("d:0:%d:%d" % (sz, seed)) if sz else ""))
        data = (b"compress me please " * 20000)[:300001]
        p = os.path.join(tree, "dir", "repeat.txt"); open(p, "wb").write(data); mt = stamp(p)
        open(os.path.join(outdir, name + ".ref_1"), "wb").write(data)
        man.append("R\tdir/repeat.txt\t%d\t%d\t%s" % (len(data), mt, name + ".ref_1"))
        import random
        rnd = random.Random(7); data = bytes(rnd.getrandbits(8) for _ in range(150000))
        p = os.path.join(tree, "dir", "sub", "random.bin"); open(p, "wb").write(data); mt = stamp(p)
        open(os.path.join(outdir, name + ".ref_2"), "wb").write(data)
        man.append("R\tdir/sub/random.bin\t%d\t%d\t%s" % (len(data), mt, name + ".ref_2"))
        os.symlink("f_100.bin", os.path.join(tree, "lnk")); man.append("L\tlnk\tf_100.bin")
        os.mkfifo(os.path.join(tree, "fifo")); man.append("S\tfifo\tp")
        for d in ("dir/sub", "dir"): man.append("D\t%s\t%d" % (d, stamp(os.path.join(tree, d))))
        img = os.path.join(outdir, name + ".img")
        if os.path.exists(img): os.remove(img)
        r = subprocess.run(["mkfs.btrfs", "-q", "-f", "-K", "--shrink", "-b", size] + args + ["-r", tree, img], capture_output=True, text=True)
        if r.returncode != 0:
            if os.path.exists(img): os.remove(img)
            return "mkfs.btrfs failed: " + (r.stderr or r.stdout).strip()[-200:]
        check_and_restore(img, tree, td, name, restore=False)
        with open(os.path.join(outdir, name + ".manifest"), "w", encoding="utf-8") as f: f.write("\n".join(man) + "\n")
        print("%s: %d manifest lines (small tree), btrfs check clean (restore skipped: sectorsize != page size)" % (name, len(man)))
    return None

def make(outdir, name, args, compressed, size="320M"):
    sq.REFDIR[0] = outdir; sq.REFNAME[0] = name
    with tempfile.TemporaryDirectory(dir=outdir) as td:
        tree = os.path.join(td, "tree"); os.makedirs(tree)
        sqb = 131072 if compressed else 4096
        man = make_tree_manifest(tree, sqb)
        img = os.path.join(outdir, name + ".img")
        if os.path.exists(img): os.remove(img)
        cmd = ["mkfs.btrfs", "-q", "-f", "-K", "--shrink", "-b", size] + args + ["-r", tree, img]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            if os.path.exists(img): os.remove(img)
            return "mkfs.btrfs failed: " + (r.stderr or r.stdout).strip()[-200:]
        check_and_restore(img, tree, td, name)
        with open(os.path.join(outdir, name + ".manifest"), "w", encoding="utf-8") as f: f.write("\n".join(man) + "\n")
        print("%s: %d manifest lines, btrfs check clean + restore identical to source tree" % (name, len(man)))
    return None

def write_text(path, data, mt):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f: f.write(data)
    os.utime(path, (mt, mt))

def make_subvols(outdir):
    """サブボリューム: トップレベルに rw / ro / ネストしたサブボリューム、既定サブボリュームを別に指定したイメージ。"""
    T = 1700000100
    for variant, args in (("subvols", ["--subvol", "rw:svA", "--subvol", "ro:svB", "--subvol", "rw:svA/inner"]),
                          ("subvols_default", ["--subvol", "default:svdef", "--subvol", "rw:svA"])):
        with tempfile.TemporaryDirectory(dir=outdir) as td:
            tree = os.path.join(td, "tree"); os.makedirs(tree)
            write_text(tree + "/top.txt", b"top level file\n", T)
            write_text(tree + "/svA/a.txt", b"in svA\n" * 100, T + 1)
            write_text(tree + "/svB/b.txt", b"in svB (read-only)\n", T + 2)
            write_text(tree + "/svdef/d.txt", b"in the default subvolume\n", T + 3)
            write_text(tree + "/svdef/sub/e.bin", bytes(range(256)) * 50, T + 4)
            if variant == "subvols":
                write_text(tree + "/svA/inner/i.txt", b"nested subvolume\n", T + 5)
            img = os.path.join(outdir, variant + ".img")
            if os.path.exists(img): os.remove(img)
            r = subprocess.run(["mkfs.btrfs", "-q", "-f", "-K", "--shrink", "-b", "200M"] + args + ["-r", tree, img], capture_output=True, text=True)
            if r.returncode != 0:
                print(variant + ": mkfs.btrfs failed (skipped):", (r.stderr or r.stdout).strip()[:160]); continue
            chk = subprocess.run(["btrfs", "check", "--readonly", img], capture_output=True, text=True)
            if chk.returncode != 0: sys.exit("btrfs check NOT clean for %s:\n%s" % (variant, chk.stdout[-1000:]))
            lst = run(["btrfs", "restore", "-l", img]).stdout
            print("%s: btrfs check clean; roots:\n%s" % (variant, "\n".join("   " + l for l in lst.splitlines()[:12])))
            # manifest はテスト側が (ソースツリーと同じ規則で) 持つので、ここでは内容の参照ファイルだけを残す
            shutil.copytree(tree, os.path.join(outdir, variant + ".src"), dirs_exist_ok=True, symlinks=True)

def make_nodatasum(outdir):
    """NODATASUM / NODATACOW のファイル (csum なし) を含むイメージ。"""
    T = 1700000200
    with tempfile.TemporaryDirectory(dir=outdir) as td:
        tree = os.path.join(td, "tree"); os.makedirs(tree)
        data = bytes((i * 7 + 3) & 0xFF for i in range(300000))
        write_text(tree + "/nodatasum.bin", data, T)
        write_text(tree + "/nodatacow.bin", data[::-1], T + 1)
        write_text(tree + "/normal.bin", data[1:], T + 2)
        img = os.path.join(outdir, "nodatasum.img")
        if os.path.exists(img): os.remove(img)
        r = subprocess.run(["mkfs.btrfs", "-q", "-f", "-K", "--shrink", "-b", "200M", "--inode-flags", "nodatasum:nodatasum.bin", "--inode-flags", "nodatacow:nodatacow.bin", "-r", tree, img], capture_output=True, text=True)
        if r.returncode != 0:
            print("nodatasum: mkfs.btrfs failed (skipped):", (r.stderr or r.stdout).strip()[:160]); return
        chk = subprocess.run(["btrfs", "check", "--readonly", img], capture_output=True, text=True)
        if chk.returncode != 0: sys.exit("btrfs check NOT clean for nodatasum:\n" + chk.stdout[-1000:])
        shutil.copytree(tree, os.path.join(outdir, "nodatasum.src"), dirs_exist_ok=True, symlinks=True)
        print("nodatasum: btrfs check clean")

def make_multi(outdir):
    """複数デバイスの RAID1 (テストは 1 台目だけでマウントを試し、EOPNOTSUPP で拒否されることを見る)。"""
    a, b = os.path.join(outdir, "multi_a.img"), os.path.join(outdir, "multi_b.img")
    for p in (a, b):
        if os.path.exists(p): os.remove(p)
        with open(p, "wb") as f: f.truncate(200 << 20)
    r = subprocess.run(["mkfs.btrfs", "-q", "-f", "-K", "-d", "raid1", "-m", "raid1", a, b], capture_output=True, text=True)
    if r.returncode != 0:
        print("multi: mkfs.btrfs failed (skipped):", (r.stderr or r.stdout).strip()[:160])
        for p in (a, b): os.remove(p)
        return
    os.remove(b)
    print("multi_a: created (2 devices, raid1)")

if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "/dev/shm/usbmem_btrfs"
    only = sys.argv[2:] if len(sys.argv) > 2 else None
    os.makedirs(out, exist_ok=True)
    skipped = []
    for name, args, compressed in VARIANTS:
        if only and name not in only: continue
        size = "64M" if name == "mixed" else "320M"
        err = make_small(out, name, args) if name.startswith("sector") else make(out, name, args, compressed, size)
        if err:
            print("%s: SKIPPED (%s)" % (name, err)); skipped.append(name)
    if not only:
        make_subvols(out); make_nodatasum(out); make_multi(out)
    if skipped: print("skipped variants:", ", ".join(skipped))
