# SPDX-License-Identifier: Apache-2.0
"""mkfs (ext2 / exFAT / FAT32) の検証: 自前の mkfs で作ったイメージを外部ツール (検査のみ) で確かめる。

  FAT32: fsck.fat -n (終了 0 かつ警告なし) / mtools (mdir・mcopy で実際に書き込んでから fsck.fat -n)
  exFAT: fsck.exfat -n
  ext2 : e2fsck -fn (終了 0) / dumpe2fs / debugfs (ファイルを書いてから e2fsck -fn)
基準: 外部ツールが問題なし ⇔ 自前 mkfs が「作成して検証まで通った」。
  ref=BAD かつ ours=ok → FN (自前の mkfs と検査が見逃した。最重要)、ref=ok かつ ours=BAD → FP (自前が拒否した)。
イメージは 0xA5 で埋めてから作る (ゼロ前提に頼っていないことの確認)。大きいものはスパースでゼロ。
"""
import json
import os
import subprocess
import sys

from dt_common import Result, run, work_dir


def cli_mkfs(cli, img, args):
    p = subprocess.run([cli, "mkfs", img] + args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        j = json.loads(p.stdout.decode("utf-8", "replace"))
    except Exception:
        j = {"plan": {}, "result": {"ok": False, "message": p.stderr.decode("utf-8", "replace")}}
    return p.returncode, j


def make_img(path, size, garbage):
    with open(path, "wb") as f:
        if garbage:
            blk = b"\xA5" * (1 << 20)
            n = size
            while n > 0:
                f.write(blk[: min(n, len(blk))])
                n -= len(blk)
        else:
            f.truncate(size)


def ref_fat32(img, off=0, size=None):
    """戻り値 (問題あり?, 出力)"""
    target = img
    p = run(["fsck.fat", "-n", "-v", target] if off == 0 else ["fsck.fat", "-n", "-v", target])
    t = p.stdout.decode("utf-8", "replace")
    bad = p.returncode != 0 or any(k in t for k in ("Not auto-correcting", "differences between boot", "Not automatically fixing", "Dirty bit"))
    return bad, t


def ref_exfat(img):
    p = run(["fsck.exfat", "-n", img])
    t = p.stdout.decode("utf-8", "replace")
    return p.returncode != 0 or "ERROR" in t or "(y/N)" in t, t


def ref_ext(img):
    p = run(["e2fsck", "-fn", img])
    t = p.stdout.decode("utf-8", "replace")
    return p.returncode != 0, t


def carve(img, off, length, out):
    """[off, off+length) を別ファイルへ切り出す (外部ツールはオフセット付きを扱えないものがあるため)"""
    with open(img, "rb") as f, open(out, "wb") as g:
        f.seek(off)
        n = length
        while n > 0:
            b = f.read(min(n, 1 << 20))
            if not b:
                break
            g.write(b)
            n -= len(b)
    return out


def case(res, name, cli, d, size, garbage, fs, args, ref, extra=None, off=0):
    img = os.path.join(d, "m.img")
    make_img(img, size, garbage)
    a = ["--fs", fs] + args
    if off:
        a += ["--range", str(off), str(size - off)]
    c, j = cli_mkfs(cli, img, a)
    ours_bad = c != 0
    txt = json.dumps(j.get("result", {}), ensure_ascii=False)[:300]
    target = img
    if off and not ours_bad:
        target = carve(img, off, size - off, os.path.join(d, "c.img"))
    if ours_bad:
        # 自前が拒否: 外部ツールの基準は「正常な FS ができる」ので、拒否は FP とする (ref は問題なし扱い)
        res.add(name, False, True, txt, [])
        return None
    rb, rt = ref(target)
    if extra and not rb:
        eb, et = extra(target)
        rb = rb or eb
        rt = rt + "\n" + et
    res.add(name, rb, False, rt)
    return target


def mtools_extra(img):
    env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    p = subprocess.run(["mdir", "-i", img, "::"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    if p.returncode != 0:
        return True, "mdir failed: " + p.stdout.decode("utf-8", "replace")
    src = os.path.join(os.path.dirname(img), "payload.bin")
    with open(src, "wb") as f:
        f.write(bytes((i * 7 + 3) & 0xFF for i in range(300000)))
    for cmd in (["mmd", "-i", img, "::/sub"], ["mcopy", "-i", img, src, "::/sub/payload.bin"], ["mcopy", "-i", img, src, "::/Long File Name.bin"]):
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
        if p.returncode != 0:
            return True, " ".join(cmd) + " failed: " + p.stdout.decode("utf-8", "replace")
    out = os.path.join(os.path.dirname(img), "back.bin")
    p = subprocess.run(["mcopy", "-n", "-i", img, "::/sub/payload.bin", out], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    if p.returncode != 0 or open(out, "rb").read() != open(src, "rb").read():
        return True, "read-back of the copied file differs"
    b, t = ref_fat32(img)
    return b, "after mcopy: " + t


def ext_extra(img):
    src = os.path.join(os.path.dirname(img), "payload.bin")
    with open(src, "wb") as f:
        f.write(bytes((i * 11 + 5) & 0xFF for i in range(700000)))
    cmds = ["mkdir /d1", "write %s /d1/payload.bin" % src, "write %s /top.bin" % src, "symlink /sl /d1/payload.bin", "mkdir /d1/d2"]
    p = subprocess.run(["debugfs", "-w", "-f", "-", img], input="\n".join(cmds).encode(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    t = p.stdout.decode("utf-8", "replace")
    b, t2 = ref_ext(img)
    if b:
        return True, "after debugfs writes: " + t2
    q = subprocess.run(["debugfs", "-R", "dump /d1/payload.bin %s" % os.path.join(os.path.dirname(img), "back.bin"), img], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if open(os.path.join(os.path.dirname(img), "back.bin"), "rb").read() != open(src, "rb").read():
        return True, "read-back of the written file differs"
    # 追記後に我々の検査も通ること
    return False, "after debugfs writes: " + t2


def ext_dump(img):
    b, t = ref_ext(img)
    p = run(["dumpe2fs", "-h", img])
    return b, t + p.stdout.decode("utf-8", "replace")[:600]


def run_mkfs(cli, res: Result, quick=False):
    d = work_dir("mkfs")
    MiB = 1 << 20
    # ---- FAT32
    f32 = [("fat32/40MiB", 40 * MiB, True, []), ("fat32/64MiB label", 64 * MiB, True, ["--label", "mylabel", "--seed", "12345"]),
           ("fat32/300MiB c4k", 300 * MiB, False, ["--label", "DATA_1"]), ("fat32/600MiB c8k", 600 * MiB, False, ["--cluster", "8192"])]
    if not quick:
        f32 += [("fat32/1100MiB sparse", 1100 * MiB, False, []), ("fat32/33MiB min", 34 * MiB, True, []), ("fat32/70MiB odd size", 70 * MiB + 777, True, []),
                ("fat32/300MiB sector4096", 300 * MiB, False, ["--sector", "4096"]), ("fat32/5GiB c64k sparse", 5120 * MiB, False, ["--cluster", "65536"])]
    for name, size, garbage, args in f32:
        case(res, name, cli, d, size, garbage, "fat32", args, ref_fat32, mtools_extra if "sector4096" not in name else None)
    if not quick:
        case(res, "fat32/offset 1MiB", cli, d, 80 * MiB, True, "fat32", ["--label", "PART"], ref_fat32, mtools_extra, off=MiB)
    # ---- exFAT
    ex = [("exfat/24MiB", 24 * MiB, True, []), ("exfat/64MiB label", 64 * MiB, True, ["--label", "ExFAT Vol", "--seed", "99"]),
          ("exfat/300MiB", 300 * MiB, False, [])]
    if not quick:
        ex += [("exfat/8MiB c512", 8 * MiB, True, ["--cluster", "512"]), ("exfat/2MiB small", 2 * MiB, True, []), ("exfat/200MiB sector4096", 200 * MiB, False, ["--sector", "4096"]),
               ("exfat/1100MiB sparse", 1100 * MiB, False, []), ("exfat/100MiB odd size", 100 * MiB + 1234, True, []),
               ("exfat/64MiB label utf8", 64 * MiB, True, ["--label", "日本語ラベル"]), ("exfat/100MiB c1M", 100 * MiB, True, ["--cluster", str(MiB)])]
    for name, size, garbage, args in ex:
        case(res, name, cli, d, size, garbage, "exfat", args, ref_exfat)
    if not quick:
        case(res, "exfat/offset 1MiB", cli, d, 80 * MiB, True, "exfat", [], ref_exfat, off=MiB)
    # ---- ext2
    e2 = [("ext2/2MiB", 2 * MiB, True, []), ("ext2/20MiB label", 20 * MiB, True, ["--label", "mylabel", "--seed", "7"]), ("ext2/130MiB multi-group", 130 * MiB, False, []),
          ("ext2/600MiB b4k", 600 * MiB, False, [])]
    if not quick:
        e2 += [("ext2/256KiB min", 256 * 1024, True, []), ("ext2/20MiB b2k", 20 * MiB, True, ["--cluster", "2048"]), ("ext2/20MiB inode128", 20 * MiB, True, ["--inode-size", "128"]),
               ("ext2/50MiB ratio4096", 50 * MiB, True, ["--ratio", "4096"]), ("ext2/50MiB ratio65536", 50 * MiB, True, ["--ratio", "65536"]),
               ("ext2/50MiB odd size", 50 * MiB + 3333, True, []), ("ext2/70MiB lastgroup tiny", 64 * MiB + 40 * 1024, True, []),
               ("ext2/300MiB b4k forced", 300 * MiB, False, ["--cluster", "4096"]), ("ext2/2200MiB sparse b4k", 2200 * MiB, False, [])]
    for name, size, garbage, args in e2:
        case(res, name, cli, d, size, garbage, "ext2", args, ref_ext, ext_extra if size <= 700 * MiB and size >= 2 * MiB else ext_dump)
    if not quick:
        case(res, "ext2/offset 1MiB", cli, d, 60 * MiB, True, "ext2", ["--label", "PART"], ref_ext, ext_extra, off=MiB)
    return res
