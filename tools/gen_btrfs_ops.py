#!/usr/bin/env python3
"""Btrfs 書き込みテスト用の決まった操作列と、mkfs.btrfs --rootdir に渡す元ファイル群を出力する。
使い方:
  gen_btrfs_ops.py seed <dir>            元ファイル群 (インライン / 通常 / 疎 / シンボリックリンク / ハードリンク / 日本語名 / 長い名前) を <dir> に作る
  gen_btrfs_ops.py ops <出力> [seeded]   操作列を出力 (書式は usbmem_btrfs_write_tests --ops / tools/ops_ref.py と同じ)。seeded なら seed のファイルも変更・削除する
tools/run_btrfs_write_tests.sh (ホスト) と tools/run_android_btrfs_write_test.sh (Android) が共有する。"""
import os
import sys


def pat(off, n, seed):
    return bytes((((off + i) * 7) + seed) & 255 for i in range(n))


def seed(d):
    os.makedirs(d, exist_ok=True)

    def w(name, data):
        p = os.path.join(d, name)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, "wb").write(data)

    w("tiny.txt", b"hello btrfs\n")                         # インライン
    w("inline_max.bin", pat(0, 2000, 1))                    # インラインの上限近く
    w("one_sector.bin", pat(0, 4096, 2))
    w("two_sectors_plus.bin", pat(0, 8192 + 100, 3))
    w("mid.bin", pat(0, 300000, 4))
    w("big.bin", pat(0, 1500000, 5))
    w("zeros.bin", b"\0" * 200000)
    w("日本語/ファイル.txt", "こんにちは".encode() * 100)
    w("deep/a/b/c/leaf.bin", pat(0, 5000, 6))
    w("n" * 255, b"long name\n")
    with open(os.path.join(d, "sparse.bin"), "wb") as f:    # 穴のあるファイル (no-holes: 穴は項目なし)
        f.write(pat(0, 5000, 7)); f.seek(400000); f.write(pat(400000, 7000, 8)); f.seek(2000000); f.write(b"end")
    os.symlink("tiny.txt", os.path.join(d, "ln_rel"))
    os.symlink("/some/absolute/target", os.path.join(d, "ln_abs"))
    os.link(os.path.join(d, "mid.bin"), os.path.join(d, "mid_hard.bin"))
    os.makedirs(os.path.join(d, "emptydir"), exist_ok=True)


