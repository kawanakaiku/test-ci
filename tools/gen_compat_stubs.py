#!/usr/bin/env python3
"""未実装の LinuxCompat シンボルのスタブ(C)を生成する。
入力: 名前一覧(1 行 1 名)。プロトタイプは compat ヘッダから探す。
スタブは呼ばれたら usbmem_unimplemented(name) で必ず記録し、-ENOSYS / NULL / 0 を返す。"""
import re, sys, glob
names = [l.strip() for l in open(sys.argv[1]) if l.strip()]
hdr = ""
for f in ["vfs_api.h", "misc.h", "sync.h", "fs.h"]:
    hdr += open("core/compat/include/compat/" + f).read() + "\n"
hdr = re.sub(r"/\*.*?\*/", "", hdr, flags=re.S)
out = ['// SPDX-License-Identifier: GPL-2.0-or-later',
       '// 自動生成: tools/gen_compat_stubs.py (手編集しない)。未実装 API のスタブ。',
       '#include <compat/kernel.h>', '#include <compat/usbmem_ntfs.h>', '']
missing = []
for n in names:
    m = re.search(r"^([A-Za-z_][\w \*]*?[ \*])" + re.escape(n) + r"\(([^;{]*?)\)\s*;", hdr, re.M | re.S)
    if not m:
        missing.append(n); continue
    ret = m.group(1).strip(); raw = m.group(2).replace("\n", " ")
    params, depth, cur = [], 0, ""
    for ch in raw:   # トップレベルのカンマだけで分割 (関数ポインタ引数を壊さない)
        if ch == "(": depth += 1
        if ch == ")": depth -= 1
        if ch == "," and depth == 0: params.append(cur.strip()); cur = ""
        else: cur += ch
    params.append(cur.strip())
    if params == ["void"] or params == [""]: params = []
    plist = []
    for i, p in enumerate(params):
        if p == "...": plist.append("..."); continue
        # 関数ポインタ引数: 名前を (*)( の中へ
        if "(*" in p: plist.append(p.replace("(*", "(*a%d" % i, 1))
        else: plist.append(p + " a%d" % i)
    if ret.startswith("static") or ret.startswith("extern"): ret = ret.split(None, 1)[1]
    if ret == "void": body = '  usbmem_unimplemented("%s");' % n
    elif "*" in ret: body = '  usbmem_unimplemented("%s"); return (%s)0;' % (n, ret)
    elif ret in ("bool",): body = '  usbmem_unimplemented("%s"); return false;' % n
    elif ret.startswith("struct") or ret.startswith("enum"):
        body = '  usbmem_unimplemented("%s"); %s r; memset(&r, 0, sizeof r); return r;' % (n, ret)
    else: body = '  usbmem_unimplemented("%s"); return (%s)-ENOSYS;' % (n, ret)
    out.append("%s %s(%s) {\n%s\n}" % (ret, n, ", ".join(plist) if plist else "void", body))
open(sys.argv[2], "w").write("\n".join(out) + "\n")
sys.stderr.write("stubs: %d, no prototype: %s\n" % (len(names) - len(missing), missing))
