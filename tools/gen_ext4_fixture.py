#!/usr/bin/env python3
"""ext4 のテスト用イメージを作る (core/compat/tests/test_ext4.cpp 用)。
使い方: tools/gen_ext4_fixture.py <outdir>   (PATH に mke2fs / e2fsck / dumpe2fs / debugfs が必要。/sbin は自動で足す)
`mke2fs -d <ツリー>` でルート権限なしにファイルを書き込む (Linux のドライバ・FUSE は使わない)。内容の規則・manifest の形式は gen_ext2_fixture.py と同じ。
ext4 に固有の内容 (htree の巨大ディレクトリ・深い extent 木・unwritten extent・inline_data) をそこへ足す。
機能の組み合わせごとにイメージを作り、e2fsck -fn で clean を確認する。.features に dumpe2fs の機能行を残す。
そのほか: needs_recovery を立てたイメージ (debugfs で feature 変更)、未対応機能 (encrypt / casefold / verity / 不明な incompat) のイメージ。"""
import os, sys, shutil, subprocess, tempfile, importlib.util

os.environ["PATH"] = os.environ.get("PATH", "") + ":/sbin:/usr/sbin"
_here = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("gen_ext2_fixture", os.path.join(_here, "gen_ext2_fixture.py"))
g2 = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(g2)

def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0: sys.exit("command failed (%d): %s\n%s%s" % (r.returncode, " ".join(cmd), r.stdout, r.stderr))
    return r

def extra_content(opts):
    def extra(c):
        put, dense, mkdir, s, stamp, man, tree, bs = (c[k] for k in ("put", "dense", "mkdir", "s", "stamp", "man", "tree", "bs"))
        # htree: 5000 エントリのディレクトリ (mke2fs -d は線形で作る → 後で e2fsck -fD で htree 化する)
        mkdir("bigdir")
        for i in range(5000):
            dense("bigdir/entry_%05d_%s.dat" % (i, "x" * (i % 40)), 1 + (i * 13) % 100, 3 + i % 200)
        man.append("D\tbigdir\t%d" % stamp(os.path.join(tree, "bigdir")))
        # 断片化した extent 木: 飛び飛びのブロック。inode 内 (4 個) を超えて深さ 1、さらに 2 になるまで
        n_frag = 1500 if bs >= 4096 else 3000
        put("frag_deep.bin", 2 * n_frag * bs, [("d", 2 * i * bs, bs, 9 + i % 50) for i in range(n_frag)])
        put("frag_shallow.bin", 20 * bs, [("d", 2 * i * bs, bs, 5 + i) for i in range(9)] + [("d", 19 * bs, 77, 99)])
        # inline_data の境界 (inode 256 バイトなら i_block 60 + 拡張属性領域 ≒ 160 バイトまで inode 内に入る) と小さなディレクトリ
        for sz in (1, 59, 60, 61, 100, 150, 160, 200, 400):
            dense("inl_%d.bin" % sz, sz, 40 + sz % 50)
        mkdir("smalldir")
        for nm in ("a", "bb", "ccc"):
            dense("smalldir/" + nm, 7, 31)
        man.append("D\tsmalldir\t%d" % stamp(os.path.join(tree, "smalldir")))
        # unwritten extent 用 (debugfs fallocate で後から 4..9 ブロックを unwritten で確保する。読み出しは 0)
        put("unwritten.bin", 10 * bs, [("d", 0, 4 * bs, 17)])
    return extra

def features(img):
    return [l for l in run(["dumpe2fs", "-h", img]).stdout.splitlines() if l.startswith(("Filesystem features", "Inode size", "Block size", "Inode count", "Block count", "Filesystem revision", "Cluster size", "Journal"))]

def make(outdir, name, mke2fs_args, bs, size_mb=96, htree=True, fallocate=True):
    with tempfile.TemporaryDirectory(dir=outdir) as td:
        tree = os.path.join(td, "tree"); os.makedirs(tree)
        man = g2.build(tree, bs, extra_content({}))
        img = os.path.join(outdir, name + ".img")
        if os.path.exists(img): os.remove(img)
        with open(img, "wb") as f: f.truncate(size_mb * 1024 * 1024)
        run(["mke2fs", "-q", "-L", "usbmem-" + name, "-U", "7d9c1f5e-0000-4000-8000-%012d" % (bs + len(name)),
             "-E", "root_owner=0:0,hash_seed=7d9c1f5e-1111-4111-8111-000000000001", "-b", str(bs)] + mke2fs_args + ["-d", tree, img])
        feats = features(img)
        has_extent = any("extent" in l for l in feats)
        if any("inline_data" in l for l in feats):
            # inline_data 有効時、mke2fs -d は末尾が穴のファイルの i_size を「最後に書いたデータの終わり」に切り詰める (mke2fs 側の挙動)。manifest のサイズに直す。
            fix = []
            for line in man:
                c = line.split("\t")
                if c[0] == "F" and int(c[2]) > 0 and c[5].split(",")[-1].startswith("z:"):
                    fix.append("set_inode_field /%s size %s" % (c[1], c[2]))
            cmdf = os.path.join(td, "fix.cmd"); open(cmdf, "w", encoding="utf-8").write("\n".join(fix) + "\n")
            run(["debugfs", "-w", "-f", cmdf, img])
        if htree and any("dir_index" in l for l in feats):
            r = subprocess.run(["e2fsck", "-fyD", img], capture_output=True, text=True)   # ディレクトリを最適化 (htree 化)。1 = 修正あり で正常
            if r.returncode not in (0, 1): sys.exit("e2fsck -D failed for %s:\n%s%s" % (name, r.stdout, r.stderr))
        if fallocate and has_extent:
            blocks = "4 9"
            out = subprocess.run(["debugfs", "-w", "-R", "fallocate /unwritten.bin 4 9", img], capture_output=True, text=True)
            ex = subprocess.run(["debugfs", "-R", "ex /unwritten.bin", img], capture_output=True, text=True).stdout
            if "Uninit" not in ex: print("  warn: %s: no uninit extent in unwritten.bin:\n%s%s%s" % (name, out.stdout, out.stderr, ex))
        r = subprocess.run(["e2fsck", "-fn", img], capture_output=True, text=True)
        if r.returncode != 0: sys.exit("e2fsck NOT clean for %s:\n%s%s" % (name, r.stdout, r.stderr))
        depth = ""
        if has_extent:
            ex = subprocess.run(["debugfs", "-R", "ex /frag_deep.bin", img], capture_output=True, text=True).stdout
            mx = max((int(l.split()[0].rstrip("/")) for l in ex.splitlines()[1:] if l.split() and l.split()[0].rstrip("/").isdigit()), default=0)
            depth = " frag_deep tree-depth=%d" % mx
        ht = ""
        if htree:
            ht = " htree=" + ("yes" if "Hash Version" in subprocess.run(["debugfs", "-R", "htree_dump /bigdir", img], capture_output=True, text=True).stdout else "no")
        open(os.path.join(outdir, name + ".features"), "w").write("\n".join(feats) + "\n" + depth + ht + "\n")
        with open(os.path.join(outdir, name + ".manifest"), "w", encoding="utf-8") as f: f.write("\n".join(man) + "\n")
        print("%s: %d manifest lines, e2fsck clean;%s%s; %s" % (name, len(man), depth, ht, feats[0] if feats else ""))

