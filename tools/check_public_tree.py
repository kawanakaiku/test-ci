#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""公開版のツリー (tools/export_public.sh の出力) を検査する。標準ライブラリのみ。ネットワーク不要。docs/editions.md

  check_public_tree.py <ツリー> [--forbid-word WORD ...] [--no-closure]

検査 (1 つでも違反があれば終了コード 1):
  license    GPL / LGPL / AGPL の SPDX (WITH の例外つきを含む。`A OR B` の片方が GPL 系でも不可) が無い。COPYING* / LICENSES/ / GPL の全文 / GPL の冒頭文面が無い。third_party/ があれば同じ検査
  spdx       ソースファイル (C / C++ / Kotlin / Java / Python / sh / CMake / Gradle / YAML / XML) のすべてに SPDX-License-Identifier があり、許可する識別子 (Apache-2.0 など permissive) である
  files      .pyc / __pycache__ / .git / ディスクイメージ・APK・鍵・資格情報の拡張子 / 個人のメモ (pw, usbmem_testset.log) が無い
  paths      個人環境の絶対パス (/home/… /run/media/… /Users/…) が無い
  email      メールアドレスらしき文字列が無い
  account    テスト用リポジトリの `<アカウント>/test-ci` が無い (`<owner>/test-ci` は可)。--forbid-word で指定した語 (CI ではリポジトリの所有者名) も無い
  root       LICENSE (Apache-2.0 の全文) と NOTICE と README.md が根にある
  closure    C / C++ の #include "…" がツリー内で解決できる。CMake が参照する tools/ のファイルがツリーにある (単独でビルドできることの静的な確認)
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

PERMISSIVE = {"apache-2.0", "mit", "bsd-2-clause", "bsd-3-clause", "0bsd", "isc", "zlib", "unlicense", "cc0-1.0", "bsl-1.0"}
SOURCE_EXT = {".c", ".h", ".cpp", ".cc", ".hpp", ".inc", ".kt", ".kts", ".java", ".gradle", ".py", ".sh", ".cmake", ".yml", ".yaml", ".xml", ".pro", ".properties"}
SOURCE_NAMES = {"CMakeLists.txt", "gradlew"}
FORBID_EXT = {".pyc", ".pyo", ".img", ".iso", ".apk", ".aab", ".jks", ".keystore", ".pem", ".key", ".p12", ".so", ".o", ".a"}
FORBID_NAMES = {"pw", "usbmem_testset.log", ".env", "id_rsa", "id_ed25519", "local.properties", "keystore.properties", "adbkey", "COPYING", "COPYING.LIB", "UPSTREAM.md"}
FORBID_DIRS = {"__pycache__", ".git", "LICENSES", "real_usb_reports", "ci-logs", "captures"}

SPDX_RX = re.compile(r"SPDX-License-Identifier:[ \t]*([^\n]*)")
TOKEN = r"[A-Za-z0-9.+\-]+"
EXPR_RX = re.compile(r"^\(?\s*(" + TOKEN + r"(?:\s+(?:WITH|OR|AND)\s+" + TOKEN + r")*)")
GPL_TEXT = re.compile(r"GNU (Lesser |Library |Affero )?General Public License|under the terms of the GNU|Free Software Foundation", re.I)

# 個人環境の絶対パス。このファイル自身が公開版の検査で引っかからないよう、文字列を分けて組む
P_HOME = "/" + "home/"
P_MEDIA = "/" + "run/media/"
P_USERS = "/" + "Users/"
PATH_RX = re.compile("(" + "|".join(re.escape(x) for x in (P_HOME, P_MEDIA, P_USERS)) + r")[A-Za-z0-9_.-]*")
EMAIL_RX = re.compile(r"[A-Za-z0-9._%+\-]+@[A-Za-z0-9\-]+(?:\.[A-Za-z0-9\-]+)*\.[A-Za-z]{2,}")
ACCOUNT_RX = re.compile(r"(<owner>|[^\s/\"'`()<>\[\],]*)/test-ci\b")


def spdx_of(text):
    m = SPDX_RX.search(text)
    if not m:
        return None
    rest = re.sub(r"\s*(\*/|-->)\s*$", "", m.group(1).strip())
    e = EXPR_RX.match(rest)
    return e.group(1) if e else rest


def spdx_ids(expr):
    out = []
    for t in re.split(r"\s+(?:OR|AND|WITH)\s+", expr):
        t = t.strip("() ")
        if t:
            out.append(t)
    return out