def ops(out, seeded):
    L = []
    A = L.append
    A("mkdir /w"); A("mkdir /w/日本語ディレクトリ")
    A("create /w/small.txt"); A("write /w/small.txt 0 11 1")
    A("create /w/empty.bin")
    for n, sz in (("sec", 4096), ("sec_plus1", 4097), ("sec_minus1", 4095), ("inl_max", 2048), ("inl_over", 2049), ("inl_under", 2047), ("two", 8192), ("m128k", 131072), ("m1m", 1048576 + 5)):
        A("create /w/%s.bin" % n); A("write /w/%s.bin 0 %d 2" % (n, sz))
    A("create /w/big.bin"); A("write /w/big.bin 0 5300000 6")
    A("write /w/big.bin 100000 70000 7"); A("write /w/big.bin 0 1 8"); A("write /w/big.bin 5299999 5000 9"); A("write /w/big.bin 4094 10 3")   # 上書き・先頭 1 バイト・EOF をまたぐ拡張・セクタ境界
    A("write /w/small.txt 5 4000 10")                      # インラインから通常 extent への変換
    A("create /w/sparse.bin"); A("write /w/sparse.bin 1000000 100 11"); A("write /w/sparse.bin 50 10 12"); A("write /w/sparse.bin 5000000 1000 13")
    A("create /w/trunc.bin"); A("write /w/trunc.bin 0 100000 14")
    for sz in (50000, 5000, 4097, 4096, 50, 0):
        A("truncate /w/trunc.bin %d" % sz)
    A("write /w/trunc.bin 0 3000 15"); A("truncate /w/trunc.bin 200000"); A("truncate /w/trunc.bin 4100"); A("truncate /w/trunc.bin 2000")   # 縮小 → 0 → 再書き込み → 拡張 (穴) → インラインへ
    A("create /w/inl.bin"); A("write /w/inl.bin 0 100 16"); A("write /w/inl.bin 100 100 17"); A("write /w/inl.bin 1000 50 18"); A("truncate /w/inl.bin 30"); A("truncate /w/inl.bin 1500")
    A("create /w/del.bin"); A("write /w/del.bin 0 700000 16"); A("unlink /w/del.bin")
    A("mkdir /w/emptydir"); A("rmdir /w/emptydir")
    A("rename /w/small.txt /w/small2.txt"); A("rename /w/small2.txt /w/日本語ディレクトリ/移動済み.txt")
    A("create /w/ow.txt"); A("write /w/ow.txt 0 1000 17"); A("rename /w/sec.bin /w/ow.txt")       # 上書き rename
    A("mkdir /w/d1"); A("mkdir /w/d1/d2"); A("create /w/d1/d2/leaf"); A("write /w/d1/d2/leaf 0 300 18")
    A("rename /w/d1 /w/d3"); A("rename /w/d3/d2 /w/d2moved")      # ディレクトリの rename・親をまたぐ移動
    A("symlink /w/short.lnk /etc/passwd"); A("symlink /w/long.lnk " + "/".join(["segment"] * 40))
    A("link /w/hard.txt /w/ow.txt"); A("unlink /w/ow.txt")           # ハードリンク: 片方を消しても内容が残る
    A("link /w/hard2.txt /w/hard.txt"); A("link /w/日本語ディレクトリ/hard3.txt /w/hard.txt"); A("unlink /w/hard2.txt")
    A("create /w/" + "x" * 255); A("write /w/" + "x" * 255 + " 0 10 19")
    A("mkdir /w/many")
    for i in range(300):
        A("create /w/many/ファイル%03d.txt" % i); A("write /w/many/ファイル%03d.txt 0 %d %d" % (i, 10 + i * 37, i % 200))
    for i in range(0, 300, 3):
        A("unlink /w/many/ファイル%03d.txt" % i)
    A("write /w/many/ファイル001.txt 100 20000 99")
    if seeded:
        # 元からあるファイルの変更・削除・上書き (インライン / 通常 / 疎 / 圧縮 / ハードリンク / シンボリックリンク)
        A("write /tiny.txt 5 20 20"); A("write /inline_max.bin 1990 100 21"); A("write /one_sector.bin 100 50 22"); A("write /two_sectors_plus.bin 4000 300 23")
        A("write /mid.bin 100000 5000 24"); A("write /mid.bin 299990 100 25"); A("write /big.bin 700000 1000 26"); A("write /big.bin 4090 20 27")
        A("write /zeros.bin 100000 10 28"); A("truncate /zeros.bin 150000"); A("truncate /zeros.bin 100001")
        A("write /sparse.bin 5000 395000 29"); A("write /sparse.bin 3000000 10 30"); A("truncate /sparse.bin 1999999")
        A("truncate /mid.bin 1000"); A("unlink /mid_hard.bin"); A("write /mid.bin 0 300 31")          # ハードリンクの片方を消してから書く
        A("unlink /big.bin"); A("rename /tiny.txt /w/tiny_moved.txt"); A("write /w/tiny_moved.txt 0 10 32")
        A("write /日本語/ファイル.txt 10 100 33"); A("unlink /ln_abs"); A("symlink /ln_new /tiny_target_gone"); A("rmdir /emptydir")
        A("write /deep/a/b/c/leaf.bin 4990 100 34"); A("rename /deep /deep2"); A("unlink /deep2/a/b/c/leaf.bin")
    open(out, "w").write("\n".join(L) + "\n")


if __name__ == "__main__":
    if sys.argv[1] == "seed":
        seed(sys.argv[2])
    else:
        ops(sys.argv[2], len(sys.argv) > 3 and sys.argv[3] == "seeded")
