#!/usr/bin/env python3
"""squashfs のテスト用イメージを作る (core/compat/tests/test_squashfs.cpp 用)。
使い方: tools/gen_squashfs_fixture.py <outdir>   (PATH に mksquashfs / unsquashfs が必要)
ツリーは tools/gen_ext2_fixture.py の build() と同じ (疎ファイル・大ディレクトリ・日本語名・255 バイト名・ハードリンク・シンボリックリンク) に、squashfs 固有の
ブロック境界のファイル、特殊ファイル (FIFO / ソケット / キャラクタ / ブロックデバイス: -p の擬似ファイル定義、ルート権限不要)、xattr を足したもの。
mksquashfs は圧縮方式 (gzip/zstd/xz/lz4/lzo)・ブロックサイズ (4 KiB〜1 MiB)・フラグメント有無・tailends・無圧縮・エクスポート無しの組み合わせで呼ぶ。
出力: <outdir>/<name>.img と <name>.manifest (gen_ext2_fixture.py と同じ書式 + 次の行):
  S <path> <c|b|p|s>                     特殊ファイル (キャラクタ / ブロック / FIFO / ソケット)
  R <path> <size> <mtime> <reffile>      期待する内容が <outdir>/<reffile> にあるファイル (乱数に近いデータ / 繰り返しデータ)
独立した検証: 作った各イメージを unsquashfs で展開し直して、元のツリーとバイト一致を確認する (ドライバの出力ではなく、参照実装との比較)。
圧縮方式 lzma (旧 LZMA1) は fs/squashfs が非対応なので、拒否テスト用に lzma_refused.img だけを作る (manifest なし)。"""
import hashlib, importlib.util, os, shutil, subprocess, sys, tempfile

os.environ["PATH"] = os.environ.get("PATH", "") + ":/sbin:/usr/sbin"
HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("gen_ext2_fixture", os.path.join(HERE, "gen_ext2_fixture.py"))
ext2 = importlib.util.module_from_spec(spec); spec.loader.exec_module(ext2)

def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw)

def tree_digest(root):
    """通常ファイル・ディレクトリ・シンボリックリンクの (相対パス, 種別, 内容のハッシュ / リンク先) を並べて返す。"""
    out = []
    for dp, dn, fn in os.walk(root):
        dn.sort()
        for n in sorted(fn) + sorted(d for d in dn if os.path.islink(os.path.join(dp, d))):
            p = os.path.join(dp, n); rel = os.path.relpath(p, root)
            if os.path.islink(p): out.append((rel, "l", os.readlink(p)))
            elif os.path.isfile(p):
                h = hashlib.sha256()
                with open(p, "rb") as f:
                    for ch in iter(lambda: f.read(1 << 20), b""): h.update(ch)
                out.append((rel, "f", h.hexdigest()))
        for d in dn:
            if not os.path.islink(os.path.join(dp, d)): out.append((os.path.relpath(os.path.join(dp, d), root), "d", ""))
    return sorted(out)

VARIANTS = [
    # name,        mksquashfs の追加引数,                              ブロックサイズ (境界ファイル用)
    ("gzip_4k",    ["-comp", "gzip", "-b", "4K"],                      4096),
    ("gzip_128k",  ["-comp", "gzip", "-b", "128K"],                    131072),
    ("gzip_1m",    ["-comp", "gzip", "-b", "1M"],                      1 << 20),
    ("zstd_128k",  ["-comp", "zstd", "-b", "128K"],                    131072),
    ("zstd_8k",    ["-comp", "zstd", "-b", "8K", "-Xcompression-level", "19"], 8192),
    ("xz_128k",    ["-comp", "xz", "-b", "128K"],                      131072),
    ("xz_bcj",     ["-comp", "xz", "-b", "64K", "-Xbcj", "x86,arm64"], 65536),
    ("lz4_128k",   ["-comp", "lz4", "-b", "128K"],                     131072),
    ("lz4_hc",     ["-comp", "lz4", "-b", "32K", "-Xhc"],              32768),
    ("lzo_128k",   ["-comp", "lzo", "-b", "128K"],                     131072),
    ("nofrag",     ["-comp", "gzip", "-b", "64K", "-no-fragments"],    65536),
    ("tailends",   ["-comp", "gzip", "-b", "32K", "-tailends"],        32768),
    ("nocomp",     ["-no-compression", "-b", "64K"],                   65536),
    ("noexport",   ["-comp", "gzip", "-b", "128K", "-no-exports"],     131072),
    ("nosparse",   ["-comp", "gzip", "-b", "128K", "-no-sparse"],      131072),
]

REFDIR = [None]; REFNAME = [None]

