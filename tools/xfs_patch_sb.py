#!/usr/bin/env python3
"""XFS のスーパーブロック (AG 0) のフィールドを書き換えて CRC を付け直す (書き込み拒否・マウント失敗のテスト用イメージを作る。xfs_db -x は検証に失敗する値を書かせてくれない)。
使い方: xfs_patch_sb.py <img> <field>=<値> ...   field: qflags / incompat / ro_compat / compat / log_incompat / rextents / rblocks / logstart / sunit / swidth / dirblklog / features2 / imax_pct
(値は 0x… も可。ここはテスト専用で、通常のツールではない。docs/xfs-write.md)"""
import struct
import sys

# 名前 -> (オフセット, struct 書式。ビッグエンディアン)
F = {"rblocks": (16, ">Q"), "rextents": (24, ">Q"), "logstart": (48, ">Q"), "imax_pct": (127, ">B"), "sunit": (184, ">I"), "swidth": (188, ">I"),
     "dirblklog": (192, ">B"), "features2": (200, ">I"), "compat": (208, ">I"), "ro_compat": (212, ">I"), "incompat": (216, ">I"), "log_incompat": (220, ">I"), "qflags": (176, ">H")}


def crc32c(data):
    tab = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0x82F63B78 if c & 1 else c >> 1
        tab.append(c)
    c = 0xFFFFFFFF
    for b in data:
        c = tab[(c ^ b) & 255] ^ (c >> 8)
    return c ^ 0xFFFFFFFF


def main():
    img = sys.argv[1]
    with open(img, "r+b") as f:
        sec = bytearray(f.read(512))
        sect = struct.unpack(">H", sec[102:104])[0]
        f.seek(0)
        sec = bytearray(f.read(sect))
        for a in sys.argv[2:]:
            k, v = a.split("=")
            off, fmt = F[k]
            struct.pack_into(fmt, sec, off, int(v, 0))
        sec[224:228] = b"\0\0\0\0"
        struct.pack_into("<I", sec, 224, crc32c(bytes(sec)))
        f.seek(0)
        f.write(sec)


if __name__ == "__main__":
    main()
