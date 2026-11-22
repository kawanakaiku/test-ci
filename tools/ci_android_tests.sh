#!/bin/bash
# CI 用のまとめスクリプト (.github/workflows/android-emulator.yml から呼ばれる。docs/ci.md)。AVD (emulator-*) だけに触る。
#   tools/ci_android_tests.sh gen <グループ>...   ホストでフィクスチャを作る (エミュレータ不要。apt のホストツールが要る)
#   tools/ci_android_tests.sh run <グループ>...   エミュレータ上で FS 別の計装テストを順に実行し、失敗を集計する (1 つでも失敗なら終了コード 1)
# グループ (workflow の matrix と同じ名前):
#   ext-fat        FAT (6 種) / FAT 書き込み / ext2 書き込み / exFAT 書き込み
#   btrfs-xfs      Btrfs (16 イメージ) / Btrfs 書き込み (btrfs check と btrfs restore で検証) / XFS (5 イメージ) / XFS 書き込み (xfs_repair -n と実カーネルで検証)
#   squash-erofs   squashfs / erofs / F2FS (mkfs.f2fs + sload.f2fs で作る 1 イメージだけ。カーネル製 k_* は VM が要るので CI では扱わない) / F2FS 書き込み (f2fs_write: 結果を fsck.f2fs とカーネルのループマウントで検証)
#   nested-ntfs    ネスト (ループバック) マウント / NTFS 書き込み (SAF)
#   crypt-ui       暗号ボリュームの UI 試験 (CryptoUiTest: 画面を操作して LUKS2 / BitLocker を解錠・書き込み・ロック。tools/gen_crypt_ui_fixture.py)
#   disk-ui        ディスクツールの UI 試験 (DiskUiTest: 検査・フォーマット・パーティション編集・バックアップ/復元・救出の画面。書き込んだイメージは adb で取り出し、e2fsck / fsck.fat / fsck.exfat / sgdisk / cmp で外から検証。tools/gen_disk_ui_fixture.py。docs/disk-ui.md)
#   wim-fuse       個人版の FUSE 層 (別プロセスの wimlib の wimmount): WimFuseTest (:wimfuse サービスの exec・RPC・WIM のツリーの一致・子プロセスの後始末)。要 wimtools (フィクスチャ)・-PusbmemWimFuse のビルド (docs/fuse-layer.md 10 章)
#   kfsd           kfsd (GPL 系の FS ドライバを別プロセスの実行ファイルに分けたもの。docs/kfsd.md) 経由のマウント: KfsdInstrumentedTest (:kfsd サービスの libkfsd.so と別 APK のプラグイン)、既存の読み取りテスト (ext2 / ext4 / exfat / iso / ntfs) と WriteSelfTest を kfsd 経由で通す。要 -PusbmemKfsd=true と :kfsd-plugin のビルド
#   ro-misc        読み取りテスト (ext2 / ext4 / exFAT / ISO 9660 / NTFS) と アプリ側テスト (DocumentsProvider / リンク / raw イメージ / 転送チューニング)。専用スクリプトが無いので ci_android_tests.sh が直接 push して実行する
# 環境変数: ANDROID_SERIAL (既定 emulator-5554。emulator-* 以外は拒否)、CI_FIX (フィクスチャ置き場。既定 ${RUNNER_TEMP:-/tmp}/usbmem_fix)、
#          CI_LOG_DIR (ログ。既定 <リポジトリ>/ci-logs)。run は ビルド済みの APK (-PusbmemNtfsWrite=true のビルド) が android/app/build/outputs/apk にあること。
# 既存の tools/run_android_*_test.sh をそのまま呼ぶ (USBMEM_SKIP_BUILD=1 でスクリプト内のビルドを省く)。
# スクリプトがフィクスチャ無し・ツールなしで「skip (終了コード 77)」と言った場合も CI では失敗として数える (黙って通さない)。
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
MODE=${1:?usage: ci_android_tests.sh gen|run <group>...}; shift
[ $# -ge 1 ] || { echo "usage: ci_android_tests.sh $MODE <group>..." >&2; exit 2; }
export PATH="$PATH:/sbin:/usr/sbin:${ANDROID_HOME:+$ANDROID_HOME/platform-tools}"
FIX=${CI_FIX:-${RUNNER_TEMP:-/tmp}/usbmem_fix}
LOG=${CI_LOG_DIR:-$ROOT/ci-logs}
mkdir -p "$FIX" "$LOG"

export ANDROID_SERIAL=${ANDROID_SERIAL:-${ADB_SERIAL:-emulator-5554}}
export ADB_SERIAL=$ANDROID_SERIAL
case "$ANDROID_SERIAL" in emulator-*) ;; *) echo "refusing: ANDROID_SERIAL must be an emulator ($ANDROID_SERIAL)" >&2; exit 2;; esac
export USBMEM_SKIP_BUILD=1
export USBMEM_TMP=${USBMEM_TMP:-$FIX/tmp}; mkdir -p "$USBMEM_TMP"   # 書き込み系スクリプトの作業場所 (ローカルは /dev/shm。runner の /dev/shm 上では FUSE の後始末が失敗した)
export ANDROID_SDK_EXEC=/usr/bin/env      # run_android_fat_test.sh の android-sdk-exec の代わり (ビルドは省くので実際には使わない)

