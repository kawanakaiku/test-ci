#!/bin/sh
# NTFS の $MFTMirr の扱い (docs/ntfs-mftmirr.md)。
#  (A) 64 KiB クラスタで $MFTMirr の先頭 4 レコードより後ろが $MFT と違っても (Windows 製の状態) 書き込み可で開けること。先頭 4 レコードの不一致は従来どおり読み取り専用に落ちること。
#  (B) torn write で壊れた MFT システムレコード (0-3) を $MFTMirr のコピーで読み出せること (読み取り専用のみ)。
# tools/gen_ntfs_mirror_fixture.py が作るイメージに対して usbmem_ntfs_tests --mirror を実行し、書き込み可で開いたイメージは独立実装で検証する
# (ntfsfix -n / ntfs-3g.probe --readwrite / 自前の fsck)。書き込みビルド (USBMEM_NTFS_WRITE=ON) でなければ読み取り専用の部分だけ。
# 使い方: run_ntfs_mirror_tests.sh <usbmem_ntfs_tests> [usbmem_disktools_cli] [作業ディレクトリ (tmpfs。既定 /dev/shm/usbmem_ntfs_mirror_$USER)]
set -eu
export PATH="$PATH:/sbin:/usr/sbin"
T=$1; CLI=${2:-}; D=${3:-/dev/shm/usbmem_ntfs_mirror_${USER:-u}}
for c in mkntfs ntfs-3g fusermount3 python3 ntfsfix ntfs-3g.probe; do command -v $c >/dev/null || { echo "skip: $c missing" >&2; exit 77; }; done
HERE=$(cd "$(dirname "$0")" && pwd)
rm -rf "$D"
trap 'rm -rf "$D"' EXIT
rc=0
python3 -I "$HERE/gen_ntfs_mirror_fixture.py" "$D" || rc=$?
[ "$rc" -eq 0 ] || exit "$rc"   # 77 (ツールなし) もそのまま返す
export ASAN_OPTIONS=${ASAN_OPTIONS:-verify_asan_link_order=0:detect_leaks=0}
rc=0
"$T" --mirror "$D" || rc=$?
[ "$rc" -eq 0 ] || exit "$rc"
# 書き込み可で開いて書いたイメージ (テストが "<名前>.written" の印を残す) を独立実装で検証する
fail=0
for img in "$D"/ntfs_mirror_*.img; do
  [ -f "$img.written" ] || continue
  n=$(basename "$img" .img); n=${n#ntfs_mirror_}
  # 自前の fsck (ミラーは先頭 4 レコードだけを見る。エラーが無いこと)
  if [ -n "$CLI" ] && [ -x "$CLI" ]; then
    v=$("$CLI" check "$img" --fs ntfs 2>/dev/null | python3 -I -c 'import sys,json; j=json.load(sys.stdin); print(j.get("verdict")); [print("  ", f["severity"], f["code"], f.get("message","")) for f in j.get("findings",[]) if f["severity"]!="info"]') || v=crash
    case "$(echo "$v" | head -1)" in
      clean|warnings) echo "fsck[$n]: $(echo "$v" | head -1)" ;;
      *) echo "FAIL[$n] own fsck: $v"; fail=1 ;;
    esac
  fi
  # ntfs-3g.probe --readwrite: ntfs-3g が書き込みマウントできる状態か (先頭 16 レコードのミラー一致を見る。win_mid は意図的に外れるので対象外)
  case "$n" in
    win_mid) ;;
    *) ntfs-3g.probe --readwrite "$img" || { echo "FAIL[$n] ntfs-3g.probe --readwrite rc=$?"; fail=1; } ;;
  esac
  # ntfsfix -n: 先頭 4 レコードより後ろのミラーが古いままの Windows 風イメージは、ntfsfix も「差分を直す」と言う (-n では書けず失敗扱い)。
  # それはテストが作った状態そのものなので、対照 (plain / plain4k) だけ成功を求める
  case "$n" in
    plain|plain4k) ntfsfix -n "$img" >"$D/ntfsfix.$n.log" 2>&1 || { echo "FAIL[$n] ntfsfix -n"; tail -5 "$D/ntfsfix.$n.log"; fail=1; } ;;
    *) ntfsfix -n "$img" >"$D/ntfsfix.$n.log" 2>&1 && echo "ntfsfix -n[$n]: ok" || echo "ntfsfix -n[$n]: reports the stale mirror records (expected for the Windows-like image): $(grep -c 'Correcting differences' "$D/ntfsfix.$n.log") record(s)" ;;
  esac
done
[ "$fail" -eq 0 ] || exit 1
echo "ntfs mirror tests: ok"