def extra_for(sqb):
    def extra(ctx):
        put, dense, mkdir, s, stamp, man, tree = ctx["put"], ctx["dense"], ctx["mkdir"], ctx["s"], ctx["stamp"], ctx["man"], ctx["tree"]
        # squashfs のデータブロック/フラグメントの境界
        for k, sz in (("b_minus1", sqb - 1), ("b_exact", sqb), ("b_plus1", sqb + 1), ("b2_minus1", 2 * sqb - 1), ("b2_plus7", 2 * sqb + 7), ("b3_exact", 3 * sqb)):
            dense("sqb/" + k + ".bin", sz, s())
        # 圧縮しにくいデータ (乱数に近い) と、圧縮しやすいデータ (繰り返し)。ブロックが圧縮されない/されるの両方が出るように
        import random
        rnd = random.Random(12345 + sqb)
        p = os.path.join(tree, "sqb", "incompressible.bin"); data = bytes(rnd.getrandbits(8) for _ in range(min(3 * sqb + 100, 700000)))
        open(p, "wb").write(data); mt = stamp(p)
        # manifest の 'd' は pattern(seed) 前提なので、この 2 ファイルだけ別の種別 R (期待する内容を別ファイル <name>.ref_<n> に置く) で記録する
        open(os.path.join(REFDIR[0], REFNAME[0] + ".ref_1"), "wb").write(data)
        man.append("R\tsqb/incompressible.bin\t%d\t%d\t%s" % (len(data), mt, REFNAME[0] + ".ref_1"))
        p = os.path.join(tree, "sqb", "repeat.bin"); data = (b"squashfs-" * 40000)[:300001]
        open(p, "wb").write(data); mt = stamp(p)
        open(os.path.join(REFDIR[0], REFNAME[0] + ".ref_2"), "wb").write(data)
        man.append("R\tsqb/repeat.bin\t%d\t%d\t%s" % (len(data), mt, REFNAME[0] + ".ref_2"))
        man.append("D\tsqb\t%d" % stamp(os.path.join(tree, "sqb")))   # 中のファイルを全部作った後に stamp (ディレクトリの mtime が変わるため)
        # 特殊ファイル (FIFO はここで作る。ソケット / デバイスは -p の擬似ファイル定義)
        os.mkfifo(os.path.join(tree, "special_fifo"))
        man.append("S\tspecial_fifo\tp")
        # 空のディレクトリを多数 / 深い階層 / 同名で大文字小文字違い
        for i in range(20): mkdir("emptydirs/e%02d" % i)
        for i in range(20): man.append("D\temptydirs/e%02d\t%d" % (i, stamp(os.path.join(tree, "emptydirs/e%02d" % i))))
        man.append("D\temptydirs\t%d" % stamp(os.path.join(tree, "emptydirs")))
    return extra

def make(outdir, name, args, sqb):
    REFDIR[0] = outdir; REFNAME[0] = name
    with tempfile.TemporaryDirectory(dir=outdir) as td:
        tree = os.path.join(td, "tree"); os.makedirs(tree)
        man = ext2.build(tree, 1024, extra=extra_for(sqb))
        # build() の hardlink 行補正は ext2 向け。squashfs は nlink を信用せず、テストは同一 inode のみ検査する
        pseudo = os.path.join(td, "pseudo.txt")
        open(pseudo, "w").write("special_sock i 644 0 0 s\nspecial_chr c 644 0 0 1 3\nspecial_blk b 644 0 0 8 1\n"
                                "small.txt x user.test=hello\n")
        man += ["S\tspecial_sock\ts", "S\tspecial_chr\tc", "S\tspecial_blk\tb"]
        img = os.path.join(outdir, name + ".img")
        if os.path.exists(img): os.remove(img)
        run(["mksquashfs", tree, img, "-noappend", "-quiet", "-no-progress", "-processors", "2", "-mkfs-time", "1700000000",
             "-all-root", "-pf", pseudo] + args)
        # 独立した検証: unsquashfs で展開し直して元のツリーとバイト一致 (デバイスノード等は作れないので -no-xattrs で無視、エラーは通常ファイル分のみ見る)
        ex = os.path.join(td, "ex")
        r = subprocess.run(["unsquashfs", "-d", ex, "-no-xattrs", "-ignore-errors", "-quiet", img], capture_output=True, text=True)
        if r.returncode not in (0, 2):
            sys.exit("unsquashfs failed for %s: %s%s" % (name, r.stdout, r.stderr))
        a, b = tree_digest(tree), tree_digest(ex)
        if a != b:
            sys.exit("unsquashfs extraction differs from source tree for %s: %d vs %d entries" % (name, len(a), len(b)))
        with open(os.path.join(outdir, name + ".manifest"), "w", encoding="utf-8") as f: f.write("\n".join(man) + "\n")
        info = run(["unsquashfs", "-s", img]).stdout.splitlines()
        keep = [l for l in info if l.startswith(("Compression", "Block size", "Filesystem is", "Fragments are", "Xattrs are", "Duplicates", "Number of fragments", "Number of inodes", "Number of ids", "Filesystem size", "Exportable", "Inodes are", "Data is", "Fragments are", "Compressed data"))]
        open(os.path.join(outdir, name + ".info"), "w").write("\n".join(keep) + "\n")
        print("%s: %d manifest lines, unsquashfs verified; %s" % (name, len(man), "; ".join(keep[:3])))

if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "/dev/shm/usbmem_squashfs"
    os.makedirs(out, exist_ok=True)
    for name, args, sqb in VARIANTS:
        make(out, name, args, sqb)
    # 拒否テスト用: 旧 LZMA1 (fs/squashfs は非対応)。小さなツリーでよい
    with tempfile.TemporaryDirectory(dir=out) as td:
        open(os.path.join(td, "a.txt"), "w").write("hello\n" * 100)
        try:
            run(["mksquashfs", td, os.path.join(out, "lzma_refused.img"), "-noappend", "-quiet", "-no-progress", "-comp", "lzma"])
            print("lzma_refused: created")
        except subprocess.CalledProcessError as e:
            print("lzma_refused: mksquashfs cannot create lzma (skipped):", (e.stderr or "")[:80])