# ---- フィクスチャ生成 -------------------------------------------------------------
py() { python3 -I "$@"; }
export USBMEM_FIXTURE_LENIENT=1   # gen_erofs_fixture.py: Ubuntu の古い erofs-utils が展開できないイメージの独立検証を省く (警告を出す)
gen_fs() {
  case $1 in
    fat)         py "$HERE/gen_fat_fixture.py" "$FIX/fat" ;;
    fat_write)   [ -f "$FIX/fat/fat32_c4k.img" ] || py "$HERE/gen_fat_fixture.py" "$FIX/fat" ;;   # fat と同じフィクスチャ (fat の後なら生成済み)
    ext2_write)  py "$HERE/gen_ext2_fixture.py" "$FIX/ext2" ;;
    exfat_write) py "$HERE/gen_exfat_fixture.py" "$FIX/exfat" ;;
    btrfs)       py "$HERE/gen_btrfs_fixture.py" "$FIX/btrfs" ;;
    btrfs_write) true ;;   # イメージは run_android_btrfs_write_test.sh が mkfs.btrfs --rootdir で作る
    xfs_write)   true ;;   # イメージは run_android_xfs_write_test.sh が mkfs.xfs -p で作る
    xfs)         py "$HERE/gen_xfs_fixture.py" "$FIX/xfs" ;;
    squashfs)    py "$HERE/gen_squashfs_fixture.py" "$FIX/squashfs" ;;
    erofs)       py "$HERE/gen_erofs_fixture.py" "$FIX/erofs" ;;
    f2fs)        py "$HERE/gen_f2fs_fixture.py" "$FIX/f2fs" ext ;;
    f2fs_write)  py "$HERE/gen_f2fs_fixture.py" "$FIX/f2fs" ext ;;
    nested)      py "$HERE/gen_nested_fixture.py" "$FIX/nested" ;;
    ext2_read)   py "$HERE/gen_ext2_fixture.py" "$FIX/ext2" ;;
    ext4_read)   py "$HERE/gen_ext4_fixture.py" "$FIX/ext4" ;;
    exfat_read)  py "$HERE/gen_exfat_fixture.py" "$FIX/exfat" ;;
    iso_read)    py "$HERE/gen_iso_fixture.py" "$FIX/iso" ;;
    ntfs_read)   bash "$HERE/gen_ntfs_fixture.sh" "$FIX/ntfs" && py "$HERE/gen_ntfs_state_fixture.py" "$FIX/ntfs" ;;   # ntfs_state_*.img (ダーティ / 未再生ログ / 休止状態。docs/dirty-volumes.md)
    app_misc)    py "$HERE/gen_ext2_fixture.py" "$FIX/ext2" && bash "$HERE/gen_ntfs_fixture.sh" "$FIX/ntfs" ;;
    write) bash "$HERE/gen_ntfs_fixture.sh" "$FIX/ntfs" ;;
    crypt_ui)    py "$HERE/gen_crypt_ui_fixture.py" "$FIX/crypt_ui" ;;
    disk_ui)     py "$HERE/gen_disk_ui_fixture.py" "$FIX/disk_ui" ;;
    kfsd_unit)   true ;;   # ext4 のフィクスチャは kfsd_read_local が作る
    kfsd_read_plugin|kfsd_write) true ;;   # kfsd_read_local のフィクスチャを使う (ext2 / ntfs も含む)
    kfsd_read_local) py "$HERE/gen_ext2_fixture.py" "$FIX/ext2" && py "$HERE/gen_ext4_fixture.py" "$FIX/ext4" && py "$HERE/gen_exfat_fixture.py" "$FIX/exfat" && py "$HERE/gen_iso_fixture.py" "$FIX/iso" \
                   && bash "$HERE/gen_ntfs_fixture.sh" "$FIX/ntfs" && py "$HERE/gen_ntfs_state_fixture.py" "$FIX/ntfs" ;;
    wim_fuse)    py "$HERE/fuseshim/gen_wim_fixture.py" "$FIX/wim" ;;   # wimlib-imagex (wimtools) が要る。lzx / none / lzms の 3 つの .wim と expected.txt
    *) echo "unknown fs $1" >&2; return 2 ;;
  esac
}

