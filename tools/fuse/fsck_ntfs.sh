#!/bin/sh
# NTFS の fsck 相当 (xfstests のアンマウント後の検査。docs/xfstests.md)。NTFS には e2fsck のような独立した検査ツールが無い (ntfsfix は修復ツールで、-n でも見る範囲が狭い) ので、
# 3 つを組み合わせる。どれかが失敗したら 1 を返す (run_xfstests.py が FSCK-FAIL を記録する)。すべて読み取り専用 (修復しない)。
#   使い方: fsck_ntfs.sh IMAGE      環境変数: USBMEM_FSCHECK (自前の検査 CLI。既定 /usr/local/bin/usbmem_fscheck)
#   1. ntfsfix -n        ブートセクタ・$MFT / $MFTMirr・ジャーナル・ダーティの簡易検査 (独立実装 = ntfs-3g)。
#   2. ntfs-3g.probe     ntfs-3g が読み書きでマウントできる状態か (--readonly = 書き込み可の試験だけで、実際には書かない)。
#   3. usbmem_fscheck    自前の整合性検査 (core/src/fscheck_ntfs.cpp。MFT 全レコード・$Bitmap とクラスタ使用・親ディレクトリ・ダーティ・$LogFile)。
#                        終了コード 0 = clean、1 = warnings (報告するが失敗にしない)、2 = errors、それ以外 (3 unsupported / 4 incomplete / 10 I/O 失敗) は失敗。
img=$1
[ -n "$img" ] || { echo "usage: $0 IMAGE" >&2; exit 2; }
bin=${USBMEM_FSCHECK:-/usr/local/bin/usbmem_fscheck}
rc=0
echo "== ntfsfix -n $img"
ntfsfix -n "$img" || { echo "NTFS-FSCK-FAIL ntfsfix -n (status $?)"; rc=1; }
echo "== ntfs-3g.probe --readonly $img"
ntfs-3g.probe --readonly "$img" || { echo "NTFS-FSCK-FAIL ntfs-3g.probe (status $?)"; rc=1; }
echo "== usbmem_fscheck check $img --fs ntfs"
"$bin" check "$img" --fs ntfs
st=$?
case $st in
  0) ;;
  1) echo "NTFS-FSCK-WARN usbmem_fscheck: warnings (失敗にはしない)" ;;
  *) echo "NTFS-FSCK-FAIL usbmem_fscheck (status $st)"; rc=1 ;;
esac
exit $rc
