# SPDX-License-Identifier: Apache-2.0
"""FAT12/16/32 の検査の検証: 実イメージ (mkfs.fat + mtools) を壊して fsck.fat -n と自前の検出を比べる。"""
import os
import random
import shutil
import struct

from dt_common import Result, codes, ours, run, work_dir


class Fat:
    def __init__(self, data):
        self.d = bytearray(data)
        d = self.d
        self.bps = struct.unpack_from("<H", d, 11)[0]
        self.spc = d[13]
        self.rsvd = struct.unpack_from("<H", d, 14)[0]
        self.nfats = d[16]
        self.root_ent = struct.unpack_from("<H", d, 17)[0]
        tot = struct.unpack_from("<H", d, 19)[0] or struct.unpack_from("<I", d, 32)[0]
        fsz = struct.unpack_from("<H", d, 22)[0]
        self.fat32 = fsz == 0
        self.fsz = fsz or struct.unpack_from("<I", d, 36)[0]
        self.root_secs = (self.root_ent * 32 + self.bps - 1) // self.bps
        self.data_start = self.rsvd + self.nfats * self.fsz + self.root_secs
        self.clusters = (tot - self.data_start) // self.spc
        self.bits = 12 if self.clusters < 4085 else 16 if self.clusters < 65525 else 32
        self.cb = self.spc * self.bps
        self.root_clus = struct.unpack_from("<I", d, 44)[0] if self.fat32 else 0

    def fat_off(self, copy=0):
        return (self.rsvd + copy * self.fsz) * self.bps

    def get(self, cl, copy=0):
        o = self.fat_off(copy)
        if self.bits == 32:
            return struct.unpack_from("<I", self.d, o + 4 * cl)[0] & 0x0FFFFFFF
        if self.bits == 16:
            return struct.unpack_from("<H", self.d, o + 2 * cl)[0]
        w = struct.unpack_from("<H", self.d, o + cl + cl // 2)[0]
        return (w >> 4) if cl & 1 else (w & 0xFFF)

    def set(self, cl, v, copies=None):
        for c in (copies if copies is not None else range(self.nfats)):
            o = self.fat_off(c)
            if self.bits == 32:
                old = struct.unpack_from("<I", self.d, o + 4 * cl)[0]
                struct.pack_into("<I", self.d, o + 4 * cl, (old & 0xF0000000) | (v & 0x0FFFFFFF))
            elif self.bits == 16:
                struct.pack_into("<H", self.d, o + 2 * cl, v & 0xFFFF)
            else:
                w = struct.unpack_from("<H", self.d, o + cl + cl // 2)[0]
                w = ((w & 0x000F) | ((v & 0xFFF) << 4)) if cl & 1 else ((w & 0xF000) | (v & 0xFFF))
                struct.pack_into("<H", self.d, o + cl + cl // 2, w)

    def eoc(self):
        return {12: 0xFF8, 16: 0xFFF8, 32: 0x0FFFFFF8}[self.bits]

    def chain(self, cl):
        out = []
        while 2 <= cl < self.clusters + 2 and len(out) < 100000:
            out.append(cl)
            cl = self.get(cl)
            if cl >= self.eoc() or cl == 0:
                break
        return out

    def clus_off(self, cl):
        return (self.data_start + (cl - 2) * self.spc) * self.bps

    def dir_bytes_offsets(self, start):
        """ディレクトリのエントリが並ぶ (ファイル内オフセット) 一覧"""
        offs = []
        if start == 0 and not self.fat32:
            base = (self.rsvd + self.nfats * self.fsz) * self.bps
            for i in range(self.root_ent):
                offs.append(base + 32 * i)
        else:
            for cl in self.chain(start if start else self.root_clus):
                for i in range(self.cb // 32):
                    offs.append(self.clus_off(cl) + 32 * i)
        return offs

    def walk(self, start=0, path=""):
        """(path, entry_offset, cluster, size, is_dir, name83) を再帰列挙 (LFN は読み飛ばす)"""
        res = []
        for o in self.dir_bytes_offsets(start):
            e = self.d[o:o + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5 or (e[11] & 0x3F) == 0x0F or (e[11] & 0x08):
                continue
            name = bytes(e[0:8]).decode("latin1").rstrip() + ("." + bytes(e[8:11]).decode("latin1").rstrip() if e[8:11] != b"   " else "")
            cl = struct.unpack_from("<H", e, 26)[0] | (struct.unpack_from("<H", e, 20)[0] << 16 if self.fat32 else 0)
            size = struct.unpack_from("<I", e, 28)[0]
            isd = bool(e[11] & 0x10)
            if name in (".", ".."):
                res.append((path + "/" + name, o, cl, size, isd, name))
                continue
            res.append((path + "/" + name, o, cl, size, isd, name))
            if isd and cl:
                res += self.walk(cl, path + "/" + name)
        return res


def make_image(path, bits, mb, spc=None, nfats=2, populate=True, extra=()):
    with open(path, "wb") as f:
        f.truncate(mb * 1024 * 1024)
    cmd = ["mkfs.fat", "-F", str(bits), "-n", "TESTVOL", "-f", str(nfats)]
    if spc:
        cmd += ["-s", str(spc)]
    cmd += list(extra) + [path]
    p = run(cmd)
    assert p.returncode == 0, p.stdout
    if not populate:
        return
    tmp = path + ".src"
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp + "/dir1/sub1/sub2")
    os.makedirs(tmp + "/dir2")
    rnd = random.Random(7)
    def wr(rel, n):
        with open(os.path.join(tmp, rel), "wb") as f:
            f.write(rnd.randbytes(n))
    wr("small.txt", 100)
    wr("Long File Name With Spaces.dat", 5000)
    wr("dir1/a.bin", 70000)
    wr("dir1/sub1/b.bin", 300000)
    wr("dir1/sub1/sub2/deep.txt", 10)
    wr("dir2/c.bin", 1234)
    wr("dir2/日本語ファイル.txt", 777)
    for i in range(30):
        wr("dir2/file_with_a_long_name_%02d.txt" % i, 50 + i)
    wr("big.bin", min(mb * 1024 * 1024 // 4, 3 * 1024 * 1024))
    env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    p = run(["mcopy", "-s", "-Q", "-i", path] + [os.path.join(tmp, n) for n in sorted(os.listdir(tmp))] + ["::"], env=env)
    assert p.returncode == 0, p.stdout
    shutil.rmtree(tmp, ignore_errors=True)


def fsck(path):
    p = run(["fsck.fat", "-n", path])
    txt = p.stdout.decode("utf-8", "replace")
    # 修復しない (-n) と、問題を表示しても終了コード 0 の項目がある。表示された問題も「問題あり」とする
    said = any(k in txt for k in ("Not auto-correcting", "differences between boot sector", "Not automatically fixing"))
    return p.returncode != 0 or said, txt


def mutations(f):
    """(名前, 関数(Fat), 期待される差の許容 or None) の一覧。f は元イメージの Fat (関数内で破壊的に変える)"""
    M = []
    ent = [e for e in f.walk() if not e[5].startswith(".")]
    files = [e for e in ent if not e[4] and e[3] > 0]
    dirs = [e for e in ent if e[4] and e[2]]
    big = max(files, key=lambda e: e[3])
    mid = [e for e in files if 2 * f.cb < e[3]][0] if any(2 * f.cb < e[3] for e in files) else big
    other = [e for e in files if e is not big][0]

    def m(name, allow=None):
        def deco(fn):
            M.append((name, fn, allow))
            return fn
        return deco

    @m("bad_boot_signature", allow=("fp",))
    def _(x): x.d[510] = 0
    @m("bad_bytes_per_sector")
    def _(x): struct.pack_into("<H", x.d, 11, 777)
    @m("bad_spc")
    def _(x): x.d[13] = 3
    @m("zero_fats")
    def _(x): x.d[16] = 0
    @m("zero_reserved")
    def _(x): struct.pack_into("<H", x.d, 14, 0)
    @m("fat_chain_free")
    def _(x):
        c = x.chain(big[2]); x.set(c[1], 0)
    @m("fat_chain_loop")
    def _(x):
        c = x.chain(big[2]); x.set(c[-1], c[0])
    @m("fat_chain_loop_mid")
    def _(x):
        c = x.chain(big[2]); x.set(c[3], c[1])
    @m("fat_crosslink")
    def _(x):
        a = x.chain(big[2]); b = x.chain(other[2]); x.set(b[0], a[2])
    @m("fat_chain_bad_cluster")
    def _(x):
        c = x.chain(big[2]); x.set(c[2], {12: 0xFF7, 16: 0xFFF7, 32: 0x0FFFFFF7}[x.bits])
    @m("fat_chain_out_of_range")
    def _(x):
        c = x.chain(big[2]); x.set(c[2], x.clusters + 40 if x.bits != 12 else 0xFF0)
    @m("size_grow")
    def _(x): struct.pack_into("<I", x.d, big[1] + 28, big[3] + 3 * x.cb)
    @m("size_shrink")
    def _(x): struct.pack_into("<I", x.d, big[1] + 28, max(1, big[3] - 3 * x.cb))
    @m("size_zero_with_chain")
    def _(x): struct.pack_into("<I", x.d, big[1] + 28, 0)
    @m("size_within_last_cluster")
    def _(x):
        # 最後のクラスタ内で size を変えるだけ (正常: クラスタ数は変わらない)
        n = (big[3] + x.cb - 1) // x.cb
        s = max((n - 1) * x.cb + 1, big[3] - 1)
        struct.pack_into("<I", x.d, big[1] + 28, s)
    M[-1] = ("size_within_last_cluster(valid)", M[-1][1], None)
    @m("start_cluster_out_of_range")
    def _(x): struct.pack_into("<H", x.d, big[1] + 26, 0xFFF0 if x.bits != 12 else 0xFF0)
    @m("start_cluster_zero_with_size")
    def _(x):
        struct.pack_into("<H", x.d, big[1] + 26, 0); struct.pack_into("<H", x.d, big[1] + 20, 0)
    @m("fat_copy_differs")
    def _(x):
        if x.nfats < 2: raise Skip()
        c = x.chain(big[2]); x.set(c[0], x.get(c[0]) ^ 0, [1]); x.set(c[-1], 0x2, [1])
    @m("orphan_chain")
    def _(x):
        free = [c for c in range(2, x.clusters + 2) if x.get(c) == 0][:3]
        x.set(free[0], free[1]); x.set(free[1], free[2]); x.set(free[2], x.eoc())
    @m("orphan_single")
    def _(x):
        free = [c for c in range(2, x.clusters + 2) if x.get(c) == 0][5]
        x.set(free, x.eoc())
    @m("dir_dotdot_wrong")
    def _(x):
        d = [e for e in dirs if e[0].upper().endswith("SUB2")][0]
        o = x.dir_bytes_offsets(d[2])[1]
        struct.pack_into("<H", x.d, o + 26, 5)
        if x.fat32: struct.pack_into("<H", x.d, o + 20, 0)
    @m("dir_dot_wrong")
    def _(x):
        d = [e for e in dirs if e[0].upper().endswith("SUB1")][0]
        o = x.dir_bytes_offsets(d[2])[0]
        struct.pack_into("<H", x.d, o + 26, 9)
    @m("dir_no_cluster")
    def _(x):
        d = [e for e in dirs if e[0].upper().endswith("DIR2")][0]
        struct.pack_into("<H", x.d, d[1] + 26, 0)
        if x.fat32: struct.pack_into("<H", x.d, d[1] + 20, 0)
    @m("lfn_checksum")
    def _(x):
        # 長い名前のファイルの直前の LFN エントリのチェックサムを壊す
        for o in x.dir_bytes_offsets(0):
            if x.d[o] != 0xE5 and (x.d[o + 11] & 0x3F) == 0x0F:
                x.d[o + 13] ^= 0xFF
                return
        raise Skip()
    @m("lfn_orphan_ordinal")
    def _(x):
        for o in x.dir_bytes_offsets(0):
            if x.d[o] != 0xE5 and (x.d[o + 11] & 0x3F) == 0x0F and not (x.d[o] & 0x40):
                x.d[o] = (x.d[o] & 0x40) | 0x3F & 0x0D; return
        raise Skip()
    @m("name_illegal_char")
    def _(x): x.d[other[1] + 1] = ord("*")
    @m("name_control_char")
    def _(x): x.d[other[1] + 2] = 0x07
    @m("duplicate_name")
    def _(x):
        e2 = [e for e in files if e is not other and e[0].count("/") == other[0].count("/")][0]
        x.d[e2[1]:e2[1] + 11] = x.d[other[1]:other[1] + 11]
    @m("dirty_flag_fat1")
    def _(x):
        if x.bits == 12: raise Skip()
        bit = 0x8000 if x.bits == 16 else 0x08000000
        v = x.get(1) & ~bit
        x.set(1, v)
    @m("boot_dirty_flag")
    def _(x): x.d[65 if x.fat32 else 37] |= 1
    @m("fat0_media_mismatch")
    def _(x):
        v = x.get(0); x.set(0, (v & ~0xFF) | 0xF0)
    @m("fsinfo_free_wrong")
    def _(x):
        if not x.fat32: raise Skip()
        fi = struct.unpack_from("<H", x.d, 48)[0]
        struct.pack_into("<I", x.d, fi * x.bps + 488, 5)
    @m("fsinfo_signature")
    def _(x):
        if not x.fat32: raise Skip()
        fi = struct.unpack_from("<H", x.d, 48)[0]
        x.d[fi * x.bps] ^= 0xFF
    @m("backup_boot_differs")
    def _(x):
        if not x.fat32: raise Skip()
        bk = struct.unpack_from("<H", x.d, 50)[0]
        x.d[bk * x.bps + 13] ^= 1 if False else 0
        x.d[bk * x.bps + 16] ^= 3
    @m("root_cluster_out_of_range")
    def _(x):
        if not x.fat32: raise Skip()
        struct.pack_into("<I", x.d, 44, 0x0FFFFF00)
    @m("total_sectors_too_big")
    def _(x):
        if x.fat32 or struct.unpack_from("<H", x.d, 19)[0]:
            v = struct.unpack_from("<I", x.d, 32)[0] if x.fat32 else struct.unpack_from("<H", x.d, 19)[0]
            if x.fat32: struct.pack_into("<I", x.d, 32, v + 100000)
            else: struct.pack_into("<H", x.d, 19, min(65535, v + 1000))
    @m("fat_too_small")
    def _(x):
        if x.fat32: struct.pack_into("<I", x.d, 36, max(1, x.fsz // 50))
        else: struct.pack_into("<H", x.d, 22, max(1, x.fsz // 50))
    @m("high_cluster_nonzero_fat16")
    def _(x):
        if x.fat32: raise Skip()
        struct.pack_into("<H", x.d, other[1] + 20, 1)
    return M


class Skip(Exception):
    pass


def run_fat(cli, res: Result, quick=False):
    d = work_dir("fat")
    configs = [(12, 2, 1), (16, 16, 4), (32, 100, None), (32, 40, 1)] if not quick else [(16, 16, 4), (32, 100, None)]
    for bits, mb, spc in configs:
        base = os.path.join(d, "base_%d_%d.img" % (bits, mb))
        make_image(base, bits, mb, spc)
        orig = open(base, "rb").read()
        tag = "fat%d/%dM" % (bits, mb)
        fsname = "fat%d" % bits
        rp, rt = fsck(base)
        c, j = ours(cli, base, fs=fsname)
        res.add(tag + " pristine", rp, c in (1, 2), rt, codes(j))
        f0 = Fat(orig)
        for name, fn, allow in mutations(f0):
            x = Fat(orig)
            try:
                fn(x)
            except Skip:
                continue
            img = os.path.join(d, "m.img")
            with open(img, "wb") as fh:
                fh.write(x.d)
            if name == "total_sectors_too_big":
                pass
            rp, rt = fsck(img)
            c, j = ours(cli, img, fs=fsname)
            is_valid = name.endswith("(valid)")
            res.add("%s %s" % (tag, name), rp, c in (1, 2), rt, codes(j), allow)
            if c == 10 or j.get("verdict") == "crash":
                res.bad += 1
                print("   CRASH/IOERR", j.get("stderr", ""))
        # 切り詰め: デバイスが FS より小さい
        img = os.path.join(d, "t.img")
        with open(img, "wb") as fh:
            fh.write(orig[: len(orig) * 3 // 4])
        rp, rt = fsck(img)
        c, j = ours(cli, img, fs=fsname)
        res.add(tag + " truncated_device", rp, c in (1, 2), rt, codes(j))
    return res
