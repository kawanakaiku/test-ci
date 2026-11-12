# SPDX-License-Identifier: Apache-2.0
"""exFAT の検査の検証: tools/gen_exfat_fixture.py のイメージを壊して fsck.exfat -n と自前の検出を比べる。"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dt_common import Result, codes, ours, run, work_dir  # noqa: E402


def rot16(s, b):
    return (((s << 15) | (s >> 1)) + b) & 0xFFFF


def rot32(s, b):
    return (((s << 31) | (s >> 1)) + b) & 0xFFFFFFFF


class ExSet:
    pass


class Ex:
    def __init__(self, data):
        self.d = bytearray(data)
        b = self.d
        self.bps_shift = b[108]
        self.spc_shift = b[109]
        self.bps = 1 << self.bps_shift
        self.cb = self.bps << self.spc_shift
        self.fat_off = struct.unpack_from("<I", b, 80)[0] * self.bps
        self.fat_len = struct.unpack_from("<I", b, 84)[0]
        self.heap_off = struct.unpack_from("<I", b, 88)[0] * self.bps
        self.count = struct.unpack_from("<I", b, 92)[0]
        self.root = struct.unpack_from("<I", b, 96)[0]

    def coff(self, cl):
        return self.heap_off + (cl - 2) * self.cb

    def fat(self, cl):
        return struct.unpack_from("<I", self.d, self.fat_off + 4 * cl)[0]

    def setfat(self, cl, v):
        struct.pack_into("<I", self.d, self.fat_off + 4 * cl, v)

    def chain(self, first, nofat=False, size=0):
        if nofat:
            return list(range(first, first + (size + self.cb - 1) // self.cb))
        out = []
        cl = first
        while 2 <= cl < self.count + 2 and len(out) < 1000000:
            out.append(cl)
            nx = self.fat(cl)
            if nx >= 0xFFFFFFF8 or nx == 0 or nx == 0xFFFFFFF7:
                break
            cl = nx
        return out

    def dir_entries(self, first, nofat=False, size=0):
        offs = []
        for cl in self.chain(first, nofat, size if nofat else 0):
            for i in range(self.cb // 32):
                offs.append(self.coff(cl) + 32 * i)
        return offs

    def sets(self, first=None, nofat=False, size=0, path=""):
        if first is None:
            first = self.root
        offs = self.dir_entries(first, nofat, size)
        res = []
        i = 0
        while i < len(offs):
            o = offs[i]
            t = self.d[o]
            if t == 0:
                break
            if t == 0x85:
                sc = self.d[o + 1]
                s = ExSet()
                s.off = o
                s.sc = sc
                s.offs = offs[i:i + 1 + sc]
                st = offs[i + 1]
                s.stream = st
                nl = self.d[st + 3]
                name = b""
                for k in range(2, sc + 1):
                    ne = offs[i + k]
                    name += bytes(self.d[ne + 2:ne + 32])
                s.name = name[:2 * nl].decode("utf-16le", "replace")
                s.attr = struct.unpack_from("<H", self.d, o + 4)[0]
                s.is_dir = bool(s.attr & 0x10)
                s.flags = self.d[st + 1]
                s.nofat = bool(s.flags & 2)
                s.vdl = struct.unpack_from("<Q", self.d, st + 8)[0]
                s.first = struct.unpack_from("<I", self.d, st + 20)[0]
                s.dlen = struct.unpack_from("<Q", self.d, st + 24)[0]
                s.path = path + "/" + s.name
                s.dir_offs = offs
                s.idx = i
                res.append(s)
                if s.is_dir and s.first:
                    res += self.sets(s.first, s.nofat, s.dlen, s.path)
                i += 1 + sc
                continue
            i += 1
        return res

    def fix_set(self, s):
        """セットのチェックサム (と名前ハッシュは触らない) を計算し直す"""
        offs = s.offs
        sm = 0
        for k, o in enumerate(offs):
            for b in range(32):
                if k == 0 and b in (2, 3):
                    continue
                sm = rot16(sm, self.d[o + b])
        struct.pack_into("<H", self.d, offs[0] + 2, sm)

    def upcase_char(self, ch):
        return ord(chr(ch).upper()) if ch < 128 else ch

    def sys_entries(self):
        res = {}
        for o in self.dir_entries(self.root):
            t = self.d[o]
            if t == 0:
                break
            if t in (0x81, 0x82, 0x83):
                res.setdefault(t, []).append(o)
        return res


class Skip(Exception):
    pass


def fsck(path):
    p = run(["fsck.exfat", "-n", path])
    txt = p.stdout.decode("utf-8", "replace")
    # -n では「ERROR: ... Delete (y/N)? n」と表示しても終了コード 0 になることがある。表示された問題も「問題あり」とする
    said = "ERROR:" in txt or "(y/N)" in txt
    return p.returncode != 0 or said, txt


def mutations(x):
    M = []
    sets = x.sets()
    files = [s for s in sets if not s.is_dir and s.dlen > 0]
    dirs = [s for s in sets if s.is_dir]
    frag = [s for s in files if not s.nofat and len(x.chain(s.first)) >= 3]
    big_f = max(files, key=lambda s: s.dlen)
    many = [s for s in dirs if s.name == "many"][0]
    fchain = frag[0] if frag else None
    cont = [s for s in files if s.nofat]
    two_f = [s for s in files if s is not big_f and len(x.chain(s.first, s.nofat, s.dlen)) >= 2]

    def m(name, allow=None):
        def deco(fn):
            M.append((name, fn, allow))
            return fn
        return deco

    def clus_list(s):
        return x.chain(s.first, s.nofat, s.dlen)

    @m("boot_checksum")
    def _(y): y.d[y.bps * 11 + 8] ^= 0xFF
    @m("boot_backup_checksum", allow=("fp",))
    def _(y): y.d[y.bps * 23 + 8] ^= 0xFF
    @m("boot_main_field_changed_checksum_stale")
    def _(y): y.d[70] ^= 1  # PartitionOffset: チェックサム対象
    @m("fs_name")
    def _(y): y.d[3] = ord("X")
    @m("bad_sector_shift")
    def _(y): y.d[108] = 20
    @m("bad_cluster_shift")
    def _(y): y.d[109] = 30
    @m("zero_fats")
    def _(y): y.d[110] = 0
    @m("revision")
    def _(y): y.d[105] = 7
    @m("vol_len_bigger_than_device")
    def _(y): struct.pack_into("<Q", y.d, 72, struct.unpack_from("<Q", y.d, 72)[0] * 3)
    @m("cluster_count_too_big")
    def _(y): struct.pack_into("<I", y.d, 92, 0x7FFFFFF0)
    @m("fat_len_small")
    def _(y): struct.pack_into("<I", y.d, 84, 1)
    @m("root_out_of_range")
    def _(y): struct.pack_into("<I", y.d, 96, 0x7FFFFFF0)
    @m("volume_dirty_flag", allow=("fp",))
    def _(y): y.d[106] |= 2
    @m("media_failure_flag", allow=("fp",))
    def _(y): y.d[106] |= 4
    @m("sig_55aa")
    def _(y): y.d[510] = 0
    if fchain:
        @m("chain_free")
        def _(y):
            c = y.chain(fchain.first); y.setfat(c[1], 0)
        @m("chain_loop")
        def _(y):
            c = y.chain(fchain.first); y.setfat(c[-1], c[0])
        @m("chain_loop_mid")
        def _(y):
            c = y.chain(fchain.first); y.setfat(c[2], c[1])
        @m("chain_bad_cluster")
        def _(y):
            c = y.chain(fchain.first); y.setfat(c[1], 0xFFFFFFF7)
        @m("chain_out_of_range")
        def _(y):
            c = y.chain(fchain.first); y.setfat(c[1], y.count + 100)
        @m("chain_too_short")
        def _(y):
            c = y.chain(fchain.first); y.setfat(c[1], 0xFFFFFFFF)
        @m("chain_too_long")
        def _(y):
            c = y.chain(fchain.first)
            free = [k for k in range(2, y.count + 2) if y.fat(k) == 0 and not (y.d[y.bm_off + (k - 2) // 8] >> ((k - 2) & 7)) & 1][0]
            y.setfat(c[-1], free); y.setfat(free, 0xFFFFFFFF)
            y.d[y.bm_off + (free - 2) // 8] |= 1 << ((free - 2) & 7)
        @m("crosslink")
        def _(y):
            o = [s for s in two_f if not s.nofat and s is not fchain]
            if not o: raise Skip()
            a = y.chain(fchain.first); b = y.chain(o[0].first)
            y.setfat(b[0], a[1])
    @m("size_grow_fixed_checksum")
    def _(y):
        s = y_set(y, big_f); struct.pack_into("<Q", y.d, s.stream + 24, s.dlen + 3 * y.cb); y.fix_set(s)
    @m("size_shrink_fixed_checksum", allow=("fp",))
    def _(y):
        s = y_set(y, big_f); struct.pack_into("<Q", y.d, s.stream + 24, max(1, s.dlen - 3 * y.cb)); struct.pack_into("<Q", y.d, s.stream + 8, 1); y.fix_set(s)
    @m("size_grow_stale_checksum")
    def _(y):
        s = y_set(y, big_f); struct.pack_into("<Q", y.d, s.stream + 24, s.dlen + 3 * y.cb)
    @m("vdl_bigger_than_size")
    def _(y):
        s = y_set(y, big_f); struct.pack_into("<Q", y.d, s.stream + 8, s.dlen + 5); y.fix_set(s)
    @m("start_cluster_out_of_range")
    def _(y):
        s = y_set(y, big_f); struct.pack_into("<I", y.d, s.stream + 20, y.count + 500); y.fix_set(s)
    @m("empty_file_with_cluster")
    def _(y):
        s = y_set(y, big_f); struct.pack_into("<Q", y.d, s.stream + 24, 0); struct.pack_into("<Q", y.d, s.stream + 8, 0); y.fix_set(s)
    @m("dir_size_not_cluster_multiple")
    def _(y):
        s = y_set(y, many); struct.pack_into("<Q", y.d, s.stream + 24, s.dlen - 7); struct.pack_into("<Q", y.d, s.stream + 8, s.dlen - 7); y.fix_set(s)
    @m("name_hash_wrong")
    def _(y):
        s = y_set(y, big_f); y.d[s.stream + 4] ^= 0x55; y.fix_set(s)
    @m("name_char_changed_hash_stale")
    def _(y):
        s = y_set(y, big_f); ne = s.offs[2]; y.d[ne + 2] ^= 1; y.fix_set(s)
    @m("name_illegal_char")
    def _(y):
        s = y_set(y, big_f); ne = s.offs[2]; y.d[ne + 2] = ord("*"); y.fix_set(s)
    @m("secondary_count_wrong")
    def _(y):
        s = y_set(y, big_f); y.d[s.off + 1] = 1; y.fix_set(s)
    @m("name_length_wrong")
    def _(y):
        s = y_set(y, big_f); y.d[s.stream + 3] = min(255, y.d[s.stream + 3] + 20); y.fix_set(s)
    @m("no_stream_entry")
    def _(y):
        s = y_set(y, big_f); y.d[s.stream] = 0xC1; y.fix_set(s)
    @m("orphan_secondary_after_delete")
    def _(y):
        s = y_set(y, big_f); y.d[s.off] = 0x05
    @m("set_checksum_zero")
    def _(y):
        s = y_set(y, files[0]); struct.pack_into("<H", y.d, s.off + 2, 0)
    @m("duplicate_name")
    def _(y):
        peers = [s for s in sets if s is not big_f and s.dir_offs is big_f.dir_offs or s.dir_offs[0] == big_f.dir_offs[0]]
        peers = [s for s in peers if s is not big_f and s.dir_offs[0] == big_f.dir_offs[0] and len(s.name) == len(big_f.name)]
        if not peers: raise Skip()
        a = y_set(y, big_f); b = y_set(y, peers[0])
        # b の名前エントリと名前ハッシュを a と同じにする
        for k in range(2, a.sc + 1):
            y.d[b.offs[k] + 2:b.offs[k] + 32] = y.d[a.offs[k] + 2:a.offs[k] + 32]
        y.d[b.stream + 3] = y.d[a.stream + 3]
        y.d[b.stream + 4:b.stream + 6] = y.d[a.stream + 4:a.stream + 6]
        y.fix_set(b)
    # ビットマップ
    @m("bitmap_bit_cleared_for_used")
    def _(y):
        c = clus_list(y_set(y, big_f))[0]; y.d[y.bm_off + (c - 2) // 8] &= ~(1 << ((c - 2) & 7))
    @m("bitmap_bit_set_for_free_leak", allow=("fp",))
    def _(y):
        for k in range(2, y.count + 2):
            if not (y.d[y.bm_off + (k - 2) // 8] >> ((k - 2) & 7)) & 1:
                y.d[y.bm_off + (k - 2) // 8] |= 1 << ((k - 2) & 7); return
        raise Skip()
    @m("bitmap_entry_removed")
    def _(y):
        y.d[y.sys_entries()[0x81][0]] = 0x01
    @m("bitmap_entry_duplicated", allow=("fp",))
    def _(y):
        e = y.sys_entries()
        # 空きの先頭にコピー
        offs = y.dir_entries(y.root)
        for o in offs:
            if y.d[o] == 0:
                y.d[o:o + 32] = y.d[e[0x81][0]:e[0x81][0] + 32]; return
        raise Skip()
    @m("bitmap_too_small")
    def _(y):
        o = y.sys_entries()[0x81][0]; struct.pack_into("<Q", y.d, o + 24, 2)
    @m("upcase_checksum")
    def _(y):
        o = y.sys_entries()[0x82][0]; y.d[o + 4] ^= 0xFF
    @m("upcase_removed")
    def _(y):
        y.d[y.sys_entries()[0x82][0]] = 0x02
    @m("upcase_size_odd")
    def _(y):
        o = y.sys_entries()[0x82][0]; struct.pack_into("<Q", y.d, o + 24, struct.unpack_from("<Q", y.d, o + 24)[0] - 1)
    @m("fat_header")
    def _(y): struct.pack_into("<I", y.d, y.fat_off, 0x12345678)
    return M


def y_set(y, s):
    """元の ExSet s に対応する、変更対象 y 上の ExSet (オフセットは同じ)"""
    return s


def run_exfat(cli, res: Result, quick=False):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import gen_exfat_fixture as g
    d = work_dir("exfat")
    configs = [("c4k", 512, 4096, 24), ("s4k_c32k", 4096, 32768, 48)] if not quick else [("c4k", 512, 4096, 24)]
    if not quick:
        configs.append(("c512", 512, 512, 8))
    for name, sector, cluster, mib in configs:
        g.make(d, name, sector, cluster, mib)
        base = os.path.join(d, name + ".img")
        orig = open(base, "rb").read()
        tag = "exfat/" + name
        rp, rt = fsck(base)
        c, j = ours(cli, base)
        res.add(tag + " pristine", rp, c in (1, 2), rt, codes(j))
        x0 = Ex(orig)
        # ビットマップの場所 (変更用)
        e = x0.sys_entries()
        bm_first = struct.unpack_from("<I", x0.d, e[0x81][0] + 20)[0]
        for mname, fn, allow in mutations(x0):
            y = Ex(orig)
            y.bm_off = y.coff(bm_first)
            try:
                fn(y)
            except Skip:
                continue
            img = os.path.join(d, "m.img")
            with open(img, "wb") as fh:
                fh.write(y.d)
            rp, rt = fsck(img)
            c, j = ours(cli, img, fs="exfat")
            res.add("%s %s" % (tag, mname), rp, c in (1, 2), rt, codes(j), allow)
            if c == 10 or j.get("verdict") == "crash":
                res.bad += 1
                print("   CRASH/IOERR", j.get("stderr", ""))
        img = os.path.join(d, "t.img")
        with open(img, "wb") as fh:
            fh.write(orig[: len(orig) * 3 // 4])
        rp, rt = fsck(img)
        c, j = ours(cli, img, fs="exfat")
        res.add(tag + " truncated_device", rp, c in (1, 2), rt, codes(j))
    return res
