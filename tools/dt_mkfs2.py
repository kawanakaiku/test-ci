# SPDX-License-Identifier: Apache-2.0
"""mkfs 拡張 (ext3 / ext4 / FAT12 / FAT16 / NTFS) の検証。docs/mkfs-extended.md。

自前の mkfs で作ったイメージを外部ツール (検査のみ。書き込みはイメージファイルに対してだけ) で確かめる。
  ext3 / ext4: e2fsck -fn (終了 0 かつ WARNING なし) / dumpe2fs の機能一覧 / debugfs でファイルを書いて再度 e2fsck -fn
  FAT12 / FAT16: fsck.fat -n / 7z (読めること) / mtools (あれば mcopy で書いてから fsck.fat -n)
  NTFS: ntfsfix -n / ntfsinfo / ntfscp で書いて ntfscat で読み戻し / ntfsfix -n (もう一度)
  容量の範囲 (caps): 一覧と、容量ごとの可否・理由
  乱数: USBMEM_MKFS_ONLY_RANDOM=1 で乱数だけ (ASan/UBSan 付き CLI と組み合わせる)。USBMEM_MKFS_RANDOM=N (既定 12、--quick では 4) 件の容量・クラスタ・ラベル・機能の組み合わせ。クラッシュ / 適用失敗 / 外部ツールの指摘があれば失敗。
基準は dt_mkfs.py と同じ (ref=BAD かつ ours=ok → FN、ref=ok かつ ours=BAD → FP)。イメージは 0xA5 で埋めてから作る。
"""
import json
import os
import random
import shutil
import subprocess

from dt_common import Result, run, work_dir
from dt_mkfs import carve, cli_mkfs, make_img, mtools_extra, ref_fat32

MiB = 1 << 20
GiB = 1 << 30


def ref_e2(img):
    p = run(["e2fsck", "-fn", img])
    t = p.stdout.decode("utf-8", "replace")
    return p.returncode != 0 or "WARNING" in t, t


def dumpe2fs_features(img):
    p = run(["dumpe2fs", "-h", img])
    t = p.stdout.decode("utf-8", "replace")
    for line in t.splitlines():
        if line.startswith("Filesystem features:"):
            return set(line.split(":", 1)[1].split()), t
    return set(), t


def ext_extra_for(expect_in, expect_out):
    def extra(img):
        feats, t = dumpe2fs_features(img)
        miss = [f for f in expect_in if f not in feats]
        extra_f = [f for f in expect_out if f in feats]
        if miss or extra_f:
            return True, "features: missing %s unexpected %s (have %s)" % (miss, extra_f, sorted(feats))
        d = os.path.dirname(img)
        src = os.path.join(d, "payload.bin")
        with open(src, "wb") as f:
            f.write(bytes((i * 11 + 5) & 0xFF for i in range(900000)))
        cmds = ["mkdir /d1", "write %s /d1/payload.bin" % src, "write %s /top.bin" % src, "symlink /sl /d1/payload.bin", "mkdir /d1/d2", "rm /top.bin"]
        run(["debugfs", "-w", "-f", "-", img], input="\n".join(cmds).encode())
        b, t2 = ref_e2(img)
        if b:
            return True, "after debugfs writes: " + t2
        back = os.path.join(d, "back.bin")
        if os.path.exists(back):
            os.remove(back)
        run(["debugfs", "-R", "dump /d1/payload.bin %s" % back, img])
        if not os.path.exists(back) or open(back, "rb").read() != open(src, "rb").read():
            return True, "read-back of the written file differs"
        return False, "after debugfs writes: ok"
    return extra


def fat_extra(img):
    p = run(["7z", "l", img]) if shutil.which("7z") else None
    if p is not None and p.returncode != 0:
        return True, "7z cannot read the image: " + p.stdout.decode("utf-8", "replace")[:300]
    if shutil.which("mcopy") and shutil.which("mdir"):
        return mtools_extra(img)
    return False, "7z ok (mtools not installed)"


def ref_fat(img):
    return ref_fat32(img)


def ref_ntfs(img):
    p = run(["ntfsfix", "-n", img])
    t = p.stdout.decode("utf-8", "replace")
    return p.returncode != 0 or "successfully" not in t, t


