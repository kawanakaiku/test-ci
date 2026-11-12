# SPDX-License-Identifier: Apache-2.0
"""破損入力に対する頑健性: 実イメージのバイトを無作為に壊して検査を走らせ、落ちない・止まらないことを確かめる。

正誤は問わない (見逃し・過検出は dt_fat / dt_exfat / dt_ext / dt_ntfs が見る)。ここで見るのは
「終了コードが 0〜4 (検査結果)」であること。ASan/UBSan 付きの CLI (USBMEM_SANITIZE=ON) で走らせると、境界外アクセス・未定義動作・無限ループも拾える。
"""
import os
import random
import shutil

from dt_common import Result, ours, work_dir

MiB = 1 << 20


def metadata_sectors(data, limit_first=8 * MiB):
    """中身のあるセクタの位置 (512B 単位)。メタデータらしい (バイトの種類が少ない) セクタを先頭に重視し、データ (乱数) は残りに回す。"""
    first, rest = [], []
    n = len(data) // 512
    for i in range(n):
        sec = data[i * 512:(i + 1) * 512]
        if not any(sec):
            continue
        (first if len(set(sec)) < 110 else rest).append(i)
    return first, rest


def damage(rnd, data, first, rest):
    d = bytearray(data)
    for _ in range(rnd.randint(1, 8)):
        pool = first if (first and (not rest or rnd.random() < 0.85)) else rest
        if not pool:
            break
        sec = rnd.choice(pool)
        pos = sec * 512 + rnd.randrange(512)
        m = rnd.randrange(5)
        if m == 0:
            d[pos] = rnd.randrange(256)
        elif m == 1:
            d[pos] ^= 1 << rnd.randrange(8)
        elif m == 2:
            d[pos] = 0xFF
        elif m == 3:
            for q in range(min(8, len(d) - pos)):
                d[pos + q] = rnd.randrange(256)
        else:
            for q in range(min(16, len(d) - pos)):
                d[pos + q] = 0
    return d


def fuzz_one(cli, res, name, base, fs, iters, seed):
    data = open(base, "rb").read()
    first, rest = metadata_sectors(data)
    rnd = random.Random(seed)
    img = base + ".fz"
    crashes = 0
    verdicts = {}
    for it in range(iters):
        with open(img, "wb") as f:
            f.write(damage(rnd, data, first, rest))
        c, j = ours(cli, img, fs=fs)
        verdicts[c] = verdicts.get(c, 0) + 1
        if c > 4 or j.get("verdict") == "crash":
            crashes += 1
            keep = base + ".crash%d" % it
            shutil.copy(img, keep)
            print("   CRASH exit=%s saved=%s %s" % (c, keep, str(j.get("stderr", ""))[:300]))
    res.add("fuzz %s x%d (exit codes %s)" % (name, iters, dict(sorted(verdicts.items()))), crashes > 0, False, "%d crash(es)" % crashes)


def run_fuzz(cli, res: Result, quick=False):
    import dt_ext
    import dt_fat
    d = work_dir("fuzz")
    iters = 30 if quick else 250
    # FAT12 / FAT16 / FAT32
    for bits, mb in ((12, 4), (16, 24), (32, 40)):
        p = os.path.join(d, "fat%d.img" % bits)
        dt_fat.make_image(p, bits, mb)
        fuzz_one(cli, res, "fat%d" % bits, p, "fat%d" % bits, iters, 100 + bits)
    # exFAT
    import gen_exfat_fixture as g
    g.make(d, "ex", 512, 4096, 24)
    fuzz_one(cli, res, "exfat", os.path.join(d, "ex.img"), "exfat", iters, 200)
    # ext2 / ext4 (extents + metadata_csum) / ext4 inline_data
    tree = os.path.join(d, "tree")
    dt_ext.make_tree(tree)
    for kind, bs in (("ext2", 1024), ("ext4", 4096), ("ext4-inline", 1024)):
        p = os.path.join(d, kind + ".img")
        dt_ext.make_image(p, kind, 24, bs, tree)
        fuzz_one(cli, res, kind, p, kind.split("-")[0], iters, 300 + bs)
    # NTFS
    import dt_ntfs
    try:
        b = dt_ntfs.make_base(os.path.join(d, "nt"))
        fuzz_one(cli, res, "ntfs", b, "ntfs", iters, 400)
    except dt_ntfs.Skip:
        print("SKIP fuzz ntfs: fixture tools not available")
    return res
