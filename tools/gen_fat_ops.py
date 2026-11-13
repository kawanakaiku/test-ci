#!/usr/bin/env python3
"""FAT 書き込みテスト用の決まった操作列を出力する (クラスタサイズ別)。使い方: gen_fat_ops.py <cluster_size> <出力> [many ディレクトリのファイル数 (既定 300)] [大きいファイルのサイズ (既定 3000000)]
書式は usbmem_fat_write_tests --ops / tools/ops_ref.py と同じ。tools/run_fat_write_tests.sh (ホスト) と tools/run_android_fat_test.sh (Android) が共有する。
フィクスチャ (tools/gen_fat_fixture.py) の元のファイル (全イメージに共通のもの) も変更・削除する。名前の大文字小文字だけが違う組は作らない
(参照側の ops_ref.py はホストの FS で実行するため。FAT は大文字小文字を区別しないので、そういう組は FAT 側が EEXIST になる)。"""
import sys
cl = int(sys.argv[1]); nmany = int(sys.argv[3]) if len(sys.argv) > 3 else 300; BIG = int(sys.argv[4]) if len(sys.argv) > 4 else 3000000
HOLE = max(BIG // 3, 20000); out = []
A = out.append
A("mkdir /w"); A("mkdir /w/日本語ディレクトリ")
A("create /w/small.txt"); A("write /w/small.txt 0 11 1")
A("create /w/empty.bin")
for n, sz in (("exact_cluster", cl), ("cl_plus1", cl + 1), ("cl_minus1", cl - 1), ("two_clusters", 2 * cl), ("odd", 7 * cl + 100)):
    A("create /w/%s.bin" % n); A("write /w/%s.bin 0 %d 2" % (n, sz))
A("create /w/big.bin"); A("write /w/big.bin 0 %d 6" % BIG)
A("write /w/big.bin %d 7000 7" % (BIG // 30)); A("write /w/big.bin 0 1 8"); A("write /w/big.bin %d 5000 9" % (BIG - 1))   # 上書き・先頭 1 バイト・EOF をまたぐ拡張
A("write /w/small.txt 5 4000 10")                                       # 小さいファイルの拡張 (クラスタをまたぐ)
A("create /w/hole.bin"); A("write /w/hole.bin %d 100 11" % HOLE); A("write /w/hole.bin 50 10 12")   # 隙間を作る書き込み (FAT はゼロで埋める。疎にはならない)
A("create /w/trunc.bin"); A("write /w/trunc.bin 0 %d 14" % (HOLE // 2))
for sz in (HOLE // 8, 50, 0): A("truncate /w/trunc.bin %d" % sz)
A("write /w/trunc.bin 0 3000 15"); A("truncate /w/trunc.bin %d" % (HOLE // 2))    # 縮小 → 0 → 再書き込み → 拡張 (ゼロ埋め)
A("truncate /w/trunc.bin %d" % (cl + 1)); A("truncate /w/trunc.bin %d" % (cl // 2))
A("create /w/del.bin"); A("write /w/del.bin 0 %d 16" % (HOLE // 3)); A("unlink /w/del.bin")
A("mkdir /w/emptydir"); A("rmdir /w/emptydir")
A("rename /w/small.txt /w/small2.txt"); A("rename /w/small2.txt /w/日本語ディレクトリ/移動済み.txt")
A("create /w/ow.txt"); A("write /w/ow.txt 0 1000 17"); A("rename /w/exact_cluster.bin /w/ow.txt")   # 上書き rename (ow.txt の元クラスタが解放される)
A("mkdir /w/d1"); A("mkdir /w/d1/d2"); A("create /w/d1/d2/leaf"); A("write /w/d1/d2/leaf 0 300 18")
A("rename /w/d1 /w/d3"); A("rename /w/d3/d2 /w/d2moved")               # ディレクトリの rename・親をまたぐ移動
A("create /w/" + "x" * 255); A("write /w/" + "x" * 255 + " 0 10 19")      # 最長の名前 (255 文字 = 21 エントリ)
A("create /w/" + "あ" * 85); A("write /w/" + "あ" * 85 + " 0 10 20")      # 日本語 85 文字
A("create /w/" + "😀" * 63); A("write /w/" + "😀" * 63 + " 0 10 21")   # サロゲートペア 63 個 = UTF-16 で 126 単位 (UTF-8 で 252 バイト: ホスト側の NAME_MAX 255 に収まる最大級)
# 8.3 名の衝突: 先頭 6 文字が同じ長い名前 (LONGNA~1 ...)・短い名前と衝突する長い名前・同じ名前で拡張子だけ違う
A("mkdir /w/coll")
for i in range(14): A("create /w/coll/Long_File_Name_%02d.text" % i); A("write /w/coll/Long_File_Name_%02d.text 0 %d %d" % (i, 100 + i, i))
A("create /w/coll/README"); A("create /w/coll/readme_long_name.md"); A("create /w/coll/readme_long_name2.md")
A("create /w/coll/日本語の名前1.txt"); A("create /w/coll/日本語の名前2.txt"); A("create /w/coll/日本語の名前3.txt")
A("create /w/coll/a.b.c.d"); A("create /w/coll/.hidden_dot"); A("create /w/coll/~tilde.txt"); A("create /w/coll/has_space.txt")
A("mkdir /w/many")
for i in range(nmany):
    A("create /w/many/ファイル%03d.txt" % i); A("write /w/many/ファイル%03d.txt 0 %d %d" % (i, 10 + i * 7, i % 200))
for i in range(0, nmany, 3): A("unlink /w/many/ファイル%03d.txt" % i)
for i in range(1, nmany, 7):
    if i % 3 != 0: A("rename /w/many/ファイル%03d.txt /w/many/改名%03d.txt" % (i, i))
for i in range(nmany, nmany + 60): A("create /w/many/再利用%03d.txt" % i); A("write /w/many/再利用%03d.txt 0 %d %d" % (i, 3 * i, i % 190))   # 消した跡 (エントリ・クラスタ) の再利用
# フィクスチャに元からあるファイルの変更・削除 (全イメージに共通のもの)
A("write /SMALL.TXT 5 20 20"); A("truncate /multi.bin 3"); A("unlink /empty.dat"); A("create /newfile"); A("write /newfile 0 5 22")
A("write /fragmented.bin 100 %d 21" % (cl + 5))
A("unlink /hidden.txt"); A("unlink /readonly.txt"); A("unlink /system.bin")        # 隠し・読み取り専用・システム属性のファイルの削除
A("rename /MiXeD.CaSe /renamed.case")
A("write /sub/deep/leaf.bin 1000 100 24"); A("unlink /sub/deep/deeper/ほげ.txt"); A("rmdir /sub/deep/deeper")
A("rename /日本語ファイル名.txt /日本語ディレクトリ/日本語ファイル名.txt")
A("rename /lower.txt /UPPER.TXT")        # 上書き rename (短い名前の大文字/小文字の組)
A("unlink /odd_second.txt")
open(sys.argv[2], "w").write("\n".join(out) + "\n")
