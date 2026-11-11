#!/usr/bin/env python3
"""ext2 のテスト用イメージを作る (core/compat/tests/test_ext2.cpp 用)。
使い方: tools/gen_ext2_fixture.py <outdir>   (PATH に mke2fs / e2fsck / dumpe2fs が必要。/sbin は自動で足す)
`mke2fs -t ext2 -d <ツリー>` でルート権限なしにファイルを書き込む (Linux のドライバ・FUSE は使わない)。ツリーの内容は決定的。
出力: <outdir>/<name>.img と <name>.manifest。manifest はタブ区切り:
  D <path> <mtime>
  F <path> <size> <mtime> <nlink> <spec>    spec = "d:<off>:<len>:<seed>" (パターンの内容) と "z:<off>:<len>" (ゼロであるべき範囲) をカンマ区切り
  L <path> <target>                          シンボリックリンク (リンク先の文字列)
内容の規則 pattern: byte(i) = (i * 31 + seed * 7 + (i >> 8)) & 0xFF   (i はファイル先頭からのオフセット)
作った後 e2fsck -fn で clean を確認し、dumpe2fs -h の features を <name>.features に残す (fs/ext2 が受理する機能だけを使っていることの記録)。"""
import os, sys, shutil, subprocess, tempfile

os.environ["PATH"] = os.environ.get("PATH", "") + ":/sbin:/usr/sbin"
FIXED_MTIME = 1700000000

def pattern(seed, off, n):
    return bytes((((off + i) * 31 + seed * 7 + ((off + i) >> 8)) & 0xFF) for i in range(n))

def build(tree, bs):
    """ツリーを作り、manifest の行を返す。bs = ブロックサイズ。"""
    man = []
    n = bs // 4                      # 1 ブロックに入るブロック番号数
    clock = [FIXED_MTIME]
    def stamp(path, is_link=False):
        clock[0] += 1
        os.utime(path, (clock[0], clock[0]), follow_symlinks=not is_link)
        return clock[0]
    def put(rel, size, items, nlink=1):
        """items: [("d", off, len, seed)]。それ以外 (穴) はゼロ。疎ファイルは seek で作る。"""
        p = os.path.join(tree, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            for it in items:
                if it[0] == "d":
                    f.seek(it[1]); f.write(pattern(it[3], it[1], it[2]))
            f.truncate(size)
        mt = stamp(p)
        spec = []
        pos = 0
        for it in items:
            if it[0] == "d":
                if it[1] > pos: spec.append("z:%d:%d" % (pos, it[1] - pos))
                spec.append("d:%d:%d:%d" % (it[1], it[2], it[3])); pos = it[1] + it[2]
        if size > pos: spec.append("z:%d:%d" % (pos, size - pos))
        man.append("F\t%s\t%d\t%d\t%d\t%s" % (rel, size, mt, nlink, ",".join(spec)))
    def dense(rel, size, seed): put(rel, size, [("d", 0, size, seed)] if size else [])
    def mkdir(rel):
        p = os.path.join(tree, rel); os.makedirs(p, exist_ok=True); return p
    seed = [1]
    def s():
        seed[0] += 1; return seed[0]

    # ---- 基本サイズ
    dense("small.txt", 11, s()); dense("empty.bin", 0, s())
    dense("one_block.bin", bs, s()); dense("block_minus1.bin", bs - 1, s()); dense("block_plus1.bin", bs + 1, s())
    # ---- ブロックマップの境界 (直接 12 / 単間接 n / 二重間接 n*n / 三重間接)
    dense("direct_max.bin", 12 * bs, s()); dense("direct_plus1.bin", 12 * bs + 1, s())
    dense("single_ind_max.bin", (12 + n) * bs, s()); dense("single_ind_plus1.bin", (12 + n) * bs + 1, s())
    dense("double_ind_start.bin", (12 + n + 3) * bs + 7, s())         # 二重間接の最初の数ブロックまで
    # 二重間接の中で境界をまたぐ疎ファイル: 二重間接の「最初のブロック」と「n 個目のブロック (2 つ目の間接ブロック)」にだけデータ
    d0 = (12 + n) * bs; d1 = (12 + n + n) * bs
    put("double_ind_sparse.bin", d1 + bs * 2, [("d", 0, 100, s()), ("d", d0, bs, s()), ("d", d1 - bs, 2 * bs, s())])
    # 三重間接 (最初のブロックにだけデータ。巨大な穴を含む疎ファイル)
    t0 = (12 + n + n * n) * bs
    put("triple_ind_sparse.bin", t0 + 3 * bs + 5, [("d", 0, bs, s()), ("d", t0, 2 * bs + 5, s()), ("d", t0 + 2 * bs + 5, bs, s())])
    # 穴だらけ (直接ブロックの途中の穴、単間接の穴)
    put("holes.bin", 20 * bs, [("d", 0, bs, s()), ("d", 5 * bs, bs, s()), ("d", 15 * bs + 10, 100, s())])
    # 全部穴 (割り当てなし)
    put("all_hole.bin", 100 * bs + 3, [])
    # ---- 名前
    dense("日本語ファイル名.txt", 300, s())
    dense("n" * 255, 5, s())                                          # 255 バイトの名前
    dense("space and 'quote'.txt", 17, s())
    # ---- ディレクトリ
    mkdir("emptydir"); man.append("D\temptydir\t%d" % stamp(os.path.join(tree, "emptydir")))
    deep = "d1/d2/d3/d4/d5/d6/d7/d8"
    dense(deep + "/leaf.txt", 1234, s())
    for k in range(1, 9):
        pth = "/".join(["d%d" % j for j in range(1, k + 1)])
        man.append("D\t%s\t%d" % (pth, stamp(os.path.join(tree, pth))))
    # 大きなディレクトリ (ディレクトリ自体が複数ブロック・複数ページにわたる)
    mkdir("many")
    for i in range(1000):
        sz = 1 + (i * 7) % 300
        dense("many/file_%04d.dat" % i, sz, 3 + i % 200)
    man.append("D\tmany\t%d" % stamp(os.path.join(tree, "many")))
    # ---- リンク
    os.link(os.path.join(tree, "small.txt"), os.path.join(tree, "hardlink_to_small.txt"))
    # (hardlink の mtime は small.txt と同じ。nlink 2 を両方の行に反映)
    man[0] = man[0].replace("\t1\t", "\t2\t", 1) if man[0].startswith("F\tsmall.txt") else man[0]
    man.append("F\thardlink_to_small.txt\t11\t%d\t2\t%s" % (FIXED_MTIME + 1, "d:0:11:2"))
    fast = "small.txt"                                                 # < 60 バイト: 高速シンボリックリンク (i_block に格納)
    slow = "d1/d2/d3/d4/d5/d6/d7/d8/leaf.txt" + "/../" * 0 + ("x" * 0)
    slow = "../" * 3 + "many/" + "f" * 80                             # >= 60 バイト: 低速シンボリックリンク (データブロック)
    for rel, tgt in (("link_fast", fast), ("link_slow", slow), ("link_dangling", "no/such/target")):
        os.symlink(tgt, os.path.join(tree, rel))
        man.append("L\t%s\t%s" % (rel, tgt))
    return man

def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw)

