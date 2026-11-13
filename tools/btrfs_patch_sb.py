#!/usr/bin/env python3
"""テスト用: Btrfs イメージ (crc32c) のスーパーブロックを書き換えてチェックサムを付け直す。書き込み拒否 (refuse) のテストイメージを作るのに使う。
使い方: btrfs_patch_sb.py <img> <操作>...    操作:
  incompat=0x.. | compat_ro=0x.. | compat=0x.. | flags=0x.. | log_root=N    全コピーに反映
  gen_mirror=N         ミラー 1 (64 MiB) の世代だけを N にする (電源断で途中までしか更新されなかった状態)
  bad_mirror           ミラー 1 を壊す (チェックサム不一致)
  dev_total=N          dev_item.total_bytes を N に
"""
import struct
import sys

OFFS = [0x10000, 0x4000000]


def crc32c(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc = TAB[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFF


TAB = []
for i in range(256):
    c = i
    for _ in range(8):
        c = (c >> 1) ^ 0x82F63B78 if c & 1 else c >> 1
    TAB.append(c)

FIELDS = {"incompat": (0xBC, "<Q"), "compat_ro": (0xB4, "<Q"), "compat": (0xAC, "<Q"), "flags": (0x38, "<Q"), "log_root": (0x60, "<Q"), "dev_total": (209, "<Q")}


def main():
    img = sys.argv[1]
    f = open(img, "r+b")
    sbs = {}
    for o in OFFS:
        f.seek(o)
        b = f.read(4096)
        if len(b) == 4096 and b[0x40:0x48] == b"_BHRfS_M":
            sbs[o] = bytearray(b)

    def put(o):
        b = sbs[o]
        b[0:4] = struct.pack("<I", crc32c(bytes(b[32:])))
        f.seek(o)
        f.write(bytes(b))

    for a in sys.argv[2:]:
        if a == "bad_mirror":
            b = sbs[OFFS[1]]
            b[100] ^= 0xFF
            f.seek(OFFS[1]); f.write(bytes(b))     # チェックサムは付け直さない
            continue
        k, _, v = a.partition("=")
        if k == "gen_mirror":
            b = sbs[OFFS[1]]
            b[0x48:0x50] = struct.pack("<Q", int(v, 0))
            put(OFFS[1])
            continue
        off, fmt = FIELDS[k]
        for o in sbs:
            sbs[o][off:off + 8] = struct.pack(fmt, int(v, 0))
            put(o)
    f.close()


main()
