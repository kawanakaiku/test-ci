#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""ディスクツールの UI 試験 (android/app/src/androidTest/.../DiskUiTest.kt) 用のイメージを作る。tools/ci_android_tests.sh の disk-ui グループが呼ぶ。
使い方: tools/gen_disk_ui_fixture.py <出力ディレクトリ>
作るもの (どれも小さい。root 不要。mke2fs / debugfs / sgdisk / mkfs.fat が要る):
  ext2_clean.img   16 MiB の ext2 (ラベル DUICLEAN)。ディスク全体が 1 つのファイルシステム。検査 (クリーン)・バックアップ・救出の元
  ext2_bad.img     ext2_clean.img に、リンク数の食い違いと、使用中ブロックの解放済み印を入れたもの (e2fsck -fn がエラーにする)。検査の結果の一覧の試験
  ext2_target.img  16 MiB の ext2 (ラベル DUITGT)。復元の書き込み先 (ext2_clean.img と同じ大きさ・同じ形式、ラベルだけ違う)
  gpt2.img         16 MiB の GPT。#1 = ext2 (DUIP1、1 MiB から 7 MiB)、#2 = FAT16 (DUIP2、8 MiB から 7 MiB)。パーティション編集の試験
生成後に、ホストの e2fsck / sgdisk で期待どおり (clean は 0、bad は 0 以外、gpt2 は 2 パーティション) かを確かめる。
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
    run([tool("mkfs.fat"), "-F", "16", "-n", "DUIP2", "--offset", str(16384), gpt, str(7 * 1024)])
    p = run([tool("sgdisk"), "-p", gpt]).stdout
    if "DUIP1" not in p or "DUIP2" not in p:
        sys.exit(f"gpt2.img layout is unexpected:\n{p}")
    if run([e2fsck, "-fn", f"{gpt}?offset={1 * MIB}"], check=False).returncode != 0:
        sys.exit("gpt2.img p1 is not clean")

    for n in sorted(os.listdir(out)):
        print(n, os.path.getsize(os.path.join(out, n)))


if __name__ == "__main__":
    main()
