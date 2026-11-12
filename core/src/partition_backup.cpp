// SPDX-License-Identifier: Apache-2.0
// パーティションテーブルのバックアップファイル形式と sfdisk --dump 互換のテキスト出力。
//
// ファイル形式 (リトルエンディアン):
//    0  8  マジック "USBMTBK1"
//    8  4  バージョン (= 1)
//   12  4  ヘッダ長 (= 64)
//   16  4  セクタサイズ
//   20  4  テーブル種別 (0 なし / 1 MBR / 2 GPT)
//   24  8  ディスクのバイト数
//   32  8  作成時刻 (UNIX 秒、0 可)
//   40 16  ディスク ID (GPT: ディスク GUID の生バイト / MBR: シグネチャ 4 バイト + 0 埋め)
//   56  4  セクタ記録の数
//   60  4  予約 (0)
//   64 ..  記録 × N: { u64 lba, u8 kind, u8 pad[3], u32 len(= セクタサイズ), u8 data[len] }  (lba の昇順・重複なし)
//   末尾 4  CRC-32 (IEEE) — 先頭から CRC フィールドの直前まで
//   末尾 32 SHA-256 — 先頭から SHA-256 フィールドの直前まで (CRC フィールドを含む)
#include <algorithm>
#include <cinttypes>
#include <cstdio>

#include "partition_edit_internal.h"
#include "usbmem/sha256.h"

