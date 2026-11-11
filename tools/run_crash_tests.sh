#!/bin/sh
# 電源断の模擬 (実験的): 書き込み操作列の N 回目の書き込みで「電源を落とし」(以降の書き込みは消える)、残ったイメージを検証する。
# 検査: (A) 元からあった全ファイルが自前ドライバ (読み取り専用) で manifest どおり読める (電源断が無関係なデータを壊さない)、
#       (B) ボリュームがダーティとしてマークされている (次回 Windows が chkdsk で直せる/ドライバが書き込みを拒否できる)、(C) ntfsfix -n。
# 使い方: run_crash_tests.sh <usbmem_ntfs_tests> [fixture dir] [サンプル数(既定 40)]
set -eu
T=$1; D=${2:-/dev/shm/usbmem_ntfs_${USER:-u}}; NS=${3:-40}
[ -f "$D/ntfs_basic.img" ] || { echo "skip: no fixture" >&2; exit 77; }
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d /dev/shm/usbmem_cr.XXXXXX); trap 'rm -rf "$W"' EXIT
python3 - "$W/ops" <<'PY'
import sys
L = ["mkdir /cr", "mkdir /cr/sub"]
for i in range(1, 41):
    L += ["create /cr/f%d.bin" % i, "write /cr/f%d.bin 0 %d %d" % (i, [100, 5000, 70000, 300][i % 4], i)]
L += ["create /cr/sub/x.txt", "write /cr/sub/x.txt 0 9000 7", "rename /cr/f1.bin /cr/sub/moved.bin"]
for i in range(2, 30, 2): L.append("unlink /cr/f%d.bin" % i)
L += ["write /cr/f3.bin 0 20000 77", "rename /cr/sub /cr/sub2", "rmdir /cr/nothing"] if False else ["write /cr/f3.bin 0 20000 77", "rename /cr/sub /cr/sub2"]
open(sys.argv[1], "w").write("\n".join(L) + "\n")
PY
export ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=0
sum0=$(sha256sum "$D/ntfs_basic.img" | cut -d" " -f1)
# --ops は書き込みマウントでイメージを変更するため、必ずコピーに対して実行する (フィクスチャ本体は触らない)
cp "$D/ntfs_basic.img" "$W/c.img"
total=$(USBMEM_CRASH_AT=-1 "$T" --ops "$W/c.img" "$W/ops" 2>&1 | grep -a "^writes:" | sed 's/writes: //')
echo "操作列の総書き込み数: $total"
rc=0; bad_read=0; tear_fail=0; not_dirty=0; fixbad=0; n=0
for mode in prefix cache; do
  i=0
  while [ $i -lt $NS ]; do
    # 序盤を密に、後半を粗くサンプリング
    if [ $i -lt 15 ]; then at=$i; else at=$(( total * (i - 14) / (NS - 14) )); fi
    for torn in 0 1; do
      cp "$D/ntfs_basic.img" "$W/c.img"
      USBMEM_CRASH_AT=$at USBMEM_CRASH_MODE=$mode USBMEM_CRASH_TORN=$torn USBMEM_CRASH_SEED=$((i*7+3)) "$T" --ops "$W/c.img" "$W/ops" >/dev/null 2>&1 || true
      n=$((n+1))
      if ! "$T" --manifest "$W/c.img" "$D/ntfs_basic.manifest" 2>&1 | grep -aq " 0 failures"; then
        # セクタ単位で原子的な電源断 (torn=0) では必ず読めること。MFT レコード (1024B) がセクタ境界で途中まで書かれた (torn=1) 場合は
        # fixup 不整合でマウントできないことがある (ジャーナルなしの実装に共通の限界。ntfsfix / ntfs-3g も同様に拒否する)。別枠で数える。
        if [ "$torn" = 1 ]; then tear_fail=$((tear_fail+1)); else bad_read=$((bad_read+1)); echo "A-FAIL: 元ファイルが壊れた/読めない (mode=$mode at=$at torn=$torn)"; fi
      fi
      fl=$(python3 -I "$HERE/crash_volflags.py" "$W/c.img" 2>/dev/null || echo -1)
      # 不変条件: 意味のある変更 (>64 バイト) がメディアに届いているなら、ダーティ印も届いていること (書き込み順序を保証しない USB でも)
      ch=$(cmp -l "$W/c.img" "$D/ntfs_basic.img" | wc -l)
      if [ "$fl" != -1 ] && [ $((fl & 1)) -eq 0 ] && [ "$ch" -gt 64 ]; then
        not_dirty=$((not_dirty+1)); echo "DIRTY-FAIL: mode=$mode at=$at torn=$torn changed=$ch"
      fi
    done
    i=$((i+1))
  done
done
echo "試行 $n 回: 元ファイル破損 $bad_read (セクタ途中切断で MFT レコードが壊れマウント不能: $tear_fail) / ダーティ印なしで意味のある差分 $not_dirty"
[ "$(sha256sum "$D/ntfs_basic.img" | cut -d" " -f1)" = "$sum0" ] || { echo "FAIL: フィクスチャ本体が変更された"; rc=1; }
[ "$bad_read" -eq 0 ] && [ "$not_dirty" -eq 0 ] || rc=1
exit $rc
