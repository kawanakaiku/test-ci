// SPDX-License-Identifier: Apache-2.0
// エディション (docs/editions.md) ごとに、このネイティブライブラリへ入っているファイルシステムの一覧。
//  - 個人版 (既定): すべてのファイルシステムをリンクする。USBMEM_FS_* をすべて 1 にする。
//  - 公開版 (CMake の -DUSBMEM_EDITION=public): GPL 系 (カーネル由来の LinuxCompat・FS ドライバ) を含まない。CMakeLists.txt が、リンクした FS の USBMEM_FS_* だけを定義する。
#pragma once
#include <string>

#ifdef USBMEM_EDITION_PUBLIC
#define USBMEM_EDITION_NAME "public"
#else
#define USBMEM_EDITION_NAME "personal"
#define USBMEM_FS_NTFS 1
#define USBMEM_FS_EXFAT 1
#define USBMEM_FS_EXT2 1
#define USBMEM_FS_EXT4 1
#define USBMEM_FS_FAT 1
#define USBMEM_FS_ISOFS 1
#define USBMEM_FS_UDF 1
#define USBMEM_FS_SQUASHFS 1
#define USBMEM_FS_EROFS 1
#define USBMEM_FS_BTRFS 1
#define USBMEM_FS_F2FS 1
#define USBMEM_FS_XFS 1
#endif

// {"edition":"personal|public","fs":["ntfs",...]}。fs の名前は、パーティション一覧 (fs_probe) の fs 文字列と同じ (Kotlin の Partition.fs)。
inline std::string usbmem_edition_json() {
  std::string fs;
  auto add = [&fs](const char* n) { if (!fs.empty()) fs += ','; fs += '"'; fs += n; fs += '"'; };
#ifdef USBMEM_FS_NTFS
  add("ntfs");
#endif
#ifdef USBMEM_FS_EXFAT
  add("exfat");
#endif
#ifdef USBMEM_FS_EXT2
  add("ext2");
#endif
#ifdef USBMEM_FS_EXT4
  add("ext3"); add("ext4");
#endif
#ifdef USBMEM_FS_FAT
  add("fat12"); add("fat16"); add("fat32");
#endif
#ifdef USBMEM_FS_ISOFS
  add("iso9660");
#endif
#ifdef USBMEM_FS_UDF
  add("udf");
#endif
#ifdef USBMEM_FS_SQUASHFS
  add("squashfs");
#endif
#ifdef USBMEM_FS_EROFS
  add("erofs");
#endif
#ifdef USBMEM_FS_BTRFS
  add("btrfs");
#endif
#ifdef USBMEM_FS_F2FS
  add("f2fs");
#endif
#ifdef USBMEM_FS_XFS
  add("xfs");
#endif
  return std::string("{\"edition\":\"") + USBMEM_EDITION_NAME + "\",\"fs\":[" + fs + "]}";
}
