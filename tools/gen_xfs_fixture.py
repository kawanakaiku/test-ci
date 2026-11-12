#!/usr/bin/env python3
"""XFS のテスト用イメージを作る (core/compat/tests/test_xfs.cpp 用)。
使い方: tools/gen_xfs_fixture.py <outdir> [variant...]   (PATH に mkfs.xfs / xfs_repair / xfs_db が必要。xfsprogs 6.x 以降: mkfs.xfs -p <dir> でルート権限なしにツリーを書き込める)
ツリーは tools/gen_ext2_fixture.py の build() と同じ (疎ファイル・1000 ファイルのディレクトリ・日本語名・255 バイト名・ハードリンク・シンボリックリンク) に、
XFS 固有のもの (断片化した大きなファイル = extent が bmbt (BTREE 形式) になる、4 GiB を超える疎ファイル、巨大ディレクトリ = node 形式、長い/短いシンボリックリンク、FIFO) を足したもの。
mkfs.xfs の組み合わせ: v5 (CRC あり) / v4 (CRC なし、ftype あり/なし) × ブロックサイズ (512〜64 KiB) × ディレクトリブロック (1〜16 ブロック) × inode サイズ × AG 数 × 機能 (sparse / nrext64 / bigtime / reflink / rmapbt / finobt / metadir / ascii-ci)。
出力: <outdir>/<name>.img と <name>.manifest (gen_ext2_fixture.py と同じ書式 + 次の行):
  S <path> <c|b|p|s>                     特殊ファイル (キャラクタ / ブロック / FIFO / ソケット)
  R <path> <size> <mtime> <reffile>      期待する内容が <outdir>/<reffile> にあるファイル
独立した検証: 作った各イメージを xfs_repair -n で検査 (クリーンであること)。ツリーは mkfs.xfs -p で書き込んだ元のディレクトリが正解 (mtime / nlink / 内容もここから)。
xfs_db でディレクトリ / ファイルの inode 形式 (local / extents / btree)、ディレクトリ形式 (block / leaf / node) を確認して、期待どおりの構造が実際に作られたことを記録する (xfs_formats.txt)。
ソケットは Python で bind して作る。キャラクタ/ブロックデバイスはルート権限なしでは作れないので、protofile 経由の小さなイメージ (special_proto) だけに入れる。"""
import importlib.util, os, re, shutil, socket, subprocess, sys, tempfile

os.environ["PATH"] = os.environ.get("PATH", "") + ":/sbin:/usr/sbin"
HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("gen_squashfs_fixture", os.path.join(HERE, "gen_squashfs_fixture.py"))
sq = importlib.util.module_from_spec(spec); spec.loader.exec_module(sq)
ext2 = sq.ext2
BUILD_TIME = 1700000000
IMG_SIZE = 300 * 1024 * 1024    # mkfs.xfs (v5) の最小は 300 MiB 前後。疎ファイルなので実使用量は fallocate -d で小さくする

def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw)

