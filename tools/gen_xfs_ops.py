#!/usr/bin/env python3
"""XFS 書き込みテスト用の決まった操作列と、mkfs.xfs -p に渡す元ファイル群を出力する。
使い方:
  gen_xfs_ops.py seed <dir>            元ファイル群 (小さい / 大きい / 疎 / シンボリックリンク / ハードリンク / 日本語名 / 長い名前 / short-form・block・leaf 形式のディレクトリ) を <dir> に作る
  gen_xfs_ops.py crashseed <dir> / crashops <出力>   電源断の検査用 (小さな操作列。ディレクトリ形式の変わり目をまたぐ)
  gen_xfs_ops.py ops <出力> [seeded|createonly]   操作列を出力 (書式は usbmem_xfs_write_tests --ops / tools/ops_ref.py と同じ)。seeded なら seed のファイル・ディレクトリにも書く。createonly は seed のディレクトリへの追加だけ (既存ファイルの中身は変えない・削除しない)
既定 (createonly 以外) の操作列は Full の操作 (truncate / unlink / rmdir / rename / link。sf / block / leaf 形式のディレクトリ・bmbt 形式のファイルを含む) も含む。createonly は含まない (アプリが CreateOnly で開く場合の列)。
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
    for i in range(1200):                     # ノード形式 (leaf1 に収まらない。mkfs.xfs -p が作る = カーネルと同じ構造のディレクトリ)
        w("nodedir/node_entry_with_a_fairly_long_name_%05d" % i, b"n%d\n" % i)


def full_ops(A, seeded):
    """Full の操作 (truncate / unlink / rmdir / rename / link)。ops() の /w の木ができたあとに続く。seeded なら元ファイルの木 (sf / block / leaf のディレクトリ) にも行う"""
    # --- truncate: 縮小 → 拡張 (EOF 以降はゼロ)・ブロック境界・0・空ファイルの拡張
    A("truncate /w/big.bin 1000000"); A("truncate /w/big.bin 4097"); A("truncate /w/big.bin 4096"); A("truncate /w/big.bin 100000"); A("write /w/big.bin 99990 30 41")
    A("truncate /w/m1m.bin 0"); A("write /w/m1m.bin 0 20000 42")
    A("truncate /w/blk.bin 10"); A("truncate /w/blk.bin 5000"); A("write /w/blk.bin 3000 20 43")
    A("truncate /w/blk_plus1.bin 4096"); A("truncate /w/blk_plus1.bin 4096")
    A("truncate /w/sparse.bin 1000050"); A("truncate /w/sparse.bin 6000000")
    A("truncate /w/empty.bin 12345")
    A("truncate /w/one.bin 0"); A("truncate /w/one.bin 1")
    # --- extent が多いファイル (bmbt 形式): 縮小で bmbt → extents に戻る / 丸ごと削除
    A("create /w/frag")
    for i in range(60): A("write /w/frag %d 100 %d" % (i * 8192, i))
    A("create /w/frag2")
    for i in range(50): A("write /w/frag2 %d 100 %d" % (i * 8192, i + 7))
    A("truncate /w/frag 245760"); A("truncate /w/frag 40960"); A("write /w/frag 40000 50000 9")
    A("truncate /w/frag2 400000")
    # --- link / unlink
    A("link /w/hard1 /w/two.bin"); A("link /w/sub0/hard2 /w/two.bin"); A("unlink /w/two.bin"); A("write /w/hard1 100 50 44"); A("unlink /w/hard1")
    A("unlink /w/small.txt"); A("unlink /w/empty.bin"); A("unlink /w/blk_minus1.bin"); A("unlink /w/short.lnk"); A("unlink /w/long.lnk"); A("unlink /w/" + "x" * 255)
    A("unlink /w/frag2")
    A("link /w/many/hl /w/app.bin")
    # --- rename: 同じディレクトリ / 別のディレクトリ / 上書き / ディレクトリ (".." と親の nlink) / 同じ inode
    A("rename /w/blk.bin /w/blk_renamed.bin"); A("rename /w/one.bin /w/sub1/one_moved")
    A("rename /w/sub2 /w/日本語ディレクトリ/sub2"); A("rename /w/sub3 /w/sub3b")
    A("mkdir /w/emptyd"); A("rename /w/sub3b /w/emptyd")
    A("mkdir /w/gone"); A("rmdir /w/gone")
    A("create /w/ov_src"); A("write /w/ov_src 0 50000 45"); A("create /w/ov_dst"); A("write /w/ov_dst 0 90000 46"); A("rename /w/ov_src /w/ov_dst")
    A("rename /w/ov_dst /w/ov_dst"); A("link /w/h_a /w/ov_dst"); A("rename /w/h_a /w/ov_dst"); A("unlink /w/h_a")
    A("rename /w/many /w/sub1/many_moved")                                                        # leaf 形式のディレクトリごと (".." はデータブロック 0)
    A("rename /w/sub1/many_moved/ファイル005.txt /w/sub1/many_moved/ファイル005b.txt"); A("rename /w/sub1/many_moved/ファイル007.txt /w/moved007.txt")
    A("symlink /w/sl /w/sub4"); A("rename /w/sl /w/sub4/sl2")
    # --- leaf 形式のディレクトリの削除: 中ほどのデータブロックが空になる (穴) → 末尾のブロックも空になる (縮む) → 再び育てる → 全部消して rmdir
    M = "/w/sub1/many_moved/"
    gone = set()
    for i in range(0, 300, 3): A("unlink %sファイル%03d.txt" % (M, i)); gone.add(i)
    for i in range(126, 252):
        if i not in gone: A("unlink %sファイル%03d.txt" % (M, i)); gone.add(i)
    for i in range(252, 300):
        if i not in gone: A("unlink %sファイル%03d.txt" % (M, i)); gone.add(i)
    for i in range(150): A("create %snew_after_delete_%03d" % (M, i)); A("write %snew_after_delete_%03d 0 %d %d" % (M, i, i * 5, i % 90))
    left = ["ファイル%03d.txt" % i for i in range(126) if i not in gone and i not in (5, 7)] + ["ファイル005b.txt", "hl"] + ["new_after_delete_%03d" % i for i in range(150)]
    for n in left: A("unlink %s%s" % (M, n))
    A("rmdir /w/sub1/many_moved")
    # --- サブツリーの削除
    for j in range(3): A("unlink /w/sub0/f%d" % j)
    A("unlink /w/sub0/hard2"); A("rmdir /w/sub0")
    if seeded:
        # 元ファイルの木: sf / block / leaf のディレクトリからの削除・ディレクトリ形式をまたぐ rename・ハードリンクの片方の削除・縮小
        A("unlink /mid_hard.bin"); A("truncate /mid.bin 150000"); A("unlink /mid.bin")
        A("unlink /ln_rel"); A("unlink /ln_abs"); A("unlink /one_block.bin")
        for i in range(0, 12, 2): A("unlink /sfdir/s%d" % i)
        for i in range(0, 40, 3): A("unlink /sfdir/new%d" % i)
        for i in range(0, 60): A("unlink /blkdir/block_entry_%03d.txt" % i)
        for i in range(0, 400, 2): A("unlink /leafdir/leaf_entry_with_longer_name_%04d.txt" % i)
        for i in range(0, 80, 5): A("unlink /leafdir/more_leaf_entry_with_longer_name_%04d.txt" % i)
        A("rename /emptydir/d /sfdir/dmoved"); A("rename /deep/a /deep/a2"); A("rename /deep/a2/b /deep/b_up")
        A("unlink /deep/b_up/c/leaf.bin"); A("rmdir /deep/b_up/c"); A("rmdir /deep/b_up")
        A("truncate /big.bin 100"); A("truncate /sparse.bin 3000"); A("truncate /sparse.bin 3000000")
        A("rename /日本語/ファイル.txt /emptydir/f.txt"); A("rmdir /日本語")
        A("rename /" + "n" * 255 + " /renamed_long"); A("link /sfdir/tiny_link /tiny.txt"); A("link /leafdir/tiny_link2 /tiny.txt")
        A("rename /blkdir /blkdir2"); A("rename /leafdir /sfdir/leafdir_moved"); A("rename /sfdir /emptydir/sfdir_moved")
        # カーネルが作ったノード形式のディレクトリ: 削除 (途中のデータブロックが空になる)・追加・別ディレクトリとの rename・ディレクトリごとの移動
        for i in range(0, 1200, 3): A("unlink /nodedir/node_entry_with_a_fairly_long_name_%05d" % i)
        for i in range(1, 600, 3): A("rename /nodedir/node_entry_with_a_fairly_long_name_%05d /emptydir/nd_%05d" % (i, i))
        for i in range(120): A("create /nodedir/again_with_a_long_enough_name_%04d" % i)
        for i in range(2, 1200, 3):
            if i > 900: A("unlink /nodedir/node_entry_with_a_fairly_long_name_%05d" % i)
        A("rename /nodedir /emptydir/nodedir_moved")


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
        for i in range(60): A("create /nodedir/added_%03d" % i); A("write /nodedir/added_%03d 0 %d %d" % (i, i * 13, i % 90))   # 既にあるノード形式への追加
    if not create_only: full_ops(A, seeded)
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
    for i in range(495): w("nodd/e%04d" % i, b"q")               # leaf1 の葉がほぼ一杯 (追加でノード形式に移る)


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
    # Full の操作: 縮小・拡張 (ゼロ)・削除 (ブロック / inode の解放)・rename (同じ / 別のディレクトリ・上書き・ディレクトリ)・link / unlink・ディレクトリ形式の中の削除 (leaf のデータブロックが空になって解放される)
    A("truncate /cr/g3 1000"); A("truncate /cr/g3 30000"); A("write /cr/g3 20000 100 5")
    A("unlink /cr/g1"); A("unlink /cr/lnk"); A("unlink /cr/longlnk")
    A("rename /cr/g2 /cr/sub/g2m"); A("rename /cr/sub /cr/sub2"); A("rename /cr/g5 /cr/f0")
    A("link /cr/hl /cr/g4"); A("unlink /cr/g4"); A("mkdir /cr/emptyd"); A("rmdir /cr/emptyd")
    for i in range(10): A("unlink /leafd/v%03d" % i)
    for i in range(4): A("unlink /sfd/t%02d" % i)
    for i in range(10): A("unlink /blkd/u%03d" % i)
    A("unlink /sfd/s00"); A("unlink /blkd/n000"); A("unlink /leafd/m000")
    A("truncate /keep/c.bin 50"); A("truncate /keep/c.bin 6000"); A("write /keep/c.bin 5990 30 8")   # 元ファイルの縮小 → 拡張 (EOF 以降のゼロ化は縮小のコミットのあと)
    A("rename /keep/c.bin /cr/c_moved"); A("unlink /keep/sub/b.bin")
    A("truncate /cr/big.bin 700000"); A("unlink /cr/sparse")
    for i in range(30): A("create /nodd/f%04d" % i)             # leaf1 → ノード形式 (索引の作り直し)
    for i in range(0, 495, 17): A("unlink /nodd/e%04d" % i)       # ノード形式からの削除
    A("rename /nodd/f0001 /cr/f_moved"); A("rename /cr/f0 /nodd/f_in")
    A("create /cr/after_free"); A("write /cr/after_free 0 300000 77"); A("create /cr/after_free2"); A("write /cr/after_free2 0 120000 78")   # 解放した領域を再利用したくなる書き込み (コミットを挟んでから)
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