def is_copyleft(i):
    return i.lower().startswith(("gpl", "lgpl", "agpl"))


def walk(root):
    for d, dirs, files in os.walk(root):
        dirs.sort()
        for f in sorted(files):
            full = os.path.join(d, f)
            yield os.path.relpath(full, root).replace(os.sep, "/"), full


def read_text(full):
    try:
        with open(full, "rb") as f:
            b = f.read()
    except OSError:
        return None
    if b"\0" in b[:8192]:
        return None
    return b.decode("utf-8", errors="replace")


def line_of(text, pos):
    return text.count("\n", 0, pos) + 1


def check(root, forbid_words, closure=True):
    errs = []

    def err(kind, rel, msg):
        errs.append("%-8s %s: %s" % (kind, rel, msg))

    files = list(walk(root))
    relset = {r for r, _ in files}
    # --- root
    for need in ("LICENSE", "NOTICE", "README.md"):
        if need not in relset:
            err("root", need, "根に無い")
    lic = read_text(os.path.join(root, "LICENSE")) or ""
    if "Apache License" not in lic or "Version 2.0, January 2004" not in lic or "TERMS AND CONDITIONS FOR USE, REPRODUCTION, AND DISTRIBUTION" not in lic:
        err("root", "LICENSE", "Apache-2.0 の全文ではない")
    # --- ディレクトリ名・ファイル名
    for rel, full in files:
        parts = rel.split("/")
        base = parts[-1]
        ext = os.path.splitext(base)[1].lower()
        if any(p in FORBID_DIRS for p in parts[:-1]):
            err("files", rel, "公開版に置かない場所 (%s)" % [p for p in parts[:-1] if p in FORBID_DIRS][0])
        if ext in FORBID_EXT or base in FORBID_NAMES or base.startswith("COPYING"):
            err("files", rel, "公開版に置かないファイル")
        if rel.startswith("third_party/"):
            err("license", rel, "third_party/ は公開版に置かない (GPL 系のカーネル由来。許可する外部コードが出たら、このチェックを見直す)")
    # --- 中身
    for rel, full in files:
        base = os.path.basename(rel)
        ext = os.path.splitext(base)[1].lower()
        text = read_text(full)
        if text is None:
            continue
        # license
        m = SPDX_RX.search(text)
        expr = spdx_of(text) if m else None
        if expr:
            ids = spdx_ids(expr)
            if any(is_copyleft(i) for i in ids):
                err("license", rel, "SPDX が GPL 系: %s (L%d)" % (expr, line_of(text, m.start())))
            elif any(i.lower() not in PERMISSIVE and i.upper() not in ("WITH", "OR", "AND") for i in ids):
                err("spdx", rel, "許可していない SPDX 識別子: %s" % expr)
        if (ext in SOURCE_EXT or base in SOURCE_NAMES) and not expr:
            err("spdx", rel, "SPDX-License-Identifier が無い")
        head = "\n".join(text.splitlines()[:80])
        if (ext in SOURCE_EXT or base in SOURCE_NAMES) and GPL_TEXT.search(head):
            err("license", rel, "冒頭に GPL 系のライセンス文面がある")
        if "GNU GENERAL PUBLIC LICENSE" in text or "GNU LESSER GENERAL PUBLIC LICENSE" in text:
            err("license", rel, "GPL / LGPL の全文を含む")
        # paths / email / account
        for m in PATH_RX.finditer(text):
            err("paths", rel, "個人環境の絶対パス: '%s' (L%d)" % (m.group(0), line_of(text, m.start())))
        for m in EMAIL_RX.finditer(text):
            err("email", rel, "メールアドレスらしき文字列: '%s' (L%d)" % (m.group(0), line_of(text, m.start())))
        for m in ACCOUNT_RX.finditer(text):
            if m.group(1) != "<owner>":
                err("account", rel, "テスト用リポジトリのアカウント名: '%s' (L%d)" % (m.group(0), line_of(text, m.start())))
        for w in forbid_words:
            i = text.find(w)
            if i >= 0:
                err("account", rel, "禁止語 '%s' (L%d)" % (w, line_of(text, i)))
    # --- closure
    if closure:
        inc_dirs = ["core/include", "core/compat/public", "core/src", "core/tests", "core/compat/tests", "android/app/src/main/cpp"]
        inc_rx = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)
        pp_rx = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)")
        personal_macro = re.compile(r"USBMEM_FS_|USBMEM_BENCH_")   # 個人版だけが定義するマクロの中 (jni_ntfs.cpp の FS ごとの include など) は、この版では評価されない
        for rel, full in files:
            if os.path.splitext(rel)[1].lower() not in (".c", ".h", ".cpp", ".cc", ".hpp", ".inc"):
                continue
            d = os.path.dirname(rel)
            stack = []
            for ln, line in enumerate((read_text(full) or "").splitlines(), 1):
                mm = pp_rx.match(line)
                if mm:
                    kw = mm.group(1)
                    if kw in ("if", "ifdef", "ifndef"):
                        stack.append(bool(personal_macro.search(mm.group(2))) and kw != "ifndef")
                    elif kw in ("elif", "else") and stack:
                        stack[-1] = False
                    elif kw == "endif" and stack:
                        stack.pop()
                    continue
                m = inc_rx.match(line)
                if not m or any(stack):
                    continue
                inc = m.group(1)
                cands = [os.path.normpath(os.path.join(d, inc)).replace(os.sep, "/")] + [(x + "/" + inc) for x in inc_dirs]
                if not any(c in relset for c in cands):
                    err("closure", rel, '#include "%s" がツリー内に無い (L%d)' % (inc, ln))
        tool_rx = re.compile(r"tools/[A-Za-z0-9_./-]+\.(?:py|sh|json|txt)")
        cmake_files = [(rel, full) for rel, full in files if os.path.basename(rel) == "CMakeLists.txt" or rel.endswith(".cmake")]
        texts = {rel: (read_text(full) or "") for rel, full in cmake_files}

        def scan(defined):
            """ツリーの CMake を、`if(... TARGET <未定義> ...)` の中を除いて読む。(その周回で定義されるターゲット, 参照するファイル)"""
            nd, refs = set(), []
            for rel, text in texts.items():
                stack = []   # 各 if ブロック: (未定義ターゲットを条件にしている = この版では評価されない, EXISTS で守られている)
                for ln, line in enumerate(text.splitlines(), 1):
                    code = line.split("#", 1)[0]
                    mm = re.match(r"\s*(if|elseif|else|endif)\s*\((.*)\)", code, re.I)
                    if mm:
                        kw, cond = mm.group(1).lower(), mm.group(2)
                        if kw == "if":
                            tg = re.findall(r"(?<!NOT )TARGET\s+([A-Za-z0-9_]+)", cond)
                            stack.append((any(t not in defined for t in tg), "EXISTS" in cond))
                        elif kw in ("elseif", "else") and stack:
                            stack[-1] = (False, False)
                        elif kw == "endif" and stack:
                            stack.pop()
                        continue
                    if any(dead for dead, _ in stack):
                        continue
                    for m in re.finditer(r"add_(?:library|executable)\(\s*([A-Za-z0-9_]+)", code):
                        nd.add(m.group(1))
                    for m in tool_rx.finditer(code):
                        refs.append((rel, ln, m.group(0), any(g for _, g in stack) or "EXISTS" in "\n".join(text.splitlines()[max(0, ln - 2):ln])))
            return nd, refs

        defined = {m.group(1) for t in texts.values() for m in re.finditer(r"add_(?:library|executable)\(\s*([A-Za-z0-9_]+)", t)}
        for _ in range(6):
            nd, refs = scan(defined)
            if nd == defined:
                break
            defined = nd
        for rel, ln, t, guarded in refs:
            if t not in relset and not guarded:
                err("closure", rel, "CMake が参照するファイルがツリーに無い: %s (L%d)" % (t, ln))
    return errs, len(files)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tree")
    ap.add_argument("--forbid-word", action="append", default=[], help="ツリーのどこにも無いはずの語 (CI ではリポジトリの所有者名など)。複数可")
    ap.add_argument("--no-closure", action="store_true")
    a = ap.parse_args()
    if not os.path.isdir(a.tree):
        print("ツリーがありません: %s" % a.tree, file=sys.stderr)
        return 2
    errs, n = check(a.tree, [w for w in a.forbid_word if w], closure=not a.no_closure)
    for e in errs:
        print("NG", e)
    if errs:
        kinds = sorted({e.split()[0] for e in errs})
        print("\nNG: %d 件 (%s)、%d ファイルを検査" % (len(errs), ", ".join(kinds), n))
        return 1
    print("OK: %d ファイルを検査。GPL 系なし・SPDX あり・個人情報なし・単独で解決可能" % n)
    return 0


if __name__ == "__main__":
    sys.exit(main())
