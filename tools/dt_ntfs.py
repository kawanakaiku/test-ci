# SPDX-License-Identifier: Apache-2.0
"""NTFS 検査の検証: mkntfs + ntfs-3g で作った実イメージを壊し、`ntfsfix -n` (何も書かない) と自前の検査を比べる。

ntfsfix -n が見るのは MFT と MFTMirr・代替ブートセクタ・ダーティフラグ・ログ程度で、検査範囲が狭い。
そのため、ntfsfix が見られない破壊については「壊した事実」(構成上の正解) を基準にして見逃しを数える。
基準の出所は結果行の ref 文に残す。
"""
import os
import shutil
import struct
import subprocess
import sys

from dt_common import Result, codes, ours, run, work_dir

HERE = os.path.dirname(os.path.abspath(__file__))


class Skip(Exception):
    pass


def ntfsfix(img):
    """ntfsfix -n (何も書かない)。戻り値 (問題あり?, 出力)"""
    p = run(["ntfsfix", "-n", img])
    txt = p.stdout.decode("utf-8", "replace")
    return (p.returncode != 0) or ("was processed successfully" not in txt), txt


class Nt:
    def __init__(self, data):
        self.d = bytearray(data)
        d = self.d
        self.bps = struct.unpack_from("<H", d, 11)[0]
        self.spc = d[13]
        self.cs = self.bps * self.spc
        self.total = struct.unpack_from("<Q", d, 0x28)[0]
        self.mft_lcn = struct.unpack_from("<Q", d, 0x30)[0]
        self.mirr_lcn = struct.unpack_from("<Q", d, 0x38)[0]
        cpr = struct.unpack_from("<b", d, 0x40)[0]
        self.rs = cpr * self.cs if cpr > 0 else 1 << (-cpr)
        rec0 = self.raw_at(self.mft_lcn * self.cs)
        self.mft_runs = self.runs_of(rec0, 0x80)
        self.nrec = None
        for (off, t, nr, nl, ln) in self.attrs(rec0):
            if t == 0x80 and nl == 0:
                self.nrec = struct.unpack_from("<Q", rec0, off + 0x30)[0] // self.rs

    # ---- レコード入出力 (fixup を解いた論理内容で扱う)
    def raw_at(self, pos):
        r = bytearray(self.d[pos:pos + self.rs])
        return self.unfix(r)

    def unfix(self, r):
        ufo, ufc = struct.unpack_from("<HH", r, 4)
        for i in range(ufc - 1):
            r[512 * (i + 1) - 2:512 * (i + 1)] = r[ufo + 2 + 2 * i:ufo + 4 + 2 * i]
        return r

    def fix(self, r):
        ufo, ufc = struct.unpack_from("<HH", r, 4)
        usn = r[ufo:ufo + 2]
        out = bytearray(r)
        for i in range(ufc - 1):
            out[ufo + 2 + 2 * i:ufo + 4 + 2 * i] = r[512 * (i + 1) - 2:512 * (i + 1)]
            out[512 * (i + 1) - 2:512 * (i + 1)] = usn
        return out

    def rec_pos(self, n):
        vcn = n * self.rs // self.cs
        within = n * self.rs % self.cs
        for (v, l, len_, sparse) in self.mft_runs:
            if v <= vcn < v + len_:
                return (l + vcn - v) * self.cs + within
        raise Skip()

    def rec(self, n):
        return self.raw_at(self.rec_pos(n))

    def put_rec(self, n, r):
        pos = self.rec_pos(n)
        self.d[pos:pos + self.rs] = self.fix(r)
        if n < 4:   # MFTMirr も同期 (ミラー不一致の変異は別に作る)
            mp = self.mirr_lcn * self.cs + n * self.rs
            self.d[mp:mp + self.rs] = self.fix(r)

    def put_rec_nomirror(self, n, r):
        pos = self.rec_pos(n)
        self.d[pos:pos + self.rs] = self.fix(r)

    def attrs(self, r):
        res = []
        off = struct.unpack_from("<H", r, 0x14)[0]
        used = struct.unpack_from("<I", r, 0x18)[0]
        while off + 8 <= used:
            t, ln = struct.unpack_from("<II", r, off)
            if t == 0xFFFFFFFF or ln == 0:
                break
            res.append((off, t, r[off + 8], r[off + 9], ln))
            off += ln
        return res

    def find_attr(self, r, typ, named=False):
        for a in self.attrs(r):
            if a[1] == typ and (a[3] != 0) == named:
                return a
        return None

    @staticmethod
    def decode_runs(buf, vcn0=0):
        runs, i, lcn, vcn = [], 0, 0, vcn0
        while i < len(buf) and buf[i]:
            ls, os_ = buf[i] & 15, buf[i] >> 4
            i += 1
            ln = int.from_bytes(buf[i:i + ls], "little")
            i += ls
            if os_:
                dl = int.from_bytes(buf[i:i + os_], "little", signed=True)
                i += os_
                lcn += dl
                runs.append((vcn, lcn, ln, False))
            else:
                runs.append((vcn, 0, ln, True))
            vcn += ln
        return runs

    @staticmethod
    def encode_runs(runs):
        out = bytearray()
        prev = 0
        for (v, l, ln, sp) in runs:
            lb = max(1, (ln.bit_length() + 8) // 8)
            if sp:
                out += bytes([lb]) + ln.to_bytes(lb, "little")
                continue
            dl = l - prev
            prev = l
            ob = 1
            while not (-(1 << (8 * ob - 1)) <= dl < (1 << (8 * ob - 1))):
                ob += 1
            out += bytes([lb | (ob << 4)]) + ln.to_bytes(lb, "little") + dl.to_bytes(ob, "little", signed=True)
        return bytes(out) + b"\0"

    def runs_of(self, r, typ, named=False):
        a = self.find_attr(r, typ, named)
        if not a or not a[2]:
            return []
        off, _, _, _, ln = a
        mp = struct.unpack_from("<H", r, off + 0x20)[0]
        svcn = struct.unpack_from("<Q", r, off + 0x10)[0]
        return self.decode_runs(bytes(r[off + mp:off + ln]), svcn)

    def set_runs(self, r, typ, runs):
        a = self.find_attr(r, typ)
        off, _, _, _, ln = a
        mp = struct.unpack_from("<H", r, off + 0x20)[0]
        enc = self.encode_runs(runs)
        if mp + len(enc) > ln:
            raise Skip()
        r[off + mp:off + ln] = enc + bytes(ln - mp - len(enc))

    # ---- ファイル探索: ルート ($INDEX_ROOT) から名前でレコード番号を得る
    def find_name(self, name):
        """MFT 全体を走査して $FILE_NAME が name に一致する使用中レコード番号 (最初)"""
        target = name.encode("utf-16-le")
        for n in range(16, self.nrec):
            try:
                r = self.rec(n)
            except Skip:
                continue
            if r[:4] != b"FILE" or not (struct.unpack_from("<H", r, 0x16)[0] & 1):
                continue
            for (off, t, nr, nl, ln) in self.attrs(r):
                if t == 0x30 and not nr:
                    vo = struct.unpack_from("<H", r, off + 0x14)[0]
                    fn_len = r[off + vo + 0x40]
                    if bytes(r[off + vo + 0x42:off + vo + 0x42 + 2 * fn_len]) == target:
                        return n
        return None

    def mft_bitmap_pos(self):
        r = self.rec(0)
        a = self.find_attr(r, 0xB0)
        runs = self.runs_of(r, 0xB0)
        return runs[0][1] * self.cs if runs else None


def make_base(d, cluster=4096):
    sh = os.path.join(HERE, "gen_ntfs_fixture.sh")
    env = dict(os.environ, USBMEM_NTFS_CLUSTER=str(cluster))
    p = run(["sh", sh, d], env=env)
    if p.returncode == 77:
        raise Skip()
    if p.returncode != 0:
        raise RuntimeError("fixture: " + p.stdout.decode("utf-8", "replace"))
    return os.path.join(d, "ntfs_basic.img")


def mutations(x):
    """(name, fn, truth, allow)。truth = ntfsfix では見えないが壊したことが確実か (True なら ref=BAD とみなす)。None = ntfsfix の判定に任せる"""
    M = []

    def m(name, truth=True, allow=None):
        def deco(fn):
            M.append((name, fn, truth, allow))
            return fn
        return deco

    big = x.find_name("big.bin")
    dir1 = x.find_name("dir1")
    afile = x.find_name("a.txt")

    # ---- ブートセクタ
    @m("boot_oem_bad")
    def _(y): y.d[3:11] = b"XXXXXXXX"

    @m("boot_signature_bad")
    def _(y): y.d[510] = 0

    @m("boot_bps_bad")
    def _(y): struct.pack_into("<H", y.d, 11, 300)

    @m("boot_spc_zero")
    def _(y): y.d[13] = 0

    @m("boot_spc_notpow2")
    def _(y): y.d[13] = 3

    @m("boot_recsize_bad")
    def _(y): y.d[0x40] = 5

    @m("boot_total_zero")
    def _(y): struct.pack_into("<Q", y.d, 0x28, 0)

    @m("boot_total_huge")
    def _(y): struct.pack_into("<Q", y.d, 0x28, y.total * 4)

    @m("boot_mft_oob")
    def _(y): struct.pack_into("<Q", y.d, 0x30, 1 << 40)

    @m("boot_mft_shifted")
    def _(y): struct.pack_into("<Q", y.d, 0x30, y.mft_lcn + 3)

    @m("boot_mirr_oob")
    def _(y): struct.pack_into("<Q", y.d, 0x38, 1 << 40)

    @m("boot_mirr_shifted")
    def _(y): struct.pack_into("<Q", y.d, 0x38, y.mirr_lcn + 5)

    @m("boot_bpb_reserved_nonzero")
    def _(y): y.d[14] = 1

    @m("backup_boot_zero", truth=True)
    def _(y):
        pos = y.total * y.bps
        y.d[pos:pos + 512] = bytes(512)

    @m("backup_boot_differs", truth=True)
    def _(y):
        pos = y.total * y.bps
        y.d[pos + 0x30] ^= 1

    # ---- MFT レコード
    @m("mft0_magic_bad")
    def _(y):
        pos = y.mft_lcn * y.cs
        y.d[pos:pos + 4] = b"XXXX"

    @m("mft0_fixup_torn")
    def _(y):
        pos = y.mft_lcn * y.cs
        y.d[pos + 1022] ^= 0xFF

    @m("mft_user_fixup_torn")
    def _(y):
        pos = y.rec_pos(afile)
        y.d[pos + 510] ^= 0xFF

    @m("mft_user_magic_bad")
    def _(y):
        pos = y.rec_pos(afile)
        y.d[pos:pos + 4] = b"XXXX"

    @m("mft_user_baad")
    def _(y):
        pos = y.rec_pos(afile)
        y.d[pos:pos + 4] = b"BAAD"

    def edit(y, n, fn):
        r = y.rec(n)
        fn(r)
        y.put_rec(n, r)

    @m("rec_used_gt_alloc")
    def _(y): edit(y, afile, lambda r: struct.pack_into("<I", r, 0x18, y.rs + 8))

    @m("rec_alloc_bad")
    def _(y): edit(y, afile, lambda r: struct.pack_into("<I", r, 0x1C, y.rs // 2))

    @m("rec_attr_offset_bad")
    def _(y): edit(y, afile, lambda r: struct.pack_into("<H", r, 0x14, 0x20))

    @m("rec_selfnum_wrong")
    def _(y): edit(y, afile, lambda r: struct.pack_into("<I", r, 0x2C, afile + 7))

    def first_attr(r):
        return struct.unpack_from("<H", r, 0x14)[0]

    @m("attr_len_zero")
    def _(y): edit(y, afile, lambda r: struct.pack_into("<I", r, first_attr(r) + 4, 0))

    @m("attr_len_unaligned")
    def _(y): edit(y, afile, lambda r: struct.pack_into("<I", r, first_attr(r) + 4, 0x61))

    @m("attr_len_overrun")
    def _(y): edit(y, afile, lambda r: struct.pack_into("<I", r, first_attr(r) + 4, 0x8000))

    @m("attr_type_bad")
    def _(y): edit(y, afile, lambda r: struct.pack_into("<I", r, first_attr(r), 0x15))

    @m("attr_order_bad")
    def _(y):
        def f(r):
            a = y.attrs(r)
            struct.pack_into("<I", r, a[0][0], 0x40)
        edit(y, afile, f)

    @m("attr_value_overrun")
    def _(y):
        def f(r):
            struct.pack_into("<I", r, first_attr(r) + 0x10, 0x4000)
        edit(y, afile, f)

    @m("attr_no_end_marker")
    def _(y):
        def f(r):
            used = struct.unpack_from("<I", r, 0x18)[0]
            r[used - 8:used] = bytes(8)
        edit(y, afile, f)

    # 非常駐属性 ($DATA of big.bin)
    def nonres(y, fn):
        def f(r):
            a = y.find_attr(r, 0x80)
            if not a or not a[2]:
                raise Skip()
            fn(r, a[0])
        edit(y, big, f)

    @m("data_runlist_garbage")
    def _(y): nonres(y, lambda r, o: r.__setitem__(slice(o + struct.unpack_from("<H", r, o + 0x20)[0], o + struct.unpack_from("<H", r, o + 0x20)[0] + 3), b"\x0F\x01\x01"))

    @m("data_run_zero_length")
    def _(y): nonres(y, lambda r, o: r.__setitem__(o + struct.unpack_from("<H", r, o + 0x20)[0] + 1, 0))

    @m("data_run_out_of_volume")
    def _(y):
        def f(r, o):
            runs = y.runs_of(r, 0x80)
            runs[0] = (runs[0][0], y.total // y.spc + 100, runs[0][2], False)
            y.set_runs(r, 0x80, runs)
        nonres(y, f)

    @m("data_run_length_mismatch")
    def _(y): nonres(y, lambda r, o: struct.pack_into("<Q", r, o + 0x18, struct.unpack_from("<Q", r, o + 0x18)[0] + 5))

    @m("data_size_gt_alloc")
    def _(y): nonres(y, lambda r, o: struct.pack_into("<Q", r, o + 0x30, struct.unpack_from("<Q", r, o + 0x28)[0] + 4096))

    @m("data_init_gt_size")
    def _(y): nonres(y, lambda r, o: struct.pack_into("<Q", r, o + 0x38, struct.unpack_from("<Q", r, o + 0x30)[0] + 1))

    @m("data_alloc_wrong")
    def _(y): nonres(y, lambda r, o: struct.pack_into("<Q", r, o + 0x28, struct.unpack_from("<Q", r, o + 0x28)[0] + 4096))

    @m("data_crosslink_logfile")
    def _(y):
        lf = y.runs_of(y.rec(2), 0x80)
        def f(r, o):
            runs = y.runs_of(r, 0x80)
            runs[0] = (runs[0][0], lf[0][1], runs[0][2], False)
            y.set_runs(r, 0x80, runs)
        nonres(y, f)

    @m("data_overlaps_bitmap_cluster_shift")
    def _(y):
        def f(r, o):
            runs = y.runs_of(r, 0x80)
            runs[0] = (runs[0][0], runs[0][1] + 1, runs[0][2], False)
            y.set_runs(r, 0x80, runs)
        nonres(y, f)

    # MFT ビットマップ
    bm = x.mft_bitmap_pos()

    @m("mftbitmap_clear_inuse")
    def _(y):
        if bm is None: raise Skip()
        y.d[bm + big // 8] &= ~(1 << (big % 8)) & 0xFF

    @m("mftbitmap_clear_system")
    def _(y):
        if bm is None: raise Skip()
        y.d[bm + 0] &= 0xFF ^ (1 << 5)

    @m("mftbitmap_set_unused")
    def _(y):
        if bm is None: raise Skip()
        n = 17   # 予約領域 (16〜23) は未使用のまま
        y.d[bm + n // 8] |= 1 << (n % 8)

    @m("rec_unused_flag_cleared")
    def _(y): edit(y, afile, lambda r: struct.pack_into("<H", r, 0x16, struct.unpack_from("<H", r, 0x16)[0] & ~1))

    @m("dir_flag_cleared")
    def _(y): edit(y, dir1, lambda r: struct.pack_into("<H", r, 0x16, struct.unpack_from("<H", r, 0x16)[0] & ~2))

    def fn_attr(y, n, fn):
        def f(r):
            for (off, t, nr, nl, ln) in y.attrs(r):
                if t == 0x30 and not nr:
                    vo = struct.unpack_from("<H", r, off + 0x14)[0]
                    fn(r, off + vo)
                    return
            raise Skip()
        edit(y, n, f)

    @m("parent_seq_wrong")
    def _(y):
        def f(r, p):
            v = struct.unpack_from("<Q", r, p)[0]
            struct.pack_into("<Q", r, p, v ^ (7 << 48))
        fn_attr(y, afile, f)

    @m("parent_out_of_range")
    def _(y): fn_attr(y, afile, lambda r, p: struct.pack_into("<Q", r, p, (1 << 20) | (1 << 48)))

    @m("parent_is_file")
    def _(y): fn_attr(y, afile, lambda r, p: struct.pack_into("<Q", r, p, big | (struct.unpack_from("<H", y.rec(big), 0x10)[0] << 48)))

    @m("mirror_record_differs", truth=None)
    def _(y):
        mp = y.mirr_lcn * y.cs + y.rs + 0x40
        y.d[mp] ^= 0xFF

    @m("mirror_record_zero", truth=None)
    def _(y):
        mp = y.mirr_lcn * y.cs + 2 * y.rs
        y.d[mp:mp + y.rs] = bytes(y.rs)

    @m("mirror_magic_bad", truth=None)
    def _(y):
        mp = y.mirr_lcn * y.cs
        y.d[mp:mp + 4] = b"XXXX"

    @m("mft_size_shrunk")
    def _(y):
        def f(r):
            a = y.find_attr(r, 0x80)
            struct.pack_into("<Q", r, a[0] + 0x30, 10 * y.rs)
        r = y.rec(0)
        f(r)
        y.put_rec(0, r)

    @m("mft_data_resident", truth=True)
    def _(y):
        r = y.rec(0)
        a = y.find_attr(r, 0x80)
        r[a[0] + 8] = 0
        y.put_rec(0, r)

    # ---- $Bitmap
    bmrec = x.rec(6)
    bruns = x.runs_of(bmrec, 0x80)
    bpos = bruns[0][1] * x.cs
    bigr = x.runs_of(x.rec(big), 0x80)
    bigcl = bigr[0][1] + 3

    @m("bitmap_clear_used")
    def _(y): y.d[bpos + bigcl // 8] &= 0xFF ^ (1 << (bigcl % 8))

    @m("bitmap_set_unused")
    def _(y):
        cl = x.total // x.spc - 50
        y.d[bpos + cl // 8] |= 1 << (cl % 8)

    @m("bitmap_zeroed_block")
    def _(y): y.d[bpos:bpos + 64] = bytes(64)

    @m("bitmap_size_short")
    def _(y):
        r = y.rec(6)
        a = y.find_attr(r, 0x80)
        struct.pack_into("<Q", r, a[0] + 0x30, 100)
        y.put_rec(6, r)

    # ---- ボリューム状態とログ
    @m("volume_dirty")
    def _(y):
        r = y.rec(3)
        a = y.find_attr(r, 0x70)
        vo = struct.unpack_from("<H", r, a[0] + 0x14)[0]
        fl = struct.unpack_from("<H", r, a[0] + vo + 10)[0]
        struct.pack_into("<H", r, a[0] + vo + 10, fl | 1)
        y.put_rec(3, r)

    @m("chkdsk_scheduled")
    def _(y):
        r = y.rec(3)
        a = y.find_attr(r, 0x70)
        vo = struct.unpack_from("<H", r, a[0] + 0x14)[0]
        fl = struct.unpack_from("<H", r, a[0] + vo + 10)[0]
        struct.pack_into("<H", r, a[0] + vo + 10, fl | 0x80)
        y.put_rec(3, r)

    lruns = x.runs_of(x.rec(2), 0x80)
    lpos = lruns[0][1] * x.cs

    def restart_page(y, pg, lsn, clean):
        p = lpos + pg
        y.d[p:p + 4096] = bytes(4096)
        y.d[p:p + 4] = b"RSTR"
        struct.pack_into("<HHQII", y.d, p + 4, 0x1E, 3, 0, 4096, 4096)
        struct.pack_into("<HHH", y.d, p + 0x18, 0x30, 1, 1)
        struct.pack_into("<QHHHH", y.d, p + 0x30, lsn, 1, 0xFFFF, 0, 2 if clean else 0)

    @m("logfile_restart_unclean")
    def _(y):
        restart_page(y, 0, 100, False)
        restart_page(y, 4096, 99, False)

    @m("logfile_newer_page_unclean")
    def _(y):
        restart_page(y, 0, 99, True)
        restart_page(y, 4096, 100, False)

    @m("logfile_one_page_garbage", truth=None)
    def _(y): y.d[lpos:lpos + 4096] = b"\xA5" * 4096

    @m("logfile_both_garbage")
    def _(y): y.d[lpos:lpos + 8192] = b"\xA5" * 8192

    @m("benign_logfile_clean_restart", truth=False)
    def _(y):
        restart_page(y, 0, 100, True)
        restart_page(y, 4096, 99, True)

    @m("benign_logfile_newer_page_clean", truth=False)
    def _(y):
        restart_page(y, 0, 99, False)
        restart_page(y, 4096, 100, True)

    # ---- 壊れていないはずの変更 (過検出の確認)
    @m("benign_file_data_flip", truth=False)
    def _(y): y.d[bigr[0][1] * x.cs + 100] ^= 0xFF

    @m("benign_timestamp_change", truth=False)
    def _(y):
        def f(r):
            for (off, t, nr, nl, ln) in y.attrs(r):
                if t == 0x10 and not nr:
                    vo = struct.unpack_from("<H", r, off + 0x14)[0]
                    struct.pack_into("<Q", r, off + vo + 8, 0x01D0000000000000)
                    return
        edit(y, afile, f)

    @m("benign_free_cluster_data", truth=False)
    def _(y):
        cl = x.total // x.spc - 60
        y.d[cl * x.cs:cl * x.cs + 64] = b"\x5A" * 64
    return M


def run_ntfs(cli, res: Result, quick=False):
    d = work_dir("ntfs")
    try:
        base = make_base(os.path.join(d, "fx"))
    except Skip:
        print("SKIP ntfs: fixture tools (mkntfs/ntfs-3g/fusermount3) not available")
        return res
    orig = open(base, "rb").read()
    x0 = Nt(orig)
    img = os.path.join(d, "m.img")
    rp, rt = ntfsfix(base)
    c, j = ours(cli, base)
    res.add("ntfs pristine", rp, c in (1, 2), rt, codes(j))
    muts = mutations(x0)
    if quick:
        keep = ("boot_oem_bad", "boot_mft_shifted", "backup_boot_zero", "mft_user_fixup_torn", "attr_len_zero", "data_run_out_of_volume",
                "mftbitmap_clear_inuse", "dir_flag_cleared", "mirror_record_differs", "bitmap_clear_used", "bitmap_set_unused", "volume_dirty",
                "benign_file_data_flip", "benign_timestamp_change", "data_crosslink_logfile")
        muts = [m for m in muts if m[0] in keep]
    for name, fn, truth, allow in muts:
        y = Nt(orig)
        try:
            fn(y)
        except Skip:
            continue
        with open(img, "wb") as fh:
            fh.write(y.d)
        rp, rt = ntfsfix(img)
        if truth is True:
            rp = True
            rt = "(by construction) " + rt
        elif truth is False:
            rt = "(benign by construction) " + rt
            rp = False if not rp else rp
        c, j = ours(cli, img, fs="ntfs")
        res.add("ntfs " + name, rp, c in (1, 2), rt, codes(j), allow)
        if c == 10 or j.get("verdict") == "crash":
            res.bad += 1
            print("   CRASH/IOERR", j.get("stderr", ""))
    if not quick:
        # クラスタ・サイズ違いの無傷イメージ (過検出の確認)
        for cl in (512, 1024, 65536):
            try:
                b2 = make_base(os.path.join(d, "fx%d" % cl), cl)
            except Skip:
                break
            rp, rt = ntfsfix(b2)
            c, j = ours(cli, b2)
            res.add("ntfs cluster%d pristine" % cl, rp, c in (1, 2), rt, codes(j))
            o2 = open(b2, "rb").read()
            z = Nt(o2)
            y = Nt(o2)
            r = y.rec(z.find_name("big.bin"))
            runs = y.runs_of(r, 0x80)
            runs[0] = (runs[0][0], runs[0][1] + 1, runs[0][2], False)
            try:
                y.set_runs(r, 0x80, runs)
                y.put_rec(z.find_name("big.bin"), r)
                with open(img, "wb") as fh:
                    fh.write(y.d)
                c, j = ours(cli, img, fs="ntfs")
                res.add("ntfs cluster%d data_run_shifted" % cl, True, c in (1, 2), "(by construction)", codes(j))
            except Skip:
                pass
    t = os.path.join(d, "t.img")
    with open(t, "wb") as fh:
        fh.write(orig[: len(orig) * 3 // 4])
    rp, rt = ntfsfix(t)
    c, j = ours(cli, t, fs="ntfs")
    res.add("ntfs truncated_device", True, c in (1, 2), "(by construction) " + rt, codes(j))
    return res
