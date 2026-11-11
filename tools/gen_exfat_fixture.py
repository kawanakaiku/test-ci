#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""exFAT のテスト用ディスクイメージを作る (読み取りテスト用)。
mkfs.exfat (exfatprogs) で空のボリュームを作り、**Linux の exfat ドライバとは独立に書いた**この Python で、
ファイル/ディレクトリを書き足す (FUSE/mount が使えない環境でも、実ドライバに対する期待値を作れる)。
作った後に fsck.exfat -n で整合性を確認する。使用: gen_exfat_fixture.py <outdir> [name sector_size cluster_size size_mib]...
出力: <outdir>/<name>.img と <name>.manifest。manifest の行 (タブ区切り):
  D <path>                          ディレクトリ
  F <path> <size> <valid> <seed>    ファイル (valid = ValidDataLength: これ以降は 0 として読めること)。内容は pattern(seed, i)
内容の規則 pattern: byte(i) = (i * 31 + seed * 7 + (i >> 8)) & 0xFF
"""
import os, struct, subprocess, sys

def pattern(seed, n):
    return bytes(((i * 31 + seed * 7 + (i >> 8)) & 0xFF) for i in range(n))

class Img:
    def __init__(self, path):
        self.f = open(path, "r+b")
        b = self.rd(0, 512)
        self.sector_shift = b[0x6C]; self.spc_shift = b[0x6D]
        self.sector = 1 << self.sector_shift; self.cluster = self.sector << self.spc_shift
        self.fat_off = struct.unpack_from("<I", b, 0x50)[0] * self.sector
        self.heap_off = struct.unpack_from("<I", b, 0x58)[0] * self.sector
        self.ccount = struct.unpack_from("<I", b, 0x5C)[0]
        self.root = struct.unpack_from("<I", b, 0x60)[0]
        self.rd_bitmap_and_upcase()
    def rd(self, off, n):
        self.f.seek(off); return self.f.read(n)
    def wr(self, off, data):
        self.f.seek(off); self.f.write(data)
    def coff(self, clu): return self.heap_off + (clu - 2) * self.cluster
    def rd_bitmap_and_upcase(self):
        root = self.rd(self.coff(self.root), self.cluster)
        for i in range(0, len(root), 32):
            e = root[i:i + 32]
            if e[0] == 0x81: self.bm_clu = struct.unpack_from("<I", e, 20)[0]; self.bm_len = struct.unpack_from("<Q", e, 24)[0]
            if e[0] == 0x82: uc_clu = struct.unpack_from("<I", e, 20)[0]; uc_len = struct.unpack_from("<Q", e, 24)[0]
            if e[0] == 0x00: break
        self.bm = bytearray(self.rd(self.coff(self.bm_clu), self.bm_len))
        raw = self.rd(self.coff(uc_clu), uc_len)
        words = struct.unpack("<%dH" % (len(raw) // 2), raw)
        up = list(range(0x10000)); i = 0; pos = 0   # 圧縮形式: 0xFFFF n = n 個は恒等
        while i < len(words):
            if words[i] == 0xFFFF and i + 1 < len(words): pos += words[i + 1]; i += 2
            else:
                if pos < 0x10000: up[pos] = words[i]
                pos += 1; i += 1
        self.upcase = up
    def used(self, clu): return (self.bm[(clu - 2) >> 3] >> ((clu - 2) & 7)) & 1
    def alloc(self, n, stride=1):
        """n 個のクラスタを確保。stride == 1 なら連続した空き n 個、stride > 1 なら間を空けて (断片化させて) 取る。確保したクラスタ番号のリスト。"""
        out = []; clu = 2
        while len(out) < n:
            assert clu < self.ccount + 2, "volume full"
            if self.used(clu): out = []; clu += 1; continue
            out.append(clu); clu += stride
            if stride > 1:
                pass
        for c in out: self.bm[(c - 2) >> 3] |= 1 << ((c - 2) & 7)
        return out
    def fat_set(self, clu, val): self.wr(self.fat_off + 4 * clu, struct.pack("<I", val))
    def chain(self, clus):
        for a, b in zip(clus, clus[1:]): self.fat_set(a, b)
        self.fat_set(clus[-1], 0xFFFFFFFF)
    def flush(self):
        self.wr(self.coff(self.bm_clu), bytes(self.bm)); self.f.flush(); self.f.close()

def name_hash(img, name16):
    h = 0
    for ch in name16:
        u = img.upcase[ch]
        for byte in (u & 0xFF, u >> 8):
            h = (((h << 15) | (h >> 1)) & 0xFFFF) + byte & 0xFFFF
    return h

def dos_time(y, mo, d, h, mi, s):
    return ((y - 1980) << 25) | (mo << 21) | (d << 16) | (h << 11) | (mi << 5) | (s // 2)

def entry_set(img, name, attr, first, size, valid, nofat, mtime=(2024, 5, 17, 13, 45, 30)):
    n16 = [int.from_bytes(name.encode("utf-16-le")[i:i + 2], "little") for i in range(0, len(name.encode("utf-16-le")), 2)]
    assert 1 <= len(n16) <= 255
    nname = (len(n16) + 14) // 15
    t = dos_time(*mtime)
    prim = bytearray(32); prim[0] = 0x85; prim[1] = 1 + nname
    struct.pack_into("<H", prim, 4, attr); struct.pack_into("<III", prim, 8, t, t, t)
    prim[21] = 25   # 10ms 単位 (+250ms)
    prim[22] = prim[23] = prim[24] = 0x80   # UTC オフセット: 有効 + 0
    st = bytearray(32); st[0] = 0xC0; st[1] = 0x03 if nofat else 0x01; st[3] = len(n16)
    struct.pack_into("<H", st, 4, name_hash(img, n16)); struct.pack_into("<Q", st, 8, valid)
    struct.pack_into("<I", st, 20, first); struct.pack_into("<Q", st, 24, size)
    ents = [prim, st]
    for k in range(nname):
        e = bytearray(32); e[0] = 0xC1
        for j, ch in enumerate(n16[k * 15:(k + 1) * 15]): struct.pack_into("<H", e, 2 + 2 * j, ch)
        ents.append(e)
    cs = 0
    for ei, e in enumerate(ents):
        for i, b in enumerate(e):
            if ei == 0 and i in (2, 3): continue
            cs = (((cs << 15) | (cs >> 1)) & 0xFFFF) + b & 0xFFFF
    struct.pack_into("<H", prim, 2, cs)
    return b"".join(bytes(e) for e in ents)

class Dir:
    """ディレクトリ: 自身のクラスタ列 (FAT チェーン) とエントリ列を持つ。root は既存のクラスタから始める。"""
    def __init__(self, img, clus, existing=b""):
        self.img = img; self.clus = clus; self.data = bytearray(existing)
    def add(self, ents):
        self.data += ents
    def flush(self):
        need = (len(self.data) + 32 + self.img.cluster - 1) // self.img.cluster   # 終端 (0x00 のエントリ) 分の余裕
        while len(self.clus) < need:
            (c,) = self.img.alloc(1); self.clus.append(c)
        self.img.chain(self.clus)
        buf = bytes(self.data).ljust(len(self.clus) * self.img.cluster, b"\0")
        for i, c in enumerate(self.clus): self.img.wr(self.img.coff(c), buf[i * self.img.cluster:(i + 1) * self.img.cluster])

def populate(path):
    img = Img(path)
    cl = img.cluster
    manifest = []
    # root: 既存エントリ (label/bitmap/upcase) を残して、そこへ追記
    root_raw = img.rd(img.coff(img.root), cl)
    end = 0
    while end < len(root_raw) and root_raw[end * 1:end * 1 + 1] != b"\0" and root_raw[end] != 0: end += 32
    root = Dir(img, [img.root], root_raw[:end])
    dirs = {"": root}

    def put_file(parent, relpath, size, seed, valid=None, frag=False, fill_beyond=0xEE):
        valid = size if valid is None else valid
        nclu = (size + cl - 1) // cl
        content = pattern(seed, valid)
        if size == 0:
            first, nofat = 0, False
        elif frag:
            clus = img.alloc(nclu, stride=2); img.chain(clus); first, nofat = clus[0], False
        else:
            clus = img.alloc(nclu)
            assert clus == list(range(clus[0], clus[0] + nclu)), "contiguous alloc expected"
            first, nofat = clus[0], True
        if size:
            data = content.ljust(nclu * cl, bytes([fill_beyond]))   # valid より後ろには 0 でない値を置く (ドライバが 0 として返すこと)
            for i, c in enumerate(clus if size else []): img.wr(img.coff(c), data[i * cl:(i + 1) * cl])
        name = relpath.rsplit("/", 1)[-1]
        parent.add(entry_set(img, name, 0x20, first, size, valid, nofat))
        manifest.append("F\t%s\t%d\t%d\t%d" % (relpath, size, valid, seed))

    def new_dir(parent, relpath):
        d = Dir(img, []); d.parent = parent; d.relpath = relpath
        dirs[relpath] = d
        manifest.append("D\t%s" % relpath)
        return d

    def finish_dir(d):
        """子を全部入れ終えてから呼ぶ: 必要なクラスタを確保し (終端の 0x00 エントリ用に 1 エントリ分の余裕)、親へエントリを足す。"""
        need = max(1, (len(d.data) + 32 + cl - 1) // cl)
        d.clus = img.alloc(need)
        name = d.relpath.rsplit("/", 1)[-1]
        d.parent.add(entry_set(img, name, 0x10, d.clus[0], need * cl, need * cl, False))

    put_file(root, "small.txt", 11, 1)
    put_file(root, "empty.bin", 0, 2)
    put_file(root, "one_cluster.bin", cl, 3)
    put_file(root, "multi.bin", cl * 5 + 123, 4)
    put_file(root, "big.bin", 1 << 20 | 4097, 5)
    put_file(root, "fragmented.bin", cl * 9 + 7, 6, frag=True)
    put_file(root, "partial_valid.bin", 8192, 7, valid=5000)   # クラスタ数はクラスタサイズ次第 (512B なら 16 個)
    put_file(root, "日本語ファイル名.txt", 300, 8)
    put_file(root, "a" * 100 + ".data", 1000, 9)          # 100+5 文字 = 名前エントリ 7 個
    put_file(root, "n" * 255, 10, 10)                      # 最長名
    put_file(root, "MiXeD.CaSe", 5, 11)
    put_file(root, "emoji_😀.bin", 64, 12)                  # サロゲートペア
    sub = new_dir(root, "sub")
    put_file(sub, "sub/inner.txt", 2000, 13)
    deep = new_dir(sub, "sub/deep")
    put_file(deep, "sub/deep/leaf.bin", cl + 1, 14)
    finish_dir(deep); finish_dir(sub)
    finish_dir(new_dir(root, "emptydir"))
    many = new_dir(root, "many")                           # 複数クラスタにまたがるディレクトリ (FAT チェーン)
    for i in range(300): put_file(many, "many/file_%04d.txt" % i, 10 + i, 100 + (i % 50))
    finish_dir(many)
    for d in dirs.values(): d.flush()
    # root の既存エントリ以降は flush で書く。FAT チェーンはクラスタ追加時に更新済み
    img.flush()
    return manifest

def make(outdir, name, sector, cluster, mib):
    path = os.path.join(outdir, name + ".img")
    if os.path.exists(path): os.unlink(path)
    with open(path, "wb") as f: f.truncate(mib << 20)
    subprocess.run(["mkfs.exfat", "-q", "-s", str(sector), "-c", str(cluster), "-L", "TESTVOL", path], check=True)
    man = populate(path)
    with open(os.path.join(outdir, name + ".manifest"), "w", encoding="utf-8") as f: f.write("\n".join(man) + "\n")
    r = subprocess.run(["fsck.exfat", "-n", path], capture_output=True, text=True)
    print(name, "fsck.exfat:", r.returncode, (r.stdout + r.stderr).strip().splitlines()[-1:] )
    if r.returncode != 0: print(r.stdout, r.stderr)
    return r.returncode

if __name__ == "__main__":
    out = sys.argv[1]; os.makedirs(out, exist_ok=True)
    variants = [("c4k", 512, 4096, 24), ("c512", 512, 512, 8), ("s4k_c32k", 4096, 32768, 48)]
    rc = 0
    for v in variants: rc |= make(out, *v)
    sys.exit(1 if rc else 0)
