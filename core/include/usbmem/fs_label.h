// SPDX-License-Identifier: Apache-2.0
// ファイルシステムのラベル / UUID / シリアルの軽量な読み取り (ドライバのマウントはしない・読み取り専用)。
// ディスク上の構造は信用せず、全オフセットを境界検査する。壊れた入力では空の結果を返す (I/O エラーだけ Status で返す)。
#pragma once
#include <string>

#include "usbmem/block_device.h"
#include "usbmem/fs_probe.h"

namespace usbmem {

struct FsLabelInfo {
  std::string label;     // UTF-8 (空 = ラベル無し / 読めない)
  std::string uuid;      // 形式は FS による (下記)。空 = 無い
  std::string serial;    // FAT / exFAT: "XXXX-XXXX"、NTFS: 16 桁の 16 進 (大文字)。それ以外は空
  std::string guid;      // exFAT のボリューム GUID (ある場合のみ。小文字の 8-4-4-4-12)
  std::string detected;  // 呼び出し側の fs が Unknown のとき、本関数が判別した名前 ("erofs" / "squashfs")
};

// 形式:
//  ext2/3/4・btrfs・xfs・erofs・luks: 小文字の 8-4-4-4-12 (バイト順そのまま。blkid と同じ)
//  FAT12/16/32・exFAT: uuid = serial = "XXXX-XXXX" (ボリュームシリアル。blkid と同じ)。exFAT のボリューム GUID は guid に出す
//  NTFS: uuid = serial = ボリュームシリアル 16 桁。ラベルは $Volume の VOLUME_NAME (読めたときだけ)
//  ISO 9660: label = ボリューム ID、uuid = 作成日時 "YYYY-MM-DD-HH-MM-SS-CC"
//  UDF: label = Logical Volume Identifier (無ければ Primary Volume の Volume Identifier)
// dev の [offset, offset+length) を対象とする。fs が Unknown でも erofs / squashfs / luks は魔法値で判別する。
Status read_fs_label(BlockDevice& dev, uint64_t offset, uint64_t length, FsType fs, FsLabelInfo* out);

}  // namespace usbmem
