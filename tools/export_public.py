#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""公開版 (public edition) のツリーを書き出す。tools/export_public.sh から呼ばれる (docs/editions.md)。標準ライブラリのみ。

  export_public.py <出力ディレクトリ> [--manifest PATH] [--allow-any-fs] [--dry-run]

git の追跡ファイル (作業ツリーの内容) に tools/public_manifest.txt の規則を当て、公開版に入るものだけを出力ディレクトリへ書く。履歴は引き継がない (git init もしない)。
  - include   : そのまま。SPDX が無いソースファイル (Kotlin・Python・CMake・XML など) には `SPDX-License-Identifier: Apache-2.0` の行を足す
  - relicense : SPDX の GPL 系の表記を Apache-2.0 に書き換える (自前のコード。理由は manifest の注記)
  - overlay   : 公開版専用の雛形 (README / LICENSE / NOTICE / .gitignore / ci.yml / compat の CMakeLists.txt) を所定の場所へ置く
  - scrub     : テキストファイルに置換を当てる (テスト用リポジトリのアカウント名の伏せ字化)
出力先は新しい (または空の) ディレクトリで、リポジトリの外、かつ tmpfs でなければならない (Data ディスクに大きな書き込みをしない)。CI など tmpfs でない場所では --allow-any-fs。
"""
import argparse
import os
import re
import stat
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import license_inventory as li  # noqa: E402

SPDX_LINE = "SPDX-License-Identifier: Apache-2.0"
# 拡張子 → コメントの形式。ここに無い拡張子は、SPDX を足さない (ドキュメント・バイナリ・設定の値だけのファイル)
SLASH = {".c", ".h", ".cpp", ".cc", ".hpp", ".inc", ".kt", ".kts", ".java", ".gradle"}
HASH = {".py", ".sh", ".cmake", ".yml", ".yaml", ".pro", ".properties"}
XML = {".xml"}


def comment_style(path):
    base = os.path.basename(path)
    ext = os.path.splitext(base)[1].lower()
    if base == "CMakeLists.txt":
        return "hash"
    if base == "gradlew":
        return "hash"
    if ext in SLASH:
        return "slash"
    if ext in HASH:
        return "hash"
    if ext in XML:
        return "xml"
    return None


def stamp(path, text):
    """SPDX の無いテキストの先頭 (シバン / XML 宣言の次) に Apache-2.0 の行を足す。足せない形式は None。"""
    st = comment_style(path)
    if st is None:
        return None
    nl = "\r\n" if "\r\n" in text[:2000] else "\n"
    lines = text.split(nl)
    at = 0
    if lines and lines[0].startswith("#!"):
        at = 1
    if st == "xml":
        if lines and lines[0].lstrip().startswith("<?xml"):
            at = 1
        line = "<!-- %s -->" % SPDX_LINE
    elif st == "slash":
        line = "// " + SPDX_LINE
    else:
        line = "# " + SPDX_LINE
    lines.insert(at, line)
    return nl.join(lines)


def relicense(text):
    m = li.SPDX_RX.search(text)
    if not m:
        return text, False
    rest = m.group(1)
    e = li.EXPR_RX.match(re.sub(r"\s*(\*/|-->)\s*$", "", rest.strip()))
    if not e:
        return text, False
    start = m.start(1) + (len(rest) - len(rest.lstrip()))
    expr = e.group(1)
    i = text.index(expr, start)
    return text[:i] + "Apache-2.0" + text[i + len(expr):], True


def is_text(data):
    if b"\0" in data[:8192]:
        return None
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return None


def fs_type(path):
    try:
        return subprocess.run(["stat", "-f", "-c", "%T", path], check=True, capture_output=True, text=True).stdout.strip()
    except Exception:
        return "?"


def check_out(out, allow_any_fs):
    out = os.path.realpath(out)
    root = os.path.realpath(li.ROOT)
    if out == root or out.startswith(root + os.sep) or root.startswith(out + os.sep):
        raise SystemExit("出力先はリポジトリの外にしてください: %s" % out)
    if os.path.exists(out):
        if not os.path.isdir(out) or os.listdir(out):
            raise SystemExit("出力先は新しい (または空の) ディレクトリにしてください: %s" % out)
    else:
        parent = os.path.dirname(out)
        if not os.path.isdir(parent):
            raise SystemExit("出力先の親ディレクトリがありません: %s" % parent)
    probe = out if os.path.isdir(out) else os.path.dirname(out)
    t = fs_type(probe)
    if t != "tmpfs" and not allow_any_fs:
        raise SystemExit("出力先 %s は tmpfs ではありません (%s)。/dev/shm の下を指定してください (Data ディスクに書かない)。CI など意図した場所なら --allow-any-fs" % (out, t))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--manifest", default=li.DEFAULT_MANIFEST)
    ap.add_argument("--allow-any-fs", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    out = check_out(a.out, a.allow_any_fs)
    rules, scrubs, rows = li.build(a.manifest)
    bad = li.problems(rules, rows)
    if bad:
        print("公開版の定義に矛盾があります (tools/license_inventory.py --check):", file=sys.stderr)
        for m in bad:
            print("  " + m, file=sys.stderr)
        return 1
    overlay_src = {r.dest: r.glob for r in rules if r.action == "overlay"}
    counts = {"include": 0, "relicense": 0, "overlay": 0, "stamped": 0}
    for row in rows:
        if not row["publish"]:
            continue
        p, act = row["path"], row["action"]
        src_rel = overlay_src[p] if act == "overlay" else p
        src = os.path.join(li.ROOT, src_rel)
        with open(src, "rb") as f:
            data = f.read()
        mode = os.stat(src).st_mode
        text = is_text(data)
        if text is not None:
            if act == "relicense":
                text, ok = relicense(text)
                if not ok:
                    raise SystemExit("relicense: SPDX の行が見つかりません: " + p)
            if act != "overlay" and li.extract_spdx(text) is None:
                t2 = stamp(p, text)
                if t2 is not None:
                    text = t2
                    counts["stamped"] += 1
            for rx, repl in scrubs:
                text = rx.sub(repl, text)
            data = text.encode("utf-8")
        counts[act] += 1
        if a.dry_run:
            continue
        dst = os.path.join(out, p)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(data)
        os.chmod(dst, 0o755 if mode & stat.S_IXUSR else 0o644)
    print("公開版のツリー: %s%s" % (out, " (dry-run: 書いていません)" if a.dry_run else ""))
    print("  include %d / relicense %d / overlay %d (うち SPDX を足したファイル %d)" % (counts["include"], counts["relicense"], counts["overlay"], counts["stamped"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
