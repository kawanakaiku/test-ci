#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""削除ファイルの一覧と救出 (core の undelete) を、本物の mkfs.* が作った FS で検証する。

  - FAT12/16/32・exFAT: mkfs.fat / mkfs.exfat の空の FS へ、tools/ud_fslib.py でファイルを書いて削除する (fsck.fat -n / fsck.exfat -n で前後を確かめる)。
    mtools (mcopy / mdel) があれば、その経路でも FAT を作って確かめる。
  - ext2/3/4: mke2fs -d で作り、debugfs -w の rm で削除する (e2fsck -fn で確かめる)。
  - NTFS: mkntfs + ntfs-3g (FUSE) で書き、Windows の削除と同じ変更 (MFT レコードの使用中フラグを落とす・$Bitmap を空ける) を直接施す。
    ntfsundelete があれば、同じイメージで候補の数を比べる (info。失敗にはしない)。
確かめること: 削除したファイルが一覧に出る / 抽出したバイト列の SHA-256 が元と一致する / 一致しないもの (上書きされた)
が「確度 高・中」になっていない (誤って復元できると言わない) / 上書きされたものは確度 低 / 生きているファイルは出ない。
使い方: check_undelete_tools.py <usbmem_undelete_cli>   (道具が無ければ SKIP = 終了コード 77)
"""
import hashlib
import json
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ud_fslib  # noqa: E402

CLI = os.path.abspath(sys.argv[1])
ENV = dict(os.environ, PATH=os.environ.get("PATH", "") + ":/sbin:/usr/sbin:/usr/local/sbin")
WORK = tempfile.mkdtemp(prefix="usbmem_ud_", dir="/dev/shm" if os.path.isdir("/dev/shm") else None)
fails = 0
checks = 0
infos = []


def which(t):
    return shutil.which(t, path=ENV["PATH"])


def check(cond, msg):
    global fails, checks
    checks += 1
    if not cond:
        fails += 1
        print("  FAIL:", msg, flush=True)
    return bool(cond)


def run(cmd, **kw):
    kw.setdefault("env", ENV)
    kw.setdefault("capture_output", True)
    kw.setdefault("timeout", 600)
    return subprocess.run(cmd, **kw)


def sha(b):
    return hashlib.sha256(b).hexdigest()


# ------------------------------------------------------------------------------------------------ 内容
def rnd(seed, n):
    r = random.Random(seed)
    return bytes(r.getrandbits(8) for _ in range(n)) if n < 4096 else r.randbytes(n)


def text(seed, n):
    r = random.Random(seed)
    words = ["alpha", "beta", "gamma", "delta", "report", "deleted", "usb", "memory", "日本語", "recover"]
    out = b""
    while len(out) < n:
        out += (" ".join(r.choice(words) for _ in range(12)) + "\n").encode("utf-8")
    return out[:n]


def jpeg(seed, n):
    return b"\xff\xd8\xff\xe0\x00\x10JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00" + rnd(seed, n - 20) + b"\xff\xd9"


def pdf(seed, n):
    return b"%PDF-1.4\n" + rnd(seed, n - 20) + b"\n%%EOF\n"


def png(seed, n):
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    ihdr = struct.pack(">IIBBBBB", 16, 16, 8, 2, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", rnd(seed, n - 60)) + chunk(b"IEND", b"")


def make_set():
    """(パス, 内容, 削除するか, FAT の NT 小文字フラグ)"""
    return [
        ("/README.TXT", text(1, 3000), True, 0),
        ("/notes.txt", text(2, 5000), True, 0x18),
        ("/Holiday Photo 2024.jpg", jpeg(3, 200 * 1024), True, 0),
        ("/日本語ファイル.txt", text(4, 70000), True, 0),
        ("/docs/report final.pdf", pdf(5, 300 * 1024), True, 0),
        ("/docs/sub/data.bin", rnd(6, 1500 * 1024), True, 0),
        ("/docs/sub/tiny.txt", text(7, 100), True, 0),
        ("/keep/alive.jpg", jpeg(8, 50 * 1024), False, 0),
        ("/keep/other.png", png(9, 20 * 1024), False, 0),
    ]


# ------------------------------------------------------------------------------------------------ CLI
def cli_list(img):
    p = run([CLI, "list", img])
    try:
        return json.loads(p.stdout.decode("utf-8", "replace"))
    except Exception:
        return {"error": p.stderr.decode("utf-8", "replace"), "files": []}


def cli_extract(img, outdir):
    os.makedirs(outdir, exist_ok=True)
    p = run([CLI, "extract", img, outdir])
    j = json.loads(p.stdout.decode())
    res = {}
    for r in j["results"]:
        with open(os.path.join(outdir, str(r["id"])), "rb") as f:
            res[r["id"]] = (r["status"], sha(f.read()), r["sha256"])
    return res


def match_listing(files, path, partial_ok):
    """真のパスに対応する一覧の項目 (先頭文字が失われた 8.3 名は先頭を _ にして比べる)"""
    for f in files:
        if f["path"] == path:
            return f
    comps = path.split("/")
    for f in files:
        fc = f["path"].split("/")
        if len(fc) == len(comps) and fc[:-1] == comps[:-1] and f["namePartial"] and fc[-1][1:].lower() == comps[-1][1:].lower() and len(fc[-1]) == len(comps[-1]):
            return f
    return None


def verify(tag, img, truth, live, expect_low=(), expect_found=None, allow_missing=()):
    """truth: 削除したファイル パス → 内容。live: 生きているファイルのパス。"""
    lst = cli_list(img)
    check("error" not in lst, "%s: list failed %s" % (tag, lst.get("error")))
    files = lst.get("files", [])
    ext = cli_extract(img, os.path.join(WORK, "out_" + tag))
    found = 0
    for path, data in truth.items():
        f = match_listing(files, path, True)
        if f is None:
            if path in allow_missing:
                continue
            check(False, "%s: deleted file not listed: %s" % (tag, path))
            continue
        found += 1
        st, h, h2 = ext.get(f["id"], (None, None, None))
        same = h == sha(data)
        if path in expect_low:
            check(f["level"] == "low", "%s: overwritten file %s must be low (got %s %s)" % (tag, path, f["level"], f["notes"]))
            check(f["reusedBlocks"] > 0, "%s: overwritten file %s must report reused blocks" % (tag, path))
        else:
            check(same, "%s: %s extracted sha mismatch (level %s, notes %s)" % (tag, path, f["level"], f["notes"]))
            check(f["level"] == "high", "%s: intact file %s should be high (got %s conf %s notes %s)" % (tag, path, f["level"], f["confidence"], f["notes"]))
            check(f["size"] == len(data), "%s: %s size %s != %s" % (tag, path, f["size"], len(data)))
        # 安全: 内容が違うのに「高・中」は許さない
        if not same and f["level"] in ("high", "medium"):
            check(False, "%s: WRONG CLAIM: %s differs from original but level=%s" % (tag, path, f["level"]))
        if f["level"] != "none":
            check(h == h2 or h2 == h, "%s: sha mismatch between list and extract" % tag)
    # 生きているファイルが出ていない
    for lv in live:
        check(not any(f["path"] == lv for f in files), "%s: live file listed: %s" % (tag, lv))
    nfound = sum(1 for p in truth if match_listing(files, p, True))
    print("  %-26s deleted=%d listed=%d levels=%s" % (tag, len(truth), nfound, ",".join(sorted({f["level"] for f in files}))), flush=True)
    return lst


# ------------------------------------------------------------------------------------------------ FAT
def mk_blank(path, mb):
    with open(path, "wb") as f:
        f.truncate(mb * 1024 * 1024)


def test_fat(bits, size_mb, extra_args=(), use_reuse=True):
    if not which("mkfs.fat"):
        print("  skip FAT (no mkfs.fat)")
        return
    tag = "fat%d" % bits
    img = os.path.join(WORK, tag + ".img")
    mk_blank(img, size_mb)
    p = run(["mkfs.fat", "-F", str(bits), "-n", "UDTEST"] + list(extra_args) + [img])
    if p.returncode != 0:
        print("  skip %s: mkfs.fat failed: %s" % (tag, p.stderr.decode()[:200]))
        return
    fs = ud_fslib.Fat(img)
    truth, live = {}, []
    for path, data, delete, nt in make_set():
        if bits == 12 and len(data) > 1200 * 1024:
            data = data[:600 * 1024]
        fs.add_file(path, data, nt=nt)
        if delete:
            truth[path] = data
        else:
            live.append(path)
    fs.flush()
    fs.close()
    if which("fsck.fat"):
        p = run(["fsck.fat", "-n", img])
        check(p.returncode == 0, "%s: fsck.fat -n before delete: %s" % (tag, p.stdout.decode()[-300:]))
    fs = ud_fslib.Fat(img)
    # ディレクトリの構造を読み直す (tree はメモリだけ)
    fs.tree = {}
    _rebuild_fat_tree(fs)
    for path in list(truth):
        if path.startswith("/docs/") and path.count("/") > 1:
            fs.delete(path)
    for path in list(truth):
        if not (path.startswith("/docs/") and path.count("/") > 1):
            fs.delete(path)
    fs.close()
    if which("fsck.fat"):
        p = run(["fsck.fat", "-n", img])
        check(p.returncode == 0, "%s: fsck.fat -n after delete: %s" % (tag, p.stdout.decode()[-300:]))
    lst0 = verify(tag, img, truth, live)
    if bits == 16:   # ディレクトリ自体も削除 (中のファイルは「親も削除済み」になる)
        fs = ud_fslib.Fat(img)
        fs.tree = {}
        _rebuild_fat_tree(fs)
        fs.delete("/docs/sub")
        fs.delete("/docs")
        fs.close()
        lst1 = verify(tag + "_deldir", img, truth, live)
        indel = [f["path"] for f in lst1["files"] if f["inDeletedDir"]]
        check(any("data.bin" in p for p in indel) and any("report" in p for p in indel), "%s: files in deleted directories should be marked inDeletedDir (%s)" % (tag, indel))
    # 上書き: 新しいファイル (live) が削除済みの先頭を再利用する
    if use_reuse:
        fs = ud_fslib.Fat(img)
        fs.tree = {}
        _rebuild_fat_tree(fs)
        newdata = rnd(77, 100 * 1024 if bits != 12 else 60 * 1024)
        fs.add_file("/new.bin", newdata)
        fs.close()
        if which("fsck.fat"):
            p = run(["fsck.fat", "-n", img])
            check(p.returncode == 0, "%s: fsck.fat -n after overwrite" % tag)
        # first-fit: 一番若いクラスタの削除済みデータが上書きされる。誰が上書きされたかはクラスタの範囲から求める
        lst = cli_list(img)
        victims = [f["path"] for f in lst["files"] if f["reusedBlocks"] > 0]
        check(len(victims) >= 1, "%s: nothing reported as overwritten after writing a new file" % tag)
        verify(tag + "_reuse", img, truth, live + ["/new.bin"], expect_low=_victim_paths(lst, truth))
    os.unlink(img)


def _victim_paths(lst, truth):
    out = []
    for f in lst["files"]:
        if f["reusedBlocks"] > 0:
            for p in truth:
                m = match_listing([f], p, True)
                if m is not None:
                    out.append(p)
    return out


def _rebuild_fat_tree(fs):
    """既存のイメージの生きているディレクトリを fs.tree に登録する (パス → 先頭クラスタ)"""
    def walk(dirc, prefix):
        for o in fs._dir_slots(dirc):
            e = bytes(fs.m[o:o + 32])
            if e[0] == 0:
                break
            if e[0] == 0xE5 or e[11] & 0x3F == 0x0F or not (e[11] & 0x10) or e[0] == 0x2E:
                continue
            first = ud_fslib.u16(e, 26) | (ud_fslib.u16(e, 20) << 16 if fs.bits == 32 else 0)
            lfn = []
            # 名前: 直前の LFN を集める
            idx = fs._dir_slots(dirc).index(o)
            sl = fs._dir_slots(dirc)
            k = idx - 1
            parts = []
            while k >= 0 and fs.m[sl[k] + 11] & 0x3F == 0x0F and fs.m[sl[k]] != 0xE5:
                le = bytes(fs.m[sl[k]:sl[k] + 32])
                parts.append(le[1:11] + le[14:26] + le[28:32])
                k -= 1
            if parts:
                name = b"".join(parts).decode("utf-16-le").split("\x00")[0].split("￿")[0]
            else:
                b = e[0:8].decode("latin-1").rstrip(" ")
                x = e[8:11].decode("latin-1").rstrip(" ")
                name = b + ("." + x if x else "")
            path = prefix + (name,)
            fs.tree[path] = first
            walk(first, path)
    walk(None, ())


# ------------------------------------------------------------------------------------------------ exFAT
def test_exfat():
    if not which("mkfs.exfat"):
        print("  skip exFAT (no mkfs.exfat)")
        return
    for mode in ("nofat", "fatchain"):
        tag = "exfat_" + mode
        img = os.path.join(WORK, tag + ".img")
        mk_blank(img, 40)
        p = run(["mkfs.exfat", "-L", "UDTEST", img])
        if p.returncode != 0:
            print("  skip exFAT: mkfs.exfat failed")
            return
        fs = ud_fslib.ExFat(img)
        truth, live = {}, []
        for path, data, delete, nt in make_set():
            fs.add_file(path, data, nofat=(mode == "nofat"))
            (truth.__setitem__(path, data) if delete else live.append(path))
        fs.add_file("/vdl.bin", rnd(31, 30000), vdl=10000, nofat=True)  # ValidDataLength が DataLength より小さい
        fs.close()
        if which("fsck.exfat"):
            p = run(["fsck.exfat", "-n", img])
            check(p.returncode == 0, "%s: fsck.exfat before delete: %s" % (tag, (p.stdout + p.stderr).decode()[-300:]))
        fs = ud_fslib.ExFat(img)
        _rebuild_exfat_tree(fs)
        vdl_expect = rnd(31, 30000)[:10000] + bytes(20000)
        for path in truth:
            fs.delete(path)
        fs.delete("/vdl.bin")
        fs.close()
        if which("fsck.exfat"):
            p = run(["fsck.exfat", "-n", img])
            check(p.returncode == 0, "%s: fsck.exfat after delete: %s" % (tag, (p.stdout + p.stderr).decode()[-300:]))
        truth2 = dict(truth)
        truth2["/vdl.bin"] = vdl_expect
        verify(tag, img, truth2, live)
        # 上書き
        fs = ud_fslib.ExFat(img)
        _rebuild_exfat_tree(fs)
        fs.add_file("/new.bin", rnd(78, 100 * 1024), nofat=True)
        fs.close()
        lst = cli_list(img)
        check(any(f["reusedBlocks"] > 0 for f in lst["files"]), "%s: nothing reported as overwritten" % tag)
        verify(tag + "_reuse", img, truth2, live + ["/new.bin"], expect_low=_victim_paths(lst, truth2))
        os.unlink(img)


def _rebuild_exfat_tree(fs):
    def walk(parts, clusters):
        slots = fs._chain_slots(0, clusters)
        for i, o in enumerate(slots):
            if fs.m[o] == 0x00:
                break
            if fs.m[o] != 0x85 or not (ud_fslib.u16(fs.m, o + 4) & 0x10):
                continue
            nl = fs.m[slots[i + 1] + 3]
            nm = b"".join(bytes(fs.m[slots[i + 2 + k] + 2:slots[i + 2 + k] + 32]) for k in range((nl + 14) // 15))
            name = nm[:nl * 2].decode("utf-16-le")
            first = ud_fslib.u32(fs.m, slots[i + 1] + 20)
            ch = fs._chain(first)
            p = tuple(parts) + (name,)
            fs.tree[p] = {"clusters": ch}
            walk(p, ch)
    walk((), fs._chain(fs.root))


# ------------------------------------------------------------------------------------------------ ext
def test_ext(kind, size_mb=40):
    if not (which("mke2fs") and which("debugfs")):
        print("  skip ext (no mke2fs / debugfs)")
        return
    tag = kind
    img = os.path.join(WORK, tag + ".img")
    src = os.path.join(WORK, tag + "_src")
    shutil.rmtree(src, ignore_errors=True)
    truth, live = {}, []
    for path, data, delete, nt in make_set():
        fp = src + path
        os.makedirs(os.path.dirname(fp), exist_ok=True)
        with open(fp, "wb") as f:
            f.write(data)
        (truth.__setitem__(path, data) if delete else live.append(path))
    mk_blank(img, size_mb)
    opts = {"ext2": ["-t", "ext2"], "ext3": ["-t", "ext3"], "ext4": ["-t", "ext4"]}[kind]
    p = run(["mke2fs", "-q", "-F", "-d", src] + opts + [img])
    check(p.returncode == 0, "%s: mke2fs failed %s" % (tag, p.stderr.decode()[-200:]))
    shutil.rmtree(src, ignore_errors=True)
    if which("e2fsck"):
        p = run(["e2fsck", "-fn", img])
        check(p.returncode == 0, "%s: e2fsck before delete" % tag)
    cmds = "\n".join('rm "%s"' % path for path in truth) + "\n"
    p = run(["debugfs", "-w", "-f", "-", img], input=cmds.encode())
    if which("e2fsck"):
        p = run(["e2fsck", "-fn", img])
        check(p.returncode == 0, "%s: e2fsck after delete: %s" % (tag, p.stdout.decode()[-300:]))
    # 名前は debugfs の rm が前のエントリの余白に inode 番号つきで残す (最初のエントリは inode がゼロになる)。名前が無いものは inode_N
    lst = cli_list(img)
    files = lst.get("files", [])
    ext = cli_extract(img, os.path.join(WORK, "out_" + tag))
    by_sha = {sha(d): pth for pth, d in truth.items()}
    got = set()
    for f in files:
        st, h, h2 = ext.get(f["id"], (None, None, None))
        if h in by_sha:
            got.add(by_sha[h])
            check(f["level"] == "high", "%s: intact %s should be high (got %s %s)" % (tag, by_sha[h], f["level"], f["notes"]))
            check(f["path"] == by_sha[h] or f["name"].startswith("inode_") or f["path"].endswith("/" + f["name"]), "%s: path mismatch %s vs %s" % (tag, f["path"], by_sha[h]))
        elif f["level"] in ("high", "medium"):
            check(False, "%s: WRONG CLAIM %s level %s" % (tag, f["path"], f["level"]))
    named = sum(1 for f in files if not f["name"].startswith("inode_"))
    check(len(got) == len(truth), "%s: recovered %d of %d (sha-verified); levels %s" % (tag, len(got), len(truth), [(f["path"], f["level"], f["size"]) for f in files]))
    print("  %-26s deleted=%d recovered=%d named=%d" % (tag, len(truth), len(got), named), flush=True)
    # 上書き: debugfs で新しいファイルを書くと空きブロックを再利用する
    newp = os.path.join(WORK, "newfile.bin")
    with open(newp, "wb") as f:
        f.write(rnd(79, 30 * 1024 * 1024))
    run(["debugfs", "-w", "-R", "write %s new.bin" % newp, img])
    if which("e2fsck"):
        p = run(["e2fsck", "-fn", img])
        check(p.returncode == 0, "%s: e2fsck after overwrite" % tag)
    lst = cli_list(img)
    files = lst.get("files", [])
    ext = cli_extract(img, os.path.join(WORK, "out2_" + tag))
    over = 0
    for f in files:
        st, h, h2 = ext.get(f["id"], (None, None, None))
        if h not in by_sha:
            check(f["level"] not in ("high", "medium"), "%s: WRONG CLAIM after overwrite: %s level %s" % (tag, f["path"], f["level"]))
            over += 1
            check(f["level"] == "low", "%s: overwritten %s not low" % (tag, f["path"]))
        else:
            check(f["level"] == "high", "%s: untouched %s should stay high" % (tag, by_sha[h]))
    check(over >= 1, "%s: expected at least one overwritten deleted file" % tag)
    os.unlink(img)


# ------------------------------------------------------------------------------------------------ NTFS
class Ntfs:
    """ntfs-3g が書いたイメージに、Windows の削除と同じ変更を施す最小の道具"""

    def __init__(self, path):
        self.f = open(path, "r+b")
        b = self.f.read(512)
        self.bps = ud_fslib.u16(b, 11); self.spc = b[13]; self.cb = self.bps * self.spc
        self.mft_lcn = struct.unpack_from("<Q", b, 48)[0]
        cpr = struct.unpack_from("<b", b, 64)[0]
        self.rs = cpr * self.cb if cpr > 0 else 1 << (-cpr)
        r0 = self._rec_at(self.mft_lcn * self.cb)
        self.mft_runs = self._data_runs(r0, 0x80, None)
        self.bitmap_runs = self._data_runs(self.read_rec(6), 0x80, None)
        self.mftbm_runs = self._data_runs(r0, 0xB0, None)

    def _rec_at(self, off):
        self.f.seek(off)
        r = bytearray(self.f.read(self.rs))
        self._fix(r, True)
        return r

    @staticmethod
    def _fixdesc(r):
        return ud_fslib.u16(r, 4), ud_fslib.u16(r, 6)

    def _fix(self, r, undo):
        uo, uc = self._fixdesc(r)
        for i in range(1, uc):
            pos = i * 512 - 2
            if undo:
                r[pos:pos + 2] = r[uo + 2 * i:uo + 2 * i + 2]
            else:
                r[uo + 2 * i:uo + 2 * i + 2] = r[pos:pos + 2]
                r[pos:pos + 2] = r[uo:uo + 2]

    @staticmethod
    def attrs(r):
        o = ud_fslib.u16(r, 20)
        while o + 8 <= len(r):
            t = ud_fslib.u32(r, o)
            if t == 0xFFFFFFFF:
                return
            l = ud_fslib.u32(r, o + 4)
            yield o, t, l
            o += l

    @staticmethod
    def runlist(r, a):
        ro = ud_fslib.u16(r, a + 32)
        o = a + ro
        lcn = 0
        out = []
        while True:
            h = r[o]
            if h == 0:
                break
            lb, ob = h & 15, h >> 4
            ln = int.from_bytes(r[o + 1:o + 1 + lb], "little")
            off = int.from_bytes(r[o + 1 + lb:o + 1 + lb + ob], "little", signed=True) if ob else None
            o += 1 + lb + ob
            if off is not None:
                lcn += off
            out.append((lcn if off is not None else None, ln))
        return out

    def _data_runs(self, r, typ, _):
        for o, t, l in self.attrs(r):
            if t == typ and r[o + 8]:
                return self.runlist(r, o)
        return []

    def _vcn_to_off(self, runs, off):
        pos = 0
        for lcn, ln in runs:
            span = ln * self.cb
            if off < pos + span:
                return lcn * self.cb + (off - pos), pos + span - off
            pos += span
        raise IndexError

    def rec_off(self, n):
        return self._vcn_to_off(self.mft_runs, n * self.rs)[0]

    def read_rec(self, n):
        return self._rec_at(self.rec_off(n))

    def write_rec(self, n, r):
        self._fix(r, False)
        self.f.seek(self.rec_off(n))
        self.f.write(bytes(r))

    def find(self, name):
        n = 24
        while True:
            try:
                r = self.read_rec(n)
            except IndexError:
                return None
            if r[0:4] == b"FILE" and ud_fslib.u16(r, 22) & 1 and not ud_fslib.u16(r, 22) & 2:
                for o, t, l in self.attrs(r):
                    if t == 0x30 and not r[o + 8]:
                        vo = ud_fslib.u16(r, o + 20)
                        nl = r[o + vo + 64]
                        nm = r[o + vo + 66:o + vo + 66 + 2 * nl].decode("utf-16-le")
                        if nm == name and r[o + vo + 65] != 2:
                            return n
            n += 1

    def _clear_bits(self, runs, first, count):
        for i in range(first, first + count):
            off, _ = self._vcn_to_off(runs, i // 8)
            self.f.seek(off)
            b = self.f.read(1)[0]
            self.f.seek(off)
            self.f.write(bytes([b & ~(1 << (i % 8)) & 0xFF]))

    def delete(self, n):
        r = self.read_rec(n)
        for o, t, l in self.attrs(r):
            if t == 0x80 and r[o + 8] and r[o + 9] == 0:
                for lcn, ln in self.runlist(r, o):
                    if lcn is not None:
                        self._clear_bits(self.bitmap_runs, lcn, ln)
        flags = ud_fslib.u16(r, 22)
        struct.pack_into("<H", r, 22, flags & ~1)
        struct.pack_into("<H", r, 16, (ud_fslib.u16(r, 16) + 1) & 0xFFFF)
        self.write_rec(n, r)
        self._clear_bits(self.mftbm_runs, n, 1)

    def close(self):
        self.f.close()


def mount_ntfs(img, mnt):
    os.makedirs(mnt, exist_ok=True)
    p = run(["ntfs-3g", "-o", "rw", img, mnt])
    return p.returncode == 0


def umount(mnt):
    run(["fusermount3", "-u", mnt])
    for _ in range(20):
        if not os.path.ismount(mnt):
            break
        import time
        time.sleep(0.2)


def test_ntfs():
    if not (which("mkntfs") and which("ntfs-3g") and which("fusermount3")):
        print("  skip NTFS (mkntfs / ntfs-3g / fusermount3 missing)")
        return
    tag = "ntfs"
    img = os.path.join(WORK, tag + ".img")
    mnt = os.path.join(WORK, "mnt")
    mk_blank(img, 48)
    p = run(["mkntfs", "-F", "-Q", "-q", "-s", "512", "-c", "4096", "-L", "UDTEST", img])
    if p.returncode != 0:
        print("  skip NTFS: mkntfs failed")
        return
    if not mount_ntfs(img, mnt):
        print("  skip NTFS: ntfs-3g mount failed (FUSE unavailable?)")
        return
    truth, live, names = {}, [], {}
    try:
        for path, data, delete, nt in make_set():
            fp = mnt + path
            os.makedirs(os.path.dirname(fp), exist_ok=True)
            with open(fp, "wb") as f:
                f.write(data)
            (truth.__setitem__(path, data) if delete else live.append(path))
        # resident になる小さなファイル (MFT レコードの中に収まる)
        with open(mnt + "/tiny_resident.txt", "wb") as f:
            f.write(b"resident data " * 5)
        truth["/tiny_resident.txt"] = b"resident data " * 5
        # 空のファイル・スパースファイル
        with open(mnt + "/sparse.bin", "wb") as f:
            f.truncate(300000)
            f.seek(250000)
            f.write(b"tail-data")
        sp = bytearray(300000)
        sp[250000:250009] = b"tail-data"
        truth["/sparse.bin"] = bytes(sp)
    finally:
        umount(mnt)
    n = Ntfs(img)
    recs = {}
    for path in truth:
        recs[path] = n.find(os.path.basename(path))
        check(recs[path] is not None, "ntfs: MFT record of %s not found" % path)
    for path, rn in recs.items():
        if rn is not None:
            n.delete(rn)
    n.close()
    if which("ntfsfix"):
        pass
    lst = verify(tag, img, truth, live)
    resident = [f for f in lst["files"] if f["resident"]]
    check(len(resident) >= 1, "ntfs: a resident deleted file should be listed")
    # 参考: ntfsundelete が見つける候補の数 (info)
    if which("ntfsundelete"):
        p = run(["ntfsundelete", "-s", img])
        txt = (p.stdout + p.stderr).decode("utf-8", "replace")
        infos.append("ntfsundelete -s reports: " + " | ".join(l.strip() for l in txt.splitlines() if "file" in l.lower())[:300] + " ; ours listed %d" % len(lst["files"]))
    # 上書き: 新しいファイルを書くと空いたクラスタを使う (マウントし直す。ntfs-3g は削除済みの MFT レコードも再利用する)
    if mount_ntfs(img, mnt):
        try:
            with open(mnt + "/new.bin", "wb") as f:
                f.write(rnd(80, 400 * 1024))
        finally:
            umount(mnt)
        lst = cli_list(img)
        ext = cli_extract(img, os.path.join(WORK, "out2_ntfs"))
        by_sha = {sha(d): p for p, d in truth.items()}
        over = 0
        for f in lst["files"]:
            st, h, h2 = ext.get(f["id"], (None, None, None))
            if f["level"] == "none":
                continue
            if h not in by_sha:
                over += 1
                check(f["level"] == "low", "ntfs: overwritten %s should be low (got %s)" % (f["path"], f["level"]))
            else:
                check(f["level"] == "high", "ntfs: untouched %s should be high" % by_sha[h])
        print("  %-26s after overwrite: overwritten=%d" % (tag + "_reuse", over), flush=True)
    os.unlink(img)


def main():
    need = [which("mkfs.fat"), which("mke2fs"), which("mkfs.exfat"), which("mkntfs")]
    if not any(need):
        print("SKIP: no mkfs tools")
        return 77
    print("FAT")
    test_fat(16, 16, ["-s", "4"])
    test_fat(12, 4, ["-s", "2"])
    test_fat(32, 40, ["-s", "2"])
    print("exFAT")
    test_exfat()
    print("ext")
    for k in ("ext2", "ext3", "ext4"):
        test_ext(k)
    print("NTFS")
    test_ntfs()
    for i in infos:
        print("info:", i)
    print("== undelete: %d checks, %d failed" % (checks, fails))
    return 1 if fails else 0


if __name__ == "__main__":
    try:
        rc = main()
    finally:
        shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(rc)
