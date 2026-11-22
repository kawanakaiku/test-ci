#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""自前の Btrfs / XFS / F2FS のコードが、Linux カーネルのコードの写しでないことを機械的に確かめる (docs/provenance-btrfs-xfs.md)。

使い方:
  tools/provenance_scan.py --kernel <カーネルのツリー> [--ours <ファイル>...] [--group btrfs|xfs|f2fs|all] [--n 10] [--nn 24] [--top 15]

比べるもの (グループごとに、カーネル側は fs/<名前>/ と include/uapi/linux/<名前>*.h):
  1. トークン列の完全一致 (識別子・数値・記号をそのまま比べる。既定 10 トークン以上の連続一致)
  2. 正規化したトークン列の一致 (識別子 → I、数値 → N、文字列 → S に潰して比べる。名前を変えただけの写しの検出。既定 24 トークン以上)
  3. コメントの一致 (英語の語 6 語以上の連続一致)
  4. 識別子の重なり (自前のコードが使う識別子のうち、カーネルにも現れるもの。xfs_ / btrfs_ / f2fs_ の接頭辞つき・大文字マクロ・構造体のフィールド名などに分けて数える)
  5. カーネルの識別子への言及 (自前のコメント中の `XFS_FOO` のような語。写しではなく、読んだ証跡)
