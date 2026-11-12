// SPDX-License-Identifier: Apache-2.0
// ファイルシステム/コンテナの「検出」のみ。検出できること = マウント対応ではない。
#pragma once
#include "usbmem/block_device.h"

namespace usbmem {

enum class FsType : uint8_t {
  Unknown = 0, Ntfs, Exfat, Fat12, Fat16, Fat32, Ext2, Ext3, Ext4,
  Iso9660, Udf, BitLocker, Luks, Btrfs, Xfs, F2fs,
  Squashfs, Erofs,
};

const char* fs_type_name(FsType t);

// dev の [offset, offset+length) を対象に検出する。読めない/範囲不足は Unknown。
// 読み取りエラー(I/O)は *io_status に返す(nullable)。
FsType probe_filesystem(BlockDevice& dev, uint64_t offset, uint64_t length, Status* io_status = nullptr);

// セクタ 0 が「ボリュームブートレコード」(パーティションを持たない FAT/NTFS/exFAT/BitLocker)
// かを判定する。MBR との誤認防止のため BPB を厳密に検証する。
FsType probe_boot_sector(const uint8_t* sector, size_t sector_size, uint64_t total_bytes);

}  // namespace usbmem
