#!/usr/bin/env python3
"""XFS 書き込みテスト用の決まった操作列と、mkfs.xfs -p に渡す元ファイル群を出力する。
使い方:
  gen_xfs_ops.py seed <dir>            元ファイル群 (小さい / 大きい / 疎 / シンボリックリンク / ハードリンク / 日本語名 / 長い名前 / short-form・block・leaf 形式のディレクトリ) を <dir> に作る
  gen_xfs_ops.py crashseed <dir> / crashops <出力>   電源断の検査用 (小さな操作列。ディレクトリ形式の変わり目をまたぐ)
  gen_xfs_ops.py ops <出力> [seeded|createonly]   操作列を出力 (書式は usbmem_xfs_write_tests --ops / tools/ops_ref.py と同じ)。seeded なら seed のファイル・ディレクトリにも書く。createonly は seed のディレクトリへの追加だけ (既存ファイルの中身は変えない)
XFS の書き込みは CreateOnly の範囲 (mkdir / create / write / symlink) だけなので、操作列も truncate / unlink / rmdir / rename / link は含まない。
tools/run_xfs_write_tests.sh (ホスト) と tools/run_android_xfs_write_test.sh (Android) が共有する。docs/xfs-write.md"""
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

    w("tiny.txt", b"hello xfs\n")
    w("one_block.bin", pat(0, 4096, 2))
    w("mid.bin", pat(0, 300000, 4))
    w("big.bin", pat(0, 1500000, 5))
    w("日本語/ファイル.txt", "こんにちは".encode() * 100)
    w("deep/a/b/c/leaf.bin", pat(0, 5000, 6))
    w("n" * 255, b"long name\n")
    with open(os.path.join(d, "sparse.bin"), "wb") as f:
        f.write(pat(0, 5000, 7)); f.seek(400000); f.write(pat(400000, 7000, 8)); f.seek(2000000); f.write(b"end")
    os.symlink("tiny.txt", os.path.join(d, "ln_rel"))
    os.symlink("/some/absolute/target", os.path.join(d, "ln_abs"))
    os.link(os.path.join(d, "mid.bin"), os.path.join(d, "mid_hard.bin"))
    os.makedirs(os.path.join(d, "emptydir"), exist_ok=True)
    for i in range(12):                       # short-form (inode 内) のディレクトリ
        w("sfdir/s%d" % i, b"s%d\n" % i)
    for i in range(60):                       # block 形式 (1 ブロックに収まる)
        w("blkdir/block_entry_%03d.txt" % i, b"b%d\n" % i)
    for i in range(400):                      # leaf 形式 (複数のデータブロック + 葉)
        w("leafdir/leaf_entry_with_longer_name_%04d.txt" % i, b"l%d\n" % i)


def ops(out, seeded, create_only=False):
    L = []
    A = L.append
    A("mkdir /w"); A("mkdir /w/日本語ディレクトリ")
    A("create /w/small.txt"); A("write /w/small.txt 0 11 1")
    A("create /w/empty.bin")
    for n, sz in (("blk", 4096), ("blk_plus1", 4097), ("blk_minus1", 4095), ("one", 1), ("two", 8192), ("m128k", 131072), ("m1m", 1048576 + 5)):
        A("create /w/%s.bin" % n); A("write /w/%s.bin 0 %d 2" % (n, sz))
    A("create /w/big.bin"); A("write /w/big.bin 0 5300000 6")
    A("write /w/big.bin 100000 70000 7"); A("write /w/big.bin 0 1 8"); A("write /w/big.bin 5299999 5000 9"); A("write /w/big.bin 4094 10 3")   # 上書き・先頭 1 バイト・EOF をまたぐ拡張・ブロック境界
    A("write /w/small.txt 5 4000 10")
    A("create /w/sparse.bin"); A("write /w/sparse.bin 1000000 100 11"); A("write /w/sparse.bin 50 10 12"); A("write /w/sparse.bin 5000000 1000 13")
    A("create /w/app.bin")
    for i in range(40):
        A("write /w/app.bin %d 777 %d" % (i * 777, i))          # 小さな追記を繰り返す (最後のブロックの継ぎ足し)
    A("symlink /w/short.lnk /etc/passwd"); A("symlink /w/long.lnk " + "/".join(["segment"] * 40))   # インライン / 別ブロック
    A("create /w/" + "x" * 255); A("write /w/" + "x" * 255 + " 0 10 19")
    A("mkdir /w/many")
    for i in range(300):                                       # sf → block → leaf 形式へ育つディレクトリ
        A("create /w/many/ファイル%03d.txt" % i); A("write /w/many/ファイル%03d.txt 0 %d %d" % (i, 10 + i * 37, i % 200))
    A("write /w/many/ファイル001.txt 100 20000 99")
    for i in range(5):
        A("mkdir /w/sub%d" % i)
        for j in range(3): A("create /w/sub%d/f%d" % (i, j)); A("write /w/sub%d/f%d 0 %d %d" % (i, j, 1000 * (i + 1) * (j + 1), i * 3 + j))
    if seeded:
        # 元からあるファイル・ディレクトリへの書き込み・追加 (sf / block / leaf 形式のディレクトリへの追加を含む)
        if not create_only:   # create_only: 既存ファイルの中身には触らない (アプリの CreateOnly モード。Android の計装テスト用)
            A("write /tiny.txt 5 20 20"); A("write /one_block.bin 100 50 22"); A("write /mid.bin 100000 5000 24"); A("write /mid.bin 299990 100 25")
            A("write /big.bin 4090 20 27"); A("write /big.bin 1499990 20000 28"); A("write /sparse.bin 5000 395000 29"); A("write /sparse.bin 3000000 10 30")
            A("write /日本語/ファイル.txt 10 100 33"); A("write /deep/a/b/c/leaf.bin 4990 100 34")
        for i in range(40): A("create /sfdir/new%d" % i)       # sf → block (→ leaf)
        for i in range(120): A("create /blkdir/added_with_a_long_name_%03d.txt" % i)   # block → leaf
        for i in range(80): A("create /leafdir/more_leaf_entry_with_longer_name_%04d.txt" % i); A("write /leafdir/more_leaf_entry_with_longer_name_%04d.txt 0 %d %d" % (i, i * 11, i % 100))
        A("create /emptydir/first"); A("symlink /emptydir/ln /tiny.txt"); A("mkdir /emptydir/d")
    open(out, "w").write("\n".join(L) + "\n")


