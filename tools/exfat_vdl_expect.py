#!/usr/bin/env python3
"""gen_exfat_ops.py の "partial_valid.bin" の操作列 (ValidDataLength=5000 / サイズ 8192 のファイルの 6000 から 100 バイト書き → 20000 に拡張) の期待されるツリー行を出力する。
exFAT の規則: ValidDataLength より後ろは (ディスク上の中身にかかわらず) ゼロとして読める。参照実装 exfat-fuse は VDL と書き込み位置の間のゼロ埋めをしない (ディスク上の古い値が見える) ので、
このファイルだけは参照実装との比較から外し、この期待値と比べる。使い方: exfat_vdl_expect.py  → "f 20000 <sha256> partial_valid.bin" """
import hashlib
data = bytes(((i * 31 + 7 * 7 + (i >> 8)) & 0xFF) for i in range(5000))   # tools/gen_exfat_fixture.py の pattern(seed=7)
data += bytes(1000) + bytes((((6000 + i) * 7) + 23) & 255 for i in range(100))
data += bytes(20000 - len(data))
print("f %d %s partial_valid.bin" % (len(data), hashlib.sha256(data).hexdigest()))
