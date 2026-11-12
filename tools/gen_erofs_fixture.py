#!/usr/bin/env python3
"""erofs のテスト用イメージを作る (core/compat/tests/test_erofs.cpp 用)。
使い方: tools/gen_erofs_fixture.py <outdir>   (PATH に mkfs.erofs / fsck.erofs が必要)
ツリーは tools/gen_squashfs_fixture.py (= gen_ext2_fixture.py の build() + ブロック境界のファイル + 乱数/繰り返しデータ + FIFO) と同じ。
mkfs.erofs は データの配置 (plain / inline tail / チャンク (blockmap・index)) × 圧縮方式 (無圧縮 / lz4 / lz4hc / lzma / deflate / zstd) × inode 形式 (compact / extended) × ブロックサイズ の組み合わせで呼ぶ。
出力: <outdir>/<name>.img と <name>.manifest (gen_squashfs_fixture.py と同じ書式: D / F / L / S / R)。
独立した検証: 作った各イメージを fsck.erofs で検査 (整合性) し、--extract で展開し直した結果が元のツリーとバイト一致することを確認する (ドライバの出力ではなく、参照実装との比較)。
ソケットは Python で bind して作る。キャラクタ/ブロックデバイスはルート権限なしでは作れないので、tar 経由の小さなイメージ (special_tar) だけに入れる。"""
import hashlib, importlib.util, os, shutil, socket, subprocess, sys, tarfile, tempfile

os.environ["PATH"] = os.environ.get("PATH", "") + ":/sbin:/usr/sbin"
HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("gen_squashfs_fixture", os.path.join(HERE, "gen_squashfs_fixture.py"))
sq = importlib.util.module_from_spec(spec); spec.loader.exec_module(sq)
ext2 = sq.ext2
BUILD_TIME = 1700000000

def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw)

# name, mkfs.erofs の追加引数, ブロック境界ファイル用のサイズ, 圧縮か
VARIANTS = [
    ("plain_4k",        ["-b4096"],                                                        4096,   False),
    ("plain_1k",        ["-b1024"],                                                        1024,   False),
    ("plain_2k",        ["-b2048"],                                                        2048,   False),
    ("plain_noinline",  ["-b4096", "-Enoinline_data"],                                     4096,   False),
    ("plain_compact",   ["-b4096", "-Eforce-inode-compact"],                               4096,   False),
    ("plain_extended",  ["-b4096", "-Eforce-inode-extended"],                              4096,   False),
    ("chunk_blockmap",  ["-b4096", "--chunksize=16384", "-Eforce-inode-blockmap"],         16384,  False),
    ("chunk_index",     ["-b4096", "--chunksize=8192"],                                    8192,   False),
    ("chunk_index_4k",  ["-b4096", "--chunksize=4096"],                                    4096,   False),
    ("lz4_4k",          ["-b4096", "-zlz4"],                                               4096,   True),
    ("lz4hc_4k",        ["-b4096", "-zlz4hc,level=12"],                                    4096,   True),
    ("lz4_big_pcluster",["-b4096", "-zlz4", "-C65536"],                                    65536,  True),
    ("lz4_1k",          ["-b1024", "-zlz4"],                                               1024,   True),
    ("lz4_tailpacking", ["-b4096", "-zlz4", "-Eztailpacking"],                             4096,   True),
    ("lz4_fragments",   ["-b4096", "-zlz4", "-Efragments"],                                4096,   True),
    ("lz4_dedupe",      ["-b4096", "-zlz4", "-Ededupe"],                                   4096,   True),
    ("lz4_legacy",      ["-b4096", "-zlz4", "-Elegacy-compress"],                          4096,   True),
    ("lzma_4k",         ["-b4096", "-zlzma"],                                              4096,   True),
    ("deflate_4k",      ["-b4096", "-zdeflate"],                                           4096,   True),
    ("zstd_4k",         ["-b4096", "-zzstd"],                                              4096,   True),
]

def make_tree_manifest(tree, sqb):
    man = ext2.build(tree, 1024, extra=sq.extra_for(sqb))
    # ソケット (Python で bind して作る。パスが長すぎると bind できないので、ツリー直下に置く)
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    cwd = os.getcwd()
    os.chdir(tree)
    try: s.bind("special_sock")
    finally: os.chdir(cwd); s.close()
    man.append("S\tspecial_sock\ts")
    return man