# name, mkfs.xfs の引数, ブロック境界ファイル用のブロックサイズ, オプション (frag=断片化ファイルの島の数, big=巨大ディレクトリのファイル数, ts=ツリーの追加)
VARIANTS = [
    ("v5_default",     [],                                                                   4096,  dict(frag=3000, big=3000)),
    ("v5_bs1k",        ["-b", "size=1024", "-n", "size=4096"],                               1024,  dict(frag=3000, big=2500)),
    ("v5_bs2k",        ["-b", "size=2048"],                                                  2048,  dict(frag=2000, big=1500)),
    ("v5_bs512",       ["-b", "size=512", "-s", "size=512", "-n", "size=2048"],              512,   dict(frag=30000, big=800)),
    ("v5_bs64k",       ["-b", "size=65536", "-s", "size=4096", "-i", "size=512"],            65536, dict(frag=700, big=2000)),
    ("v5_dir8k",       ["-n", "size=8192"],                                                  4096,  dict(frag=1500, big=3000)),
    ("v5_dir64k",      ["-n", "size=65536"],                                                 4096,  dict(frag=500, big=4000)),
    ("v5_isize1k",     ["-i", "size=1024"],                                                  4096,  dict(frag=3000, big=1500)),
    ("v5_isize2k",     ["-b", "size=4096", "-i", "size=2048"],                               4096,  dict(frag=3000, big=1500)),
    ("v5_old_features",["-i", "sparse=0,nrext64=0", "-m", "bigtime=0,rmapbt=0,reflink=0,finobt=0,inobtcount=0"], 4096, dict(frag=3000, big=1500)),
    ("v5_nrext64",     ["-i", "nrext64=1", "-m", "bigtime=1"],                               4096,  dict(frag=3000, big=1500)),
    ("v5_agcount1",    ["-d", "agcount=1"],                                                  4096,  dict(frag=1500, big=1500)),
    ("v5_agcount16",   ["-d", "agcount=16"],                                                 4096,  dict(frag=1500, big=1500)),
    ("v5_metadir",     ["-m", "metadir=1"],                                                  4096,  dict(frag=1500, big=1500)),
    ("v5_ci",          ["-n", "version=ci"],                                                 4096,  dict(frag=500, big=600)),
    ("v5_noftype",     ["-m", "crc=1", "-n", "ftype=1"],                                     4096,  dict(frag=1000, big=1000)),
    ("v4_ftype1",      ["-m", "crc=0", "-n", "ftype=1"],                                     4096,  dict(frag=3000, big=3000)),
    ("v4_ftype0",      ["-m", "crc=0", "-n", "ftype=0"],                                     4096,  dict(frag=3000, big=3000)),
    ("v4_bs1k_dir4k",  ["-m", "crc=0", "-n", "ftype=0,size=4096", "-b", "size=1024"],        1024,  dict(frag=3000, big=2500)),
    ("v4_bs512",       ["-m", "crc=0", "-n", "ftype=1", "-b", "size=512", "-s", "size=512"], 512,   dict(frag=30000, big=800)),
]

def xdb(img, *cmds):
    args = ["xfs_db", "-r"]
    for c in cmds: args += ["-c", c]
    return subprocess.run(args + [img], capture_output=True, text=True).stdout

def ino_of(img, path):
    """xfs_db の ls で path の inode 番号を得る (親を順にたどる)。"""
    cur = "/"
    ino = None
    for part in [p for p in path.split("/") if p]:
        out = xdb(img, "ls " + cur)
        ino = None
        for line in out.splitlines():
            m = re.match(r"^\s*\d+\s+(\d+)\s+\S+\s+0x[0-9a-f]+\s+\d+\s+(.*?)(?:\s+\(good\))?\s*$", line)
            if m and m.group(2) == part: ino = int(m.group(1)); break
        if ino is None: return None
        cur = cur.rstrip("/") + "/" + part
    return ino

def inode_info(img, path):
    ino = ino_of(img, path)
    if ino is None: return None
    out = xdb(img, "inode %d" % ino, "p core.format core.size core.nextents")
    d = {}
    for line in out.splitlines():
        m = re.match(r"^\s*core\.(\w+)\s*=\s*(\S+)", line)
        if m: d[m.group(1)] = m.group(2)
    return d

def dir_kind(img, path, bs):
    """ディレクトリの構造: sf (local) / block / leaf / node。bmap の extent を、データ空間 (< 32 GiB)・leaf 空間 (32〜64 GiB)・free 空間 (>= 64 GiB) に分けて判定する。"""
    ino = ino_of(img, path)
    if ino is None: return "?"
    info = inode_info(img, path) or {}
    if info.get("format") == "1": return "sf"   # XFS_DINODE_FMT_LOCAL
    out = xdb(img, "inode %d" % ino, "bmap")
    leaf_off = (1 << 35) // bs
    free_off = (2 << 35) // bs
    leaf_blocks = 0
    for line in out.splitlines():
        m = re.search(r"offset (\d+) startblock \d+ \(.*\) count (\d+)", line)   # "data offset N startblock S (agno/agbno) count C flag F"
        if m and leaf_off <= int(m.group(1)) < free_off: leaf_blocks += int(m.group(2))
    sb = xdb(img, "sb 0", "p dirblklog")
    m = re.search(r"dirblklog = (\d+)", sb)
    dirblocks = 1 << int(m.group(1)) if m else 1
    if leaf_blocks == 0: return "block"
    return "leaf" if leaf_blocks <= dirblocks else "node"

