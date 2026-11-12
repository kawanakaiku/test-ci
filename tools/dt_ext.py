# SPDX-License-Identifier: Apache-2.0
"""ext2/3/4 の検査の検証: mke2fs -d で作った実イメージを壊し、e2fsck -fn と自前の検出を比べる。"""
import os
import random
import shutil
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dt_common import Result, codes, ours, run, work_dir  # noqa: E402

_T = []
for _i in range(256):
    _c = _i
    for _k in range(8):
        _c = (0x82F63B78 ^ (_c >> 1)) if (_c & 1) else (_c >> 1)
    _T.append(_c)


def crc32c(crc, data):
    for b in data:
        crc = _T[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc & 0xFFFFFFFF


class Skip(Exception):
    pass


class Ext:
    def __init__(self, data):
        self.d = bytearray(data)
        sb = self.d
        self.inodes = struct.unpack_from("<I", sb, 1024)[0]
        self.log_bs = struct.unpack_from("<I", sb, 1024 + 24)[0]
        self.bs = 1024 << self.log_bs
        self.bpg = struct.unpack_from("<I", sb, 1024 + 32)[0]
        self.ipg = struct.unpack_from("<I", sb, 1024 + 40)[0]
        self.fdb = struct.unpack_from("<I", sb, 1024 + 20)[0]
        self.isz = struct.unpack_from("<H", sb, 1024 + 0x58)[0]
        self.compat, self.incompat, self.ro = struct.unpack_from("<III", sb, 1024 + 0x5C)
        self.uuid = bytes(sb[1024 + 0x68:1024 + 0x78])
        self.first_ino = struct.unpack_from("<I", sb, 1024 + 0x54)[0]
        self.is64 = bool(self.incompat & 0x80)
        self.dsz = struct.unpack_from("<H", sb, 1024 + 0xFE)[0] if self.is64 else 32
        blocks = struct.unpack_from("<I", sb, 1024 + 4)[0] | ((struct.unpack_from("<I", sb, 1024 + 0x150)[0] << 32) if self.is64 else 0)
        self.blocks = blocks
        self.groups = (blocks - self.fdb + self.bpg - 1) // self.bpg
        self.meta = bool(self.ro & 0x400)
        self.gdt = bool(self.ro & 0x10) and not self.meta
        self.extents = bool(self.incompat & 0x40)
        self.seed = crc32c(0xFFFFFFFF, self.uuid) if self.meta else 0
        self.gdt_off = (self.fdb + 1) * self.bs

    # ---- チェックサム
    def fix_sb(self):
        if self.meta:
            struct.pack_into("<I", self.d, 1024 + 0x3FC, crc32c(0xFFFFFFFF, bytes(self.d[1024:1024 + 1020])))

    def gd_off(self, g):
        return self.gdt_off + g * self.dsz

    def gd(self, g):
        o = self.gd_off(g)
        bb, ib, it = struct.unpack_from("<III", self.d, o)
        if self.is64:
            bb |= struct.unpack_from("<I", self.d, o + 32)[0] << 32
            ib |= struct.unpack_from("<I", self.d, o + 36)[0] << 32
            it |= struct.unpack_from("<I", self.d, o + 40)[0] << 32
        return bb, ib, it

    def fix_gd(self, g):
        o = self.gd_off(g)
        d = bytearray(self.d[o:o + self.dsz])
        d[30] = d[31] = 0
        if self.meta:
            c = crc32c(self.seed, struct.pack("<I", g))
            c = crc32c(c, bytes(d))
            struct.pack_into("<H", self.d, o + 30, c & 0xFFFF)
        elif self.gdt:
            c = 0xFFFF
            def c16(c, data):
                for b in data:
                    c ^= b
                    for _ in range(8):
                        c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
                return c
            c = c16(c, self.uuid)
            c = c16(c, struct.pack("<I", g))
            c = c16(c, bytes(self.d[o:o + 30]))
            if self.is64 and self.dsz > 32:
                c = c16(c, bytes(self.d[o + 32:o + self.dsz]))
            struct.pack_into("<H", self.d, o + 30, c & 0xFFFF)

    def fix_bitmaps_csum(self, g):
        if not self.meta:
            return
        bb, ib, it = self.gd(g)
        o = self.gd_off(g)
        c = crc32c(self.seed, bytes(self.d[bb * self.bs:bb * self.bs + self.bpg // 8]))
        struct.pack_into("<H", self.d, o + 24, c & 0xFFFF)
        if self.dsz >= 64: struct.pack_into("<H", self.d, o + 56, c >> 16)
        c = crc32c(self.seed, bytes(self.d[ib * self.bs:ib * self.bs + self.ipg // 8]))
        struct.pack_into("<H", self.d, o + 26, c & 0xFFFF)
        if self.dsz >= 64: struct.pack_into("<H", self.d, o + 58, c >> 16)
        self.fix_gd(g)

    def ino_off(self, ino):
        g, i = (ino - 1) // self.ipg, (ino - 1) % self.ipg
        return self.gd(g)[2] * self.bs + i * self.isz

    def inode(self, ino):
        o = self.ino_off(ino)
        return bytearray(self.d[o:o + self.isz])

    def put_inode(self, ino, raw, fix=True):
        raw = bytearray(raw)
        if fix and self.meta:
            t = bytearray(raw)
            t[0x7C] = t[0x7D] = 0
            hi = self.isz > 128 and struct.unpack_from("<H", raw, 128)[0] >= 4
            if hi: t[0x82] = t[0x83] = 0
            gen = struct.unpack_from("<I", raw, 100)[0]
            c = crc32c(self.seed, struct.pack("<I", ino))
            c = crc32c(c, struct.pack("<I", gen))
            c = crc32c(c, bytes(t))
            struct.pack_into("<H", raw, 0x7C, c & 0xFFFF)
            if hi: struct.pack_into("<H", raw, 0x82, c >> 16)
        o = self.ino_off(ino)
        self.d[o:o + self.isz] = raw

    # ---- ブロック列
    def iblocks(self, ino):
        """(論理 -> 物理) の物理ブロック一覧 (論理順。穴は 0)"""
        raw = self.inode(ino)
        flags = struct.unpack_from("<I", raw, 32)[0]
        out = {}
        if flags & 0x80000:
            self._ext(bytes(raw[40:100]), out)
        else:
            ib = struct.unpack_from("<15I", raw, 40)
            for i in range(12):
                if ib[i]: out[i] = ib[i]
            per = self.bs // 4
            def ind(b, level, base):
                if not b: return
                data = struct.unpack_from("<%dI" % per, self.d, b * self.bs)
                span = per ** (level - 1)
                for i, x in enumerate(data):
                    if not x: continue
                    if level == 1: out[base + i] = x
                    else: ind(x, level - 1, base + i * span)
            ind(ib[12], 1, 12); ind(ib[13], 2, 12 + per); ind(ib[14], 3, 12 + per + per * per)
        return out

    def _ext(self, node, out):
        magic, ent, mx, depth = struct.unpack_from("<HHHH", node, 0)
        for i in range(ent):
            e = node[12 + 12 * i:24 + 12 * i]
            if depth == 0:
                lb, ln, hi, lo = struct.unpack("<IHHI", e)
                ln = ln - 32768 if ln > 32768 else ln
                for k in range(ln): out[lb + k] = ((hi << 32) | lo) + k
            else:
                lb, lo, hi = struct.unpack_from("<IIH", e, 0)
                b = (hi << 32) | lo
                self._ext(bytes(self.d[b * self.bs:(b + 1) * self.bs]), out)

    def dir_entries(self, ino):
        """[(エントリのファイル内オフセット, inode, rec_len, name, file_type)]"""
        res = []
        raw = self.inode(ino)
        if struct.unpack_from("<I", raw, 32)[0] & 0x10000000:   # inline_data: i_block[4:60] にエントリ
            base = self.ino_off(ino) + 40 + 4
            off = 0
            while off < 56:
                t, rec, nl, ft = struct.unpack_from("<IHBB", self.d, base + off)
                if rec < 12 or off + rec > 56: break
                if t:
                    res.append((base + off, t, rec, bytes(self.d[base + off + 8:base + off + 8 + nl]).decode("latin1"), ft, -1))
                off += rec
            return res
        blocks = self.iblocks(ino)
        for lb in sorted(blocks):
            base = blocks[lb] * self.bs
            off = 0
            limit = self.bs - (12 if self.meta else 0)
            while off < limit:
                t, rec, nl, ft = struct.unpack_from("<IHBB", self.d, base + off)
                if rec < 12: break
                if t:
                    res.append((base + off, t, rec, bytes(self.d[base + off + 8:base + off + 8 + nl]).decode("latin1"), ft, lb))
                off += rec
        return res

    def lookup(self, path):
        ino = 2
        for part in [p for p in path.split("/") if p]:
            for (_, t, _, nm, _, _) in self.dir_entries(ino):
                if nm == part:
                    ino = t
                    break
            else:
                raise KeyError(path)
        return ino

    def fix_dir_block_csum(self, ino, lb):
        if not self.meta or lb < 0: return
        raw = self.inode(ino)
        gen = struct.unpack_from("<I", raw, 100)[0]
        base = self.iblocks(ino)[lb] * self.bs
        c = crc32c(self.seed, struct.pack("<I", ino))
        c = crc32c(c, struct.pack("<I", gen))
        c = crc32c(c, bytes(self.d[base:base + self.bs - 12]))
        struct.pack_into("<I", self.d, base + self.bs - 4, c)

    def bit_block(self, b):
        g = (b - self.fdb) // self.bpg
        bb = self.gd(g)[0]
        i = (b - self.fdb) % self.bpg
        return g, bb * self.bs + i // 8, i & 7

    def bit_inode(self, ino):
        g = (ino - 1) // self.ipg
        ib = self.gd(g)[1]
        i = (ino - 1) % self.ipg
        return g, ib * self.bs + i // 8, i & 7


def make_image(path, kind, size_mb, bs, tree):
    with open(path, "wb") as f:
        f.truncate(size_mb * 1024 * 1024)
    cmd = ["mke2fs", "-q", "-F", "-b", str(bs), "-d", tree, "-L", "dtvol"]
    if kind == "ext2":
        cmd += ["-t", "ext2"]
    elif kind == "ext3":
        cmd += ["-t", "ext3"]
    elif kind == "ext4":
        cmd += ["-t", "ext4"]
    elif kind == "ext4-nocsum":
        cmd += ["-t", "ext4", "-O", "^metadata_csum,uninit_bg,^64bit"]
    elif kind == "ext4-inline":
        cmd += ["-t", "ext4", "-O", "inline_data,^metadata_csum,64bit", "-I", "256"]
    elif kind == "ext4-csum-small-inode":
        cmd += ["-t", "ext4", "-O", "metadata_csum,^has_journal", "-I", "128"]
    cmd += [path]
    p = run(cmd)
    assert p.returncode == 0, p.stdout
    # mke2fs が付ける「最後にマウントした時刻」等のため e2fsck -fn で確認 (pristine は呼び出し側)


def make_tree(root):
    shutil.rmtree(root, ignore_errors=True)
    os.makedirs(root + "/dir1/sub1/sub2")
    os.makedirs(root + "/dir2")
    os.makedirs(root + "/empty")
    rnd = random.Random(11)
    def wr(rel, n):
        with open(os.path.join(root, rel), "wb") as f:
            f.write(rnd.randbytes(n))
    wr("small.txt", 100)
    wr("tiny.txt", 5)
    wr("zero.txt", 0)
    wr("dir1/a.bin", 70000)
    wr("dir1/sub1/b.bin", 3 * 1024 * 1024 + 123)       # 間接ブロック / 複数 extent
    wr("dir1/sub1/sub2/deep.txt", 10)
    wr("dir2/c.bin", 1234)
    wr("big.bin", 6 * 1024 * 1024)
    for i in range(120):
        wr("dir2/file_with_a_longish_name_%03d.txt" % i, 20 + i)
    os.symlink("small.txt", root + "/fast_link")
    os.symlink("x" * 200, root + "/slow_link")
    os.link(root + "/small.txt", root + "/hardlink_to_small")
    # 疎ファイル
    with open(root + "/sparse.bin", "wb") as f:
        f.seek(5 * 1024 * 1024)
        f.write(b"tail")


def e2fsck(path):
    p = run(["e2fsck", "-fn", path])
    txt = p.stdout.decode("utf-8", "replace")
    # 主スーパーブロックが壊れていてバックアップで救われた場合も、終了コード 0 でも問題として表示される
    said = any(k in txt for k in ("trying backup blocks", "Superblock invalid", "Group descriptors look bad", "checksums are invalid", "IGNORED", "likely to be corrupt"))
    return bool(p.returncode & 4) or p.returncode >= 8 or said, txt, p.returncode


def mutations(x):
    M = []

    def m(name, allow=None):
        def deco(fn):
            M.append((name, fn, allow))
            return fn
        return deco

    sm = x.lookup("small.txt")
    big = x.lookup("big.bin")
    deep = x.lookup("dir1/sub1/b.bin")
    d2 = x.lookup("dir2")
    d1 = x.lookup("dir1")
    sub1 = x.lookup("dir1/sub1")
    fl = x.lookup("fast_link")

    # ---- スーパーブロック
    @m("sb_magic")
    def _(y): y.d[1024 + 0x38] ^= 0xFF
    @m("sb_log_block_size")
    def _(y): struct.pack_into("<I", y.d, 1024 + 24, 9); y.fix_sb()
    @m("sb_inodes_count")
    def _(y): struct.pack_into("<I", y.d, 1024, y.inodes + 8); y.fix_sb()
    @m("sb_blocks_per_group_zero")
    def _(y): struct.pack_into("<I", y.d, 1024 + 32, 0); y.fix_sb()
    @m("sb_first_data_block")
    def _(y): struct.pack_into("<I", y.d, 1024 + 20, 5); y.fix_sb()
    @m("sb_checksum")
    def _(y):
        if not y.meta: raise Skip()
        y.d[1024 + 0x3FC] ^= 0xFF
    @m("sb_error_state", allow=("fp",))
    def _(y): struct.pack_into("<H", y.d, 1024 + 0x3A, 2); y.fix_sb()
    @m("sb_not_clean_state", allow=("fp",))
    def _(y): struct.pack_into("<H", y.d, 1024 + 0x3A, 0); y.fix_sb()
    @m("sb_needs_recovery_flag", allow=("fp",))
    def _(y):
        if not (y.compat & 4): raise Skip()
        struct.pack_into("<I", y.d, 1024 + 0x60, y.incompat | 4); y.fix_sb()
    @m("sb_blocks_count_bigger_than_device")
    def _(y): struct.pack_into("<I", y.d, 1024 + 4, struct.unpack_from("<I", y.d, 1024 + 4)[0] * 4); y.fix_sb()
    # ---- グループ記述子
    @m("gd_block_bitmap_out_of_range")
    def _(y):
        struct.pack_into("<I", y.d, y.gd_off(0), 0x7FFFFF00); y.fix_gd(0)
    @m("gd_inode_table_out_of_range")
    def _(y):
        struct.pack_into("<I", y.d, y.gd_off(0) + 8, y.blocks - 3); y.fix_gd(0)
    @m("gd_bitmaps_overlap")
    def _(y):
        bb, ib, it = y.gd(0)
        struct.pack_into("<I", y.d, y.gd_off(0) + 4, bb & 0xFFFFFFFF); y.fix_gd(0)
    @m("gd_checksum")
    def _(y):
        if not (y.meta or y.gdt): raise Skip()
        y.d[y.gd_off(0) + 30] ^= 0xFF
    @m("gd_free_counts")
    def _(y):
        struct.pack_into("<H", y.d, y.gd_off(0) + 12, 3); y.fix_gd(0)
    # ---- ビットマップ
    @m("block_bitmap_clear_used")
    def _(y):
        b = y.iblocks(big)[0]
        g, o, bit = y.bit_block(b); y.d[o] &= ~(1 << bit); y.fix_bitmaps_csum(g)
    @m("block_bitmap_set_free_leak", allow=("fp",))
    def _(y):
        # 使われていないブロックのビットを立てる (最後のグループの終端)
        used = set()
        for ino in range(1, y.inodes + 1):
            pass
        g = 0
        bb = y.gd(g)[0]
        for i in range(y.bpg - 1, 0, -1):
            if not (y.d[bb * y.bs + i // 8] >> (i & 7)) & 1:
                y.d[bb * y.bs + i // 8] |= 1 << (i & 7); y.fix_bitmaps_csum(g); return
        raise Skip()
    @m("inode_bitmap_clear_used")
    def _(y):
        g, o, bit = y.bit_inode(sm); y.d[o] &= ~(1 << bit); y.fix_bitmaps_csum(g)
    @m("inode_bitmap_set_unused_leak", allow=("fp",))
    def _(y):
        ino = y.inodes - 5 if y.inodes - 5 > 20 else 40
        g, o, bit = y.bit_inode(ino); y.d[o] |= 1 << bit; y.fix_bitmaps_csum(g)
    @m("block_bitmap_csum_stale")
    def _(y):
        if not y.meta: raise Skip()
        g = 0; bb = y.gd(g)[0]
        y.d[bb * y.bs + 3] ^= 0x01   # ビットマップだけ変えてチェックサムは更新しない。(使用中のビットなら e2fsck も見つける)
    # ---- inode
    @m("inode_mode_invalid")
    def _(y):
        r = y.inode(sm); struct.pack_into("<H", r, 0, 0x3000 | 0o644); y.put_inode(sm, r)
    @m("inode_links_plus_one")
    def _(y):
        r = y.inode(sm); struct.pack_into("<H", r, 26, 5); y.put_inode(sm, r)
    @m("inode_links_zero_but_referenced")
    def _(y):
        r = y.inode(big); struct.pack_into("<H", r, 26, 0); y.put_inode(big, r)
    @m("inode_dtime_set", allow=("fp",))
    def _(y):
        r = y.inode(sm); struct.pack_into("<I", r, 20, 1234567); y.put_inode(sm, r)
    @m("inode_size_too_small")
    def _(y):
        r = y.inode(big); struct.pack_into("<I", r, 4, 1000); y.put_inode(big, r)
    @m("inode_size_bigger_ok(valid)")
    def _(y):
        r = y.inode(big); struct.pack_into("<I", r, 4, struct.unpack_from("<I", r, 4)[0] + 8192); y.put_inode(big, r)
    @m("inode_i_blocks_wrong")
    def _(y):
        r = y.inode(big); struct.pack_into("<I", r, 28, struct.unpack_from("<I", r, 28)[0] + 8); y.put_inode(big, r)
    @m("inode_csum_stale")
    def _(y):
        if not y.meta: raise Skip()
        o = y.ino_off(sm); y.d[o + 0x7C] ^= 0xFF
    @m("inode_block_out_of_range")
    def _(y):
        r = y.inode(sm)
        if struct.unpack_from("<I", r, 32)[0] & 0x80000:
            # extent の開始ブロックを範囲外に
            struct.pack_into("<I", r, 40 + 12 + 8, y.blocks + 100)
        else:
            struct.pack_into("<I", r, 40, y.blocks + 100)
        y.put_inode(sm, r)
    @m("inode_dup_block")
    def _(y):
        a = y.inode(sm); b = y.inode(x.lookup("dir2/c.bin"))
        o = y.ino_off(sm)
        if struct.unpack_from("<I", a, 32)[0] & 0x80000:
            a[40 + 12 + 8:40 + 12 + 12] = b[40 + 12 + 8:40 + 12 + 12]
        else:
            a[40:44] = b[40:44]
        y.put_inode(sm, a)
    @m("inode_extent_magic")
    def _(y):
        r = y.inode(big)
        if not (struct.unpack_from("<I", r, 32)[0] & 0x80000): raise Skip()
        struct.pack_into("<H", r, 40, 0x1234); y.put_inode(big, r)
    @m("inode_extent_len_huge")
    def _(y):
        r = y.inode(big)
        if not (struct.unpack_from("<I", r, 32)[0] & 0x80000): raise Skip()
        struct.pack_into("<H", r, 40 + 12 + 4, 32768); y.put_inode(big, r)
    @m("inode_indirect_pointer_bad")
    def _(y):
        r = y.inode(deep)
        if struct.unpack_from("<I", r, 32)[0] & 0x80000: raise Skip()
        struct.pack_into("<I", r, 40 + 48, y.blocks + 77); y.put_inode(deep, r)
    @m("inode_file_acl_bad")
    def _(y):
        r = y.inode(sm); struct.pack_into("<I", r, 104, y.blocks + 5); y.put_inode(sm, r)
    @m("inode_root_not_dir")
    def _(y):
        r = y.inode(2); struct.pack_into("<H", r, 0, 0x8000 | 0o644); y.put_inode(2, r)
    @m("inode_dir_size_wrong")
    def _(y):
        r = y.inode(d2); struct.pack_into("<I", r, 4, struct.unpack_from("<I", r, 4)[0] + y.bs); y.put_inode(d2, r)
    # ---- ディレクトリ
    def entry(y, d, name):
        for e in y.dir_entries(d):
            if e[3] == name: return e
        raise KeyError(name)
    @m("dir_rec_len_zero")
    def _(y):
        e = entry(y, d2, "c.bin"); struct.pack_into("<H", y.d, e[0] + 4, 0); y.fix_dir_block_csum(d2, e[5])
    @m("dir_rec_len_unaligned")
    def _(y):
        e = entry(y, d2, "c.bin"); struct.pack_into("<H", y.d, e[0] + 4, e[2] - 1); y.fix_dir_block_csum(d2, e[5])
    @m("dir_entry_inode_unused")
    def _(y):
        e = entry(y, d2, "c.bin"); struct.pack_into("<I", y.d, e[0], y.inodes - 3); y.fix_dir_block_csum(d2, e[5])
    @m("dir_entry_inode_out_of_range")
    def _(y):
        e = entry(y, d2, "c.bin"); struct.pack_into("<I", y.d, e[0], y.inodes + 50); y.fix_dir_block_csum(d2, e[5])
    @m("dir_entry_name_slash")
    def _(y):
        e = entry(y, d2, "c.bin"); y.d[e[0] + 8] = ord("/"); y.fix_dir_block_csum(d2, e[5])
    @m("dir_duplicate_name")
    def _(y):
        e1 = entry(y, d2, "c.bin"); e2 = [e for e in y.dir_entries(d2) if e[3].startswith("file_with") and len(e[3]) == len("file_with_a_longish_name_000.txt")][0]
        y.d[e2[0] + 8:e2[0] + 8 + 5] = b"c.bin"; y.d[e2[0] + 6] = 5
        # 名前の長さを変えたら重複のため rec_len は変えずに残す
        y.fix_dir_block_csum(d2, e2[5])
    @m("dir_dot_wrong")
    def _(y):
        e = y.dir_entries(d1)[0]; struct.pack_into("<I", y.d, e[0], 2); y.fix_dir_block_csum(d1, e[5])
    @m("dir_dotdot_wrong")
    def _(y):
        e = y.dir_entries(sub1)[1]; struct.pack_into("<I", y.d, e[0], d2); y.fix_dir_block_csum(sub1, e[5])
    @m("dir_filetype_mismatch")
    def _(y):
        if not (y.incompat & 2): raise Skip()
        e = entry(y, d2, "c.bin"); y.d[e[0] + 7] = 2; y.fix_dir_block_csum(d2, e[5])
    @m("dir_entry_removed_unattached")
    def _(y):
        e = entry(y, d2, "c.bin"); struct.pack_into("<I", y.d, e[0], 0); y.fix_dir_block_csum(d2, e[5])
    @m("dir_subdir_unlinked_unconnected")
    def _(y):
        e = entry(y, d1, "sub1"); struct.pack_into("<I", y.d, e[0], 0); y.fix_dir_block_csum(d1, e[5])
    @m("dir_lost_found_removed")
    def _(y):
        e = entry(y, 2, "lost+found"); struct.pack_into("<I", y.d, e[0], 0); y.fix_dir_block_csum(2, e[5])
    @m("dir_hole")
    def _(y):
        r = y.inode(d2)
        if struct.unpack_from("<I", r, 32)[0] & 0x80000: raise Skip()
        struct.pack_into("<I", r, 40, 0); y.put_inode(d2, r)
    @m("dir_csum_stale")
    def _(y):
        if not y.meta: raise Skip()
        e = entry(y, d2, "c.bin"); y.d[e[0] + 8] ^= 0x01   # 名前を 1 ビット変える (チェックサム更新せず)
    @m("dir_loop_parent_swap")
    def _(y):
        # sub1 のエントリを sub2 の中に移したように見せる: sub1 の '..' を sub2 にする + d1 のエントリ削除
        sub2 = y.lookup("dir1/sub1/sub2")
        e = entry(y, d1, "sub1"); struct.pack_into("<I", y.d, e[0], 0); y.fix_dir_block_csum(d1, e[5])
        # sub2 の中に sub1 へのエントリを作るのは複雑なので、'..' を sub2 にするだけ
        e2 = y.dir_entries(sub1)[1]; struct.pack_into("<I", y.d, e2[0], sub2); y.fix_dir_block_csum(sub1, e2[5])
    # ---- ジャーナル
    @m("journal_magic")
    def _(y):
        if not (y.compat & 4): raise Skip()
        b = y.iblocks(8)[0]; y.d[b * y.bs] ^= 0xFF
    @m("journal_dirty_start", allow=("fp",))
    def _(y):
        if not (y.compat & 4): raise Skip()
        b = y.iblocks(8)[0]; struct.pack_into(">I", y.d, b * y.bs + 28, 5)
    @m("journal_inode_unused", allow=("fp",))   # ext3 では e2fsck がスーパーブロック内の予備 (s_jnl_blocks) で直せるため問題なしと言う
    def _(y):
        if not (y.compat & 4): raise Skip()
        r = y.inode(8); struct.pack_into("<H", r, 26, 0); y.put_inode(8, r)
    return M


def run_ext(cli, res: Result, quick=False):
    d = work_dir("ext")
    tree = os.path.join(d, "tree")
    make_tree(tree)
    configs = [("ext2", 1024, 24), ("ext2", 4096, 24), ("ext3", 4096, 32), ("ext4", 4096, 40), ("ext4-nocsum", 1024, 40),
               ("ext4-inline", 4096, 40), ("ext4-csum-small-inode", 1024, 30)]
    if quick:
        configs = [("ext2", 1024, 24), ("ext4", 4096, 40), ("ext4-nocsum", 1024, 40)]
    for kind, bs, mb in configs:
        base = os.path.join(d, "base.img")
        make_image(base, kind, mb, bs, tree)
        orig = open(base, "rb").read()
        tag = "%s/%d" % (kind, bs)
        fsname = "ext2" if kind == "ext2" else "ext3" if kind == "ext3" else "ext4"
        rp, rt, rc = e2fsck(base)
        c, j = ours(cli, base, fs=fsname)
        res.add(tag + " pristine", rp, c in (1, 2), rt, codes(j))
        x0 = Ext(orig)
        for name, fn, allow in mutations(x0):
            y = Ext(orig)
            try:
                fn(y)
            except Skip:
                continue
            except KeyError as e:
                print("   (mutation %s not applicable: %s)" % (name, e))
                continue
            img = os.path.join(d, "m.img")
            with open(img, "wb") as fh:
                fh.write(y.d)
            rp, rt, rc = e2fsck(img)
            c, j = ours(cli, img, fs=fsname)
            valid = name.endswith("(valid)")
            res.add("%s %s" % (tag, name), rp, c in (1, 2, 4), rt, codes(j), allow)
            if c == 10 or j.get("verdict") == "crash":
                res.bad += 1
                print("   CRASH/IOERR", j.get("stderr", ""))
        img = os.path.join(d, "t.img")
        with open(img, "wb") as fh:
            fh.write(orig[: len(orig) * 3 // 4])
        rp, rt, rc = e2fsck(img)
        c, j = ours(cli, img, fs=fsname)
        res.add(tag + " truncated_device", rp, c in (1, 2, 4), rt, codes(j))
    return res
