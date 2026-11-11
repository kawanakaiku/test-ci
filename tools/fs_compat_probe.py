#!/usr/bin/env python3
"""LinuxCompat に、あるカーネルのファイルシステムを載せるのに、何が足りないかを測る (移植の見積もり用)。
使い方: tools/fs_compat_probe.py <linux ソースのルート> <fs 名> [<fs 名> ...]
 1. 使っている #include <...> のうち、互換層 (core/compat/include) に無いもの
 2. 無いヘッダを空にして gcc -fsyntax-only し、エラーを (型 / 関数 / メンバ / 識別子) に分類して、ユニークな名前を数える
推測ではなく実コンパイラの結果を根拠にする。ファイルは変更しない。"""
import os, re, subprocess, sys, tempfile, collections

root, names = sys.argv[1], sys.argv[2:]
here = os.path.dirname(os.path.abspath(__file__))
inc = os.path.join(here, "..", "core", "compat", "include")

for fs in names:
    d = os.path.join(root, "fs", fs)
    cs = sorted(f for f in os.listdir(d) if f.endswith(".c"))
    hs = sorted(f for f in os.listdir(d) if f.endswith(".h"))
    lines = sum(len(open(os.path.join(d, f), errors="replace").read().splitlines()) for f in cs + hs)
    incs = collections.Counter()
    for f in cs + hs:
        for m in re.finditer(r'^\s*#\s*include\s*<([^>]+)>', open(os.path.join(d, f), errors="replace").read(), re.M):
            incs[m.group(1)] += 1
    missing = sorted(h for h in incs if not os.path.exists(os.path.join(inc, h)))
    print(f"\n=== fs/{fs}: {len(cs)} .c / {lines} 行, 使うカーネルヘッダ {len(incs)} 種, 互換層に無いもの {len(missing)} 種")
    with tempfile.TemporaryDirectory() as td:
        for h in missing:
            p = os.path.join(td, h); os.makedirs(os.path.dirname(p), exist_ok=True); open(p, "w").write("#pragma once\n")
        errs = collections.defaultdict(set)
        total = 0
        for c in cs:
            r = subprocess.run(["gcc", "-fsyntax-only", "-std=gnu11", "-w", "-fmax-errors=0", "-I", inc, "-isystem", td, "-iquote", d, "-DCONFIG_BLOCK=1", os.path.join(d, c)], capture_output=True, text=True, env=dict(os.environ, LC_ALL="C"))
            for l in r.stderr.splitlines():
                if "error:" not in l: continue
                total += 1
                m = re.search(r"error: (.*)", l); msg = m.group(1)
                if (x := re.match(r"unknown type name '(\w+)'", msg)): errs["型"].add(x.group(1))
                elif (x := re.match(r"implicit declaration of function '(\w+)'", msg)): errs["関数"].add(x.group(1))
                elif (x := re.match(r"'(\w+)' undeclared", msg)): errs["識別子"].add(x.group(1))
                elif (x := re.match(r"'([\w ]+)' has no member named '(\w+)'", msg)): errs["メンバ"].add(x.group(1) + "." + x.group(2))
                elif (x := re.match(r"invalid use of undefined type '([\w ]+)'", msg)): errs["未定義の構造体"].add(x.group(1))
                else: errs["その他"].add(msg[:70])
        print(f"  gcc エラー {total} 件。ユニーク: " + ", ".join(f"{k} {len(v)}" for k, v in sorted(errs.items())))
        for k in ("型", "関数", "識別子", "未定義の構造体", "メンバ"):
            if errs.get(k): print(f"   {k}: " + " ".join(sorted(errs[k])[:60]) + (" ..." if len(errs[k]) > 60 else ""))
    print("  無いヘッダ: " + " ".join(missing[:50]) + (" ..." if len(missing) > 50 else ""))
