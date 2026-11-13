#!/bin/sh
# XFS の書き込みテスト (tools/run_xfs_*.sh) の共通関数。source して使う。docs/xfs-write.md
# 使い方: . "$HERE/xfs_common.sh"
#   mk_xfs <img> <size> <proto-dir または ""> [mkfs.xfs の追加オプション...]
#       mkfs.xfs でイメージを作る。書き込みの対象は rmapbt=0 / parent=0 (どちらも自前の書き込みは未対応で、書き込みを拒否する構成)。古い mkfs.xfs に無いオプションは外して再試行する。
#       終了コード 0 = 作れた / 3 = このmkfsに無いオプション (追加オプションが原因。呼び出し側が SKIP 扱いにする) / 1 = その他の失敗
#   xfs_clean <img>          xfs_repair -n が何も見つけない (終了コード 0。出力に "would" / "ERROR" / "corrupt" なし)
#   xfs_logprint_ok <img>    ログが CLEAN と xfs_logprint が言う
export PATH="$PATH:/sbin:/usr/sbin"

mk_xfs_plain() { _plain=1; mk_xfs "$@"; _r=$?; _plain=0; return $_r; }   # 既定のオプション (rmapbt=0 / parent=0) を付けずに、渡したオプションだけで作る (拒否される構成のイメージ用)
_plain=0
mk_xfs() {
  _img=$1; _sz=$2; _proto=$3; shift 3
  rm -f "$_img"; truncate -s "$_sz" "$_img"
  _n=1
  while [ $_n -le 3 ]; do
    case "$_plain.$_n" in
      0.1) _base="-m rmapbt=0 -n parent=0" ;;
      0.2) _base="-m rmapbt=0" ;;
      *) _base="" ;;
    esac
    if [ -n "$_proto" ]; then
      # shellcheck disable=SC2086
      _out=$(mkfs.xfs -q -f $_base "$@" -p "$_proto" "$_img" 2>&1) && return 0
    else
      # shellcheck disable=SC2086
      _out=$(mkfs.xfs -q -f $_base "$@" "$_img" 2>&1) && return 0
    fi
    [ "$_plain" = 1 ] && break
    _n=$((_n + 1))   # 古い mkfs.xfs に無いオプション (parent=0 など) を外して再試行
  done
  echo "mkfs.xfs failed: $* : $(printf '%s\n' "$_out" | head -2)" >&2
  case "$_out" in *"unknown"*|*"invalid"*|*"not supported"*|*"Minimum"*|*"must be"*|*"too small"*) return 3 ;; esac
  return 1
}

xfs_clean() {
  _o=$(xfs_repair -n "$1" 2>&1) && _rc=0 || _rc=$?
  _f=$(printf '%s\n' "$_o" | grep -v "^        - \|^Phase\|Cannot get host\|Repair may fail\|the image and" || true)
  if [ $_rc -ne 0 ]; then printf '%s\n' "$_f" | head -25; return 1; fi
  if printf '%s\n' "$_f" | grep -qE "would |ERROR|corrupt|bad |inconsistent|illegal|invalid|not match|mismatch|WARNING"; then
    printf '%s\n' "$_f" | head -25; return 1
  fi
  return 0
}

xfs_logprint_ok() {
  xfs_logprint -t "$1" 2>&1 | grep -q "<CLEAN>"
}
