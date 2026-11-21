#!/usr/bin/env python3
# fuseshim の wimlib 試験用: ディレクトリツリーを作り、期待するダンプ (fsdump と同じ行形式) と、wimlib-imagex で作った .wim を出力する。
# 使い方: python3 -I gen_wim_fixture.py <出力ディレクトリ>   → tree/ expected.txt lzx.wim none.wim lzms.wim、multi.wim (2 イメージ) と expected_multi.txt
import hashlib, os, random, subprocess, sys

out = sys.argv[1]
tree = os.path.join(out, 'tree')
os.makedirs(tree, exist_ok=True)
r = random.Random(20261118)
words = [b'alpha', b'bravo', b'charlie', b'delta', b'echo', b'foxtrot', b'golf', b'hotel', b'india', b'juliet', b'kilo', b'lima']

def text(n):
    b = bytearray()
    while len(b) < n:
        b += r.choice(words) + b' '
    return bytes(b[:n])

def w(rel, data):
    p = os.path.join(tree, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'wb') as f:
        f.write(data)

w('a.txt', b'hello\n')
w('dir1/b.txt', b'deep\n')
w('dir1/sub/c.txt', text(5000))
w('日本語.txt', 'ファイル\n'.encode())
w('with space/名前 with spaces.txt', b'x' * 10)
w('empty.txt', b'')
w('text.bin', text(3 * 1024 * 1024 + 777))          # 圧縮できる
w('random.bin', bytes(r.getrandbits(8) for _ in range(300000)))  # 圧縮できない
w('big.bin', text(32 * 1024 * 1024))                # 連続読みの測定用
for i in range(800):
    w('many/file_%d.txt' % i, b'file number %d\n' % i)

lines = []
for dp, dns, fns in os.walk(tree):
    for n in dns:
        lines.append('d - 0 - /' + os.path.relpath(os.path.join(dp, n), tree))
    for n in fns:
        p = os.path.join(dp, n)
        with open(p, 'rb') as f:
            data = f.read()
        lines.append('f %d 0 %s /%s' % (len(data), hashlib.sha256(data).hexdigest(), os.path.relpath(p, tree)))
with open(os.path.join(out, 'expected.txt'), 'w') as f:
    f.write('\n'.join(lines) + '\n')

for name, args in (('lzx', ['--compress=LZX']), ('none', ['--compress=none']), ('lzms', ['--compress=LZMS', '--solid'])):
    dst = os.path.join(out, name + '.wim')
    if os.path.exists(dst):
        os.remove(dst)
    subprocess.run(['wimlib-imagex', 'capture', tree, dst, 'img'] + args, check=True, stdout=subprocess.DEVNULL)

# 複数イメージの WIM (wimfuse の WimVolume: ルート直下にイメージごとのフォルダ "1 - 名前" / "2 - 名前")。小さいツリー 2 つ。expected_multi.txt はそのフォルダ込みのダンプ
multi_names = ['first', 'second image']
trees = [os.path.join(out, 'tree_m1'), os.path.join(out, 'tree_m2')]
files = [{'one.txt': b'image one\n', 'dir/deep.txt': b'deep in one\n', 'same.txt': b'same name, different image 1\n'},
         {'two.txt': b'image two\n', 'sub/x.bin': bytes(range(256)) * 40, 'same.txt': b'same name, different image 2\n', '日本語/ファイル.txt': 'こんにちは\n'.encode()}]
mlines = []
for k, (t, fs) in enumerate(zip(trees, files)):
    os.makedirs(t, exist_ok=True)
    folder = '/%d - %s' % (k + 1, multi_names[k])
    mlines.append('d - 0 - ' + folder)
    for rel, data in fs.items():
        pth = os.path.join(t, rel)
        os.makedirs(os.path.dirname(pth), exist_ok=True)
        with open(pth, 'wb') as f:
            f.write(data)
        d = os.path.dirname(rel)
        while d:
            line = 'd - 0 - %s/%s' % (folder, d)
            if line not in mlines:
                mlines.append(line)
            d = os.path.dirname(d)
        mlines.append('f %d 0 %s %s/%s' % (len(data), hashlib.sha256(data).hexdigest(), folder, rel))
with open(os.path.join(out, 'expected_multi.txt'), 'w') as f:
    f.write('\n'.join(mlines) + '\n')
mw = os.path.join(out, 'multi.wim')
if os.path.exists(mw):
    os.remove(mw)
subprocess.run(['wimlib-imagex', 'capture', trees[0], mw, multi_names[0], '--compress=LZX'], check=True, stdout=subprocess.DEVNULL)
subprocess.run(['wimlib-imagex', 'append', trees[1], mw, multi_names[1]], check=True, stdout=subprocess.DEVNULL)
print('ok', len(lines), 'multi', len(mlines))
