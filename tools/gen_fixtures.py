#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""C++ パーサとは独立に書いた Python 実装でテスト用ディスクイメージと期待値を生成する。
同じ誤解(仕様の読み違い)を C++ 側と共有しないための相互検証用。使用: gen_fixtures.py <outdir>
期待値ファイル形式: 1 行目 = kind(mbr|gpt|none)、以降 "index start_lba sectors fs"。
"""
import os, struct, sys, uuid, zlib

def sparse(path, size, writes):
    with open(path, "wb") as f:
        f.truncate(size)
        for off, data in writes:
            f.seek(off); f.write(data)

def mbr_entry(status, ptype, start, count):
    return struct.pack("<B3sB3sII", status, b"\xfe\xff\xff", ptype, b"\xfe\xff\xff", start, count)

def mbr_sector(entries, sig=0x12345678):
    s = bytearray(512)
    struct.pack_into("<I", s, 440, sig)
    for i, e in enumerate(entries):
        s[446 + 16 * i: 446 + 16 * i + 16] = e
    s[510:512] = b"\x55\xaa"
    return bytes(s)

def fat32_vbr(total):
    s = bytearray(512)
    s[0:3] = b"\xeb\x58\x90"; s[3:11] = b"mkfs.fat"
    struct.pack_into("<HBHBHHBHHHII", s, 11, 512, 1, 32, 2, 0, 0, 0xF8, 0, 63, 255, 0, total)
    struct.pack_into("<I", s, 36, (total // 128) + 1)  # sectors per FAT
    s[82:90] = b"FAT32   "; s[510:512] = b"\x55\xaa"
    return bytes(s)

def ntfs_vbr():
    s = bytearray(512); s[0:3] = b"\xeb\x52\x90"; s[3:11] = b"NTFS    "; s[510:512] = b"\x55\xaa"
    return bytes(s)

def expected(path, kind, rows):
    with open(path, "w") as f:
        f.write(kind + "\n")
        for r in rows: f.write(" ".join(str(x) for x in r) + "\n")

def gpt_disk(out, name, ss, total, parts, with_fs):
    """parts: [(first, last)]"""
    nent = 128
    arr = bytearray(nent * 128)
    type_guid = uuid.UUID("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7").bytes_le
    for i, (a, b) in enumerate(parts):
        arr[i*128:(i+1)*128] = type_guid + uuid.UUID(int=i + 1).bytes_le + struct.pack("<QQQ", a, b, 0) + ("p%d" % (i + 1)).encode("utf-16-le").ljust(72, b"\0")
    arr_secs = len(arr) // ss if len(arr) % ss == 0 else len(arr) // ss + 1
    first_u, last_u = 2 + arr_secs, total - 2 - arr_secs
    def hdr(my, alt, elba):
        h = bytearray(92)
        h[0:8] = b"EFI PART"
        struct.pack_into("<IIIIQQQQ", h, 8, 0x10000, 92, 0, 0, my, alt, first_u, last_u)
        h[56:72] = uuid.UUID(int=0xABCDEF).bytes_le
        struct.pack_into("<QIII", h, 72, elba, nent, 128, zlib.crc32(bytes(arr)))
        struct.pack_into("<I", h, 16, zlib.crc32(bytes(h)))
        return bytes(h).ljust(ss, b"\0")
    writes = [(0, mbr_sector([mbr_entry(0, 0xEE, 1, min(total - 1, 0xFFFFFFFF))]).ljust(ss, b"\0")),
              (ss, hdr(1, total - 1, 2)), (2 * ss, bytes(arr)),
              ((total - 1 - arr_secs) * ss, bytes(arr)), ((total - 1) * ss, hdr(total - 1, 1, total - 1 - arr_secs))]
    rows = []
    for i, (a, b) in enumerate(parts):
        fs = "unknown"
        if with_fs[i] == "ntfs": writes.append((a * ss, ntfs_vbr())); fs = "ntfs"
        if with_fs[i] == "fat32": writes.append((a * ss, fat32_vbr((b - a + 1) * ss // 512))); fs = "fat32"
        rows.append((i + 1, a, b - a + 1, fs))
    sparse(os.path.join(out, name + ".img"), total * ss, writes)
    expected(os.path.join(out, name + ".expected"), "gpt", rows)

def main(out):
    os.makedirs(out, exist_ok=True)
    # 主 MBR 3 本 (NTFS VBR / FAT32 VBR / 空)
    total = 20000
    parts = [(2048, 4000, "ntfs"), (8192, 5000, "fat32"), (14000, 3000, "")]
    writes = [(0, mbr_sector([mbr_entry(0x80, 7, 2048, 4000), mbr_entry(0, 0x0C, 8192, 5000), mbr_entry(0, 0x83, 14000, 3000)]))]
    rows = []
    for i, (s, n, fs) in enumerate(parts):
        if fs == "ntfs": writes.append((s * 512, ntfs_vbr()))
        if fs == "fat32": writes.append((s * 512, fat32_vbr(n)))
        rows.append((i + 1, s, n, fs or "unknown"))
    sparse(os.path.join(out, "mbr_primary.img"), total * 512, writes)
    expected(os.path.join(out, "mbr_primary.expected"), "mbr", rows)

    # 拡張 + 論理 2 本
    total = 40000
    writes = [(0, mbr_sector([mbr_entry(0, 7, 2048, 4000), mbr_entry(0, 0x0F, 10000, 20000)]))]
    writes.append((10000 * 512, mbr_sector([mbr_entry(0, 0x83, 63, 1000), mbr_entry(0, 0x05, 5000, 3000)])))
    writes.append((15000 * 512, mbr_sector([mbr_entry(0, 0x83, 63, 2000)])))
    sparse(os.path.join(out, "mbr_extended.img"), total * 512, writes)
    expected(os.path.join(out, "mbr_extended.expected"), "mbr",
             [(1, 2048, 4000, "unknown"), (5, 10063, 1000, "unknown"), (6, 15063, 2000, "unknown")])

    gpt_disk(out, "gpt_basic", 512, 20000, [(2048, 9999), (10000, 19000)], ["ntfs", ""])
    gpt_disk(out, "gpt_4k", 4096, 4000, [(256, 999), (1000, 3500)], ["", "ntfs"])

    total = 300000
    sparse(os.path.join(out, "superfloppy_fat32.img"), total * 512, [(0, fat32_vbr(total))])
    expected(os.path.join(out, "superfloppy_fat32.expected"), "none", [(1, 0, total, "fat32")])
    sparse(os.path.join(out, "superfloppy_ntfs.img"), 4096 * 512, [(0, ntfs_vbr())])
    expected(os.path.join(out, "superfloppy_ntfs.expected"), "none", [(1, 0, 4096, "ntfs")])

if __name__ == "__main__":
    main(sys.argv[1])