def make(outdir, name, args, bs, opt):
    sq.REFDIR[0] = outdir; sq.REFNAME[0] = name
    with tempfile.TemporaryDirectory(dir=outdir) as td:
        tree = os.path.join(td, "tree"); os.makedirs(tree)
        base_extra = sq.extra_for(max(bs, 512))
        def extra(ctx):
            base_extra(ctx)
            put, dense, mkdir, s, stamp, man = ctx["put"], ctx["dense"], ctx["mkdir"], ctx["s"], ctx["stamp"], ctx["man"]
            # 短い/長いシンボリックリンク (長いものは inode に入らず、リモートのシンボリックリンクブロックになる)
            for rel, ln in (("lnk_len60", 60), ("lnk_len300", 300), ("lnk_len1000", 1000), ("lnk_len1023", 1023)):
                tgt = ("t" + rel + "/") * 200
                tgt = tgt[:ln]
                os.symlink(tgt, os.path.join(tree, rel))
                man.append("L\t%s\t%s" % (rel, tgt))
            # 断片化したファイル: 1 ブロックのデータと 1 ブロックの穴を交互に並べる → extent が増えて BTREE 形式の bmbt になる
            n = opt["frag"]
            items = [("d", 2 * i * bs, bs, 7 + i % 50) for i in range(n)]
            put("frag/islands.bin", 2 * n * bs, items)
            # 島の途中にも半端な長さのデータを入れた断片化ファイル
            items = [("d", i * 3 * bs + (i % 5) * 7, 100 + (i % 11) * 13, 3 + i % 31) for i in range(min(n, 400))]
            put("frag/partial.bin", 3 * min(n, 400) * bs + bs, items)
            man.append("D\tfrag\t%d" % stamp(os.path.join(tree, "frag")))
            # 4 GiB を超える疎ファイル (先頭・4 GiB 付近・末尾にだけデータ)
            big_sz = 5 * (1 << 30) + 123
            put("huge_sparse.bin", big_sz, [("d", 0, 100, s()), ("d", (4 << 30) + 4 * bs - 5, 5000, s()), ("d", big_sz - 123, 123, s())])
            # 巨大ディレクトリ (node 形式になるまで) と、少数のファイルだけのディレクトリ (shortform / block)
            nb = opt["big"]
            for i in range(nb): dense("bigdir/e%06d" % i, 1, s())
            man.append("D\tbigdir\t%d" % stamp(os.path.join(tree, "bigdir")))
            for k, cnt in (("dir_sf", 3), ("dir_block", 40), ("dir_leaf", 300)):
                for i in range(cnt): dense("%s/entry_%03d_%s" % (k, i, "x" * (i % 20)), 1, s())
                man.append("D\t%s\t%d" % (k, stamp(os.path.join(tree, k))))
            # 空でないディレクトリの中の空ディレクトリ、同じ名前の別階層
            mkdir("nest/a/b/c"); man.append("D\tnest/a/b/c\t%d" % stamp(os.path.join(tree, "nest/a/b/c")))
            man.append("D\tnest/a/b\t%d" % stamp(os.path.join(tree, "nest/a/b")))
            man.append("D\tnest/a\t%d" % stamp(os.path.join(tree, "nest/a")))
            man.append("D\tnest\t%d" % stamp(os.path.join(tree, "nest")))
        man = ext2.build(tree, 1024, extra=extra)
        # ソケット (パスが長すぎると bind できないので、ツリー直下に置く)
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        cwd = os.getcwd(); os.chdir(tree)
        try: s.bind("special_sock")
        finally: os.chdir(cwd); s.close()
        man.append("S\tspecial_sock\ts")
        img = os.path.join(outdir, name + ".img")
        if os.path.exists(img): os.remove(img)
        with open(img, "wb") as f: f.truncate(IMG_SIZE)
        cmd = ["mkfs.xfs", "-f", "-q"] + args + ["-p", tree, img]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            return "mkfs.xfs failed: " + (r.stderr or r.stdout).strip()[:300]
        subprocess.run(["fallocate", "-d", img], check=False)   # ゼロのブロックを穴に戻す (tmpfs の使用量を減らす)
        chk = subprocess.run(["xfs_repair", "-n", img], capture_output=True, text=True)
        if chk.returncode != 0:
            sys.exit("xfs_repair -n NOT clean for %s:\n%s%s" % (name, chk.stdout[-800:], chk.stderr[-800:]))
        # ミューテーション確認用: ログはクリーン。構造の記録
        info = []
        for path, label in (("/", "root"), ("/dir_sf", "dir_sf"), ("/dir_block", "dir_block"), ("/dir_leaf", "dir_leaf"), ("/bigdir", "bigdir"), ("/many", "many"),
                            ("/frag/islands.bin", "frag"), ("/link_slow", "link_slow"), ("/lnk_len300", "lnk_len300"), ("/lnk_len1000", "lnk_len1000"), ("/huge_sparse.bin", "huge_sparse")):
            ii = inode_info(img, path)
            info.append("%s: %s" % (label, " ".join("%s=%s" % kv for kv in sorted((ii or {}).items()))))
            if label in ("dir_block", "dir_leaf", "bigdir", "many"):
                info[-1] += " kind=" + dir_kind(img, path, bs)
        open(os.path.join(outdir, name + ".formats"), "w", encoding="utf-8").write("\n".join(info) + "\n")
        with open(os.path.join(outdir, name + ".manifest"), "w", encoding="utf-8") as f: f.write("\n".join(man) + "\n")
        print("%s: %d manifest lines, xfs_repair -n clean" % (name, len(man)))
        for l in info: print("    " + l)
    return None