def make(outdir, name, bs, inode_size):
    with tempfile.TemporaryDirectory(dir=outdir) as td:
        tree = os.path.join(td, "tree"); os.makedirs(tree)
        man = build(tree, bs)
        img = os.path.join(outdir, name + ".img")
        if os.path.exists(img): os.remove(img)
        with open(img, "wb") as f: f.truncate(96 * 1024 * 1024)
        run(["mke2fs", "-q", "-t", "ext2", "-b", str(bs), "-I", str(inode_size), "-L", "usbmem-" + name,
             "-U", "7d9c1f5e-0000-4000-8000-%012d" % bs, "-E", "root_owner=0:0,hash_seed=7d9c1f5e-1111-4111-8111-000000000001",
             "-d", tree, img])
        # 「ツリーをそのまま書いた」ので、ホストが見た内容と manifest が一致することを確認する材料として、そのまま残す
        r = subprocess.run(["e2fsck", "-fn", img], capture_output=True, text=True)
        if r.returncode != 0: sys.exit("e2fsck NOT clean for %s:\n%s%s" % (name, r.stdout, r.stderr))
        feats = [l for l in run(["dumpe2fs", "-h", img]).stdout.splitlines() if l.startswith(("Filesystem features", "Inode size", "Block size", "Inode count", "Block count", "Filesystem revision"))]
        open(os.path.join(outdir, name + ".features"), "w").write("\n".join(feats) + "\n")
        with open(os.path.join(outdir, name + ".manifest"), "w", encoding="utf-8") as f: f.write("\n".join(man) + "\n")
        print("%s: %d manifest lines, e2fsck clean; %s" % (name, len(man), "; ".join(feats[:1])))

def make_other(outdir, name, fstype, opts):
    """ext3 / ext4 の空のボリューム (fs/ext2 が受理するか・拒否するかのテスト用)。内容は無い。"""
    img = os.path.join(outdir, name + ".img")
    if os.path.exists(img): os.remove(img)
    with open(img, "wb") as f: f.truncate(16 * 1024 * 1024)
    run(["mke2fs", "-q", "-t", fstype, "-b", "4096"] + opts + [img])
    feats = [l for l in run(["dumpe2fs", "-h", img]).stdout.splitlines() if l.startswith("Filesystem features")]
    open(os.path.join(outdir, name + ".features"), "w").write("\n".join(feats) + "\n")
    print("%s: %s" % (name, feats[0] if feats else ""))

if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "/dev/shm/usbmem_ext2"
    os.makedirs(out, exist_ok=True)
    make(out, "b1k", 1024, 128)
    make(out, "b2k", 2048, 256)
    make(out, "b4k", 4096, 256)
    make_other(out, "ext3_empty", "ext3", [])
    make_other(out, "ext4_empty", "ext4", [])
