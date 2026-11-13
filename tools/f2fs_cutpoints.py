#!/usr/bin/env python3
"""電源断テスト (tools/run_f2fs_crash_tests.sh) の切断位置を選ぶ。使い方: f2fs_cutpoints.py <書き込みログ (USBMEM_WRITE_LOG の出力)> <cp_blkaddr> <サンプル数> <総書き込み数>
先頭 6 回 + 全体に均等 + チェックポイント領域 (cp_blkaddr から 1024 ブロック) への書き込みの直前・直後 (CP パックの本体と末尾の間が本命) を、重複なしで昇順に 1 行ずつ出す。"""
import re, sys
log, cp, ns, total = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
lo, hi = cp * 4096, (cp + 1024) * 4096
cpw = []
for line in open(log, errors="replace"):
    m = re.match(r"W(\d+) off=0x([0-9a-f]+) len=(\d+)", line)
    if m and lo <= int(m.group(2), 16) < hi: cpw.append(int(m.group(1)))
pts = set(range(0, min(6, total + 1)))
for k in range(1, max(ns - 5, 1)): pts.add(total * k // max(ns - 5, 1))
step = max(len(cpw) // max(ns, 1), 1)
for p in cpw[::step]: pts.update((p - 1, p))   # n 番目の書き込みの直前 (limit = n-1) と直後 (limit = n)
for p in sorted(x for x in pts if 0 <= x <= total): print(p)
