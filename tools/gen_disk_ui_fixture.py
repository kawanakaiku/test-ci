#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""ディスクツールの UI 試験 (android/app/src/androidTest/.../DiskUiTest.kt) 用のイメージを作る。tools/ci_android_tests.sh の disk-ui グループが呼ぶ。
使い方: tools/gen_disk_ui_fixture.py <出力ディレクトリ>
作るもの (どれも小さい。root 不要。mke2fs / debugfs / sgdisk / mkfs.fat が要る):
  ext2_clean.img   16 MiB の ext2 (ラベル DUICLEAN)。ディスク全体が 1 つのファイルシステム。検査 (クリーン)・バックアップ・救出の元
  ext2_bad.img     ext2_clean.img に、リンク数の食い違いと、使用中ブロックの解放済み印を入れたもの (e2fsck -fn がエラーにする)。検査の結果の一覧の試験
  ext2_target.img  16 MiB の ext2 (ラベル DUITGT)。復元の書き込み先 (ext2_clean.img と同じ大きさ・同じ形式、ラベルだけ違う)
  gpt2.img         16 MiB の GPT。#1 = ext2 (DUIP1、1 MiB から 7 MiB)、#2 = FAT16 (DUIP2、8 MiB から 7 MiB)。パーティション編集の試験
  lost_gpt.img     gpt2.img と同じ構成で、先頭の GPT (保護 MBR + ヘッダ + 配列 = LBA 0〜33) をゼロで消したもの。末尾の控えは生きている。失われたパーティションを探す試験
  lost_mbr.img     16 MiB の MBR。#1 = ext2 (DUIP1、1 MiB から 7 MiB)、#2 = FAT16 (DUIP2、8 MiB から 7 MiB)。LBA 0 (MBR) をゼロで消したもの。同上
  boot_multi.img   54 MiB の MBR。#1 = FAT32 (36 MiB、LBA 2048)、#2 = NTFS (8 MiB、LBA 75776)、#3 = exFAT (8 MiB、LBA 92160)。3 つとも先頭のセクタ (ブートセクタ) だけをゼロで消した
                   (控えは生きている)。ブートセクタの検査 / 修復の試験。配置は tools/ci_android_tests.sh の disk_ui_verify も使う (BOOT_* と同じ値)