# ---- テスト実行 -------------------------------------------------------------------
ADB=$(command -v adb || true)
adbs() { "$ADB" -s "$ANDROID_SERIAL" "$@"; }
APKD="$ROOT/android/app/build/outputs/apk"
RUNNER=org.usbmem.app.test/androidx.test.runner.AndroidJUnitRunner

# F2FS: 全部入りのスクリプト (run_android_f2fs_test.sh) は k_* (QEMU VM が作るカーネル製イメージ) を要求するので、ここでは ext だけを送って 1 テストを実行する
run_f2fs() {
  local d=$FIX/f2fs tmp; tmp=$(mktemp -d)
  adbs install -r -t "$APKD/debug/app-debug.apk" >/dev/null || return 1
  adbs install -r -t "$APKD/androidTest/debug/app-debug-androidTest.apk" >/dev/null || return 1
  gzip -1 -c "$d/ext.img" > "$tmp/ext.img.gz" || return 1
  adbs push "$tmp/ext.img.gz" /data/local/tmp/f2fs_ext.img.gz >/dev/null || return 1
  for e in manifest ref_1 ref_2; do adbs push "$d/ext.$e" "/data/local/tmp/f2fs_ext.$e" >/dev/null || return 1; done
  rm -rf "$tmp"
  local out; out=$(adbs shell am instrument -w -e class org.usbmem.app.F2fsInstrumentedTest#mountsAndVerifiesMkfsSload "$RUNNER")
  echo "$out" | tail -25
  adbs shell 'rm -f /data/local/tmp/f2fs_*' >/dev/null
  echo "$out" | grep -q "^OK (" || return 1
}

# 専用スクリプトの無いテスト: <ディレクトリ> の *.img / *.manifest / *.ref_* を /data/local/tmp/<接頭辞><名前> として送り、計装テストのクラスを実行する。
# am instrument -r の結果から、成功 / 失敗 / スキップ (assumption 失敗 = フィクスチャ未送信など) を数える。スキップがあれば失敗扱い (黙って通さない)
push_dir() {   # push_dir <dir> <prefix>
  local f
  for f in "$1"/*.img "$1"/*.manifest "$1"/*.ref_*; do
    [ -f "$f" ] || continue
    adbs push "$f" "/data/local/tmp/$2$(basename "$f")" >/dev/null || return 1
  done
}
instr_classes() {   # instr_classes <クラス,クラス,...>
  adbs install -r -t "$APKD/debug/app-debug.apk" >/dev/null || return 1
  adbs install -r -t "$APKD/androidTest/debug/app-debug-androidTest.apk" >/dev/null || return 1
  local out; out=$(adbs shell am instrument -r -w -e class "$1" "$RUNNER")
  local pass fail skip
  pass=$(echo "$out" | grep -c '^INSTRUMENTATION_STATUS_CODE: 0')
  fail=$(echo "$out" | grep -cE '^INSTRUMENTATION_STATUS_CODE: -[12]')
  skip=$(echo "$out" | grep -cE '^INSTRUMENTATION_STATUS_CODE: -[34]')
  echo "$out" | grep -E '^INSTRUMENTATION_STATUS: (class|test|stack)=' | paste -sd' ' | fold -w 200 | head -40
  echo "$out" | grep -A12 '^INSTRUMENTATION_STATUS: stack=' | head -60
  echo "passed: $pass failed: $fail skipped: $skip"
  [ "$fail" -eq 0 ] && [ "$skip" -eq 0 ] && [ "$pass" -gt 0 ] && echo "OK ($pass tests)"
  [ "$fail" -eq 0 ] && [ "$skip" -eq 0 ] && [ "$pass" -gt 0 ]
}
run_generic() {   # run_generic <クラス,...> <dir>:<prefix> ...
  local cls=$1; shift
  local dp
  for dp in "$@"; do push_dir "${dp%%:*}" "${dp#*:}" || return 1; done
  instr_classes "$cls"; local r=$?
  adbs shell 'rm -f /data/local/tmp/ext2_* /data/local/tmp/ext4_* /data/local/tmp/exfat_* /data/local/tmp/iso_* /data/local/tmp/ntfs_basic.* /data/local/tmp/ntfs_state_* /data/local/tmp/cui_*' >/dev/null
  return $r
}
APP=org.usbmem.app

# 個人版の FUSE 層 (wimfuse): .wim と期待のツリーを送って WimFuseTest を実行する。終わったあと、子プロセス (libwimfuse.so) が残っていないことも確かめる
run_wim_fuse() {
  local d=$FIX/wim v
  for v in lzx none lzms multi; do adbs push "$d/$v.wim" "/data/local/tmp/wimfuse_$v.wim" >/dev/null || return 1; done
  adbs push "$d/expected.txt" /data/local/tmp/wimfuse_expected.txt >/dev/null || return 1
  adbs push "$d/expected_multi.txt" /data/local/tmp/wimfuse_expected_multi.txt >/dev/null || return 1
  adbs push "$d/parent.img" /data/local/tmp/wimfuse_parent.img >/dev/null || return 1
  adbs logcat -c
  instr_classes $APP.wimfuse.WimFuseTest,$APP.wimfuse.WimNestedTest; local r=$?
  echo "--- 測定値 (WIMFUSE_METRIC)"; adbs logcat -d -s WIMFUSE_METRIC:I | sed 's/^.*WIMFUSE_METRIC *: *//' | head -80
  sleep 3
  local left; left=$(adbs shell 'ps -A' | grep -c libwimfuse || true)
  echo "テスト後に残っている libwimfuse の子プロセス: $left"
  adbs shell 'ps -A' | grep -E 'wimfuse' || true
  [ "$left" = 0 ] || r=1
  # アプリの強制終了: 子を開いたまま計装を終える (アプリと :wimfuse が消える) → 子が残らない
  echo "--- アプリ終了で子が消えるか"
  adbs shell am instrument -r -w -e wimfuse_leave 1 -e class "$APP.wimfuse.WimFuseTest#leaveChildOpen_forAppDeath" "$RUNNER" | grep -E 'INSTRUMENTATION_STATUS_CODE|stack' | head -5
  adbs shell 'ps -A' | grep -E 'wimfuse|usbmem' || true
  sleep 3
  left=$(adbs shell 'ps -A' | grep -c libwimfuse || true)
  echo "アプリ終了後に残っている libwimfuse の子プロセス: $left"
  [ "$left" = 0 ] || r=1
  adbs shell 'rm -f /data/local/tmp/wimfuse_*' >/dev/null
  return $r
}

# kfsd (docs/kfsd.md): 本体を「kfsd 経由」にして (デバッグビルドだけが見る /data/local/tmp/usbmem_kfsd。内容 = local / plugin)、既存の試験を通す。アプリのプロセスが起きるときに読まれる。
kfsd_mode() {   # kfsd_mode local|plugin|off
  local f; f=$(mktemp); echo "$1" > "$f"
  adbs push "$f" /data/local/tmp/usbmem_kfsd >/dev/null && adbs shell chmod 644 /data/local/tmp/usbmem_kfsd; local r=$?
  rm -f "$f"; return $r
}
kfsd_install_plugin() {
  local apk=$ROOT/android/kfsd-plugin/build/outputs/apk/debug/kfsd-plugin-debug.apk
  [ -f "$apk" ] || { echo "plugin APK が無い: $apk"; return 1; }
  adbs install -r "$apk" >/dev/null || return 1
}
kfsd_ps() { adbs shell 'ps -A -o USER,PID,PPID,NAME,ARGS' 2>/dev/null | grep -E 'kfsd' | grep -v grep || true; }
run_kfsd_unit() {
  kfsd_mode off || return 1
  kfsd_install_plugin || return 1
  push_dir "$FIX/ext4" ext4_ || return 1
  adbs logcat -c
  instr_classes $APP.kfsd.KfsdInstrumentedTest; local r=$?
  echo "--- kfsd の測定値 (KFSD_METRIC)"; adbs logcat -d -s KFSD_METRIC:I | sed 's/^.*KFSD_METRIC *: *//' | head -40
  echo "--- kfsd の出力 (kfsd-child)"; adbs logcat -d -s kfsd-child:I | tail -20
  sleep 3
  local left; left=$(adbs shell 'ps -A' | grep -c 'libkfsd' || true)
  echo "テスト後に残っている libkfsd の子プロセス: $left"; kfsd_ps
  [ "$left" = 0 ] || r=1
  adbs shell 'rm -f /data/local/tmp/ext4_*' >/dev/null
  return $r
}
# 既存の読み取りテストを kfsd 経由で通す (source = local: libkfsd.so / plugin: 別 APK)
run_kfsd_read() {
  local src=$1 r=0
  kfsd_install_plugin || return 1
  kfsd_mode "$src" || return 1
  run_generic $APP.Ext2InstrumentedTest,$APP.Ext4InstrumentedTest,$APP.ExfatInstrumentedTest,$APP.IsofsInstrumentedTest,$APP.NtfsInstrumentedTest,$APP.VolumeStateInstrumentedTest,$APP.DirtyConfirmUiTest \
    "$FIX/ext2:ext2_" "$FIX/ext4:ext4_" "$FIX/exfat:exfat_" "$FIX/iso:iso_" "$FIX/ntfs:" || r=1
  kfsd_mode off
  return $r
}
run_kfsd_write() {
  local r=0
  mkdir -p "$FIX/misc" && cp "$FIX/ext2/b1k.img" "$FIX/misc/ext2_wr.img" || return 1
  kfsd_mode local || return 1
  run_generic $APP.WriteSelfTestInstrumented "$FIX/ntfs:" "$FIX/misc:" || r=1
  kfsd_mode off
  return $r
}

# ディスクツールの UI 試験 (docs/disk-ui.md): フィクスチャを送って DiskUiTest を実行し、試験が書いたイメージ (アプリの cache/dui_out) を adb で取り出して、
# ホストの道具 (e2fsck / fsck.fat / fsck.exfat / sgdisk / cmp) で外から検証する。run-as が使えるのは debuggable なビルド (CI の debug APK)
disk_ui_verify() {   # disk_ui_verify <取り出したイメージのディレクトリ>
  local o=$1 rc=0 f
  chk() { local what=$1; shift; if "$@" >"$o/chk.out" 2>&1; then echo "ok:   $what"; else echo "FAIL: $what"; sed 's/^/      /' "$o/chk.out" | head -20; rc=1; fi; }
  for f in fmt_ext2 usb_fmt; do
    [ -f "$o/$f.img" ] || { echo "FAIL: $f.img was not produced"; rc=1; continue; }
    chk "e2fsck -fn $f.img" e2fsck -fn "$o/$f.img"
  done
  [ -f "$o/fmt_ext2.img" ] && chk "label DUIFMT (fmt_ext2.img)" bash -c "dumpe2fs -h '$o/fmt_ext2.img' 2>/dev/null | grep -q 'volume name: *DUIFMT'"
  [ -f "$o/fmt_fat32.img" ] && chk "fsck.fat -n fmt_fat32.img" fsck.fat -n "$o/fmt_fat32.img" || { [ -f "$o/fmt_fat32.img" ] || { echo "FAIL: fmt_fat32.img was not produced"; rc=1; }; }
  [ -f "$o/fmt_exfat.img" ] && chk "fsck.exfat -n fmt_exfat.img" fsck.exfat -n "$o/fmt_exfat.img" || { [ -f "$o/fmt_exfat.img" ] || { echo "FAIL: fmt_exfat.img was not produced"; rc=1; }; }
  # パーティション編集: 削除のあと GPT が 1 つ・健全、残った ext2 も健全。取り消し / 読み取り専用のイメージは 2 つのまま
  if [ -f "$o/pt_remove.img" ]; then
    chk "sgdisk -v pt_remove.img" sgdisk -v "$o/pt_remove.img"
    chk "pt_remove.img has exactly 1 partition" bash -c "[ \"\$(sgdisk -p '$o/pt_remove.img' | grep -cE '^ +[0-9]+ ')\" = 1 ]"
    chk "e2fsck -fn pt_remove.img p1" e2fsck -fn "$o/pt_remove.img?offset=1048576"
  else echo "FAIL: pt_remove.img was not produced"; rc=1; fi
  for f in pt_cancel pt_ro; do
    [ -f "$o/$f.img" ] || { echo "FAIL: $f.img was not produced"; rc=1; continue; }
    chk "$f.img still has 2 partitions" bash -c "[ \"\$(sgdisk -p '$o/$f.img' | grep -cE '^ +[0-9]+ ')\" = 2 ]"
  done
  # 復元: 書き戻した先が健全で、ラベルが元 (DUICLEAN) になっている
  if [ -f "$o/res_target.img" ]; then
    chk "e2fsck -fn res_target.img" e2fsck -fn "$o/res_target.img"
    chk "res_target.img label restored to DUICLEAN" bash -c "dumpe2fs -h '$o/res_target.img' 2>/dev/null | grep -q 'volume name: *DUICLEAN'"
  else echo "FAIL: res_target.img was not produced"; rc=1; fi
  # 救出: 出力が元と一致
  if [ -f "$o/rescue_out.img" ] && [ -f "$o/rescue_src.img" ]; then chk "cmp rescue_src.img rescue_out.img" cmp "$o/rescue_src.img" "$o/rescue_out.img"
  else echo "FAIL: rescue images were not produced"; rc=1; fi
  # 検査・バックアップ・取り消しは、元のイメージを変えない (フィクスチャとの一致)
  for f in chk_clean:ext2_clean chk_bad:ext2_bad bak_src:ext2_clean res_src:ext2_clean fmt_ro:ext2_clean usb_ro:ext2_clean; do
    [ -f "$o/${f%%:*}.img" ] || { echo "FAIL: ${f%%:*}.img was not produced"; rc=1; continue; }
    chk "${f%%:*}.img unchanged" cmp "$o/${f%%:*}.img" "$FIX/disk_ui/${f##*:}.img"
  done
  rm -f "$o/chk.out"
  return $rc
}
run_disk_ui() {
  push_dir "$FIX/disk_ui" dui_ || return 1
  instr_classes $APP.DiskUiTest; local r=$?
  local o="$LOG/disk_ui_out"; rm -rf "$o"; mkdir -p "$o"
  local f
  for f in $(adbs shell run-as org.usbmem.app ls cache/dui_out 2>/dev/null | tr -d '\r'); do
    adbs exec-out run-as org.usbmem.app cat "cache/dui_out/$f" > "$o/$f" 2>/dev/null
  done
  echo "pulled: $(ls "$o" | wc -l) files"
  adbs shell run-as org.usbmem.app rm -rf cache/dui_out >/dev/null 2>&1
  adbs shell 'rm -f /data/local/tmp/dui_*' >/dev/null
  disk_ui_verify "$o"; local v=$?
  rm -rf "$o"
  [ $r -eq 0 ] && [ $v -eq 0 ]
}

run_fs() {
  case $1 in
    kfsd_unit)   run_kfsd_unit ;;
    kfsd_read_local)  run_kfsd_read local ;;
    kfsd_read_plugin) run_kfsd_read plugin ;;
    kfsd_write)  run_kfsd_write ;;
    fat) bash "$HERE/run_android_fat_test.sh" "$FIX/fat" ;;
    fat_write) bash "$HERE/run_android_fat_write_test.sh" "$FIX/fat" ;;
    ext2_write) bash "$HERE/run_android_ext2_write_test.sh" "$FIX/ext2" ;;
    exfat_write) bash "$HERE/run_android_exfat_write_test.sh" "$FIX/exfat" ;;
    btrfs)       bash "$HERE/run_android_btrfs_test.sh" "$FIX/btrfs" "$ADB" ;;
    btrfs_write) bash "$HERE/run_android_btrfs_write_test.sh" ;;
    xfs_write)   bash "$HERE/run_android_xfs_write_test.sh" ;;
    xfs)         bash "$HERE/run_android_xfs_test.sh" "$FIX/xfs" "$ADB" ;;
    squashfs)    bash "$HERE/run_android_squashfs_test.sh" "$FIX/squashfs" "$ADB" ;;
    erofs)       bash "$HERE/run_android_erofs_test.sh" "$FIX/erofs" "$ADB" ;;
    f2fs)        run_f2fs ;;
    f2fs_write)  bash "$HERE/run_android_f2fs_write_test.sh" "$FIX/f2fs" ;;
    nested) bash "$HERE/run_android_nested_test.sh" "$FIX/nested" ;;
    write) bash "$HERE/run_android_write_test.sh" "$FIX/ntfs" ;;
    ext2_read)   run_generic $APP.Ext2InstrumentedTest "$FIX/ext2:ext2_" ;;
    ext4_read)   run_generic $APP.Ext4InstrumentedTest "$FIX/ext4:ext4_" ;;
    exfat_read)  run_generic $APP.ExfatInstrumentedTest "$FIX/exfat:exfat_" ;;
    iso_read)    run_generic $APP.IsofsInstrumentedTest "$FIX/iso:iso_" ;;
    ntfs_read)   run_generic $APP.NtfsInstrumentedTest,$APP.VolumeStateInstrumentedTest,$APP.DirtyConfirmUiTest "$FIX/ntfs:" "$FIX/ext4:ext4_" ;;
    # LinkProviderTest は ext2_wr.img (= b1k.img のコピー) と ntfs_basic を使う
    app_misc)    mkdir -p "$FIX/misc" && cp "$FIX/ext2/b1k.img" "$FIX/misc/ext2_wr.img" \
                   && run_generic $APP.DocumentsProviderTest,$APP.LinkProviderTest,$APP.WriteSelfTestInstrumented,$APP.RawImageInstrumentedTest,$APP.TransferTuningTest,$APP.VaultRealPathTest "$FIX/ntfs:" "$FIX/misc:" ;;
    crypt_ui)    run_generic $APP.CryptoUiTest "$FIX/crypt_ui:cui_" ;;
    disk_ui)     run_disk_ui ;;
    wim_fuse)    run_wim_fuse ;;
    *) echo "unknown fs $1" >&2; return 2 ;;
  esac
}

