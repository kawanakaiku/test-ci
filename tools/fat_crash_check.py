#!/usr/bin/env python3
"""tools/run_fat_crash_tests.sh の補助検査 (docs/fat-write.md)。電源断のあと、修復前に読み出したイメージについて:
  marker <展開したディレクトリ>        (F) どれかのファイルに、空きクラスタの目印 (tools/fat_fill_free.py の 0xEE の連続) が見えていれば、そのパスを出力して終了コード 1。
                                    = 書かれていない新しいクラスタの中身がファイルの一部として見えている (データより先にエントリ/FAT が届いた)。
  names <ops> <元のツリー> <ツリー>     (G) ツリーのパスの各要素 (大文字小文字は区別しない) が、操作列に現れる名前か元からある名前でなければ、出力して終了コード 1。
                                    = 壊れた名前 (長い名前のスロットが無いまま短い名前のエントリだけが見える: `VERY_L~1.TXT` など)。
"""
import os, sys


def marker(root):
    bad = []
    for dp, _, fns in os.walk(root):
        for n in fns:
            p = os.path.join(dp, n)
            try:
                d = open(p, "rb").read()
            except OSError:
                continue
            if b"\xee" * 32 in d:
                bad.append(os.path.relpath(p, root))
    return bad


def names(ops, orig, tree):
    ok = set()
    for line in open(ops, encoding="utf-8"):
        for tok in line.split():
            if tok.startswith("/"):
                ok.update(c.lower() for c in tok.split("/") if c)
    for t in (orig,):
        for line in open(t, encoding="utf-8"):
            parts = line.rstrip("\n").split(" ", 3)
            if len(parts) == 4:
                ok.update(c.lower() for c in parts[3].split("/") if c)
    bad = []
    for line in open(tree, encoding="utf-8"):
        parts = line.rstrip("\n").split(" ", 3)
        if len(parts) == 4 and any(c.lower() not in ok for c in parts[3].split("/") if c):
            bad.append(parts[3])
    return bad


def fsckfilter(log):
    """fsck.fat -n の出力から、rename の途中 (新しい名前の追加と古い名前の削除の間) の電源断で必ず現れる組を取り除いて出力する:
         /a  and\n/b\n  share clusters.\n  Truncating second to 0 bytes.      (同じ実体を指す名前が 2 つ = FAT の rename は原子的でない。Linux の vfat も同じ)
         /b\n  File size is N bytes, cluster chain length is 0 bytes.\n  Truncating file to 0 bytes.   (上の 2 つめを fsck が切り詰めた結果の追随)
       実体は残る (1 つめの名前が連鎖を持つ)。クロスリンクそのもの (別々のファイルが同じクラスタ) をここで通すわけではなく、
       (A)(E) が元のファイルの内容を、(F) が書かれていないクラスタの露出を検査する。"""
    L = [x.rstrip("\n") for x in open(log, encoding="utf-8", errors="replace")]
    drop = set(); seconds = []
    for i in range(len(L) - 3):
        if L[i].startswith("/") and L[i].rstrip().endswith(" and") and L[i + 1].startswith("/") and L[i + 2].strip() == "share clusters." and L[i + 3].strip() == "Truncating second to 0 bytes.":
            drop.update((i, i + 1, i + 2, i + 3)); seconds.append(L[i + 1].strip())
    for i in range(len(L) - 2):
        if L[i].strip() in seconds and L[i + 1].strip().startswith("File size is") and "cluster chain length is 0 bytes" in L[i + 1] and L[i + 2].strip() == "Truncating file to 0 bytes.":
            drop.update((i, i + 1, i + 2))
    return [x for i, x in enumerate(L) if i not in drop]


if __name__ == "__main__":
    if sys.argv[1] == "fsckfilter":
        print("\n".join(fsckfilter(sys.argv[2])))
        sys.exit(0)
    if sys.argv[1] == "marker":
        b = marker(sys.argv[2])
    else:
        b = names(sys.argv[2], sys.argv[3], sys.argv[4])
    if b:
        print(" ".join(b[:3]))
        sys.exit(1)
