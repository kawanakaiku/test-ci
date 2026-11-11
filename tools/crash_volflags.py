#!/usr/bin/env python3
"""NTFS イメージ (MBR 無しの単体ボリューム) の $Volume の VOLUME_INFORMATION フラグを読む。0x0001 = ダーティ。出力: 10 進のフラグ値。"""
import struct, sys
f = open(sys.argv[1], 'rb'); b = f.read(512)
bps = struct.unpack('<H', b[11:13])[0]; spc = b[13]; cs = bps * spc; mft = struct.unpack('<Q', b[0x30:0x38])[0]
rs = struct.unpack('<b', b[0x40:0x41])[0]; rec = (1 << -rs) if rs < 0 else rs * cs
f.seek(mft * cs + 3 * rec); r = bytearray(f.read(rec))
uo, uc = struct.unpack('<HH', r[4:8])
for i in range(1, uc): r[i*512-2:i*512] = r[uo+2*i:uo+2*i+2]
a = struct.unpack('<H', r[0x14:0x16])[0]
while a < len(r):
    t, l = struct.unpack('<II', r[a:a+8])
    if t == 0xFFFFFFFF: break
    if t == 0x70:
        vo = struct.unpack('<H', r[a+0x14:a+0x16])[0]; print(struct.unpack('<H', r[a+vo+10:a+vo+12])[0]); sys.exit(0)
    a += l
print(-1)
