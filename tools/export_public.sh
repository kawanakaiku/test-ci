#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# 公開版 (public edition) のツリーを書き出して検査する (docs/editions.md)。
#   使い方: tools/export_public.sh <出力ディレクトリ> [--allow-any-fs] [--forbid-word WORD ...]
#   - 出力先: 新しい (または空の) ディレクトリ。リポジトリの外・tmpfs (/dev/shm の下など)。Data ディスクには作らない。
#   - 内容: git の追跡ファイル (作業ツリーの内容) から、tools/public_manifest.txt の規則どおりに公開版のツリーを作る。履歴は引き継がない。
#   - 書き出したあと、tools/check_public_tree.py で検査する (GPL 系・SPDX・絶対パス・メールアドレス・アカウント名・単独で解決できるか)。
#   - 公開 (push・リモートの作成・可視性の変更) はしない。公開用の履歴は、書き出したツリーで `git init` して別に作る (docs/editions.md 7 章)。
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
[ $# -ge 1 ] || { sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
out=$1; shift
export_args=(); check_args=()
while [ $# -gt 0 ]; do
  case "$1" in
    --allow-any-fs) export_args+=("$1") ;;
    --forbid-word)  check_args+=("$1" "${2:?--forbid-word の値}"); shift ;;
    *) echo "不明な引数: $1" >&2; exit 2 ;;
  esac
  shift
done
python3 "$HERE/license_inventory.py" --check
python3 "$HERE/export_public.py" "$out" "${export_args[@]}"
python3 -I "$HERE/check_public_tree.py" "$out" "${check_args[@]}"
