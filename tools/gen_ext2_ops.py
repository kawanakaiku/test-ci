#!/usr/bin/env python3
"""ext2 書き込みテスト用の決まった操作列を出力する (ブロックサイズ別)。使い方: gen_ext2_ops.py <blocksize> <出力> [many ディレクトリのファイル数 (既定 300)]
書式は usbmem_ext2_write_tests --ops / tools/ops_ref.py と同じ。tools/run_ext2_write_tests.sh (ホスト) と tools/run_android_ext2_write_test.sh (Android) が共有する。"""
import sys
bs = int(sys.argv[1]); nmany = int(sys.argv[3]) if len(sys.argv) > 3 else 300; out = []
A = out.append
A("mkdir /w"); A("mkdir /w/日本語ディレクトリ")
A("create /w/small.txt"); A("write /w/small.txt 0 11 1")
A("create /w/empty.bin")
for n, sz in (("exact_block", bs), ("bs_plus1", bs + 1), ("bs_minus1", bs - 1), ("cross_single", 12 * bs + 100), ("cross_double", (12 + bs // 4) * bs + 100)):
    A("create /w/%s.bin" % n); A("write /w/%s.bin 0 %d 2" % (n, sz))
A("create /w/big.bin"); A("write /w/big.bin 0 5300000 6")            # 二重間接まで (4k でも 4MiB+48KiB を超える)
A("write /w/big.bin 100000 70000 7"); A("write /w/big.bin 0 1 8"); A("write /w/big.bin 5299999 5000 9")   # 上書き・先頭 1 バイト・EOF をまたぐ拡張
A("write /w/small.txt 5 4000 10")                                      # 小さいファイルの拡張
A("create /w/sparse.bin"); A("write /w/sparse.bin 1000000 100 11"); A("write /w/sparse.bin 50 10 12")
triple = (12 + bs // 4 + (bs // 4) ** 2) * bs + 12345                  # 三重間接の先頭付近 (疎: ブロックはほとんど使わない)
A("write /w/sparse.bin %d 1000 13" % triple)
A("create /w/trunc.bin"); A("write /w/trunc.bin 0 100000 14")
for sz in (50000, 50, 0): A("truncate /w/trunc.bin %d" % sz)
A("write /w/trunc.bin 0 3000 15"); A("truncate /w/trunc.bin 200000")  # 縮小 → 0 → 再書き込み → 拡張 (穴)
A("truncate /w/trunc.bin %d" % (bs + 1)); A("truncate /w/trunc.bin %d" % (bs // 2))
A("create /w/del.bin"); A("write /w/del.bin 0 70000 16"); A("unlink /w/del.bin")
A("mkdir /w/emptydir"); A("rmdir /w/emptydir")
A("rename /w/small.txt /w/small2.txt"); A("rename /w/small2.txt /w/日本語ディレクトリ/移動済み.txt")
A("create /w/ow.txt"); A("write /w/ow.txt 0 1000 17"); A("rename /w/exact_block.bin /w/ow.txt")      # 上書き rename (ow.txt の元ブロックが解放される)
A("mkdir /w/d1"); A("mkdir /w/d1/d2"); A("create /w/d1/d2/leaf"); A("write /w/d1/d2/leaf 0 300 18")
A("rename /w/d1 /w/d3"); A("rename /w/d3/d2 /w/d2moved")             # ディレクトリの rename・親をまたぐ移動 (.. の付け替え)
A("symlink /w/fast.lnk /etc/passwd"); A("symlink /w/slow.lnk " + "/".join(["segment"] * 40))          # 高速 (i_data 内) / 低速 (ブロック)
A("link /w/hard.txt /w/ow.txt"); A("unlink /w/ow.txt")             # ハードリンク: 片方を消しても内容が残る
A("create /w/" + "x" * 255); A("write /w/" + "x" * 255 + " 0 10 19")
A("mkdir /w/many")
for i in range(nmany):
    A("create /w/many/ファイル%03d.txt" % i); A("write /w/many/ファイル%03d.txt 0 %d %d" % (i, 10 + i * 7, i % 200))
for i in range(0, nmany, 3): A("unlink /w/many/ファイル%03d.txt" % i)
# フィクスチャに元からあるファイルの変更・削除
A("write /small.txt 5 20 20"); A("truncate /direct_max.bin 3"); A("write /single_ind_max.bin 100 5000 21"); A("unlink /empty.bin"); A("create /newfile"); A("write /newfile 0 5 22")
open(sys.argv[2], "w").write("\n".join(out) + "\n")