def make(outdir, name, args, sqb, compressed):
    sq.REFDIR[0] = outdir; sq.REFNAME[0] = name
    with tempfile.TemporaryDirectory(dir=outdir) as td:
        tree = os.path.join(td, "tree"); os.makedirs(tree)
        man = make_tree_manifest(tree, sqb)
        img = os.path.join(outdir, name + ".img")
        if os.path.exists(img): os.remove(img)
        # --preserve-mtime: ファイルごとの mtime を厳密に残す (extended inode になる)。-T: ビルド時刻
        cmd = ["mkfs.erofs", "--quiet", "--workers=2", "--preserve-mtime", "--mkfs-time", "-T", str(BUILD_TIME), "--all-root", "-UclearUUID"] + args + [img, tree]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            # -UclearUUID が無い版でも作れるようにやり直す
            cmd = [c for c in cmd if c != "-UclearUUID"]
            r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            return "mkfs.erofs failed: " + (r.stderr or r.stdout).strip()[:160]
        chk = subprocess.run(["fsck.erofs", img], capture_output=True, text=True)
        if chk.returncode != 0:
            sys.exit("fsck.erofs NOT clean for %s:\n%s%s" % (name, chk.stdout, chk.stderr))
        ex = os.path.join(td, "ex")
        r = subprocess.run(["fsck.erofs", "--extract=" + ex, "--no-preserve", img], capture_output=True, text=True)
        if r.returncode != 0:
            r = subprocess.run(["fsck.erofs", "--extract=" + ex, img], capture_output=True, text=True)
        a, b = sq.tree_digest(tree), sq.tree_digest(ex)
        if a != b:
            sys.exit("fsck.erofs extraction differs from source tree for %s: %d vs %d entries (rc %d: %s)" % (name, len(a), len(b), r.returncode, r.stderr[:200]))
        if "-Eforce-inode-compact" in args:
            # compact inode は mtime を持たない (スーパーブロックの時刻になる)。manifest の期待値もそれに合わせる
            def fix(line):
                t = line.split("\t")
                if t[0] == "D": t[2] = str(BUILD_TIME)
                elif t[0] in ("F", "R"): t[3] = str(BUILD_TIME)
                return "\t".join(t)
            man = [fix(l) for l in man]
        with open(os.path.join(outdir, name + ".manifest"), "w", encoding="utf-8") as f: f.write("\n".join(man) + "\n")
        print("%s: %d manifest lines, fsck.erofs clean + extraction identical to source tree" % (name, len(man)))
    return None

def make_special_tar(outdir):
    """キャラクタ/ブロックデバイス/FIFO/ソケット (tar のエントリとして) を含む小さなイメージ。"""
    name = "special_tar"
    with tempfile.TemporaryDirectory(dir=outdir) as td:
        tpath = os.path.join(td, "s.tar")
        man = []
        with tarfile.open(tpath, "w", format=tarfile.GNU_FORMAT) as t:
            def add(path, typ, data=b"", mode=0o644, devmajor=0, devminor=0, link=""):
                ti = tarfile.TarInfo(path); ti.type = typ; ti.mode = mode; ti.mtime = BUILD_TIME + 5; ti.uid = 0; ti.gid = 0
                ti.size = len(data); ti.devmajor = devmajor; ti.devminor = devminor; ti.linkname = link
                t.addfile(ti, io_data(data) if data else None)
            import io
            def io_data(b): return io.BytesIO(b)
            add("hello.txt", tarfile.REGTYPE, b"hello erofs\n")
            man.append("F\thello.txt\t12\t%d\t1\td:0:12:0" % (BUILD_TIME + 5))
            add("chr", tarfile.CHRTYPE, devmajor=1, devminor=3); man.append("S\tchr\tc")
            add("blk", tarfile.BLKTYPE, devmajor=8, devminor=1); man.append("S\tblk\tb")
            add("fifo", tarfile.FIFOTYPE); man.append("S\tfifo\tp")
            add("lnk", tarfile.SYMTYPE, link="hello.txt"); man.append("L\tlnk\thello.txt")
        # F の内容はパターンではないので、R (参照ファイル) 形式で書き直す
        open(os.path.join(outdir, name + ".ref_1"), "wb").write(b"hello erofs\n")
        man[0] = "R\thello.txt\t12\t%d\t%s" % (BUILD_TIME + 5, name + ".ref_1")
        img = os.path.join(outdir, name + ".img")
        if os.path.exists(img): os.remove(img)
        r = subprocess.run(["mkfs.erofs", "--quiet", "--tar=f", "--preserve-mtime", "--mkfs-time", "-T", str(BUILD_TIME), "-b4096", img, tpath], capture_output=True, text=True)
        if r.returncode != 0:
            print("special_tar: mkfs.erofs --tar failed (skipped):", (r.stderr or r.stdout).strip()[:120]); return
        chk = subprocess.run(["fsck.erofs", img], capture_output=True, text=True)
        if chk.returncode != 0: sys.exit("fsck.erofs NOT clean for special_tar:\n%s%s" % (chk.stdout, chk.stderr))
        open(os.path.join(outdir, name + ".manifest"), "w", encoding="utf-8").write("\n".join(man) + "\n")
        print("special_tar: fsck.erofs clean (char/block/fifo/symlink via tar)")

if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "/dev/shm/usbmem_erofs"
    only = sys.argv[2:] if len(sys.argv) > 2 else None
    os.makedirs(out, exist_ok=True)
    skipped = []
    for name, args, sqb, compressed in VARIANTS:
        if only and name not in only: continue
        err = make(out, name, args, sqb, compressed)
        if err:
            print("%s: SKIPPED (%s)" % (name, err)); skipped.append(name)
    if not only: make_special_tar(out)
    # 拒否テスト用: 必ずマウントを拒否するはずの小さなイメージ (非対応機能のビットを立てる) はテスト側がバイト操作で作る
    if skipped: print("skipped variants:", ", ".join(skipped))
