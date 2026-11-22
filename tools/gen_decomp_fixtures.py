#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""展開器 (core/include/usbmem/decomp.h) の試験データを作る。docs/decomp.md

  gen_decomp_fixtures.py <出力先> [--quick]

本物の圧縮器の出力を作る: zstd コマンド (レベル・戦略・窓・チェックサムの有無・標準入力 / ファイル)、lzop コマンド (-1 .. -9。LZO1X のブロック (256 KiB ごと) を
lzop のコンテナから取り出す)、Python の zlib (レベル・戦略)、LZ4 ブロック (Python で書いた単純な圧縮器。乱数で合法な系列も作る)。
出力先には、<名前>.raw (元データ)・<名前>.<zst|lzo|zlib|lz4> (圧縮データ)・manifest.txt (「方式 圧縮ファイル 元ファイル」の行) を置く。
zstd / lzop が無ければ、その方式は作らない (manifest に載せない)。tmpfs (/dev/shm) に作ること。
"""
import os, random, shutil, struct, subprocess, sys, zlib


def gen_data(kind, size, rnd):
    if size == 0:
        return b''
    if kind == 'zeros':
        return bytes(size)
    if kind == 'random':
        return rnd.randbytes(size)
    if kind == 'text':
        words = [b'the', b'of', b'btrfs', b'extent', b'xfs', b'f2fs', b'cluster', b'compress', b'zstd', b'lzo', b'block', b'inode', b'data', b'tree', b'a', b'to', b'and']
        out = bytearray()
        while len(out) < size:
            out += rnd.choice(words) + (b'\n' if rnd.random() < 0.08 else b' ')
        return bytes(out[:size])
    if kind == 'period':
        p = rnd.randbytes(rnd.choice([1, 2, 3, 7, 13, 64, 255]))
        return (p * (size // len(p) + 1))[:size]
    if kind == 'counter':
        return b''.join(struct.pack('<I', i * 3) for i in range(size // 4 + 1))[:size]
    if kind == 'sparse':
        b = bytearray(size)
        for _ in range(max(1, size // 200)):
            b[rnd.randrange(size)] = rnd.randrange(1, 256)
        return bytes(b)
    if kind == 'mixed':
        out = bytearray()
        while len(out) < size:
            k = rnd.choice(['zeros', 'random', 'text', 'period', 'counter', 'sparse'])
            out += gen_data(k, rnd.choice([10, 100, 1000, 5000, 30000]), rnd)
        return bytes(out[:size])
    raise ValueError(kind)


def lz4_compress_simple(data, rnd=None, wild=False):
    """LZ4 ブロック形式の単純な圧縮器 (貪欲法・4 バイトのハッシュ)。wild=True なら、乱数で一致を選んで合法な系列を作る。"""
    n = len(data)
    out = bytearray()
    anchor = 0
    i = 0
    table = {}
    mflimit = n - 12
    last_lit_start = n - 5

    def emit(lits_start, lits_end, ml, off):
        lit = lits_end - lits_start
        tok = (min(lit, 15) << 4) | (min(ml - 4, 15) if ml else 0)
        out.append(tok)
        if lit >= 15:
            r = lit - 15
            while r >= 255:
                out.append(255); r -= 255
            out.append(r)
        out.extend(data[lits_start:lits_end])
        if ml:
            out.extend(struct.pack('<H', off))
            if ml - 4 >= 15:
                r = ml - 4 - 15
                while r >= 255:
                    out.append(255); r -= 255
                out.append(r)

    while i < mflimit:
        key = data[i:i + 4]
        cand = table.get(key)
        table[key] = i
        if cand is not None and i - cand <= 65535 and not (wild and rnd.random() < 0.3):
            ml = 4
            maxl = last_lit_start - i
            while ml < maxl and data[cand + ml] == data[i + ml]:
                ml += 1
            if wild and ml > 4:
                ml = rnd.randint(4, ml)
            emit(anchor, i, ml, i - cand)
            i += ml
            anchor = i
        else:
            i += 1
    emit(anchor, n, 0, 0)
    return bytes(out)


def lzop_blocks(path_raw, level):
    """lzop で圧縮し、コンテナから LZO1X のブロック [(圧縮データ, 元データ)] を取り出す。"""
    p = subprocess.run(['lzop', f'-{level}', '-c', path_raw], capture_output=True)
    if p.returncode != 0:
        return None
    d = p.stdout
    if d[:9] != b'\x89LZO\x00\r\n\x1a\n':
        return None
    pos = 9
    version, lib, need = struct.unpack('>HHH', d[pos:pos + 6]); pos += 6
    method, lvl = d[pos], d[pos + 1]; pos += 2
    flags, = struct.unpack('>I', d[pos:pos + 4]); pos += 4
    if flags & 0x800:
        pos += 4
    pos += 4 + 4 + 4   # mode, mtime_low, mtime_high
    nl = d[pos]; pos += 1 + nl
    pos += 4           # header checksum
    if flags & 0x40:
        el, = struct.unpack('>I', d[pos:pos + 4]); pos += 4 + el + 4
    blocks = []
    raw = open(path_raw, 'rb').read()
    off = 0
    while True:
        usz, = struct.unpack('>I', d[pos:pos + 4]); pos += 4
        if usz == 0:
            break
        csz, = struct.unpack('>I', d[pos:pos + 4]); pos += 4
        if flags & 0x1: pos += 4     # adler32 (元)
        if flags & 0x100: pos += 4   # crc32 (元)
        if csz < usz:
            if flags & 0x2: pos += 4
            if flags & 0x200: pos += 4
        blk = d[pos:pos + csz]; pos += csz
        if csz < usz:
            blocks.append((blk, raw[off:off + usz]))
        off += usz
    return blocks


def main():
    if len(sys.argv) < 2:
        print(__doc__); return 2
    out = sys.argv[1]
    quick = '--quick' in sys.argv
    os.makedirs(out, exist_ok=True)
    rnd = random.Random(20261123)
    have_zstd = shutil.which('zstd') is not None
    have_lzop = shutil.which('lzop') is not None
    manifest = []
    counter = [0]

    def add(alg, ext, comp, raw):
        counter[0] += 1
        name = f'c{counter[0]:04d}'
        open(f'{out}/{name}.raw', 'wb').write(raw)
        open(f'{out}/{name}.{ext}', 'wb').write(comp)
        manifest.append(f'{alg} {name}.{ext} {name}.raw')

    sizes = [0, 1, 2, 3, 5, 17, 100, 1000, 4096, 4097, 20000, 65535, 131072, 131073, 262144 + 5]
    if not quick:
        sizes += [600000, 1 << 20]
    kinds = ['zeros', 'random', 'text', 'period', 'counter', 'sparse', 'mixed']
    datasets = []
    for size in sizes:
        for kind in kinds:
            if size > 300000 and kind not in ('text', 'mixed', 'zeros'):
                continue
            datasets.append((kind, size, gen_data(kind, size, rnd)))
    tmp = f'{out}/_tmp.bin'

    # ---- zstd
    if have_zstd:
        opts_list = [['-1'], ['-3'], ['-6'], ['-10'], ['-15'], ['-19'], ['--fast=3'], ['-1', '--no-check'], ['-3', '--check'], ['-5', '--no-dictID'],
                     ['--zstd=strategy=1'], ['--zstd=strategy=2'], ['--zstd=strategy=3'], ['--zstd=strategy=4'], ['--zstd=strategy=5'], ['--zstd=strategy=6'],
                     ['--zstd=strategy=7'], ['--zstd=strategy=8'], ['--zstd=strategy=9'], ['-3', '--zstd=wlog=10'], ['-3', '--zstd=wlog=17'], ['-19', '--zstd=wlog=17,hlog=12'],
                     ['--ultra', '-22'], ['-9', '--long=20'], ['-3', '--zstd=tlen=0'], ['-3', '--zstd=ldmMinMatch=4,windowLog=24'], ['-1', '--rsyncable'],
                     ['--zstd=strategy=3,tlen=64,minMatch=3'], ['--zstd=strategy=6,minMatch=7']]
        for idx, (kind, size, data) in enumerate(datasets):
            open(tmp, 'wb').write(data)
            for oi, opts in enumerate(opts_list):
                # すべての組を作ると多すぎる。データごとに、決まった個数をずらして選ぶ
                if (idx + oi) % (4 if quick else 2) != 0:
                    continue
                for via_stdin in (False, True):
                    if via_stdin and (idx + oi) % 3 != 0:
                        continue
                    cmd = ['zstd', '-q', '-c', '-T1'] + opts
                    if via_stdin:
                        p = subprocess.run(cmd, input=data, capture_output=True)
                    else:
                        p = subprocess.run(cmd + [tmp], capture_output=True)
                    if p.returncode == 0:
                        add('zstd', 'zst', p.stdout, data)
        # 複数ブロックに跨る・窓を越える大きいデータ (btrfs / f2fs は 128 KiB 窓で来る)
        big = gen_data('mixed', 700000, rnd)
        open(tmp, 'wb').write(big)
        for opts in (['-3', '--zstd=wlog=17'], ['-12', '--zstd=wlog=17'], ['-1']):
            p = subprocess.run(['zstd', '-q', '-c', '-T1'] + opts, input=big, capture_output=True)
            if p.returncode == 0:
                add('zstd', 'zst', p.stdout, big)
    else:
        print('zstd コマンドが無いので zstd の試験データは作らない', file=sys.stderr)

    # ---- lzop (LZO1X)
    if have_lzop:
        for idx, (kind, size, data) in enumerate(datasets):
            if size == 0:
                continue
            open(tmp, 'wb').write(data)
            for level in (1, 3, 7, 9):
                if (idx + level) % (3 if quick else 2) != 0:
                    continue
                blocks = lzop_blocks(tmp, level)
                for comp, raw in (blocks or []):
                    add('lzo', 'lzo', comp, raw)
    else:
        print('lzop コマンドが無いので LZO の試験データは作らない', file=sys.stderr)

    # ---- zlib
    for idx, (kind, size, data) in enumerate(datasets):
        for level in (0, 1, 6, 9):
            for strat in (zlib.Z_DEFAULT_STRATEGY, zlib.Z_FILTERED, zlib.Z_HUFFMAN_ONLY, zlib.Z_RLE, zlib.Z_FIXED):
                if (idx + level + strat) % (5 if quick else 3) != 0:
                    continue
                c = zlib.compressobj(level, zlib.DEFLATED, 15, 9, strat)
                add('zlib', 'zlib', c.compress(data) + c.flush(), data)

    # ---- LZ4 ブロック (自前の単純な圧縮器。wild = 乱数で一致を選ぶ)
    for idx, (kind, size, data) in enumerate(datasets):
        if size == 0 or size > 300000:
            continue
        add('lz4', 'lz4', lz4_compress_simple(data), data)
        add('lz4', 'lz4', lz4_compress_simple(data, rnd, True), data)

    if os.path.exists(tmp):
        os.remove(tmp)
    open(f'{out}/manifest.txt', 'w').write('\n'.join(manifest) + '\n')
    algs = {}
    for l in manifest:
        algs[l.split()[0]] = algs.get(l.split()[0], 0) + 1
    print(f'decomp fixtures: {len(manifest)} 件 {algs} → {out}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