生成後に、ホストの e2fsck / sgdisk / sfdisk / fsck.* で期待どおり (clean は 0、bad は 0 以外、gpt2 は 2 パーティション、lost_* は表が無い、boot_multi は fsck が失敗) かを確かめる。
sgdisk が無い環境 (手元) では gpt2.img だけが作れない。lost_* と boot_multi は sfdisk (util-linux) と mkfs.* だけで作る (TMPDIR を tmpfs にすると速い)。
"""
import os
import random
import shutil
import subprocess
import sys
import tempfile

MIB = 1 << 20


def tool(name):
    p = shutil.which(name)
    if p:
        return p
    for d in ("/sbin", "/usr/sbin"):
        c = os.path.join(d, name)
        if os.access(c, os.X_OK):
            return c
    sys.exit(f"missing host tool: {name}")


def run(cmd, check=True, **kw):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **kw)
    if check and r.returncode != 0:
        sys.exit(f"command failed ({r.returncode}): {' '.join(cmd)}\n{r.stdout}")
    return r


def make_tree(root, tag):
    os.makedirs(os.path.join(root, "dir"), exist_ok=True)
    with open(os.path.join(root, "hello.txt"), "w") as f:
        f.write(f"disk ui fixture {tag}\n")
    rnd = random.Random(7001)
    with open(os.path.join(root, "dir", "a.bin"), "wb") as f:
        f.write(bytes(rnd.getrandbits(8) for _ in range(100 * 1024)))
    with open(os.path.join(root, "dir", "b.txt"), "w") as f:
        f.write("line\n" * 2000)


def truncate(path, size):
    with open(path, "wb") as f:
        f.truncate(size)


def make_ext2(path, size, label, tag):
    truncate(path, size)
    with tempfile.TemporaryDirectory() as tree:
        make_tree(tree, tag)
        run([tool("mke2fs"), "-q", "-t", "ext2", "-b", "1024", "-L", label, "-F", "-E", "root_owner=0:0", "-d", tree, path])


SECTOR = 512
# boot_multi.img の配置 (セクタ)。tools/ci_android_tests.sh の disk_ui_verify の BOOT_* と同じ値にする
BOOT_P1 = (2048, 73728)      # FAT32 36 MiB
BOOT_P2 = (75776, 16384)     # NTFS 8 MiB
BOOT_P3 = (92160, 16384)     # exFAT 8 MiB
BOOT_TOTAL_SECTORS = 110592  # 54 MiB

GPT_LINUX = "0FC63DAF-8483-4772-8E79-3D69D8477DE4"
GPT_BASIC = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"


def zero_sectors(path, first, count):
    with open(path, "r+b") as f:
        f.seek(first * SECTOR)
        f.write(bytes(count * SECTOR))


def embed(img, part, start_sector):
    with open(part, "rb") as s, open(img, "r+b") as d:
        d.seek(start_sector * SECTOR)
        shutil.copyfileobj(s, d)


def two_partitions(path, gpt):
    """16 MiB のイメージに、#1 = ext2 (1 MiB から 7 MiB)、#2 = FAT16 (8 MiB から 7 MiB) を作る (表は GPT か MBR)。"""
    truncate(path, 16 * MIB)
    if gpt:
        script = (f"label: gpt\nunit: sectors\nstart=2048, size=14336, type={GPT_LINUX}, name=\"DUIP1\"\n"
                  f"start=16384, size=14336, type={GPT_BASIC}, name=\"DUIP2\"\n")
    else:
        script = "label: dos\nlabel-id: 0x0d0e0a0d\nunit: sectors\nstart=2048, size=14336, type=83\nstart=16384, size=14336, type=6\n"
    run([tool("sfdisk"), "--no-reread", "--no-tell-kernel", path], input=script)
    with tempfile.TemporaryDirectory() as tree:
        make_tree(tree, "p1")
        run([tool("mke2fs"), "-q", "-t", "ext2", "-b", "1024", "-L", "DUIP1", "-F", "-E", f"offset={1 * MIB},root_owner=0:0", "-d", tree, path, str(7 * 1024)])
    run([tool("mkfs.fat"), "-F", "16", "-s", "1", "-n", "DUIP2", "--offset", str(16384), path, str(7 * 1024)])
    if run([tool("e2fsck"), "-fn", f"{path}?offset={1 * MIB}"], check=False).returncode != 0:
        sys.exit(f"{path}: p1 is not clean")


def make_lost(out):
    # GPT: 先頭の GPT (LBA 0〜33) だけ消す。末尾の控え (ヘッダ + 配列) は生きている
    g = os.path.join(out, "lost_gpt.img")
    two_partitions(g, gpt=True)
    zero_sectors(g, 0, 34)
    # MBR: LBA 0 だけ消す
    m = os.path.join(out, "lost_mbr.img")
    two_partitions(m, gpt=False)
    zero_sectors(m, 0, 1)
    for f in (g, m):
        with open(f, "rb") as fh:
            if fh.read(SECTOR) != bytes(SECTOR):
                sys.exit(f"{f}: sector 0 was not erased")