カーネルのツリーは tools/provenance_fetch.sh が /dev/shm に取得する (終わったら削除する)。
"""
import argparse, collections, os, re, sys, glob

TOKEN_RE = re.compile(r'''
    (?P<lc>//[^\n]*)
  | (?P<bc>/\*.*?\*/)
  | (?P<str>"(?:\\.|[^"\\\n])*")
  | (?P<chr>'(?:\\.|[^'\\\n])+')
  | (?P<num>0[xX][0-9a-fA-F']+[uUlLzZ]*|\d[\d']*(?:\.\d*)?(?:[eE][+-]?\d+)?[uUlLfFzZ]*)
  | (?P<id>[A-Za-z_][A-Za-z_0-9]*)
  | (?P<op>->\*?|\+\+|--|<<=?|>>=?|<=>|[-+*/%&|^!=<>]=|&&|\|\||::|\.\.\.|[{}()\[\];,.:?~!<>=+\-*/%&|^#\\@])
''', re.X | re.S)

C_KEYWORDS = set('''auto break case char const continue default do double else enum extern float for goto if inline int long
register return short signed sizeof static struct switch typedef union unsigned void volatile while bool true false nullptr
class namespace template typename using constexpr new delete public private protected this operator noexcept override final
uint8_t uint16_t uint32_t uint64_t int8_t int16_t int32_t int64_t size_t ssize_t std string vector map set array
__u8 __u16 __u32 __u64 __s8 __s16 __s32 __s64 __le16 __le32 __le64 __be16 __be32 __be64 u8 u16 u32 u64 s8 s16 s32 s64
NULL errno EIO EINVAL ENOMEM ENOSPC EROFS EUCLEAN EOPNOTSUPP ENOENT EEXIST EFBIG ENOTDIR EISDIR ENOTEMPTY EMLINK EPERM EACCES EBUSY ENAMETOOLONG ERANGE EDEADLK EDOM EAGAIN EILSEQ ENODATA ENOTSUP EXDEV'''.split())


def tokenize(text):
    """(トークン列 [(種別, 文字列, 行)], コメント列 [(文字列, 行)]) を返す。"""
    toks, comments = [], []
    line = 1
    pos = 0
    for m in TOKEN_RE.finditer(text):
        line += text.count('\n', pos, m.start())
        pos = m.start()
        k = m.lastgroup
        s = m.group()
        if k in ('lc', 'bc'):
            comments.append((s, line))
        else:
            toks.append((k, s, line))
        line += s.count('\n')
        pos = m.end()
    return toks, comments


def norm_tok(t):
    k, s, _ = t
    if k == 'id':
        return s if s in C_KEYWORDS else 'I'
    if k == 'num':
        return 'N'
    if k in ('str', 'chr'):
        return 'S'
    return s


def hash_ngrams(seq, n):
    """seq の n-gram のハッシュ列 (長さ len(seq)-n+1)。"""
    return [hash(tuple(seq[i:i + n])) for i in range(len(seq) - n + 1)]


def runs(flags, n):
    """n-gram の位置ごとの真偽 flags から、連続した一致の (開始, トークン数) の列を返す。"""
    out = []
    i = 0
    while i < len(flags):
        if flags[i]:
            j = i
            while j + 1 < len(flags) and flags[j + 1]:
                j += 1
            out.append((i, j - i + n))
            i = j + 1
        else:
            i += 1
    return out


def words(c):
    c = re.sub(r'^/[/*]+|\*/$', ' ', c)
    c = re.sub(r'(?m)^\s*\*+', ' ', c)
    return re.findall(r"[A-Za-z][A-Za-z0-9_']*", c.lower())


GROUPS = {
    'btrfs': (['fs/btrfs', 'lib/extra'], ['include/uapi/linux/btrfs*.h']),
    'xfs': (['fs/xfs', 'lib/extra'], ['include/uapi/linux/xfs*.h', 'include/uapi/linux/dqblk_xfs.h']),
    'f2fs': (['fs/f2fs', 'lib/extra'], ['include/uapi/linux/f2fs.h']),
}
# 展開器の確認 (btrfs / f2fs の圧縮): カーネルの lib と比べる
DECOMP = (['lib/lzo', 'lib/zstd', 'lib/lz4', 'lib/zlib_inflate'], ['include/linux/lzo.h', 'include/linux/zstd*.h', 'include/linux/lz4.h'])


def kernel_files(root, spec):
    dirs, globs = spec
    out = []
    for d in dirs:
        for dp, _, fn in os.walk(os.path.join(root, d)):
            for f in fn:
                if f.endswith(('.c', '.h', '.inc')):
                    out.append(os.path.join(dp, f))
    for g in globs:
        out += glob.glob(os.path.join(root, g))
    return sorted(out)


def load(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        return f.read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--kernel', required=True, action='append', help='カーネルのツリーの根 (複数可: 版ごと)')
    ap.add_argument('--ours', nargs='*', default=None)
    ap.add_argument('--group', default='all', choices=['all', 'btrfs', 'xfs', 'f2fs', 'decomp'])
    ap.add_argument('--n', type=int, default=10)
    ap.add_argument('--nn', type=int, default=24)
    ap.add_argument('--cw', type=int, default=6, help='コメントの語の連続一致の長さ')
    ap.add_argument('--top', type=int, default=12)
    ap.add_argument('--show', type=int, default=0, help='長い一致の実際の内容 (自前側) を表示する数')
    a = ap.parse_args()

    ours_by_group = collections.defaultdict(list)
    for p in (a.ours or []):
        b = os.path.basename(p)
        g = 'btrfs' if 'btrfs' in b else 'xfs' if 'xfs' in b else 'f2fs' if 'f2fs' in b else 'decomp'
        ours_by_group[g].append(p)
    groups = [a.group] if a.group != 'all' else ['btrfs', 'xfs', 'f2fs']
    for g in groups:
        files = ours_by_group.get(g, [])
        if not files:
            continue
        spec = GROUPS.get(g, DECOMP)
        print(f'\n################ グループ {g}: 自前 {len(files)} ファイル')
        # カーネル側 (版ごとに全部を 1 つの集合へ)
        kex, knm, kcw, kids, kcomm_ids = set(), set(), set(), set(), set()
        kfile_count = 0
        kern_tok = {}
        for root in a.kernel:
            for kp in kernel_files(root, spec):
                kfile_count += 1
                txt = load(kp)
                toks, comments = tokenize(txt)
                ids = [t[1] for t in toks]
                kern_tok[kp] = (toks, ids)
                kex.update(hash_ngrams(ids, a.n))
                knm.update(hash_ngrams([norm_tok(t) for t in toks], a.nn))
                for t in toks:
                    if t[0] == 'id':
                        kids.add(t[1])
                for c, _ in comments:
                    w = words(c)
                    for h in hash_ngrams(w, a.cw):
                        kcw.add(h)
                    for m in re.finditer(r'[A-Za-z_][A-Za-z_0-9]*', c):
                        kcomm_ids.add(m.group())
        print(f'カーネル側: {kfile_count} ファイル / 版 {len(a.kernel)} / 完全一致用 {len(kex)} / 正規化 {len(knm)} / コメント {len(kcw)}')
        # 自前
        print(f'{"ファイル":38} {"トークン":>8} {"完全一致(>= %d)":>14} {"最長":>5} {"正規化(>= %d)":>14} {"最長":>5} {"コメント一致":>10} {"共通識別子":>8}' % (a.n, a.nn))
        total_ex = total_tok = 0
        details = []
        for p in files:
            txt = load(p)
            toks, comments = tokenize(txt)
            ids = [t[1] for t in toks]
            n = len(toks)
            ex = [h in kex for h in hash_ngrams(ids, a.n)]
            ex_runs = runs(ex, a.n)
            nm = [h in knm for h in hash_ngrams([norm_tok(t) for t in toks], a.nn)]
            nm_runs = runs(nm, a.nn)
            ex_cov = sum(r[1] for r in ex_runs)
            nm_cov = sum(r[1] for r in nm_runs)
            # コメント
            cw_hits = []
            for c, ln in comments:
                w = words(c)
                hs = hash_ngrams(w, a.cw)
                if any(h in kcw for h in hs):
                    cw_hits.append((ln, ' '.join(w[:14])))
            # 識別子の重なり
            my_ids = collections.Counter(t[1] for t in toks if t[0] == 'id')
            shared = {i for i in my_ids if i in kids and i not in C_KEYWORDS}
            details.append((p, toks, ex_runs, nm_runs, cw_hits, shared, my_ids, comments))
            total_ex += ex_cov
            total_tok += n
            print(f'{os.path.basename(p):38} {n:8} {ex_cov:8} ({100.0*ex_cov/max(n,1):4.1f}%) {max([r[1] for r in ex_runs], default=0):5} {nm_cov:8} ({100.0*nm_cov/max(n,1):4.1f}%) {max([r[1] for r in nm_runs], default=0):5} {len(cw_hits):10} {len(shared):8}')
        print(f'合計: トークン {total_tok}、完全一致 {total_ex} ({100.0*total_ex/max(total_tok,1):.2f}%)')
        # 詳細
        for p, toks, ex_runs, nm_runs, cw_hits, shared, my_ids, comments in details:
            print(f'\n--- {os.path.basename(p)}')
            if ex_runs:
                top = sorted(ex_runs, key=lambda r: -r[1])[:a.top]
                print(f'  完全一致の長い順 (開始行: トークン数):', ', '.join(f'L{toks[s][2]}:{ln}' for s, ln in top))
                for s, ln in top[:a.show]:
                    print('    | ' + ' '.join(t[1] for t in toks[s:s + min(ln, 60)]))
            if nm_runs:
                top = sorted(nm_runs, key=lambda r: -r[1])[:a.top]
                print(f'  正規化一致の長い順:', ', '.join(f'L{toks[s][2]}:{ln}' for s, ln in top))
                for s, ln in top[:a.show]:
                    print('    | ' + ' '.join(t[1] for t in toks[s:s + min(ln, 60)]))
            if cw_hits:
                print('  コメント一致:', '; '.join(f'L{ln} "{w}"' for ln, w in cw_hits[:a.top]))
            # 識別子の分類
            pref = [i for i in shared if re.match(r'(xfs|btrfs|f2fs|XFS|BTRFS|F2FS)_', i)]
            upper = [i for i in shared if re.fullmatch(r'[A-Z][A-Z0-9_]{3,}', i) and i not in pref]
            under = [i for i in shared if '_' in i and i not in pref and i not in upper and not i.startswith('_')]
            print(f'  共通識別子 {len(shared)} (全 {len(my_ids)} 種): 接頭辞つき {len(pref)} {sorted(pref)[:a.top]} / 大文字 {len(upper)} {sorted(upper)[:a.top]} / 下線あり {len(under)} {sorted(under)[:a.top]}')
            # コメントが言及するカーネルの識別子
            men = set()
            for c, _ in comments:
                for m in re.finditer(r'\b(?:XFS|BTRFS|F2FS|xfs|btrfs|f2fs)_[A-Za-z0-9_]+', c):
                    if m.group() in kids:
                        men.add(m.group())
            if men:
                print(f'  コメント中のカーネル識別子への言及 {len(men)}: {sorted(men)[:a.top * 2]}')


if __name__ == '__main__':
    main()