group_fs() {
  case $1 in
    ext-fat)       echo "fat fat_write ext2_write exfat_write" ;;
    btrfs-xfs)     echo "btrfs btrfs_write xfs xfs_write" ;;
    squash-erofs)  echo "squashfs erofs f2fs f2fs_write" ;;
    nested-ntfs)   echo "nested write" ;;
    crypt-ui)      echo "crypt_ui" ;;
    disk-ui)       echo "disk_ui" ;;
    ro-misc)       echo "ext2_read ext4_read exfat_read iso_read ntfs_read app_misc" ;;
    wim-fuse)      echo "wim_fuse" ;;
    kfsd)          echo "kfsd_unit kfsd_read_local kfsd_read_plugin kfsd_write" ;;
    *) echo "unknown group $1" >&2; return 2 ;;
  esac
}

FSS=()
for g in "$@"; do
  l=$(group_fs "$g") || exit 2
  # shellcheck disable=SC2206
  FSS+=($l)
done

if [ "$MODE" = gen ]; then
  rc=0
  for fs in "${FSS[@]}"; do
    echo "::group::gen $fs"
    gen_fs "$fs" > "$LOG/gen_$fs.log" 2>&1; r=$?
    tail -15 "$LOG/gen_$fs.log"
    echo "::endgroup::"
    [ $r -eq 0 ] || { echo "FAIL: fixture generation for $fs (rc=$r)"; rc=1; }
  done
  du -sh "$FIX"/* 2>/dev/null
  exit $rc
fi
[ "$MODE" = run ] || { echo "unknown mode $MODE" >&2; exit 2; }

[ -n "$ADB" ] || { echo "adb not found" >&2; exit 2; }
adbs wait-for-device
echo "device: $(adbs shell getprop ro.product.model | tr -d '\r') API $(adbs shell getprop ro.build.version.sdk | tr -d '\r') ($ANDROID_SERIAL)"
adbs shell df -h /data | tee "$LOG/df_start.txt"
echo "heap: growthlimit=$(adbs shell getprop dalvik.vm.heapgrowthlimit | tr -d '\r') heapsize=$(adbs shell getprop dalvik.vm.heapsize | tr -d '\r')"
adbs logcat -c
adbs logcat -v threadtime > "$LOG/logcat.txt" 2>&1 &
LOGCAT_PID=$!
cleanup() { kill "$LOGCAT_PID" 2>/dev/null; true; }
trap cleanup EXIT

rc=0; SUMMARY=()
for fs in "${FSS[@]}"; do
  echo "::group::android $fs"
  t0=$(date +%s)
  run_fs "$fs" > "$LOG/run_$fs.log" 2>&1; r=$?
  cat "$LOG/run_$fs.log"
  echo "::endgroup::"
  t1=$(date +%s)
  n=$(grep -oE 'OK \(([0-9]+) tests?\)' "$LOG/run_$fs.log" | grep -oE '[0-9]+' | awk '{s+=$1} END{print s+0}')
  n=${n:-0}
  if [ $r -eq 0 ] && [ "$n" -gt 0 ]; then st=PASS
  elif [ $r -eq 0 ]; then st="FAIL (テスト 0 件)"; rc=1
  elif [ $r -eq 77 ]; then st="FAIL (skip 77: フィクスチャ / ツールなし)"; rc=1
  else st="FAIL (rc=$r)"; rc=1; fi
  [ "$st" = PASS ] || echo "::error::android $fs: $st"
  SUMMARY+=("| $fs | $st | $n | $((t1 - t0)) s |")
  adbs shell df -h /data | tail -1
done

{
  echo "### Android 計装テスト ($*)"
  echo
  echo "| FS | 結果 | テスト件数 (OK の合計) | 時間 |"
  echo "|---|---|---|---|"
  printf '%s\n' "${SUMMARY[@]}"
} | tee "$LOG/summary.md" > "${GITHUB_STEP_SUMMARY:-/dev/null}"
cleanup
exit $rc