def make_boot_multi(out):
    path = os.path.join(out, "boot_multi.img")
    truncate(path, BOOT_TOTAL_SECTORS * SECTOR)
    script = ("label: dos\nlabel-id: 0x0d0e0a0e\nunit: sectors\n"
              f"start={BOOT_P1[0]}, size={BOOT_P1[1]}, type=c\nstart={BOOT_P2[0]}, size={BOOT_P2[1]}, type=7\nstart={BOOT_P3[0]}, size={BOOT_P3[1]}, type=7\n")
    run([tool("sfdisk"), "--no-reread", "--no-tell-kernel", path], input=script)
    with tempfile.TemporaryDirectory() as tmp:
        parts = []
        for name, (start, count), mk in (
            ("fat32", BOOT_P1, lambda p: run([tool("mkfs.fat"), "-F", "32", "-s", "1", "-n", "DUIBOOT1", p])),
            ("ntfs", BOOT_P2, lambda p: run([tool("mkntfs"), "-F", "-Q", "-q", "-s", "512", "-c", "4096", "-L", "DUIBOOT2", p])),
            ("exfat", BOOT_P3, lambda p: run([tool("mkfs.exfat"), "-L", "DUIBOOT3", p])),
        ):
            pf = os.path.join(tmp, name + ".img")
            truncate(pf, count * SECTOR)
            mk(pf)
            parts.append((name, pf, start))
        checks = {"fat32": [tool("fsck.fat"), "-n"], "ntfs": [tool("ntfsfix"), "-n"], "exfat": [tool("fsck.exfat"), "-n"]}
        for name, pf, start in parts:
            if run(checks[name] + [pf], check=False).returncode != 0:
                sys.exit(f"boot_multi: the fresh {name} is not clean")
            zero_sectors(pf, 0, 1)      # 先頭のセクタだけ壊す (控えは生きている)
            if run(checks[name] + [pf], check=False).returncode == 0:
                sys.exit(f"boot_multi: the damaged {name} is unexpectedly clean")
            embed(path, pf, start)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else sys.exit(__doc__)
    os.makedirs(out, exist_ok=True)
    e2fsck = tool("e2fsck")
    debugfs = tool("debugfs")

    clean = os.path.join(out, "ext2_clean.img")
    make_ext2(clean, 16 * MIB, "DUICLEAN", "clean")
    if run([e2fsck, "-fn", clean], check=False).returncode != 0:
        sys.exit("ext2_clean.img is not clean")

    bad = os.path.join(out, "ext2_bad.img")
    shutil.copyfile(clean, bad)
    run([debugfs, "-w", "-R", "sif /hello.txt links_count 7", bad])
    blk = run([debugfs, "-R", "bmap /dir/a.bin 0", bad]).stdout.strip().splitlines()[-1].strip()
    if not blk.isdigit():
        sys.exit(f"bmap failed: {blk}")
    run([debugfs, "-w", "-R", f"freeb {blk}", bad])
    if run([e2fsck, "-fn", bad], check=False).returncode == 0:
        sys.exit("ext2_bad.img is unexpectedly clean")

    make_ext2(os.path.join(out, "ext2_target.img"), 16 * MIB, "DUITGT", "target")

    gpt = os.path.join(out, "gpt2.img")
    truncate(gpt, 16 * MIB)
    run([tool("sgdisk"), "-o", "-n", "1:2048:16383", "-t", "1:8300", "-c", "1:DUIP1", "-n", "2:16384:30719", "-t", "2:0700", "-c", "2:DUIP2", gpt])
    with tempfile.TemporaryDirectory() as tree:
        make_tree(tree, "p1")
        run([tool("mke2fs"), "-q", "-t", "ext2", "-b", "1024", "-L", "DUIP1", "-F", "-E", f"offset={1 * MIB},root_owner=0:0", "-d", tree, gpt, str(7 * 1024)])
    run([tool("mkfs.fat"), "-F", "16", "-s", "1", "-n", "DUIP2", "--offset", str(16384), gpt, str(7 * 1024)])
    p = run([tool("sgdisk"), "-p", gpt]).stdout
    if "DUIP1" not in p or "DUIP2" not in p:
        sys.exit(f"gpt2.img layout is unexpected:\n{p}")
    if run([e2fsck, "-fn", f"{gpt}?offset={1 * MIB}"], check=False).returncode != 0:
        sys.exit("gpt2.img p1 is not clean")

    make_lost(out)
    make_boot_multi(out)

    for n in sorted(os.listdir(out)):
        print(n, os.path.getsize(os.path.join(out, n)))


if __name__ == "__main__":
    main()
