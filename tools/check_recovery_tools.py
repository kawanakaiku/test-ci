#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""データ救出 (ddrescue 方式) とバックアップ (Partclone 方式) を、本物のツールと突き合わせて検証する。

  - ddrescue: LD_PRELOAD で「不良セクタを含む読み取りは EIO」にして、ddrescue と usbmem の最終 mapfile・出力が一致することを確かめる。
              ddrescue ↔ usbmem で途中から引き継ぐ (どちら向きも) ことも確かめる。
  - partclone: mke2fs / mkfs.fat / mkfs.exfat / mkntfs で作ったイメージ (と既存の実フィクスチャ) について、
              使用中ビットマップ・画像の内容を partclone と比べ、相互に復元して fsck する。lz4 コマンドとも往復する。
使い方: check_recovery_tools.py <usbmem_recovery_cli>   (SKIP は終了コード 77)
"""
import hashlib
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

CLI = os.path.abspath(sys.argv[1])
QUICK = os.environ.get("USBMEM_RB_QUICK") == "1"
ENV = dict(os.environ, PATH=os.environ.get("PATH", "") + ":/sbin:/usr/sbin:/usr/local/sbin")
fails = 0
checks = 0


def which(t):
    return shutil.which(t, path=ENV["PATH"])


def check(cond, msg):
    global fails, checks
    checks += 1
    if not cond:
        fails += 1
        print("  FAIL:", msg, flush=True)
    return cond


def run(cmd, **kw):
    kw.setdefault("env", ENV)
    kw.setdefault("capture_output", True)
    kw.setdefault("timeout", 1800)
    return subprocess.run(cmd, **kw)


def jline(out):
    import json
    for ln in out.decode("utf-8", "replace").splitlines():
        ln = ln.strip()
        if ln.startswith("{"):
            try:
                return json.loads(ln)
            except ValueError:
                pass
    return {}


def last_json(out):
    import json
    lines = [ln for ln in out.decode("utf-8", "replace").splitlines() if ln.strip().startswith("{")]
    return json.loads(lines[-1]) if lines else {}


def parse_map(path):
    """GNU ddrescue の mapfile を [(pos, size, status)] にする (隣り合う同じ状態は併合)"""
    blocks = []
    first = True
    for ln in open(path, encoding="utf-8"):
        ln = ln.strip()
        if not ln or ln.startswith("#"):
            continue
        if first:
            first = False
            continue
        pos, size, st = ln.split()
        pos, size = int(pos, 0), int(size, 0)
        if blocks and blocks[-1][2] == st and blocks[-1][0] + blocks[-1][1] == pos:
            blocks[-1] = (blocks[-1][0], blocks[-1][1] + size, st)
        else:
            blocks.append((pos, size, st))
    return blocks


def read_pc(path):
    d = open(path, "rb").read()
    assert d[:15] == b"partclone-image", "not partclone"
    info = dict(fs=d[0x24:0x34].split(b"\0")[0].decode(), device_size=struct.unpack_from("<Q", d, 0x34)[0],
                total=struct.unpack_from("<Q", d, 0x3C)[0], used=struct.unpack_from("<Q", d, 0x44)[0],
                bs=struct.unpack_from("<I", d, 0x54)[0], bpc=struct.unpack_from("<I", d, 0x64)[0])
    nb = (info["total"] + 7) // 8
    info["bits"] = d[0x6E:0x6E + nb]
    info["data_off"] = 0x6E + nb + 4
    info["raw"] = d
    return info


def bit(bits, i):
    return (bits[i >> 3] >> (i & 7)) & 1


def used_blocks_equal(a_path, b_path, bits, bs, total, upto=None):
    """a と b の使用中ブロックの内容が一致するか (b が短ければ不一致)"""
    with open(a_path, "rb") as fa, open(b_path, "rb") as fb:
        for blk in range(total):
            if not bit(bits, blk):
                continue
            fa.seek(blk * bs)
            fb.seek(blk * bs)
            x = fa.read(bs)
            y = fb.read(bs)
            if x != y:
                return blk
    return -1


# ================================================================================================ ddrescue
SHIM_C = r"""
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
static int tracked = -1;
static unsigned long long R[512][2];
static int NR = -1;
static void init(void) {
  if (NR >= 0) return;
  NR = 0;
  const char* s = getenv("FAULT_RANGES");
  while (s && *s && NR < 512) {
    unsigned long long a, b; int n = 0;
    if (sscanf(s, "%llu:%llu%n", &a, &b, &n) < 2) break;
    R[NR][0] = a; R[NR][1] = b; NR++;
    s += n; if (*s == ',') s++;
  }
}
static int hits(unsigned long long off, size_t n) {
  init();
  for (int i = 0; i < NR; i++) if (off < R[i][0] + R[i][1] && R[i][0] < off + n) return 1;
  return 0;
}
static int track(const char* p, int fd) {
  const char* t = getenv("FAULT_PATH");
  if (fd >= 0 && t && p && !strcmp(p, t)) tracked = fd;
  return fd;
}
#define DEFOPEN(NAME) \
int NAME(const char* p, int flags, ...) { \
  static int (*real)(const char*, int, ...); if (!real) real = dlsym(RTLD_NEXT, #NAME); \
  mode_t m = 0; if (flags & O_CREAT) { va_list ap; va_start(ap, flags); m = va_arg(ap, mode_t); va_end(ap); } \
  return track(p, real(p, flags, m)); }
DEFOPEN(open)
DEFOPEN(open64)
int __open_2(const char* p, int flags) { static int (*real)(const char*, int); if (!real) real = dlsym(RTLD_NEXT, "__open_2"); return track(p, real(p, flags)); }
int __open64_2(const char* p, int flags) { static int (*real)(const char*, int); if (!real) real = dlsym(RTLD_NEXT, "__open64_2"); return track(p, real(p, flags)); }
int close(int fd) { static int (*real)(int); if (!real) real = dlsym(RTLD_NEXT, "close"); if (fd == tracked) tracked = -1; return real(fd); }
ssize_t pread(int fd, void* b, size_t n, off_t off) {
  static ssize_t (*real)(int, void*, size_t, off_t); if (!real) real = dlsym(RTLD_NEXT, "pread");
  if (fd == tracked && hits((unsigned long long)off, n)) { errno = EIO; return -1; }
  return real(fd, b, n, off);
}
ssize_t pread64(int fd, void* b, size_t n, off_t off) {
  static ssize_t (*real)(int, void*, size_t, off_t); if (!real) real = dlsym(RTLD_NEXT, "pread64");
  if (fd == tracked && hits((unsigned long long)off, n)) { errno = EIO; return -1; }
  return real(fd, b, n, off);
}
ssize_t read(int fd, void* b, size_t n) {
  static ssize_t (*real)(int, void*, size_t); if (!real) real = dlsym(RTLD_NEXT, "read");
  if (fd == tracked) { off_t o = lseek(fd, 0, SEEK_CUR); if (o >= 0 && hits((unsigned long long)o, n)) { errno = EIO; return -1; } }
  return real(fd, b, n);
}
"""


def section_ddrescue(tmp):
    if not which("ddrescue") or not which("cc"):
        print("SKIP ddrescue section (ddrescue or cc missing)")
        return
    print("== ddrescue", flush=True)
    d = os.path.join(tmp, "dd")
    os.mkdir(d)
    src_c = os.path.join(d, "shim.c")
    open(src_c, "w").write(SHIM_C)
    shim = os.path.join(d, "shim.so")
    r = run(["cc", "-shared", "-fPIC", "-O1", "-o", shim, src_c, "-ldl"])
    if not check(r.returncode == 0, "building the LD_PRELOAD shim: " + r.stderr.decode()[:300]):
        return
    nsec = 4096 if QUICK else 16384
    rnd = random.Random(7)
    img = os.path.join(d, "in.img")
    with open(img, "wb") as f:
        f.write(bytes(rnd.getrandbits(8) for _ in range(512 * 64)) * (nsec // 64))
    # 不良セクタ: 先頭、単発、帯、2 つ隣り合う帯、末尾
    faults = [(0, 1024), (100 * 512, 512), (1500 * 512, 70 * 512), (2000 * 512, 512), (2001 * 512, 512), (3000 * 512, 3 * 512), ((nsec - 3) * 512, 1024)]
    fr = ",".join("%d:%d" % x for x in faults)
    cli_faults = []
    for a, b in faults:
        cli_faults += ["--fault", "%d:%d" % (a, b)]

    def run_dd(out, mp, extra=(), ranges=fr):
        env = dict(ENV, LD_PRELOAD=shim, FAULT_PATH=img, FAULT_RANGES=ranges)
        return run(["ddrescue", "-b", "512", "-f", *extra, img, out, mp], env=env)

    def run_cli(out, mp, extra=()):
        return run([CLI, "rescue", img, out, mp, *cli_faults, *extra])

    def pad(path, size):
        with open(path, "ab") as f:
            f.truncate(max(size, os.path.getsize(path)))

    for name, dd_extra, cli_extra in [("default", [], []), ("retries1", ["-r1"], ["--retries", "1"])]:
        o1, m1, o2, m2 = [os.path.join(d, name + s) for s in (".dd.out", ".dd.map", ".us.out", ".us.map")]
        run_dd(o1, m1, dd_extra)
        r = run_cli(o2, m2, cli_extra)
        j = jline(r.stdout)
        check(j.get("inputWrites", 1) == 0, name + ": the input device was written to")
        pad(o1, nsec * 512)
        bm1, bm2 = parse_map(m1), parse_map(m2)
        check(bm1 == bm2, "%s: final mapfile differs\n   ddrescue: %s\n   usbmem:   %s" % (name, bm1[:12], bm2[:12]))
        check(open(o1, "rb").read() == open(o2, "rb").read(), name + ": rescued output differs")
        print("   %s: %d map blocks, bad=%d bytes" % (name, len(bm2), sum(s for _, s, st in bm2 if st == "-")), flush=True)

    # 引き継ぎ: usbmem が途中で止まり、ddrescue が続ける
    ref_out, ref_map = os.path.join(d, "default.dd.out"), os.path.join(d, "default.dd.map")
    for cut in (5, 40):
        o, mp = os.path.join(d, "x%d.out" % cut), os.path.join(d, "x%d.map" % cut)
        r = run_cli(o, mp, ["--max-reads", str(cut)])
        j = jline(r.stdout)
        check(j.get("stop") == 1 or j.get("stop") == 0, "partial run produced a result")
        mid = parse_map(mp)
        check(any(st == "?" for _, _, st in mid), "the interrupted run left unread areas (cut=%d)" % cut)
        run_dd(o, mp)
        pad(o, nsec * 512)
        check(parse_map(mp) == parse_map(ref_map), "usbmem -> ddrescue hand-over gives a different map (cut=%d)" % cut)
        check(open(o, "rb").read() == open(ref_out, "rb").read(), "usbmem -> ddrescue hand-over gives different data (cut=%d)" % cut)
    # ddrescue が途中まで (-s 1 MiB)、usbmem が続ける
    o, mp = os.path.join(d, "y.out"), os.path.join(d, "y.map")
    run_dd(o, mp, ["-s", "1M"])
    pad(o, nsec * 512)
    check(any(st == "?" for _, _, st in parse_map(mp)), "ddrescue -s left unread areas")
    r = run([CLI, "rescue", img, o, mp, *cli_faults])
    check(parse_map(mp) == parse_map(ref_map), "ddrescue -> usbmem hand-over gives a different map")
    check(open(o, "rb").read() == open(ref_out, "rb").read(), "ddrescue -> usbmem hand-over gives different data")
    # 同じファイル (シンボリックリンク経由も) は拒否
    link = os.path.join(d, "link.img")
    os.symlink(img, link)
    r = run([CLI, "rescue", img, link, os.path.join(d, "z.map")])
    check(r.returncode == 4 and b"same-file" in r.stdout, "rescue into the input (via symlink) must be refused")
    check(open(img, "rb").read(512 * 64) == open(img, "rb").read(512 * 64), "input intact")


# ================================================================================================ partclone
def make_tree(root, rnd, big=True):
    os.makedirs(os.path.join(root, "d1", "d2"), exist_ok=True)
    sizes = [0, 1, 511, 512, 4095, 4096, 4097, 100000]
    if big:
        sizes += [3 << 20]
    for i, n in enumerate(sizes):
        with open(os.path.join(root, "d1", "f%d.bin" % i), "wb") as f:
            f.write(bytes(rnd.getrandbits(8) for _ in range(min(n, 4096))) * (n // 4096 + 1) if n else b"")
            f.truncate(n)
    for i in range(40):
        with open(os.path.join(root, "d1", "d2", "s%02d.txt" % i), "w") as f:
            f.write("file %d\n" % i * (i + 1))


def mkimg(path, size_mb):
    with open(path, "wb") as f:
        f.truncate(size_mb << 20)


def build_images(tmp):
    """(名前, パス, partclone のツール名, fsck コマンド) の一覧"""
    rnd = random.Random(3)
    tree = os.path.join(tmp, "tree")
    make_tree(tree, rnd, big=not QUICK)
    sz = 12 if QUICK else 40
    out = []

    def add(name, path, tool, fsck):
        out.append((name, path, tool, fsck))

    if which("mke2fs"):
        for name, opts in [("ext4", ["-t", "ext4", "-b", "4096"]), ("ext3", ["-t", "ext3", "-b", "1024"]), ("ext2", ["-t", "ext2", "-b", "2048"]),
                           ("ext4-nojournal", ["-t", "ext4", "-O", "^has_journal", "-b", "4096"])]:
            p = os.path.join(tmp, name + ".img")
            mkimg(p, sz)
            r = run(["mke2fs", "-q", "-F", *opts, "-d", tree, p])
            if r.returncode == 0:
                add(name, p, "extfs", ["e2fsck", "-fn"])
    if which("mkfs.fat") and which("mcopy"):
        for name, bits, mb, extra in [("fat32", "32", 64, ["-s", "1"]), ("fat16", "16", 24, []), ("fat12", "12", 8, [])]:
            if QUICK and name == "fat32":
                mb = 40
            p = os.path.join(tmp, name + ".img")
            mkimg(p, mb)
            r = run(["mkfs.fat", "-F", bits, *extra, p])
            if r.returncode != 0:
                continue
            entries = [os.path.join(tree, "d1")]
            run(["mcopy", "-s", "-i", p, *entries, "::"])
            add(name, p, "fat", ["fsck.fat", "-n"])
    if which("mkfs.exfat"):
        p = os.path.join(tmp, "exfat.img")
        mkimg(p, sz)
        if run(["mkfs.exfat", p]).returncode == 0:
            add("exfat-empty", p, "exfat", ["fsck.exfat", "-n"])
    if which("mkntfs"):
        p = os.path.join(tmp, "ntfs.img")
        mkimg(p, sz)
        if run(["mkntfs", "-F", "-Q", "-q", p]).returncode == 0:
            add("ntfs-empty", p, "ntfs", None)
    # 既存の実フィクスチャ (他のテストが作ったもの。あれば使う。読み取りだけ)
    user = os.environ.get("USER", "u")
    for sub, names, tool, fsck in [("exfat", ["c4k.img", "s4k_c32k.img"], "exfat", ["fsck.exfat", "-n"]), ("fat", ["fat32_c4k.img", "fat32_s4k.img", "fat16_c2k.img"], "fat", ["fsck.fat", "-n"]),
                                   ("ext4", ["e4_def4k.img", "e4_def1k.img", "e4_metabg4k.img"], "extfs", ["e2fsck", "-fn"])]:
        for n in names[:1 if QUICK else 3]:
            p = "/dev/shm/usbmem_%s_%s/%s" % (sub, user, n)
            if os.path.exists(p):
                add("fixture-" + n, p, tool, fsck)
    return out


def section_partclone(tmp):
    if not which("partclone.extfs"):
        print("SKIP partclone section (partclone missing)")
        return
    print("== partclone", flush=True)
    d = os.path.join(tmp, "pc")
    os.mkdir(d)
    log = os.path.join(d, "pc.log")
    imgs = build_images(tmp)
    for name, img, tool, fsck in imgs:
        print("  -", name, flush=True)
        ref = os.path.join(d, name + ".ref.pc")
        r = run(["partclone." + tool, "-c", "-q", "-L", log, "-s", img, "-o", ref])
        if not check(r.returncode == 0, "%s: partclone could not read the image: %s" % (name, (r.stdout + r.stderr).decode()[-200:])):
            continue
        rp = read_pc(ref)
        # 1. 使用中ビットマップ
        bits_file = os.path.join(d, name + ".bits")
        r = run([CLI, "bits", img, bits_file])
        j = jline(r.stdout)
        if not check(j.get("ok"), "%s: usbmem could not build the used map: %s" % (name, r.stdout.decode()[:200])):
            continue
        ours = open(bits_file, "rb").read()
        check(j["blockSize"] == rp["bs"] and j["totalBlocks"] == rp["total"], "%s: geometry differs (usbmem %s/%s vs partclone %s/%s)" % (name, j["blockSize"], j["totalBlocks"], rp["bs"], rp["total"]))
        extra = [i for i in range(rp["total"]) if bit(ours, i) and not bit(rp["bits"], i)]
        missing = [i for i in range(rp["total"]) if bit(rp["bits"], i) and not bit(ours, i)]
        check(not missing, "%s: usbmem misses %d blocks partclone saves (first %s)" % (name, len(missing), missing[:5]))
        check(not extra, "%s: usbmem saves %d blocks partclone does not (first %s)" % (name, len(extra), extra[:5]))
        # 2. usbmem の画像
        mine = os.path.join(d, name + ".us.pc")
        r = run([CLI, "backup", img, mine])
        j = jline(r.stdout)
        if not check(j.get("ok"), "%s: usbmem backup failed: %s" % (name, r.stdout.decode()[:300])):
            continue
        mp = read_pc(mine)
        if not missing and not extra:
            a, b = rp["raw"], mp["raw"]
            same_tail = a[0x6E:] == b[0x6E:]
            check(same_tail, "%s: bitmap + data of the two images are not byte-identical" % name)
            check(a[0x24:0x44] == b[0x24:0x44] and a[0x54:0x6A] == b[0x54:0x6A], "%s: header fields differ" % name)
        r = run(["partclone.chkimg", "-L", log, "-s", mine])
        check(r.returncode == 0, "%s: partclone.chkimg rejects the usbmem image: %s" % (name, (r.stdout + r.stderr).decode()[-200:]))
        r = run(["partclone.info", "-L", log, "-s", mine])
        txt = (r.stdout + r.stderr).decode()
        check(r.returncode == 0 and ("Block size:   %d Byte" % mp["bs"]) in txt, "%s: partclone.info cannot read the usbmem image" % name)
        # 3. partclone.restore が usbmem の画像を復元 → 使用中ブロックが一致し fsck が通る
        rest = os.path.join(d, name + ".pc-restored")
        r = run(["partclone.restore", "-C", "-L", log, "-O", rest, "-s", mine])
        if check(r.returncode == 0, "%s: partclone.restore failed on the usbmem image: %s" % (name, (r.stdout + r.stderr).decode()[-200:])):
            os.chmod(rest, 0o600)  # partclone.restore は権限 000 で作る
            bad = used_blocks_equal(img, rest, mp["bits"], mp["bs"], mp["total"])
            check(bad < 0, "%s: partclone-restored copy differs at block %d" % (name, bad))
            if fsck and which(fsck[0]):
                with open(rest, "ab") as f:
                    f.truncate(os.path.getsize(img))
                r = run([*fsck, rest])
                check(r.returncode == 0, "%s: %s on the restored copy: %s" % (name, fsck[0], (r.stdout + r.stderr).decode()[-300:]))
        # 4. usbmem が partclone の画像を復元
        tgt = os.path.join(d, name + ".us-restored")
        mkimg(tgt, (rp["device_size"] + (1 << 20) - 1) >> 20)
        r = run([CLI, "restore", ref, tgt])
        rj = last_json(r.stdout)
        if check(r.returncode == 0 and rj.get("ok") and rj.get("verified"), "%s: usbmem could not restore the partclone image: %s" % (name, r.stdout.decode()[-300:])):
            bad = used_blocks_equal(img, tgt, rp["bits"], rp["bs"], rp["total"])
            check(bad < 0, "%s: usbmem-restored copy differs at block %d" % (name, bad))
            if fsck and which(fsck[0]):
                with open(tgt, "ab") as f:
                    f.truncate(os.path.getsize(img))
                r = run([*fsck, tgt])
                check(r.returncode == 0, "%s: %s on the usbmem-restored copy: %s" % (name, fsck[0], (r.stdout + r.stderr).decode()[-300:]))
        # 5. lz4 (lz4 コマンドと往復)
        if which("lz4"):
            z = os.path.join(d, name + ".us.pc.lz4")
            r = run([CLI, "backup", img, z, "--lz4"])
            if check(jline(r.stdout).get("ok"), "%s: usbmem lz4 backup failed" % name):
                r = run(["lz4", "-dc", z])
                check(r.returncode == 0 and r.stdout == mp["raw"], "%s: `lz4 -d` of the usbmem output differs from the plain image" % name)
            for variant in (["-1"], ["-9", "-BD"], ["-B4", "-BX"], ["-4", "--no-frame-crc"], ["-9", "-B5", "-BD", "--content-size"]):
                e = os.path.join(d, name + ".ext.lz4")
                with open(e, "wb") as f:
                    rr = subprocess.run(["lz4", "-c", "-q", "-f", *variant, ref], stdout=f, stderr=subprocess.PIPE, env=ENV)
                if rr.returncode != 0:
                    continue
                r = run([CLI, "info", e, "--verify"])
                check(jline(r.stdout).get("ok"), "%s: usbmem cannot read `lz4 %s` output: %s" % (name, " ".join(variant), r.stdout.decode()[:200]))
        # 6. 改ざんの検出 (partclone と usbmem の両方が拒否する)
        bad_img = os.path.join(d, name + ".bad.pc")
        raw = bytearray(mp["raw"])
        if len(raw) > mp["data_off"] + 100:
            pos = mp["data_off"] + 50 + (len(raw) - mp["data_off"]) // 2 % 100000
            raw[pos] ^= 0x04
            open(bad_img, "wb").write(raw)
            check(run(["partclone.chkimg", "-L", log, "-s", bad_img]).returncode != 0, "%s: partclone.chkimg accepted a corrupted image" % name)
            check(not jline(run([CLI, "info", bad_img, "--verify"]).stdout).get("ok"), "%s: usbmem accepted a corrupted image" % name)
            tg = os.path.join(d, name + ".guard")
            mkimg(tg, (rp["device_size"] + (1 << 20) - 1) >> 20)
            before = hashlib.sha256(open(tg, "rb").read()).hexdigest()
            r = run([CLI, "restore", bad_img, tg])
            check(r.returncode != 0 and hashlib.sha256(open(tg, "rb").read()).hexdigest() == before, "%s: a corrupted image touched the target" % name)
        # 7. 使用中のターゲットは拒否
        tg = os.path.join(d, name + ".busy")
        mkimg(tg, (rp["device_size"] + (1 << 20) - 1) >> 20)
        r = run([CLI, "restore", mine, tg, "--in-use"])
        check(r.returncode != 0 and b"target-in-use" in r.stdout, "%s: restore into an in-use target must be refused" % name)
        for f in (rest, tgt):
            if os.path.exists(f):
                os.unlink(f)


def main():
    if not os.path.exists(CLI):
        print("CLI not found:", CLI)
        return 2
    if not (which("ddrescue") or which("partclone.extfs")):
        print("SKIP: neither ddrescue nor partclone is installed")
        return 77
    base = "/dev/shm" if os.path.isdir("/dev/shm") and os.access("/dev/shm", os.W_OK) else None
    tmp = tempfile.mkdtemp(prefix="usbmem_rb_", dir=base)
    try:
        section_ddrescue(tmp)
        section_partclone(tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("%d checks, %d failed" % (checks, fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
