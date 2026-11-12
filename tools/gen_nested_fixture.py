#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""ネスト (ループバック) マウントのテスト用イメージを作る (core/compat/tests/test_nested.cpp 用)。docs/nested-mount.md
使い方: gen_nested_fixture.py <出力ディレクトリ>

子 (中に入れるイメージ。元のファイルもそのまま残す = 「ネストで読んだ結果が元と同じか」を比べる基準):
  c_ext2.img  ext2 (3 MiB)      c_ext4.img  ext4 (6 MiB、extent / metadata_csum)    c_fat.img   FAT (3 MiB)
  c_ntfs.img  NTFS (6 MiB、ルート直下のファイルだけ)
  c_gpt.img   GPT (12 MiB): 1 = ext2 (c_ext2.img)、2 = FAT (c_fat.img)
  c_mbr.img   MBR (12 MiB): 1 = ext2、2 = FAT、3 = 拡張 { 5 = 論理 ext2 }
  c_empty.img 全部ゼロの 1 MiB (中身が判別できないイメージ)
入れ子の鎖 (深さ上限のテスト): l1.img (ext4) ⊃ l2.img (ext2) ⊃ l3.img (FAT) ⊃ l4.img (ext2)
親 (子を全部入れる。l1.img は p_ext4 だけ):
  p_ext4.img (ext4 + ジャーナル、/imgs/)  p_ext2.img (ext2、/imgs/)  p_fat.img (FAT32、/imgs/)
  p_exfat.img (exFAT、/imgs/)             p_ntfs.img (NTFS、ルート直下)
