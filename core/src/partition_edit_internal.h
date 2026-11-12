// SPDX-License-Identifier: Apache-2.0
// partition_edit*.cpp の内部共有部品 (公開しない)。
#pragma once
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "usbmem/block_device.h"
#include "usbmem/crc32.h"
#include "usbmem/partition_edit.h"

namespace usbmem {
namespace pe {

inline uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t le64(const uint8_t* p) { return le32(p) | (static_cast<uint64_t>(le32(p + 4)) << 32); }
inline void put16(uint8_t* p, uint16_t v) { p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8); }
inline void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>(v >> (8 * i)); }
inline void put64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = static_cast<uint8_t>(v >> (8 * i)); }
inline bool all_zero(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (p[i]) return false;
  return true;
}

constexpr uint32_t kMaxGptEntries = 8192;
constexpr size_t kMaxGptEntriesBytes = 4u << 20;

// GPT の配置。ヘッダ 2 本とエントリ配列 2 本の位置。
struct GptGeo {
  uint32_t ss = 512;
  uint64_t disk_sectors = 0;
  uint64_t first_usable = 0, last_usable = 0;
  uint64_t alt_lba = 0;        // バックアップヘッダの位置
  uint64_t p_ent_lba = 2;      // プライマリのエントリ配列
  uint64_t b_ent_lba = 0;      // バックアップのエントリ配列
  uint32_t num_entries = 128, entry_size = 128;
  uint64_t arr_sectors() const { return (static_cast<uint64_t>(num_entries) * entry_size + ss - 1) / ss; }
  // 標準配置 (プライマリ: LBA1 + 配列 LBA2~, バックアップ: 末尾)
  static bool standard(uint32_t ss, uint64_t disk_sectors, uint32_t num, uint32_t esz, GptGeo* g);
};

std::vector<uint8_t> build_gpt_header(const GptGeo& g, bool primary, const std::array<uint8_t, 16>& disk_guid,
                                      uint32_t entries_crc);

// UTF-8 → UTF-16LE (最大 units 単位に収まらなければ false)。不正な UTF-8 も false。
bool utf8_to_utf16le(const std::string& s, size_t max_units, std::vector<uint8_t>* out);

// 元デバイスの上に、指定セクタだけ差し替えた内容を見せる読み取り専用デバイス (計画の適用結果をパースするため)。
class OverlayDevice final : public BlockDevice {
 public:
  OverlayDevice(BlockDevice& base, uint32_t ss, std::map<uint64_t, std::vector<uint8_t>> sectors)
      : base_(base), ss_(ss), sectors_(std::move(sectors)) {}
  Status read_at(uint64_t off, void* buf, size_t len, const IoOptions& = {}) override;
  Status write_at(uint64_t, const void*, size_t, const IoOptions& = {}) override { return Status::Make(Errc::ReadOnly); }
  Status flush() override { return Status::Ok(); }
  Status close() override { return Status::Ok(); }
  uint64_t size() const override { return base_.size(); }
  uint32_t logical_block_size() const override { return ss_; }
  bool is_read_only() const override { return true; }
  std::string describe() const override { return "overlay"; }

 private:
  BlockDevice& base_;
  uint32_t ss_;
  std::map<uint64_t, std::vector<uint8_t>> sectors_;
};

// セクタの役割 (バックアップ記録のタグ・復元順序・書き込み許可の判定に使う)
enum SectorKind : uint8_t {
  kOther = 0,
  kMbr = 1,
  kGptPrimaryHeader = 2,
  kGptPrimaryEntries = 3,
  kGptBackupHeader = 4,
  kGptBackupEntries = 5,
  kEbr = 6,
};
SectorKind kind_of_what(const std::string& what);

struct TableSector {
  uint64_t lba;
  SectorKind kind;
  std::vector<uint8_t> data;
};
// 現在のディスク上にあるテーブル関連セクタを集める (セクタ 0、GPT の有効なヘッダと配列、EBR チェーン)。
Status collect_table_sectors(BlockDevice& dev, uint32_t ss, const GptInspection& gi, std::vector<TableSector>* out);

// バックアップファイルの組み立て / 解析
struct BackupFile {
  uint32_t version = 1, sector_size = 0, table_kind = 0;
  uint64_t disk_bytes = 0, timestamp = 0;
  std::array<uint8_t, 16> disk_id{};
  std::vector<TableSector> sectors;
};
std::vector<uint8_t> serialize_backup(const BackupFile& b);
Status parse_backup(const std::vector<uint8_t>& file, BackupFile* out);

std::string sfdisk_dump(const PartitionTable& t, const GptInspection& gi, const std::string& device_name);

}  // namespace pe
}  // namespace usbmem
