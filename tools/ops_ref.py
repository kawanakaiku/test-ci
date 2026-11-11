#!/usr/bin/env python3
"""run_write_tests.sh 用: 操作列 (usbmem_ntfs_tests --ops と同じ書式) を実ファイルシステム上で POSIX 操作として実行する (リファレンス)。
使い方: ops_ref.py <root> <script>    /   ops_ref.py --tree <root>  (全エントリを "種別 サイズ sha256 パス" で出力して比較に使う)"""
import os, sys, hashlib
def tree(root):
    out = []
    for dp, dns, fns in os.walk(root):
        dns.sort()
        rel = os.path.relpath(dp, root)
        if rel == "System Volume Information" or rel.startswith("System Volume Information"): continue
        for n in sorted(dns + fns):
            p = os.path.join(dp, n)
            r = os.path.relpath(p, root)
            if r.startswith("System Volume Information") or r.startswith("$"): continue
            if os.path.isdir(p): out.append("d 0 - " + r)
            else:
                try: d = open(p, "rb").read()
                except OSError: out.append("u 0 - " + r); continue     # ntfs-3g が読めない (WOF 圧縮・特殊リンク等)
                out.append("f %d %s %s" % (len(d), hashlib.sha256(d).hexdigest(), r))
    return sorted(out)
if sys.argv[1] == "--tree":
    print("\n".join(tree(sys.argv[2]))); sys.exit(0)
root, script = sys.argv[1], sys.argv[2]
P = lambda a: os.path.join(root, a.lstrip("/"))
for line in open(script):
    f = line.split()
    if not f or f[0].startswith("#"): continue
    op = f[0]
    if op == "mkdir": os.mkdir(P(f[1]))
    elif op == "create": open(P(f[1]), "xb").close()
    elif op == "unlink": os.unlink(P(f[1]))
    elif op == "rmdir": os.rmdir(P(f[1]))
    elif op == "rename": os.rename(P(f[1]), P(f[2]))
    elif op == "write":
        off, ln, seed = int(f[2]), int(f[3]), int(f[4])
        with open(P(f[1]), "r+b") as fh:
            fh.seek(0, 2)
            if fh.tell() < off: fh.write(bytes(off - fh.tell()))
            fh.seek(off); fh.write(bytes((((off + i) * 7) + seed) & 255 for i in range(ln)))
    else: raise SystemExit("bad op " + line)