def make_special(outdir):
    """キャラクタ/ブロックデバイス/FIFO/シンボリックリンクを含む小さなイメージ (protofile 経由)。"""
    name = "special_proto"
    with tempfile.TemporaryDirectory(dir=outdir) as td:
        src = os.path.join(td, "hello.txt"); open(src, "wb").write(b"hello xfs\n")
        os.utime(src, (BUILD_TIME + 5, BUILD_TIME + 5))
        proto = os.path.join(td, "proto")
        open(proto, "w").write("/ignored\n0 0\nd--755 0 0\n chr c--644 0 0 1 3\n blk b--600 0 0 8 1\n fifo p--644 0 0\n hello.txt ---644 0 0 %s\n sub d--755 0 0\n  lnk l--777 0 0 ../hello.txt\n $\n$\n" % src)
        img = os.path.join(outdir, name + ".img")
        with open(img, "wb") as f: f.truncate(IMG_SIZE)
        r = subprocess.run(["mkfs.xfs", "-f", "-q", "-p", proto, img], capture_output=True, text=True)
        if r.returncode != 0:
            print("special_proto: mkfs.xfs protofile failed (skipped):", (r.stderr or r.stdout).strip()[:160]); return
        subprocess.run(["fallocate", "-d", img], check=False)
        chk = subprocess.run(["xfs_repair", "-n", img], capture_output=True, text=True)
        if chk.returncode != 0: sys.exit("xfs_repair -n NOT clean for special_proto:\n%s%s" % (chk.stdout[-600:], chk.stderr[-600:]))
        open(os.path.join(outdir, name + ".ref_1"), "wb").write(b"hello xfs\n")
        man = ["R\thello.txt\t10\t%d\t%s" % (BUILD_TIME + 5, name + ".ref_1"), "S\tchr\tc", "S\tblk\tb", "S\tfifo\tp", "L\tsub/lnk\t../hello.txt"]
        open(os.path.join(outdir, name + ".manifest"), "w", encoding="utf-8").write("\n".join(man) + "\n")
        print("special_proto: xfs_repair -n clean (char/block/fifo/symlink via protofile)")

if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "/dev/shm/usbmem_xfs"
    only = sys.argv[2:] if len(sys.argv) > 2 else None
    os.makedirs(out, exist_ok=True)
    skipped = []
    for name, args, bs, opt in VARIANTS:
        if only and name not in only: continue
        err = make(out, name, args, bs, opt)
        if err:
            print("%s: SKIPPED (%s)" % (name, err)); skipped.append(name)
    if not only or "special_proto" in only: make_special(out)
    if skipped: print("skipped variants:", ", ".join(skipped))