def ntfs_extra(img):
    d = os.path.dirname(img)
    src = os.path.join(d, "payload.bin")
    with open(src, "wb") as f:
        f.write(bytes((i * 13 + 1) & 0xFF for i in range(400000)))
    p = run(["ntfsinfo", "-m", img])
    if p.returncode != 0:
        return True, "ntfsinfo failed: " + p.stdout.decode("utf-8", "replace")[:300]
    if shutil.which("ntfscp") and shutil.which("ntfscat"):
        for dest in ("/payload.bin", "/second.bin"):
            q = run(["ntfscp", "-f", img, src, dest])
            if q.returncode != 0:
                return True, "ntfscp failed: " + q.stdout.decode("utf-8", "replace")[:300]
        q = subprocess.run(["ntfscat", img, "/payload.bin"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if q.returncode != 0 or q.stdout != open(src, "rb").read():
            return True, "read-back of the written file differs"
    b, t = ref_ntfs(img)
    return b, "after ntfscp: " + t


def case(res, name, cli, d, size, garbage, fs, args, ref, extra=None):
    img = os.path.join(d, "m.img")
    make_img(img, size, garbage)
    c, j = cli_mkfs(cli, img, ["--fs", fs] + args)
    if c != 0:
        res.add(name, False, True, json.dumps(j.get("result", {}), ensure_ascii=False)[:300], [])
        return False
    rb, rt = ref(img)
    if extra and not rb:
        eb, et = extra(img)
        rb, rt = rb or eb, rt + "\n" + et
    res.add(name, rb, False, rt)
    return not rb


def expect_refused(res, name, cli, d, size, fs, args, code):
    """作れないはずの指定: 計画が不可 (終了コード 2) で、理由が code であること。外部ツールの出番はない。"""
    img = os.path.join(d, "r.img")
    make_img(img, size, False)
    c, j = cli_mkfs(cli, img, ["--fs", fs] + args)
    codes = [p.get("code") for p in j.get("plan", {}).get("problems", []) if p.get("severity") == "error"]
    ok = c == 2 and code in codes
    res.add(name, not ok, False, "rc=%s codes=%s expected %s" % (c, codes, code), [])


def caps_checks(cli, res, d):
    def caps(length=0, sector=512, cluster=0):
        p = subprocess.run([cli, "caps", str(length), str(sector), str(cluster)], stdout=subprocess.PIPE)
        return {c["fs"]: c for c in json.loads(p.stdout.decode())}
    c = caps()
    want = {"ext2", "ext3", "ext4", "fat12", "fat16", "fat32", "exfat", "ntfs"}
    res.add("caps/list", not want <= {k for k, v in c.items() if v["supported"]}, False, "fs=%s" % sorted(c), [])
    res.add("caps/ntfs experimental", not c["ntfs"]["experimental"], False, "", [])
    res.add("caps/ranges", any(int(v["minBytes"]) <= 0 or int(v["maxBytes"]) <= int(v["minBytes"]) or not v["clusterSizes"] for v in c.values()), False, "", [])
    c = caps(3 * MiB)
    res.add("caps/3MiB too small for ext4 and fat32", not (not c["ext4"]["sizeOk"] and c["ext4"]["reasonCode"] == "too-small" and not c["fat32"]["sizeOk"] and c["fat12"]["sizeOk"] and c["ntfs"]["sizeOk"]), False,
            "ext4=%s fat32=%s" % (c["ext4"]["reasonCode"], c["fat32"]["reasonCode"]), [])
    c = caps(2 * GiB)
    res.add("caps/2GiB fat12 too large", not (not c["fat12"]["sizeOk"] and c["fat12"]["reasonCode"] == "too-large" and c["fat16"]["sizeOk"] and c["ext4"]["sizeOk"]), False,
            "fat12=%s fat16=%s" % (c["fat12"]["reasonCode"], c["fat16"]["reasonCode"]), [])
    c = caps(8 * GiB)
    res.add("caps/8GiB fat16 too large", not (not c["fat16"]["sizeOk"] and c["fat32"]["sizeOk"]), False, "fat16=%s" % c["fat16"]["reasonCode"], [])


def run_mkfs2(cli, res: Result, quick=False):
    d = work_dir("mkfs2")
    if os.environ.get("USBMEM_MKFS_ONLY_RANDOM"):
        run_random(cli, res, d, int(os.environ.get("USBMEM_MKFS_RANDOM", "500")), int(os.environ.get("USBMEM_MKFS_RANDOM_SEED", "1")))
        return res
    caps_checks(cli, res, d)
    e4_in = ["extent", "flex_bg", "huge_file", "dir_nlink", "extra_isize", "metadata_csum", "has_journal", "sparse_super", "large_file", "ext_attr", "dir_index", "filetype"]
    # ---- ext4
    e4 = [("ext4/16MiB", 16 * MiB, True, []), ("ext4/64MiB label uuid", 64 * MiB, True, ["--label", "my ext4", "--uuid", "01234567-89ab-cdef-0123-456789abcdef", "--seed", "3"]),
          ("ext4/300MiB", 300 * MiB, False, []), ("ext4/4MiB min", 4 * MiB, True, [])]
    if not quick:
        e4 += [("ext4/32MiB b1k", 32 * MiB, True, ["--cluster", "1024"]), ("ext4/100MiB b2k", 100 * MiB, True, ["--cluster", "2048"]), ("ext4/1500MiB multi-group", 1500 * MiB, False, []),
               ("ext4/50MiB no resize_inode", 50 * MiB, True, ["--features", "^resize_inode"]), ("ext4/50MiB no journal", 50 * MiB, True, ["--features", "^has_journal"]),
               ("ext4/50MiB no metadata_csum", 50 * MiB, True, ["--features", "^metadata_csum"]), ("ext4/80MiB journal 8MiB", 80 * MiB, True, ["--journal-size", str(8 * MiB)]),
               ("ext4/60MiB ratio 4096", 60 * MiB, True, ["--ratio", "4096"]),
               ("ext4/50MiB odd size", 50 * MiB + 4321, True, []), ("ext4/6GiB sparse", 6 * GiB, False, []), ("ext4/70GiB sparse", 70 * GiB, False, []),
               ("ext4/3TiB sparse", 3 * 1024 * GiB, False, []), ("ext4/20TiB sparse 64bit", 20 * 1024 * GiB, False, [])]
    for name, size, garbage, args in e4:
        out = []
        feats_in = list(e4_in)
        if "^resize_inode" in args or size >= 16 * 1024 * GiB:
            out.append("resize_inode")   # 2^32 ブロック以上では mke2fs と同じく付けない
        else:
            feats_in.append("resize_inode")
        if "^has_journal" in args:
            feats_in.remove("has_journal")
            out.append("has_journal")
        if "^metadata_csum" in args:
            feats_in.remove("metadata_csum")
            out.append("metadata_csum")
        if size >= 20 * 1024 * GiB:
            feats_in.append("64bit")
        case(res, name, cli, d, size, garbage, "ext4", args, ref_e2, ext_extra_for(feats_in, out) if size <= 100 * GiB else None)
    # ---- ext3
    e3 = [("ext3/16MiB", 16 * MiB, True, []), ("ext3/64MiB label", 64 * MiB, True, ["--label", "my ext3", "--seed", "9"]), ("ext3/300MiB", 300 * MiB, False, [])]
    if not quick:
        e3 += [("ext3/4MiB min", 4 * MiB, True, []), ("ext3/32MiB b1k", 32 * MiB, True, ["--cluster", "1024"]), ("ext3/100MiB b2k", 100 * MiB, True, ["--cluster", "2048"]),
               ("ext3/1200MiB multi-group", 1200 * MiB, False, []), ("ext3/50MiB no journal", 50 * MiB, True, ["--features", "^has_journal"]), ("ext3/60MiB inode128", 60 * MiB, True, ["--inode-size", "128"]), ("ext3/6GiB sparse", 6 * GiB, False, [])]
    for name, size, garbage, args in e3:
        out = ["extent", "flex_bg", "metadata_csum", "64bit"]
        feats_in = ["has_journal", "sparse_super", "large_file", "ext_attr", "dir_index", "filetype", "resize_inode"]
        if "^has_journal" in args:
            feats_in.remove("has_journal")
            out.append("has_journal")
        case(res, name, cli, d, size, garbage, "ext3", args, ref_e2, ext_extra_for(feats_in, out))
    # ---- FAT16 / FAT12
    f16 = [("fat16/16MiB", 16 * MiB, True, []), ("fat16/64MiB label", 64 * MiB, True, ["--label", "mylabel", "--seed", "5"]), ("fat16/300MiB", 300 * MiB, False, [])]
    if not quick:
        f16 += [("fat16/3MiB min", 3 * MiB, True, []), ("fat16/32MiB c512", 32 * MiB, True, ["--cluster", "512"]), ("fat16/100MiB c4k", 100 * MiB, True, ["--cluster", "4096"]),
                ("fat16/500MiB c8k", 500 * MiB, False, ["--cluster", "8192"]), ("fat16/1GiB", 1 * GiB, False, []), ("fat16/2047MiB", 2047 * MiB, False, []),
                ("fat16/3GiB c64k", 3 * GiB, False, ["--cluster", "65536"]), ("fat16/100MiB sector4096", 100 * MiB, True, ["--sector", "4096"]), ("fat16/50MiB odd size", 50 * MiB + 777, True, [])]
    for name, size, garbage, args in f16:
        case(res, name, cli, d, size, garbage, "fat16", args, ref_fat, fat_extra if "sector4096" not in name else None)
    f12 = [("fat12/1MiB", 1 * MiB, True, []), ("fat12/1440KiB floppy", 1440 * 1024, True, ["--label", "FLOPPY"]), ("fat12/100KiB", 100 * 1024, True, [])]
    if not quick:
        f12 += [("fat12/64KiB min", 64 * 1024, True, []), ("fat12/8MiB", 8 * MiB, True, []), ("fat12/8MiB c4k", 8 * MiB, True, ["--cluster", "4096"]), ("fat12/200MiB c64k", 200 * MiB, False, ["--cluster", "65536"]),
                ("fat12/4MiB sector4096", 4 * MiB, True, ["--sector", "4096"]), ("fat12/2000KiB odd", 2000 * 1024 + 100, True, [])]
    for name, size, garbage, args in f12:
        case(res, name, cli, d, size, garbage, "fat12", args, ref_fat, fat_extra if "sector4096" not in name else None)
    # ---- NTFS
    nt = [("ntfs/64MiB", 64 * MiB, True, []), ("ntfs/100MiB label serial", 100 * MiB, True, ["--label", "NTFS test", "--seed", "11"]), ("ntfs/300MiB c64k", 300 * MiB, False, ["--cluster", "65536"])]
    if not quick:
        nt += [("ntfs/2MiB c512 min", 2 * MiB, True, ["--cluster", "512"]), ("ntfs/3MiB", 3 * MiB, True, []), ("ntfs/16MiB c1k", 16 * MiB, True, ["--cluster", "1024"]), ("ntfs/16MiB c2k", 16 * MiB, True, ["--cluster", "2048"]),
               ("ntfs/100MiB c8k", 100 * MiB, True, ["--cluster", "8192"]), ("ntfs/100MiB sector4096", 100 * MiB, True, ["--sector", "4096"]),
               ("ntfs/100MiB sector4096 c64k", 100 * MiB, True, ["--sector", "4096", "--cluster", "65536"]), ("ntfs/1GiB", 1 * GiB, False, []), ("ntfs/50MiB odd size", 50 * MiB + 1234, True, []),
               ("ntfs/64MiB label utf8", 64 * MiB, True, ["--label", "日本語ラベル"]), ("ntfs/2TiB sparse", 2 * 1024 * GiB, False, [])]
    for name, size, garbage, args in nt:
        case(res, name, cli, d, size, garbage, "ntfs", args, ref_ntfs, ntfs_extra)
    # ---- 拒否 (理由つき)
    expect_refused(res, "refuse/ext4 2MiB", cli, d, 2 * MiB, "ext4", [], "too-small")
    expect_refused(res, "refuse/fat16 1MiB", cli, d, 1 * MiB, "fat16", [], "too-small")
    expect_refused(res, "refuse/fat12 3GiB", cli, d, 3 * GiB, "fat12", [], "too-large")
    expect_refused(res, "refuse/fat16 8GiB", cli, d, 8 * GiB, "fat16", [], "too-large")
    expect_refused(res, "refuse/ntfs 1MiB", cli, d, 1 * MiB, "ntfs", [], "too-small")
    expect_refused(res, "refuse/ntfs c3000", cli, d, 64 * MiB, "ntfs", ["--cluster", "3000"], "bad-cluster-size")
    expect_refused(res, "refuse/ext4 inode128 with extra_isize", cli, d, 60 * MiB, "ext4", ["--inode-size", "128"], "bad-inode-size")
    expect_refused(res, "refuse/fat12 16MiB c4k", cli, d, 16 * MiB, "fat12", ["--cluster", "4096"], "too-large")
    expect_refused(res, "refuse/ext4 bad feature", cli, d, 32 * MiB, "ext4", ["--features", "no_such_feature"], "bad-features")
    expect_refused(res, "refuse/ext4 bad uuid", cli, d, 32 * MiB, "ext4", ["--uuid", "xyz"], "bad-uuid")
    # ---- 乱数
    n = int(os.environ.get("USBMEM_MKFS_RANDOM", "4" if quick else "12"))
    run_random(cli, res, d, n, int(os.environ.get("USBMEM_MKFS_RANDOM_SEED", "1")))
    return res


def run_random(cli, res, d, n, seed):
    """容量・クラスタ・ラベル・機能をばらまいて、クラッシュ・適用失敗・外部ツールの指摘がないことを見る (拒否は理由コードが妥当なら許容)"""
    rnd = random.Random(seed)
    refs = {"ext3": (ref_e2, None), "ext4": (ref_e2, None), "fat12": (ref_fat, None), "fat16": (ref_fat, None), "ntfs": (ref_ntfs, None)}
    fss = sorted(refs)
    allowed = {"too-small", "too-large", "bad-cluster-size", "bad-inode-size"}
    for i in range(n):
        fs = rnd.choice(fss)
        lo = {"ext3": 4 * MiB, "ext4": 4 * MiB, "fat12": 64 * 1024, "fat16": 2 * MiB, "ntfs": 2 * MiB}[fs]
        hi = {"ext3": 2 * GiB, "ext4": 4 * GiB, "fat12": 64 * MiB, "fat16": 2 * GiB, "ntfs": 4 * GiB}[fs]
        size = int(2 ** rnd.uniform(__import__("math").log2(lo), __import__("math").log2(hi)))
        size += rnd.choice([0, 0, rnd.randrange(0, 4096)])
        sector = rnd.choice([512, 512, 512, 4096])
        args = []
        if sector != 512:
            args += ["--sector", str(sector)]
        if rnd.random() < 0.5:
            cs = rnd.choice([512, 1024, 2048, 4096, 8192, 16384, 32768, 65536] if fs in ("fat12", "fat16", "ntfs") else [1024, 2048, 4096])
            args += ["--cluster", str(cs)]
        if rnd.random() < 0.4:
            lab = rnd.choice(["A", "mylabel", "VOL_1", "x" * 11, "日本語"] if fs == "ntfs" else ["A", "mylabel", "VOL_1", "DATA"])
            args += ["--label", lab]
        if rnd.random() < 0.3:
            args += ["--seed", str(rnd.randrange(1, 10 ** 6))]
        if fs == "ext4" and rnd.random() < 0.4:
            args += ["--features", rnd.choice(["^resize_inode", "^has_journal", "^metadata_csum", "^huge_file", "^dir_nlink", "^flex_bg,^metadata_csum"])]
        if fs.startswith("ext") and rnd.random() < 0.2:
            args += ["--inode-size", rnd.choice(["128", "256"])]
        name = "random/%d %s %d %s" % (i, fs, size, " ".join(args))
        img = os.path.join(d, "x.img")
        make_img(img, size, rnd.random() < 0.3 and size <= 64 * MiB)
        c, j = cli_mkfs(cli, img, ["--fs", fs] + args)
        if c == 0:
            rb, rt = refs[fs][0](img)
            res.add(name, rb, False, rt)
        elif c == 2:
            codes = {p.get("code") for p in j.get("plan", {}).get("problems", []) if p.get("severity") == "error"}
            res.add(name + " (refused: %s)" % ",".join(sorted(codes)), not (codes and codes <= allowed | {"bad-features"}), False, "refused for %s" % codes, [])
        else:
            res.add(name, True, False, "rc=%s %s" % (c, json.dumps(j.get("result", {}))[:300]), [])
    try:
        os.remove(os.path.join(d, "x.img"))
    except OSError:
        pass
