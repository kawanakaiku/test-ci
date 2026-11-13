#!/usr/bin/env python3
"""FAT12/16/32 イメージの空きクラスタ (FAT のエントリが 0) の中身を 0xEE で埋める (tools/run_fat_crash_tests.sh 用。メタデータは変えない)。
使い方: fat_fill_free.py <img> [埋める値 (既定 0xEE)]
電源断のテストで「まだ書かれていない新しいクラスタの中身」が見えてしまうのを検出するための目印。mkfs.fat は空きを 0 で埋めるので、そのままではデータより先に
エントリ/FAT が届いても 0 が見えるだけで区別できない。"""
import struct, sys

img = sys.argv[1]
val = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0xEE
with open(img, "r+b") as f:
    bs = f.read(512)
    bps, spc, rsv, nf, rootent, tot16, _, fsz16, _, _, _, tot32 = struct.unpack_from("<HBHBHHBHHHII", bs, 11)
    fsz = fsz16 if fsz16 else struct.unpack_from("<I", bs, 36)[0]
    tot = tot16 if tot16 else tot32
    rootsec = (rootent * 32 + bps - 1) // bps
    data0 = rsv + nf * fsz + rootsec
    ncl = (tot - data0) // spc
    kind = 12 if ncl < 4085 else (16 if ncl < 65525 else 32)
    f.seek(rsv * bps)
    fat = f.read(fsz * bps)
    chunk = bytes([val]) * (spc * bps)
    n = 0
    for c in range(2, ncl + 2):
        if kind == 12:
            o = c * 3 // 2
            e = fat[o] | (fat[o + 1] << 8)
            e = (e >> 4) if (c & 1) else (e & 0xFFF)
        elif kind == 16:
            e = fat[c * 2] | (fat[c * 2 + 1] << 8)
        else:
            e = struct.unpack_from("<I", fat, c * 4)[0] & 0x0FFFFFFF
        if e == 0:
            f.seek((data0 + (c - 2) * spc) * bps)
            f.write(chunk)
            n += 1
    print("filled %d free clusters (FAT%d) with 0x%02x" % (n, kind, val), file=sys.stderr)
