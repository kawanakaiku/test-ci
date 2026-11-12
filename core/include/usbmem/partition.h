// SPDX-License-Identifier: Apache-2.0
// MBR / EBR / GPT の解析。ディスク上の構造は信用せず、全フィールドを検証する。
#pragma once
#include <array>
#include <string>
#include <vector>

#include "usbmem/block_device.h"
#include "usbmem/fs_probe.h"

namespace usbmem {

enum class TableKind : uint8_t { None = 0, Mbr, Gpt };
enum class Severity : uint8_t { Info = 0, Warning, Error };

struct Diagnostic {
  Severity severity;
  std::string code;     // 安定した機械可読 ID (例: "gpt-header-crc")
  std::string message;  // 人間向け(英語)。UI 側で code から翻訳可能
};

struct Partition {
  uint32_t index = 0;          // 表示番号。MBR: 主 1-4 / 論理 5-。GPT: エントリ位置+1
  uint64_t start_lba = 0;      // table.sector_size 単位
  uint64_t sector_count = 0;
  uint64_t start_bytes = 0;
  uint64_t size_bytes = 0;
  bool logical = false;        // EBR 内の論理パーティション
  bool extended_container = false;  // MBR の拡張パーティション枠(マウント対象外)
  bool protective = false;     // GPT 保護 MBR (0xEE) のエントリ
  bool bootable = false;       // MBR active flag
  uint8_t mbr_type = 0;
  std::array<uint8_t, 16> type_guid{};    // GPT: ディスク上の生バイト
  std::array<uint8_t, 16> unique_guid{};
  uint64_t gpt_attributes = 0;
  std::string name;            // GPT 名 (UTF-8)
  std::string type_name;       // 既知タイプの名前。不明なら空
  FsType fs = FsType::Unknown; // 中身の検出結果(任意)
  // ---- 以下は追記 (パーティション編集 / ラベル対応)
  std::string part_uuid;       // blkid の PARTUUID 形式。GPT: 一意 GUID (小文字)。MBR: "<ディスクシグネチャ 8 桁>-<番号 2 桁>"
  std::string fs_label;        // ファイルシステムのラベル (読めたときだけ。ParseOptions::read_labels)
  std::string fs_uuid;         // ファイルシステムの UUID / ボリュームシリアル (形式は fs_label.h)
  std::string fs_serial;       // FAT / exFAT / NTFS のボリュームシリアル
  std::string fs_guid;         // exFAT のボリューム GUID (あれば)
  std::string fs_detected;     // fs が Unknown のとき、ラベル読み取りが魔法値で判別した名前 ("erofs" / "squashfs" / "luks")
};

struct PartitionTable {
  TableKind kind = TableKind::None;
  uint32_t sector_size = 512;
  uint64_t disk_bytes = 0;
  uint64_t disk_sectors = 0;
  std::string disk_guid;              // GPT のみ
  uint32_t mbr_disk_signature = 0;    // MBR のみ
  bool used_backup_gpt = false;       // プライマリ破損でバックアップから復元した
  bool whole_device_volume = false;   // パーティション無しで全体が 1 ボリューム
  std::string mbr_disk_id;            // MBR のみ: ディスクシグネチャ (8 桁の 16 進、PARTUUID の前半)
  FsType whole_device_fs = FsType::Unknown;
  std::vector<Partition> partitions;
  std::vector<Diagnostic> diagnostics;

  bool has_errors() const;
};

struct ParseOptions {
  uint32_t sector_size = 0;  // 0 = 自動 (dev.logical_block_size(), 512, 4096 を試す)
  bool probe_filesystems = true;
  bool read_labels = true;   // probe_filesystems が true のとき、FS ラベル / UUID も読む (読めなくても失敗にしない)
};

// I/O エラー以外(不正なテーブル)は Status::Ok を返し diagnostics に記録する。
Status read_partition_table(BlockDevice& dev, PartitionTable* out, const ParseOptions& opt = {});

std::string guid_to_string(const std::array<uint8_t, 16>& raw);  // 混合エンディアン→標準表記
const char* mbr_type_name(uint8_t t);
const char* gpt_type_name(const std::array<uint8_t, 16>& raw);

// JSON (UTF-8)。JNI/Kotlin 側はこれをパースする。
std::string partition_table_to_json(const PartitionTable& t);

}  // namespace usbmem
