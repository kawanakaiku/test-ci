#!/usr/bin/env python3
"""exFAT 書き込みテスト用の決まった操作列を出力する (クラスタサイズ別)。使い方: gen_exfat_ops.py <cluster_size> <出力> [many ディレクトリのファイル数 (既定 300)]
書式は usbmem_exfat_write_tests --ops / tools/ops_ref.py と同じ。tools/run_exfat_write_tests.sh (ホスト) と tools/run_android_exfat_write_test.sh (Android) が共有する。
フィクスチャ (tools/gen_exfat_fixture.py) の元のファイルも変更・削除する (partial_valid.bin = ValidDataLength < サイズ、fragmented.bin、MiXeD.CaSe ほか)。"""
import sys
cl = int(sys.argv[1]); nmany = int(sys.argv[3]) if len(sys.argv) > 3 else 300; out = []
A = out.append
A("mkdir /w"); A("mkdir /w/日本語ディレクトリ")
A("create /w/small.txt"); A("write /w/small.txt 0 11 1")
A("create /w/empty.bin")
for n, sz in (("exact_cluster", cl), ("cl_plus1", cl + 1), ("cl_minus1", cl - 1), ("two_clusters", 2 * cl), ("odd", 7 * cl + 100)):
    A("create /w/%s.bin" % n); A("write /w/%s.bin 0 %d 2" % (n, sz))
A("create /w/big.bin"); A("write /w/big.bin 0 3000000 6")
A("write /w/big.bin 100000 70000 7"); A("write /w/big.bin 0 1 8"); A("write /w/big.bin 2999999 5000 9")   # 上書き・先頭 1 バイト・EOF をまたぐ拡張
A("write /w/small.txt 5 4000 10")                                       # 小さいファイルの拡張 (クラスタをまたぐ)
A("create /w/sparse.bin"); A("write /w/sparse.bin 1000000 100 11"); A("write /w/sparse.bin 50 10 12")   # 穴 (ゼロ) を作る書き込み
A("create /w/trunc.bin"); A("write /w/trunc.bin 0 100000 14")
for sz in (50000, 50, 0): A("truncate /w/trunc.bin %d" % sz)
A("write /w/trunc.bin 0 3000 15"); A("truncate /w/trunc.bin 200000")    # 縮小 → 0 → 再書き込み → 拡張 (穴)
A("truncate /w/trunc.bin %d" % (cl + 1)); A("truncate /w/trunc.bin %d" % (cl // 2))
A("create /w/del.bin"); A("write /w/del.bin 0 70000 16"); A("unlink /w/del.bin")
A("mkdir /w/emptydir"); A("rmdir /w/emptydir")
A("rename /w/small.txt /w/small2.txt"); A("rename /w/small2.txt /w/日本語ディレクトリ/移動済み.txt")
A("create /w/ow.txt"); A("write /w/ow.txt 0 1000 17"); A("rename /w/exact_cluster.bin /w/ow.txt")   # 上書き rename (ow.txt の元クラスタが解放される)
A("mkdir /w/d1"); A("mkdir /w/d1/d2"); A("create /w/d1/d2/leaf"); A("write /w/d1/d2/leaf 0 300 18")
A("rename /w/d1 /w/d3"); A("rename /w/d3/d2 /w/d2moved")               # ディレクトリの rename・親をまたぐ移動
A("create /w/" + "x" * 255); A("write /w/" + "x" * 255 + " 0 10 19")      # 最長の名前 (255 文字 = 19 エントリ)
A("create /w/" + "あ" * 85); A("write /w/" + "あ" * 85 + " 0 10 20")      # 日本語 85 文字
A("create /w/" + "😀" * 127); A("write /w/" + "😀" * 127 + " 0 10 21")   # サロゲートペア 127 個 = UTF-16 で 254 単位
A("mkdir /w/many")
for i in range(nmany):
    A("create /w/many/ファイル%03d.txt" % i); A("write /w/many/ファイル%03d.txt 0 %d %d" % (i, 10 + i * 7, i % 200))
for i in range(0, nmany, 3): A("unlink /w/many/ファイル%03d.txt" % i)
for i in range(1, nmany, 7):
    if i % 3 != 0: A("rename /w/many/ファイル%03d.txt /w/many/改名%03d.txt" % (i, i))
for i in range(nmany, nmany + 60): A("create /w/many/再利用%03d.txt" % i); A("write /w/many/再利用%03d.txt 0 %d %d" % (i, 3 * i, i % 190))   # 消した跡の再利用
# フィクスチャに元からあるファイルの変更・削除
A("write /small.txt 5 20 20"); A("truncate /multi.bin 3"); A("write /big.bin 1000000 5000 21"); A("unlink /empty.bin"); A("create /newfile"); A("write /newfile 0 5 22")
A("write /partial_valid.bin 6000 100 23")        # ValidDataLength (5000) の先から書く: 間 (5000..6000) はゼロとして読めること
A("truncate /partial_valid.bin 20000")           # 拡張 → 穴
A("unlink /fragmented.bin")
A("rename /MiXeD.CaSe /mixed.case")              # 大文字小文字だけの名前変更
A("write /sub/inner.txt 1990 100 24"); A("unlink /sub/deep/leaf.bin"); A("rmdir /sub/deep")
open(sys.argv[2], "w").write("\n".join(out) + "\n")