def make_reject(outdir, name, args):
    img = os.path.join(outdir, name + ".img")
    if os.path.exists(img): os.remove(img)
    with open(img, "wb") as f: f.truncate(16 * 1024 * 1024)
    r = subprocess.run(["mke2fs", "-q", "-t", "ext4", "-b", "4096"] + args + [img], capture_output=True, text=True)
    if r.returncode != 0:
        os.remove(img); print("%s: SKIP (mke2fs: %s)" % (name, r.stderr.strip().splitlines()[-1] if r.stderr.strip() else r.returncode)); return
    print("%s: %s" % (name, (features(img) or [""])[0]))

def main(out):
    os.makedirs(out, exist_ok=True)
    CSUM = "has_journal,extent,huge_file,flex_bg,metadata_csum,64bit,dir_nlink,extra_isize,resize_inode,dir_index,sparse_super,large_file,ext_attr"
    make(out, "e4_def4k", ["-t", "ext4", "-I", "256", "-O", CSUM], 4096)
    make(out, "e4_def1k", ["-t", "ext4", "-I", "256", "-O", CSUM], 1024)
    make(out, "e4_nocsum2k", ["-t", "ext4", "-I", "256", "-O", "^has_journal,^metadata_csum,^uninit_bg,^64bit,extent,flex_bg,huge_file,dir_nlink,extra_isize"], 2048)
    make(out, "e4_gdtcsum4k", ["-t", "ext4", "-I", "256", "-O", "^has_journal,^metadata_csum,uninit_bg,^64bit,extent,flex_bg"], 4096)
    make(out, "e4_inline1k", ["-t", "ext4", "-I", "256", "-O", "inline_data,metadata_csum,64bit,extent,flex_bg,huge_file,extra_isize,^has_journal"], 1024)
    make(out, "e4_bigalloc4k", ["-t", "ext4", "-I", "256", "-C", "16384", "-O", "bigalloc,^resize_inode,extent,flex_bg,metadata_csum,64bit,has_journal"], 4096, size_mb=512)
    make(out, "e4_metabg4k", ["-t", "ext4", "-I", "256", "-O", "meta_bg,^resize_inode,extent,flex_bg,metadata_csum,64bit,has_journal"], 4096)
    make(out, "e3_4k", ["-t", "ext3", "-I", "256"], 4096)                          # ブロックマップ + journal (クリーン)
    make(out, "e2_4k", ["-t", "ext2", "-I", "128"], 4096, fallocate=False)           # journal なし・ブロックマップ (ext4 ドライバが ext2 も読めること)
    # needs_recovery を立てたイメージ (内容は e4_def4k と同じ。journal の中身は空なので、noload でも内容は一致する)
    nr = os.path.join(out, "e4_needs_recovery.img")
    shutil.copyfile(os.path.join(out, "e4_def4k.img"), nr)
    run(["debugfs", "-w", "-R", "feature needs_recovery", nr])
    shutil.copyfile(os.path.join(out, "e4_def4k.manifest"), os.path.join(out, "e4_needs_recovery.manifest"))
    print("e4_needs_recovery: %s" % ([l for l in features(nr) if l.startswith("Filesystem features")] or [""])[0])
    # 未対応機能
    make_reject(out, "rej_encrypt", ["-O", "encrypt"])
    make_reject(out, "rej_casefold", ["-O", "casefold", "-E", "encoding=utf8"])
    make_reject(out, "rej_verity", ["-O", "verity"])
    unk = os.path.join(out, "rej_unknown.img")
    shutil.copyfile(os.path.join(out, "e4_def1k.img"), unk)
    with open(unk, "r+b") as f:   # incompat に未定義のビット (0x00100000) を足す
        f.seek(1024 + 0x60); v = int.from_bytes(f.read(4), "little"); f.seek(1024 + 0x60); f.write((v | 0x00100000).to_bytes(4, "little"))
    print("rej_unknown: incompat bit 0x100000 added")

if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "/dev/shm/usbmem_ext4")
