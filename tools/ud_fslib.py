# SPDX-License-Identifier: Apache-2.0
"""削除ファイルの検証用に、本物の mkfs.* が作った空の FS へファイルを書き込み・削除する最小の書き手 (FAT12/16/32・exFAT)。

本物の mkfs.fat / mkfs.exfat がレイアウトを決め、ここはファイルの追加 (連続した最初の空きクラスタ = 実際のドライバと同じ最初の空きからの割り当て)
と削除 (FAT: 先頭バイトを 0xE5 に・FAT の連鎖をゼロに / exFAT: エントリの InUse ビットを落とす・ビットマップを空けに) だけを行う。
作ったイメージが正しいことは fsck.fat -n / fsck.exfat -n で確かめる (テスト側)。mcopy / mdel (mtools) の出力とは別の経路で、ローカル (mtools の無い環境) でも動く。
"""
import mmap
import struct


def u16(b, o): return struct.unpack_from("<H", b, o)[0]
def u32(b, o): return struct.unpack_from("<I", b, o)[0]


def to_dos(ts):
    import time
    t = time.gmtime(ts)
    return ((t.tm_year - 1980) << 9 | t.tm_mon << 5 | t.tm_mday), (t.tm_hour << 11 | t.tm_min << 5 | t.tm_sec // 2)


class _Img:
    def __init__(self, path):
        self.f = open(path, "r+b")
        self.m = mmap.mmap(self.f.fileno(), 0)

    def close(self):
        self.m.flush()
        self.m.close()
        self.f.close()


# --------------------------------------------------------------------------------------------- FAT
def _fat_checksum(n11):
    s = 0
    for c in n11:
        s = (((s & 1) << 7) + (s >> 1) + c) & 0xFF
    return s


class Fat(_Img):
    def __init__(self, path):
        super().__init__(path)
        b = self.m[:512]
        self.bps = u16(b, 11); self.spc = b[13]; self.rsvd = u16(b, 14); self.nfats = b[16]
        self.root_ents = u16(b, 17)
        tot = u16(b, 19) or u32(b, 32)
        self.fatsz = u16(b, 22) or u32(b, 36)
        self.root_clus = u32(b, 44)
        self.fat_off = self.rsvd * self.bps
        self.root_off = self.fat_off + self.nfats * self.fatsz * self.bps
        root_secs = (self.root_ents * 32 + self.bps - 1) // self.bps
        self.data_off = self.root_off + root_secs * self.bps
        self.cbytes = self.spc * self.bps
        self.clusters = (tot - self.data_off // self.bps) // self.spc
        self.bits = 32 if (self.root_ents == 0 and u16(b, 22) == 0) else (12 if self.clusters < 4085 else 16)
        self.fat = bytearray(self.m[self.fat_off:self.fat_off + self.fatsz * self.bps])
        self.eoc = {12: 0xFFF, 16: 0xFFFF, 32: 0x0FFFFFFF}[self.bits]
        self.tree = {}   # パス (タプル) → 先頭クラスタ (ディレクトリ)

    def get(self, c):
        if self.bits == 12:
            v = u16(self.fat, c + c // 2)
            return v >> 4 if c & 1 else v & 0xFFF
        if self.bits == 16:
            return u16(self.fat, c * 2)
        return u32(self.fat, c * 4) & 0x0FFFFFFF

    def set(self, c, v):
        if self.bits == 12:
            o = c + c // 2
            old = u16(self.fat, o)
            new = (old & 0x000F) | (v << 4) if c & 1 else (old & 0xF000) | v
            struct.pack_into("<H", self.fat, o, new)
        elif self.bits == 16:
            struct.pack_into("<H", self.fat, c * 2, v)
        else:
            old = u32(self.fat, c * 4)
            struct.pack_into("<I", self.fat, c * 4, (old & 0xF0000000) | v)

    def alloc(self, n):
        """最初の連続した空き n クラスタ"""
        run = 0
        for c in range(2, self.clusters + 2):
            if self.get(c) == 0:
                run += 1
                if run == n:
                    first = c - n + 1
                    for k in range(n):
                        self.set(first + k, first + k + 1 if k + 1 < n else self.eoc)
                    return first
            else:
                run = 0
        raise RuntimeError("no space")

    def coff(self, c):
        return self.data_off + (c - 2) * self.cbytes

    def flush(self):
        if self.bits == 32:   # FSInfo の空きクラスタ数 (fsck.fat の指摘を避ける)
            fsinfo = u16(self.m, 48) * self.bps
            free = sum(1 for c in range(2, self.clusters + 2) if self.get(c) == 0)
            struct.pack_into("<I", self.m, fsinfo + 488, free)
        for i in range(self.nfats):
            o = self.fat_off + i * self.fatsz * self.bps
            self.m[o:o + len(self.fat)] = bytes(self.fat)
        self.m.flush()

    def _is_link(self, nx):
        return 2 <= nx < {12: 0xFF8, 16: 0xFFF8, 32: 0x0FFFFFF8}[self.bits]

    # ディレクトリ: スロットの一覧 (バイト位置のリスト)
    def _dir_slots(self, dirc):
        if dirc is None and self.bits != 32:
            return [self.root_off + i * 32 for i in range(self.root_ents)]
        c = dirc if dirc is not None else self.root_clus
        slots = []
        while True:
            slots += [self.coff(c) + i * 32 for i in range(self.cbytes // 32)]
            nx = self.get(c)
            if self._is_link(nx):
                c = nx
            else:
                break
        return slots

    def _extend_dir(self, dirc):
        c = dirc if dirc is not None else self.root_clus
        while True:
            nx = self.get(c)
            if self._is_link(nx):
                c = nx
            else:
                break
        n = self.alloc(1)
        self.m[self.coff(n):self.coff(n) + self.cbytes] = bytes(self.cbytes)
        self.set(c, n)

    def _find_free(self, dirc, need):
        while True:
            slots = self._dir_slots(dirc)
            for idx, o in enumerate(slots):
                if self.m[o] == 0x00:   # 終端。0xE5 の再利用は避ける (削除したファイルのエントリを残す)
                    if len(slots) - idx >= need:
                        return slots[idx:idx + need]
                    break
            if dirc is None and self.bits != 32:
                raise RuntimeError("root directory full")
            self._extend_dir(dirc)

    @staticmethod
    def _alias(name, taken):
        base, _, ext = name.rpartition(".") if "." in name else (name, "", "")
        up = lambda s: "".join(c if (c.isascii() and (c.isalnum() or c in "$%'-_@~`!(){}^#&")) else "_" for c in s.upper())
        b8, e3 = up(base)[:8], up(ext)[:3]
        fits = (name.upper() == name and len(base) <= 8 and len(ext) <= 3 and up(base) == base and up(ext) == ext and name.isascii())
        if fits:
            return (b8.ljust(8) + e3.ljust(3)).encode("ascii"), 0, False
        n = 1
        while True:
            tail = "~%d" % n
            alias = (b8[:8 - len(tail)] + tail).ljust(8) + e3.ljust(3)
            if alias not in taken:
                taken.add(alias)
                return alias.encode("ascii"), 0, True
            n += 1

    def _lookup_dir(self, parts):
        if not parts:
            return None
        return self.tree[tuple(parts)]

    def mkdir(self, parts, mtime=1700000000):
        parts = tuple(parts)
        parent = self.tree.get(parts[:-1]) if len(parts) > 1 else None
        c = self.alloc(1)
        self.m[self.coff(c):self.coff(c) + self.cbytes] = bytes(self.cbytes)
        d, t = to_dos(mtime)
        for i, nm in enumerate((b".          ", b"..         ")):
            ent = bytearray(32)
            ent[0:11] = nm
            ent[11] = 0x10
            struct.pack_into("<H", ent, 20, (c if i == 0 else (parent or 0)) >> 16)
            struct.pack_into("<HH", ent, 22, t, d)
            struct.pack_into("<H", ent, 26, (c if i == 0 else (parent or 0)) & 0xFFFF)
            o = self.coff(c) + i * 32
            self.m[o:o + 32] = bytes(ent)
        self._add_entry(parent, parts[-1], 0x10, c, 0, mtime)
        self.tree[parts] = c

    def _add_entry(self, dirc, name, attr, first, size, mtime, nt=0):
        taken = set()
        for o in self._dir_slots(dirc):
            if self.m[o] == 0x00:
                break
            taken.add(bytes(self.m[o:o + 11]).decode("latin-1"))
        alias, _, need_lfn = self._alias(name, taken)
        u = name.encode("utf-16-le")
        nparts = (len(name) + 12) // 13 if need_lfn else 0
        slots = self._find_free(dirc, nparts + 1)
        d, t = to_dos(mtime)
        if need_lfn:
            chk = _fat_checksum(alias)
            chars = list(struct.unpack("<%dH" % (len(u) // 2), u)) + [0]
            chars += [0xFFFF] * (13 * nparts - len(chars))
            for k in range(nparts):
                ord_ = nparts - k
                part = chars[13 * (ord_ - 1):13 * ord_]
                ent = bytearray(32)
                ent[0] = ord_ | (0x40 if k == 0 else 0)
                ent[11] = 0x0F
                ent[13] = chk
                for i in range(5): struct.pack_into("<H", ent, 1 + 2 * i, part[i])
                for i in range(6): struct.pack_into("<H", ent, 14 + 2 * i, part[5 + i])
                for i in range(2): struct.pack_into("<H", ent, 28 + 2 * i, part[11 + i])
                self.m[slots[k]:slots[k] + 32] = bytes(ent)
        ent = bytearray(32)
        ent[0:11] = alias
        ent[11] = attr
        ent[12] = nt
        struct.pack_into("<H", ent, 14, t); struct.pack_into("<H", ent, 16, d)
        struct.pack_into("<H", ent, 20, first >> 16 if self.bits == 32 else 0)
        struct.pack_into("<HH", ent, 22, t, d)
        struct.pack_into("<H", ent, 26, first & 0xFFFF)
        struct.pack_into("<I", ent, 28, size)
        self.m[slots[-1]:slots[-1] + 32] = bytes(ent)
        return slots

    def add_file(self, path, data, mtime=1700000000, nt=0):
        parts = [p for p in path.split("/") if p]
        for i in range(1, len(parts)):
            if tuple(parts[:i]) not in self.tree:
                self.mkdir(parts[:i], mtime)
        parent = self.tree[tuple(parts[:-1])] if len(parts) > 1 else None
        n = (len(data) + self.cbytes - 1) // self.cbytes
        first = self.alloc(n) if n else 0
        if n:
            o = self.coff(first)
            self.m[o:o + len(data)] = data
        self._add_entry(parent, parts[-1], 0x20, first, len(data), mtime, nt)

    def _find_entry(self, parts):
        """パスの最後のエントリの (短い名前のスロットのバイト位置, LFN のスロットの位置の列, 先頭クラスタ)"""
        parent = self.tree[tuple(parts[:-1])] if len(parts) > 1 else None
        want = parts[-1]
        slots = self._dir_slots(parent)
        lfn_slots, lfn_chars = [], []
        for o in slots:
            e = bytes(self.m[o:o + 32])
            if e[0] == 0x00:
                break
            if e[0] == 0xE5:
                lfn_slots, lfn_chars = [], []
                continue
            if e[11] & 0x3F == 0x0F:
                lfn_slots.append(o)
                lfn_chars.append(e)
                continue
            name = None
            if lfn_chars:
                u = b""
                for le in reversed(lfn_chars):
                    u += le[1:11] + le[14:26] + le[28:32]
                s = u.decode("utf-16-le", "replace")
                name = s.split("\x00")[0].split("￿")[0]
            else:
                b = e[0:8].decode("latin-1").rstrip(" "); x = e[8:11].decode("latin-1").rstrip(" ")
                name = b + ("." + x if x else "")
            if name == want:
                first = u16(e, 26) | (u16(e, 20) << 16 if self.bits == 32 else 0)
                return o, lfn_slots, first
            lfn_slots, lfn_chars = [], []
        raise KeyError(want)

    def delete(self, path):
        parts = [p for p in path.split("/") if p]
        o, lfn, first = self._find_entry(parts)
        # FAT の連鎖をゼロに (実際のドライバと同じ)
        c = first
        while c >= 2:
            nx = self.get(c)
            self.set(c, 0)
            if self.bits == 12: end = nx >= 0xFF8 or nx < 2
            elif self.bits == 16: end = nx >= 0xFFF8 or nx < 2
            else: end = nx >= 0x0FFFFFF8 or nx < 2
            if end:
                break
            c = nx
        for s in lfn + [o]:
            self.m[s] = 0xE5

    def close(self):
        self.flush()
        super().close()


# --------------------------------------------------------------------------------------------- exFAT
def _ex_set_checksum(entries):
    chk = 0
    for i, b in enumerate(b"".join(entries)):
        if i in (2, 3):
            continue
        chk = (((chk << 15) | (chk >> 1)) + b) & 0xFFFF
    return chk


def _ex_name_hash(name):
    h = 0
    for b in name.upper().encode("utf-16-le"):
        h = (((h << 15) | (h >> 1)) + b) & 0xFFFF
    return h


class ExFat(_Img):
    def __init__(self, path):
        super().__init__(path)
        b = self.m[:512]
        self.bps = 1 << b[108]; self.spc = 1 << b[109]
        self.fat_off = u32(b, 80) * self.bps; self.fat_len = u32(b, 84) * self.bps
        self.heap_off = u32(b, 88) * self.bps; self.clusters = u32(b, 92); self.root = u32(b, 96)
        self.cbytes = self.bps * self.spc
        # 割り当てビットマップ (ルートの 0x81 エントリ)
        bm = None
        for o in self._chain_slots(self.root):
            if self.m[o] == 0x81:
                bm = (u32(self.m, o + 20), struct.unpack_from("<Q", self.m, o + 24)[0])
                break
        self.bm_first, self.bm_len = bm
        self.bm_off = self.coff(self.bm_first)
        self.tree = {}
        self.dirents = {}  # ディレクトリ (タプル) → 先頭クラスタ

    def coff(self, c):
        return self.heap_off + (c - 2) * self.cbytes

    def fget(self, c):
        return u32(self.m, self.fat_off + 4 * c)

    def fset(self, c, v):
        struct.pack_into("<I", self.m, self.fat_off + 4 * c, v)

    def _chain(self, first):
        out, c = [], first
        while 2 <= c < self.clusters + 2 and len(out) < 100000:
            out.append(c)
            nx = self.fget(c)
            if nx == c or nx < 2 or nx >= 0xFFFFFFF7:
                break
            c = nx
        return out

    def _chain_slots(self, first, nocontig=None):
        cl = self._chain(first) if nocontig is None else nocontig
        return [self.coff(c) + i * 32 for c in cl for i in range(self.cbytes // 32)]

    def bit(self, c):
        i = c - 2
        return (self.m[self.bm_off + i // 8] >> (i % 8)) & 1

    def setbit(self, c, v):
        i = c - 2
        o = self.bm_off + i // 8
        if v: self.m[o] |= 1 << (i % 8)
        else: self.m[o] &= ~(1 << (i % 8)) & 0xFF

    def alloc(self, n, fat_chain):
        run = 0
        for c in range(2, self.clusters + 2):
            if not self.bit(c):
                run += 1
                if run == n:
                    first = c - n + 1
                    for k in range(n):
                        self.setbit(first + k, 1)
                        if fat_chain:
                            self.fset(first + k, first + k + 1 if k + 1 < n else 0xFFFFFFFF)
                    return first
            else:
                run = 0
        raise RuntimeError("no space")

    def _dir_chain(self, parts):
        return self._chain(self.root) if not parts else self.tree[tuple(parts)]["clusters"]

    def _slots(self, parts):
        return self._chain_slots(0, self._dir_chain(parts))

    def mkdir(self, parts, mtime=1700000000):
        parts = tuple(parts)
        c = self.alloc(1, True)
        self.m[self.coff(c):self.coff(c) + self.cbytes] = bytes(self.cbytes)
        self._add_set(parts[:-1], parts[-1], 0x10, c, self.cbytes, self.cbytes, 0, mtime)
        self.tree[parts] = {"clusters": [c]}

    def _add_set(self, parent, name, attr, first, size, vdl, nofat, mtime):
        u = name.encode("utf-16-le")
        nname = (len(name) + 14) // 15
        d, t = to_dos(mtime)
        ts = (d << 16) | t
        prim = bytearray(32)
        prim[0] = 0x85; prim[1] = 1 + nname
        struct.pack_into("<H", prim, 4, attr)
        struct.pack_into("<III", prim, 8, ts, ts, ts)
        stream = bytearray(32)
        stream[0] = 0xC0; stream[1] = 0x01 | (0x02 if nofat else 0); stream[3] = len(name)
        struct.pack_into("<H", stream, 4, _ex_name_hash(name))
        struct.pack_into("<Q", stream, 8, vdl)
        struct.pack_into("<I", stream, 20, first)
        struct.pack_into("<Q", stream, 24, size)
        ents = [prim, stream]
        for k in range(nname):
            e = bytearray(32)
            e[0] = 0xC1
            chunk = u[30 * k:30 * (k + 1)]
            e[2:2 + len(chunk)] = chunk
            ents.append(e)
        struct.pack_into("<H", prim, 2, _ex_set_checksum([bytes(e) for e in ents]))
        need = len(ents)
        while True:
            slots = self._slots(parent)
            idx = None
            for i, o in enumerate(slots):
                if self.m[o] == 0x00:
                    idx = i
                    break
            if idx is not None and len(slots) - idx >= need:
                break
            # ディレクトリを 1 クラスタ延ばす
            ch = self._dir_chain(parent)
            n = self.alloc(1, True)
            self.m[self.coff(n):self.coff(n) + self.cbytes] = bytes(self.cbytes)
            self.fset(ch[-1], n)
            if parent:
                self.tree[tuple(parent)]["clusters"].append(n)
            else:
                pass
        for k, e in enumerate(ents):
            o = slots[idx + k]
            self.m[o:o + 32] = bytes(e)

    def add_file(self, path, data, mtime=1700000000, nofat=True, vdl=None):
        parts = [p for p in path.split("/") if p]
        for i in range(1, len(parts)):
            if tuple(parts[:i]) not in self.tree:
                self.mkdir(parts[:i], mtime)
        n = (len(data) + self.cbytes - 1) // self.cbytes
        first = self.alloc(n, not nofat) if n else 0
        if n:
            o = self.coff(first)
            self.m[o:o + len(data)] = data
        self._add_set(parts[:-1], parts[-1], 0x20, first, len(data), len(data) if vdl is None else vdl, 1 if nofat else 0, mtime)

    def delete(self, path):
        parts = [p for p in path.split("/") if p]
        slots = self._slots(parts[:-1])
        want = parts[-1]
        for i, o in enumerate(slots):
            if self.m[o] == 0x00:
                break
            if self.m[o] != 0x85:
                continue
            sec = self.m[o + 1]
            st = o + 32  # slots は連続とは限らないが、クラスタ内では連続
            nm = b""
            nl = self.m[slots[i + 1] + 3]
            for k in range((nl + 14) // 15):
                no = slots[i + 2 + k]
                nm += bytes(self.m[no + 2:no + 32])
            name = nm[:nl * 2].decode("utf-16-le")
            if name != want:
                continue
            first = u32(self.m, slots[i + 1] + 20)
            size = struct.unpack_from("<Q", self.m, slots[i + 1] + 24)[0]
            nofat = self.m[slots[i + 1] + 1] & 2
            n = (size + self.cbytes - 1) // self.cbytes
            for c in range(first, first + n):
                self.setbit(c, 0)
                if not nofat:
                    self.fset(c, 0)
            for k in range(sec + 1):
                self.m[slots[i + k]] &= 0x7F
            return
        raise KeyError(path)

    def close(self):
        super().close()