作成には mke2fs / mkfs.fat + mcopy / mkntfs + ntfscp / mkfs.exfat / sgdisk / sfdisk を使う (Linux の FS ドライバや FUSE は使わない)。
必要なツールが無ければ終了コード 77 (CTest で SKIP)。
"""
import os, shutil, subprocess, sys, tempfile

OUT = sys.argv[1] if len(sys.argv) > 1 else "/dev/shm/usbmem_nested_%s" % os.environ.get("USER", "u")
os.environ["PATH"] = os.environ.get("PATH", "") + ":/sbin:/usr/sbin"
ENV = dict(os.environ, TZ="UTC", LC_ALL="C.UTF-8", MTOOLS_SKIP_CHECK="1")
HERE = os.path.dirname(os.path.abspath(__file__))
MIB = 1 << 20


def need(*tools):
    miss = [t for t in tools if shutil.which(t) is None]
    if miss:
        print("skip: missing tools: " + " ".join(miss), file=sys.stderr)
        sys.exit(77)


def run(*a, check=True):
    r = subprocess.run(a, env=ENV, capture_output=True)
    if check and r.returncode != 0:
        sys.exit("command failed: %s\n%s\n%s" % (" ".join(a), r.stdout.decode(errors="replace")[-600:], r.stderr.decode(errors="replace")[-600:]))
    return r


def pattern(seed, n):
    return bytes(((i * 31 + seed * 7 + (i >> 8)) & 0xFF) for i in range(n))


def truncate(path, size):
    if os.path.exists(path):
        os.unlink(path)
    with open(path, "wb") as f:
        f.truncate(size)


def build_tree(root, seed, flat=False):
    """決定的なツリー。flat のときはルート直下のファイルだけ (ntfscp はディレクトリを作れない)。"""
    os.makedirs(root, exist_ok=True)
    def put(rel, data):
        p = os.path.join(root, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)
    put("hello.txt", b"hello nested %d\n" % seed)
    put("big.bin", pattern(seed, 700000 + seed))
    put("mid.bin", pattern(seed + 1, 100000))
    for i in range(30):
        put("f_%03d.txt" % i if flat else "many/f_%03d.txt" % i, pattern(seed + i, 40 + i * 37))
    if not flat:
        put("dir/sub/deep.txt", b"deep %d\n" % seed)
        put("dir/other.bin", pattern(seed + 9, 33333))


def mk_ext(img, size, kind, tree, label):
    truncate(img, size)
    if kind == "ext2":
        run("mke2fs", "-q", "-t", "ext2", "-b", "1024", "-L", label, "-d", tree, img)
    else:
        feats = "extent,huge_file,flex_bg,metadata_csum,64bit,dir_nlink,extra_isize,resize_inode,dir_index,sparse_super,large_file,ext_attr"
        run("mke2fs", "-q", "-t", "ext4", "-b", "4096", "-I", "256", "-O", feats, "-L", label, "-d", tree, img)
    run("e2fsck", "-fn", img)


def mk_fat(img, size, tree, label, extra=()):
    truncate(img, size)
    run("mkfs.fat", "-n", label, *extra, img)
    run("mcopy", "-s", "-i", img, *[os.path.join(tree, n) for n in sorted(os.listdir(tree))], "::")


def mk_ntfs(img, size, tree, label):
    truncate(img, size)
    run("mkntfs", "-F", "-Q", "-q", "-L", label, img)
    for n in sorted(os.listdir(tree)):
        run("ntfscp", "-f", "-q", img, os.path.join(tree, n), "/" + n)


def dd_into(disk, part, start_sector):
    with open(part, "rb") as s, open(disk, "r+b") as d:
        d.seek(start_sector * 512)
        shutil.copyfileobj(s, d, 1 << 20)


def mk_gpt(img, ext2, fat):
    truncate(img, 12 * MIB)
    run("sgdisk", "-o", "-n", "1:2048:+6144", "-t", "1:8300", "-c", "1:linuxpart", "-n", "2:8192:+6144", "-t", "2:0700", "-c", "2:datapart", img)
    dd_into(img, ext2, 2048)
    dd_into(img, fat, 8192)


def mk_mbr(img, ext2, fat):
    truncate(img, 12 * MIB)
    script = "label: dos\nlabel-id: 0x4e455354\nstart=2048, size=6144, type=83\nstart=8192, size=6144, type=c\nstart=14336, size=10240, type=5\nstart=16384, size=6144, type=83\n"
    r = subprocess.run(["sfdisk", img], input=script.encode(), env=ENV, capture_output=True)
    if r.returncode != 0:
        sys.exit("sfdisk failed: " + r.stderr.decode(errors="replace"))
    dd_into(img, ext2, 2048)
    dd_into(img, fat, 8192)
    dd_into(img, ext2, 16384)


def mk_exfat_parent(img, size, files):
    """exFAT: mkfs.exfat で空のボリュームを作り、tools/gen_exfat_fixture.py の書き込み部品で /imgs/ に連続クラスタでファイルを置く。"""
    sys.path.insert(0, HERE)
    import gen_exfat_fixture as g
    truncate(img, size)
    run("mkfs.exfat", "-q", "-s", "512", "-c", "4096", "-L", "NESTPAR", img)
    im = g.Img(img)
    cl = im.cluster
    root_raw = im.rd(im.coff(im.root), cl)
    end = 0
    while end < len(root_raw) and root_raw[end] != 0:
        end += 32
    root = g.Dir(im, [im.root], root_raw[:end])
    imgs = g.Dir(im, [])
    for name, path in files:
        data = open(path, "rb").read()
        nclu = (len(data) + cl - 1) // cl
        clus = im.alloc(nclu)
        assert clus == list(range(clus[0], clus[0] + nclu)), "contiguous alloc expected"
        padded = data.ljust(nclu * cl, b"\0")
        # まとめて 1 回で書く (連続しているので)
        im.wr(im.coff(clus[0]), padded)
        imgs.add(g.entry_set(im, name, 0x20, clus[0], len(data), len(data), True))
    need_clu = max(1, (len(imgs.data) + 32 + cl - 1) // cl)
    imgs.clus = im.alloc(need_clu)
    root.add(g.entry_set(im, "imgs", 0x10, imgs.clus[0], need_clu * cl, need_clu * cl, False))
    imgs.flush()
    root.flush()
    im.flush()
    r = subprocess.run(["fsck.exfat", "-n", img], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("fsck.exfat failed on %s:\n%s%s" % (img, r.stdout, r.stderr))


def main():
    need("mke2fs", "e2fsck", "mkfs.fat", "mcopy", "mkntfs", "ntfscp", "mkfs.exfat", "fsck.exfat", "sgdisk", "sfdisk")
    os.makedirs(OUT, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=OUT) as td:
        t = lambda n, seed, flat=False: (build_tree(os.path.join(td, n), seed, flat), os.path.join(td, n))[1]
        J = lambda n: os.path.join(OUT, n)
        # ---- 子
        mk_ext(J("c_ext2.img"), 3 * MIB, "ext2", t("t_ext2", 1), "c_ext2")
        mk_ext(J("c_ext4.img"), 6 * MIB, "ext4", t("t_ext4", 2), "c_ext4")
        mk_fat(J("c_fat.img"), 3 * MIB, t("t_fat", 3), "C_FAT")
        mk_ntfs(J("c_ntfs.img"), 6 * MIB, t("t_ntfs", 4, flat=True), "c_ntfs")
        mk_gpt(J("c_gpt.img"), J("c_ext2.img"), J("c_fat.img"))
        mk_mbr(J("c_mbr.img"), J("c_ext2.img"), J("c_fat.img"))
        truncate(J("c_empty.img"), 1 * MIB)
        # ---- 鎖 l4 ⊂ l3 ⊂ l2 ⊂ l1
        shutil.copyfile(J("c_ext2.img"), J("l4.img"))
        d3 = os.path.join(td, "chain3"); os.makedirs(d3); shutil.copyfile(J("l4.img"), os.path.join(d3, "l4.img"))
        mk_fat(J("l3.img"), 6 * MIB, d3, "L3")
        d2 = os.path.join(td, "chain2"); os.makedirs(d2); shutil.copyfile(J("l3.img"), os.path.join(d2, "l3.img"))
        build_tree(os.path.join(d2, "doc"), 20)
        mk_ext(J("l2.img"), 10 * MIB, "ext2", d2, "l2")
        d1 = os.path.join(td, "chain1"); os.makedirs(d1); shutil.copyfile(J("l2.img"), os.path.join(d1, "l2.img"))
        build_tree(os.path.join(d1, "doc"), 10)
        mk_ext(J("l1.img"), 16 * MIB, "ext4", d1, "l1")
        # ---- 親
        kids = ["c_ext2.img", "c_ext4.img", "c_fat.img", "c_ntfs.img", "c_gpt.img", "c_mbr.img", "c_empty.img"]
        tp = os.path.join(td, "parent_tree"); os.makedirs(os.path.join(tp, "imgs"))
        for k in kids + ["l1.img"]:
            shutil.copyfile(J(k), os.path.join(tp, "imgs", k))
        build_tree(os.path.join(tp, "docs"), 50)
        feats = "has_journal,extent,huge_file,flex_bg,metadata_csum,64bit,dir_nlink,extra_isize,resize_inode,dir_index,sparse_super,large_file,ext_attr"
        truncate(J("p_ext4.img"), 96 * MIB)
        run("mke2fs", "-q", "-t", "ext4", "-b", "4096", "-I", "256", "-O", feats, "-L", "p_ext4", "-d", tp, J("p_ext4.img"))
        run("e2fsck", "-fn", J("p_ext4.img"))
        tp2 = os.path.join(td, "parent_tree2"); os.makedirs(os.path.join(tp2, "imgs"))
        for k in kids:
            shutil.copyfile(J(k), os.path.join(tp2, "imgs", k))
        build_tree(os.path.join(tp2, "docs"), 51)
        truncate(J("p_ext2.img"), 64 * MIB)
        run("mke2fs", "-q", "-t", "ext2", "-b", "4096", "-L", "p_ext2", "-d", tp2, J("p_ext2.img"))
        run("e2fsck", "-fn", J("p_ext2.img"))
        truncate(J("p_fat.img"), 80 * MIB)
        run("mkfs.fat", "-F", "32", "-s", "2", "-n", "P_FAT", J("p_fat.img"))
        run("mcopy", "-s", "-i", J("p_fat.img"), os.path.join(tp2, "imgs"), os.path.join(tp2, "docs"), "::")
        mk_exfat_parent(J("p_exfat.img"), 80 * MIB, [(k, J(k)) for k in kids])
        # NTFS: ルート直下に子を置く (ntfscp は新規ファイルを作る)
        truncate(J("p_ntfs.img"), 64 * MIB)
        run("mkntfs", "-F", "-Q", "-q", "-L", "p_ntfs", J("p_ntfs.img"))
        for k in kids:
            run("ntfscp", "-f", "-q", J("p_ntfs.img"), J(k), "/" + k)
    for n in sorted(os.listdir(OUT)):
        print("%-14s %d" % (n, os.path.getsize(os.path.join(OUT, n))))


main()