def crash_seed(d):
    """電源断の検査用の元ファイル: ディレクトリ形式の変わり目の直前 (short-form が一杯 / block が一杯 / leaf の 1 ブロック目が一杯) と、変更される元ファイル"""
    os.makedirs(d, exist_ok=True)

    def w(name, data):
        p = os.path.join(d, name)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, "wb").write(data)

    w("keep/a.txt", pat(0, 5000, 5)); w("keep/sub/b.bin", pat(0, 70000, 6)); w("keep/c.bin", pat(0, 123, 7))
    w("tiny.txt", pat(0, 300, 3)); w("one_block.bin", pat(0, 4096, 4)); w("mid.bin", pat(0, 200000, 8))
    os.symlink("keep/a.txt", os.path.join(d, "keep/ln"))
    for i in range(28): w("sfd/s%02d" % i, b"x")                 # short-form が (512 バイト inode で) ほぼ一杯
    for i in range(160): w("blkd/n%03d" % i, b"y")               # block 形式がほぼ一杯 (4 KiB ブロック)
    for i in range(245): w("leafd/m%03d" % i, b"z")              # leaf 形式で 1 ブロック目がほぼ一杯


def crash_ops(out):
    L = []
    A = L.append
    A("mkdir /cr"); A("create /cr/f0"); A("write /cr/f0 0 5000 1")
    for i in range(1, 6): A("create /cr/g%d" % i); A("write /cr/g%d 0 %d %d" % (i, [100, 4096, 70000, 300, 9000][i - 1], i))
    for i in range(4): A("create /sfd/t%02d" % i)               # short-form → block
    for i in range(10): A("create /blkd/u%03d" % i)             # block → leaf
    for i in range(10): A("create /leafd/v%03d" % i)            # leaf: 2 ブロック目のデータブロックを足す
    A("mkdir /cr/sub"); A("create /cr/sub/x.txt"); A("write /cr/sub/x.txt 0 9000 7"); A("symlink /cr/lnk /cr/g3"); A("symlink /cr/longlnk " + "/".join(["seg"] * 30))
    A("write /tiny.txt 100 5000 44"); A("write /one_block.bin 4000 200 45"); A("write /mid.bin 199990 70000 46"); A("write /keep/a.txt 10 20 3")
    A("create /cr/big.bin"); A("write /cr/big.bin 0 1400000 61"); A("create /cr/sparse"); A("write /cr/sparse 300000 100 62")
    open(out, "w").write("\n".join(L) + "\n")


if __name__ == "__main__":
    if sys.argv[1] == "crashseed":
        crash_seed(sys.argv[2])
    elif sys.argv[1] == "crashops":
        crash_ops(sys.argv[2])
    elif sys.argv[1] == "seed":
        seed(sys.argv[2])
    else:
        ops(sys.argv[2], len(sys.argv) > 3 and sys.argv[3] in ("seeded", "createonly"), len(sys.argv) > 3 and sys.argv[3] == "createonly")
