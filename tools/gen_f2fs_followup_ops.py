#!/usr/bin/env python3
"""F2FS 書き込みの相互検証用: ツリーのダンプ (tools/ops_ref.py --tree と同じ書式) を読み、そのツリーの上で必ず成功する追加の操作列を作る。
使い方: gen_f2fs_followup_ops.py <tree> <seed> <出力> [操作数 (既定 120)]
カーネルが自前ドライバの書いたイメージの上で操作する / 自前ドライバがカーネルの書いたイメージの上で操作する、の両方向で使う (書式は ops_ref.py / test_f2fs_write --ops と同じ)。
ファイル名に空白を含むものは避ける (操作列は空白区切り)。"""
import random, sys

tree, seed, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
nops = int(sys.argv[4]) if len(sys.argv) > 4 else 120
rnd = random.Random(seed)
files, dirs, links = {}, ["" ], set()
for line in open(tree, encoding="utf-8", errors="surrogateescape"):
    line = line.rstrip("\n")
    if not line: continue
    k, size, rest = line.split(" ", 2)
    if k == "d": dirs.append(rest.split(" ", 1)[1])
    elif k == "f": files[rest.split(" ", 1)[1]] = int(size)
    elif k == "l": links.add(rest.rsplit(" ", 1)[1])
def ok(p): return " " not in p and "\n" not in p
files = {p: s for p, s in files.items() if ok(p)}
dirs = [d for d in dirs if ok(d)]
L = []
used = set(files) | set(dirs) | links
cnt = [0]
def newname(d):
    while True:
        cnt[0] += 1
        n = "fu%d_%d" % (seed, cnt[0])
        p = (d + "/" if d else "") + n
        if p not in used: used.add(p); return p
# 名前が長い・特殊なものは扱わない。操作は、既存のファイルへの追記 / 上書き / 縮小 / 削除 / rename、新規のファイル / ディレクトリ / リンク。
for _ in range(nops):
    k = rnd.randrange(10)
    fl = sorted(files)
    if k < 3 or not fl:
        d = rnd.choice(dirs); p = newname(d)
        L.append("create /" + p); sz = rnd.choice([0, 5, 100, 3000, 4096, 5000, 70000, 200000])
        files[p] = 0
        if sz: L.append("write /%s 0 %d %d" % (p, sz, rnd.randrange(256))); files[p] = sz
    elif k == 3:
        p = rnd.choice(fl); sz = files[p]; ln = rnd.choice([1, 100, 4096, 9000, 50000]); off = sz if rnd.random() < 0.6 else rnd.randrange(sz + 1)
        L.append("write /%s %d %d %d" % (p, off, ln, rnd.randrange(256))); files[p] = max(sz, off + ln)
    elif k == 4:
        p = rnd.choice(fl); sz = rnd.choice([0, 1, 100, files[p] // 2, files[p] + 5000]); L.append("truncate /%s %d" % (p, sz)); files[p] = sz
    elif k == 5:
        p = rnd.choice(fl); L.append("unlink /" + p); del files[p]
    elif k == 6:
        p = rnd.choice(fl); d = rnd.choice(dirs); q = newname(d); L.append("rename /%s /%s" % (p, q)); files[q] = files.pop(p)
    elif k == 7:
        d = rnd.choice(dirs); p = newname(d); L.append("mkdir /" + p); dirs.append(p)
    elif k == 8:
        d = rnd.choice(dirs); p = newname(d); L.append("symlink /%s /target/%d" % (p, rnd.randrange(1000))); links.add(p)
    else:
        p = rnd.choice(fl); d = rnd.choice(dirs); q = newname(d); L.append("link /%s /%s" % (q, p)); files[q] = files[p]
        # ハードリンクは inode が同じなので、以後の変更が両方に及ぶ: 追跡が複雑になるので、このリンクと元の名前は以後触らない
        del files[q]; del files[p]
open(out, "w").write("\n".join(L) + "\n")