namespace usbmem {
using namespace pe;

namespace {
constexpr char kMagic[8] = {'U', 'S', 'B', 'M', 'T', 'B', 'K', '1'};
constexpr size_t kHeader = 64, kTrailer = 4 + 32;
constexpr uint32_t kMaxRecords = 1u << 16;
constexpr size_t kMaxFile = 256u << 20;
}  // namespace

namespace pe {

std::vector<uint8_t> serialize_backup(const BackupFile& b) {
  std::vector<uint8_t> f(kHeader, 0);
  std::memcpy(f.data(), kMagic, 8);
  put32(&f[8], 1);
  put32(&f[12], kHeader);
  put32(&f[16], b.sector_size);
  put32(&f[20], b.table_kind);
  put64(&f[24], b.disk_bytes);
  put64(&f[32], b.timestamp);
  std::memcpy(&f[40], b.disk_id.data(), 16);
  put32(&f[56], static_cast<uint32_t>(b.sectors.size()));
  for (const auto& s : b.sectors) {
    uint8_t rec[16] = {};
    put64(rec, s.lba);
    rec[8] = s.kind;
    put32(rec + 12, static_cast<uint32_t>(s.data.size()));
    f.insert(f.end(), rec, rec + 16);
    f.insert(f.end(), s.data.begin(), s.data.end());
  }
  uint8_t crc[4];
  put32(crc, crc32(f.data(), f.size()));
  f.insert(f.end(), crc, crc + 4);
  auto h = Sha256::hash(f.data(), f.size());
  f.insert(f.end(), h.begin(), h.end());
  return f;
}

Status parse_backup(const std::vector<uint8_t>& f, BackupFile* out) {
  const Status bad = Status::Make(Errc::Corrupt);
  if (f.size() < kHeader + kTrailer || f.size() > kMaxFile) return bad;
  if (std::memcmp(f.data(), kMagic, 8) != 0) return bad;
  size_t body = f.size() - kTrailer;
  if (le32(&f[body]) != crc32(f.data(), body)) return bad;
  auto h = Sha256::hash(f.data(), f.size() - 32);
  if (std::memcmp(h.data(), &f[f.size() - 32], 32) != 0) return bad;
  BackupFile b;
  b.version = le32(&f[8]);
  if (b.version != 1) return Status::Make(Errc::Unsupported);
  if (le32(&f[12]) != kHeader) return bad;
  b.sector_size = le32(&f[16]);
  if (b.sector_size < 512 || b.sector_size > 65536 || (b.sector_size & (b.sector_size - 1))) return bad;
  b.table_kind = le32(&f[20]);
  if (b.table_kind > 2) return bad;
  b.disk_bytes = le64(&f[24]);
  b.timestamp = le64(&f[32]);
  std::memcpy(b.disk_id.data(), &f[40], 16);
  uint32_t n = le32(&f[56]);
  if (n > kMaxRecords) return bad;
  size_t pos = kHeader;
  uint64_t prev = 0;
  for (uint32_t i = 0; i < n; i++) {
    if (body - pos < 16) return bad;
    TableSector s;
    s.lba = le64(&f[pos]);
    uint8_t k = f[pos + 8];
    uint32_t len = le32(&f[pos + 12]);
    if (k > kEbr || len != b.sector_size || body - pos - 16 < len) return bad;
    if (i && s.lba <= prev) return bad;
    if (s.lba > b.disk_bytes / b.sector_size) return bad;
    s.kind = static_cast<SectorKind>(k);
    s.data.assign(f.begin() + pos + 16, f.begin() + pos + 16 + len);
    prev = s.lba;
    pos += 16 + len;
    b.sectors.push_back(std::move(s));
  }
  if (pos != body) return bad;
  *out = std::move(b);
  return Status::Ok();
}

// ------------------------------------------------------------------ sfdisk --dump 互換

namespace {
std::string upper_guid(const std::array<uint8_t, 16>& g) { return guid_to_string(g); }  // guid_to_string は大文字

std::string escape_name(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    o += c;
  }
  return o;
}

std::string attrs_text(uint64_t a) {
  std::string o;
  auto add = [&](const std::string& t) { if (!o.empty()) o += ' '; o += t; };
  if (a & 1) add("RequiredPartition");
  if (a & 2) add("NoBlockIOProtocol");
  if (a & 4) add("LegacyBIOSBootable");
  std::string g;
  for (int b = 48; b < 64; b++)
    if (a >> b & 1) { if (!g.empty()) g += ','; g += std::to_string(b); }
  if (!g.empty()) add("GUID:" + g);
  return o;
}
}  // namespace

std::string sfdisk_dump(const PartitionTable& t, const GptInspection& gi, const std::string& dev) {
  std::string o;
  char b[512];
  bool gpt = t.kind == TableKind::Gpt;
  if (t.kind == TableKind::None) return "";
  o += gpt ? "label: gpt\n" : "label: dos\n";
  if (gpt) o += "label-id: " + t.disk_guid + "\n";
  else { std::snprintf(b, sizeof b, "label-id: 0x%08x\n", t.mbr_disk_signature); o += b; }
  o += "device: " + dev + "\nunit: sectors\n";
  if (gpt) {
    const GptCopyInfo* c = (t.used_backup_gpt ? &gi.backup : &gi.primary);
    if (c->header_valid) {
      std::snprintf(b, sizeof b, "first-lba: %" PRIu64 "\nlast-lba: %" PRIu64 "\n", c->first_usable, c->last_usable);
      o += b;
    }
  }
  std::snprintf(b, sizeof b, "sector-size: %u\n\n", t.sector_size);
  o += b;
  int w = static_cast<int>(std::to_string(t.disk_sectors).size());
  std::string base = dev;
  if (!base.empty() && base.back() >= '0' && base.back() <= '9') base += 'p';
  for (const auto& p : t.partitions) {
    if (p.protective) continue;
    std::snprintf(b, sizeof b, "%s%u : start=%*" PRIu64 ", size=%*" PRIu64, base.c_str(), p.index, w, p.start_lba, w, p.sector_count);
    o += b;
    if (gpt) {
      o += ", type=" + upper_guid(p.type_guid) + ", uuid=" + upper_guid(p.unique_guid);
      if (!p.name.empty()) o += ", name=\"" + escape_name(p.name) + "\"";
      if (p.gpt_attributes) o += ", attrs=\"" + attrs_text(p.gpt_attributes) + "\"";
    } else {
      std::snprintf(b, sizeof b, ", type=%x", p.mbr_type);
      o += b;
      if (p.bootable) o += ", bootable";
    }
    o += "\n";
  }
  return o;
}

}  // namespace pe

Status parse_table_backup(const std::vector<uint8_t>& file, BackupInfo* info) {
  if (!info) return Status::Make(Errc::InvalidArgument);
  BackupFile b;
  Status st = parse_backup(file, &b);
  if (!st.ok()) return st;
  *info = BackupInfo{};
  info->version = b.version;
  info->sector_size = b.sector_size;
  info->table_kind = b.table_kind;
  info->disk_bytes = b.disk_bytes;
  info->timestamp = b.timestamp;
  info->sector_count = b.sectors.size();
  if (b.table_kind == 2) info->disk_id = guid_to_string(b.disk_id);
  else if (b.table_kind == 1) { char s[16]; std::snprintf(s, sizeof s, "%08x", le32(b.disk_id.data())); info->disk_id = s; }
  return Status::Ok();
}

}  // namespace usbmem
