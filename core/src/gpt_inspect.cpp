// SPDX-License-Identifier: Apache-2.0
// GPT の詳細検査と、パーティション編集の共通部品 (GUID / UTF-16 / GPT ヘッダ生成 / オーバーレイ / テーブルセクタ収集)。
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <set>

#include "partition_edit_internal.h"

namespace usbmem {

using namespace pe;

const char* gpt_repair_name(GptRepairKind k) {
  switch (k) {
    case GptRepairKind::CopyPrimaryToBackup: return "copy-primary-to-backup";
    case GptRepairKind::CopyBackupToPrimary: return "copy-backup-to-primary";
    case GptRepairKind::MoveBackupToEnd: return "move-backup-to-end";
    case GptRepairKind::RebuildProtectiveMbr: return "rebuild-protective-mbr";
  }
  return "";
}

// ------------------------------------------------------------------ GUID

bool parse_guid(const std::string& text, std::array<uint8_t, 16>* raw) {
  std::string s = text;
  if (s.size() == 38 && s.front() == '{' && s.back() == '}') s = s.substr(1, 36);
  if (s.size() != 36) return false;
  uint8_t b[16];
  int n = 0;
  for (size_t i = 0; i < s.size();) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (s[i] != '-') return false;
      i++;
      continue;
    }
    auto hex = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    if (i + 1 >= s.size()) return false;
    int h = hex(s[i]), l = hex(s[i + 1]);
    if (h < 0 || l < 0 || n >= 16) return false;
    b[n++] = static_cast<uint8_t>(h * 16 + l);
    i += 2;
  }
  if (n != 16) return false;
  // 標準表記 → ディスク上 (先頭 3 フィールドがリトルエンディアン)
  static const int order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
  for (int i = 0; i < 16; i++) (*raw)[order[i]] = b[i];
  return true;
}

std::array<uint8_t, 16> make_random_guid(uint64_t seed) {
  std::array<uint8_t, 16> g{};
  if (seed == 0) {
    bool ok = false;
    if (FILE* f = std::fopen("/dev/urandom", "rb")) {
      ok = std::fread(g.data(), 1, 16, f) == 16;
      std::fclose(f);
    }
    if (!ok) {
      std::random_device rd;
      for (auto& b : g) b = static_cast<uint8_t>(rd());
    }
  } else {
    uint64_t x = seed;
    auto next = [&]() {  // splitmix64
      uint64_t z = (x += 0x9E3779B97F4A7C15ull);
      z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
      z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
      return z ^ (z >> 31);
    };
    uint64_t a = next(), b = next();
    for (int i = 0; i < 8; i++) { g[i] = static_cast<uint8_t>(a >> (8 * i)); g[8 + i] = static_cast<uint8_t>(b >> (8 * i)); }
  }
  g[7] = static_cast<uint8_t>((g[7] & 0x0F) | 0x40);
  g[8] = static_cast<uint8_t>((g[8] & 0x3F) | 0x80);
  return g;
}

namespace pe {

bool GptGeo::standard(uint32_t ss, uint64_t disk_sectors, uint32_t num, uint32_t esz, GptGeo* g) {
  if (ss < 512 || num == 0 || num > kMaxGptEntries || esz < 128 || (esz & (esz - 1)) || esz > 4096) return false;
  *g = GptGeo{};
  g->ss = ss;
  g->disk_sectors = disk_sectors;
  g->num_entries = num;
  g->entry_size = esz;
  uint64_t arr = g->arr_sectors();
  // MBR(1) + ヘッダ(1) + 配列 + 使用領域(最低 1) + 配列 + ヘッダ(1)
  if (disk_sectors < 2 * arr + 5) return false;
  g->p_ent_lba = 2;
  g->first_usable = 2 + arr;
  g->alt_lba = disk_sectors - 1;
  g->b_ent_lba = disk_sectors - 1 - arr;
  g->last_usable = g->b_ent_lba - 1;
  return true;
}

std::vector<uint8_t> build_gpt_header(const GptGeo& g, bool primary, const std::array<uint8_t, 16>& disk_guid,
                                      uint32_t entries_crc) {
  std::vector<uint8_t> s(g.ss, 0);
  std::memcpy(s.data(), "EFI PART", 8);
  put32(s.data() + 8, 0x00010000);
  put32(s.data() + 12, 92);
  put64(s.data() + 24, primary ? 1 : g.alt_lba);
  put64(s.data() + 32, primary ? g.alt_lba : 1);
  put64(s.data() + 40, g.first_usable);
  put64(s.data() + 48, g.last_usable);
  std::memcpy(s.data() + 56, disk_guid.data(), 16);
  put64(s.data() + 72, primary ? g.p_ent_lba : g.b_ent_lba);
  put32(s.data() + 80, g.num_entries);
  put32(s.data() + 84, g.entry_size);
  put32(s.data() + 88, entries_crc);
  put32(s.data() + 16, crc32(s.data(), 92));
  return s;
}

bool utf8_to_utf16le(const std::string& str, size_t max_units, std::vector<uint8_t>* out) {
  out->clear();
  size_t units = 0;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(str.data());
  size_t n = str.size();
  for (size_t i = 0; i < n;) {
    uint32_t c = p[i];
    size_t extra;
    uint32_t minv;
    if (c < 0x80) { extra = 0; minv = 0; }
    else if ((c >> 5) == 6) { extra = 1; c &= 0x1F; minv = 0x80; }
    else if ((c >> 4) == 14) { extra = 2; c &= 0x0F; minv = 0x800; }
    else if ((c >> 3) == 30) { extra = 3; c &= 0x07; minv = 0x10000; }
    else return false;
    for (size_t k = 1; k <= extra; k++) {
      if (i + k >= n || (p[i + k] & 0xC0) != 0x80) return false;
      c = (c << 6) | (p[i + k] & 0x3F);
    }
    if (c < minv || c > 0x10FFFF || (c >= 0xD800 && c < 0xE000) || c == 0) return false;
    i += extra + 1;
    auto push = [&](uint32_t u) { out->push_back(static_cast<uint8_t>(u)); out->push_back(static_cast<uint8_t>(u >> 8)); units++; };
    if (c >= 0x10000) {
      c -= 0x10000;
      push(0xD800 + (c >> 10));
      push(0xDC00 + (c & 0x3FF));
    } else {
      push(c);
    }
    if (units > max_units) return false;
  }
  return true;
}

Status OverlayDevice::read_at(uint64_t off, void* buf, size_t len, const IoOptions& o) {
  Status st = base_.read_at(off, buf, len, o);
  if (!st.ok() || len == 0) return st;
  uint8_t* b = static_cast<uint8_t*>(buf);
  uint64_t first = off / ss_, last = (off + len - 1) / ss_;
  for (auto it = sectors_.lower_bound(first); it != sectors_.end() && it->first <= last; ++it) {
    uint64_t sec_off = it->first * ss_;
    uint64_t lo = std::max<uint64_t>(sec_off, off), hi = std::min<uint64_t>(sec_off + ss_, off + len);
    if (it->second.size() == ss_ && lo < hi) std::memcpy(b + (lo - off), it->second.data() + (lo - sec_off), hi - lo);
  }
  return Status::Ok();
}

SectorKind kind_of_what(const std::string& w) {
  if (w == "mbr" || w == "protective-mbr") return kMbr;
  if (w == "gpt-primary-header" || w == "gpt-erase-primary-header") return kGptPrimaryHeader;
  if (w == "gpt-primary-entries") return kGptPrimaryEntries;
  if (w == "gpt-backup-header" || w == "gpt-erase-backup-header" || w == "gpt-erase-stale-header") return kGptBackupHeader;
  if (w == "gpt-backup-entries") return kGptBackupEntries;
  if (w == "ebr") return kEbr;
  return kOther;
}

namespace {

// 1 本のヘッダを調べる。I/O エラーだけ Status で返す。
Status examine(BlockDevice& dev, uint32_t ss, uint64_t lba, uint64_t disk_sectors, GptCopyInfo* c) {
  *c = GptCopyInfo{};
  c->lba = lba;
  if (lba >= disk_sectors) return Status::Ok();
  std::vector<uint8_t> s(ss);
  Status st = dev.read_at(lba * ss, s.data(), ss);
  if (!st.ok()) return st;
  if (std::memcmp(s.data(), "EFI PART", 8) != 0) return Status::Ok();
  c->present = true;
  c->header_sector = s;
  uint32_t hsize = le32(s.data() + 12);
  if (hsize < 92 || hsize > ss) { c->reason = "bad header size"; return Status::Ok(); }
  std::vector<uint8_t> tmp(s.begin(), s.begin() + hsize);
  std::memset(tmp.data() + 16, 0, 4);
  if (crc32(tmp.data(), tmp.size()) != le32(s.data() + 16)) { c->reason = "header CRC mismatch"; return Status::Ok(); }
  c->revision = le32(s.data() + 8);
  uint64_t my = le64(s.data() + 24);
  c->alt_lba = le64(s.data() + 32);
  c->first_usable = le64(s.data() + 40);
  c->last_usable = le64(s.data() + 48);
  std::memcpy(c->disk_guid.data(), s.data() + 56, 16);
  c->entries_lba = le64(s.data() + 72);
  c->num_entries = le32(s.data() + 80);
  c->entry_size = le32(s.data() + 84);
  c->entries_crc = le32(s.data() + 88);
  if (my != lba) { c->reason = "MyLBA does not match location"; return Status::Ok(); }
  if (c->entry_size < 128 || (c->entry_size & (c->entry_size - 1)) || c->entry_size > 4096) { c->reason = "bad entry size"; return Status::Ok(); }
  if (c->num_entries == 0 || c->num_entries > kMaxGptEntries ||
      static_cast<uint64_t>(c->num_entries) * c->entry_size > kMaxGptEntriesBytes) { c->reason = "bad entry count"; return Status::Ok(); }
  if (c->first_usable > c->last_usable || c->first_usable >= disk_sectors) { c->reason = "bad usable range"; return Status::Ok(); }
  bool beyond = c->last_usable >= disk_sectors || (lba == 1 && c->alt_lba >= disk_sectors);
  uint64_t arr = (static_cast<uint64_t>(c->num_entries) * c->entry_size + ss - 1) / ss;
  if (c->entries_lba < 2 || c->entries_lba >= disk_sectors || arr > disk_sectors - c->entries_lba) {
    c->reason = "entry array outside disk";
    return Status::Ok();
  }
  size_t bytes = static_cast<size_t>(c->num_entries) * c->entry_size;
  c->entries.assign(bytes, 0);
  st = dev.read_at(c->entries_lba * ss, c->entries.data(), bytes);
  if (!st.ok()) return st;
  c->entries_valid = crc32(c->entries.data(), bytes) == c->entries_crc;
  if (beyond) {  // 縮んだイメージ: 修復の元には使えるが、通常の編集の元にはしない
    c->truncated = true;
    c->reason = "usable range or backup location lies beyond the end of the device";
    return Status::Ok();
  }
  c->header_valid = true;
  return Status::Ok();
}

void add(GptInspection* g, Severity sv, const char* code, std::string msg) { g->diagnostics.push_back({sv, code, std::move(msg)}); }

void check_entries(GptInspection* g, const GptCopyInfo& c, const char* which) {
  struct R { uint64_t a, b; uint32_t idx; };
  std::vector<R> rs;
  std::set<std::string> uuids;
  for (uint32_t i = 0; i < c.num_entries; i++) {
    const uint8_t* e = c.entries.data() + static_cast<size_t>(i) * c.entry_size;
    if (all_zero(e, 16)) continue;
    uint64_t a = le64(e + 32), b = le64(e + 40);
    std::string tag = std::string(which) + " entry " + std::to_string(i + 1);
    if (a > b || b >= g->disk_sectors) { add(g, Severity::Error, "gpt-entry-invalid-range", tag + " has an invalid or out-of-disk range"); continue; }
    if (a < c.first_usable || b > c.last_usable) add(g, Severity::Warning, "gpt-entry-outside-usable", tag + " is outside the usable range");
    if (all_zero(e + 16, 16)) add(g, Severity::Warning, "gpt-entry-zero-unique-guid", tag + " has an all-zero unique GUID");
    else if (!uuids.insert(std::string(reinterpret_cast<const char*>(e + 16), 16)).second)
      add(g, Severity::Warning, "gpt-entry-duplicate-guid", tag + " duplicates another entry's unique GUID");
    rs.push_back({a, b, i + 1});
  }
  std::sort(rs.begin(), rs.end(), [](const R& x, const R& y) { return x.a < y.a; });
  for (size_t i = 1; i < rs.size(); i++)
    if (rs[i].a <= rs[i - 1].b || rs[i].a <= rs[i - 1].a)
      add(g, Severity::Warning, "partitions-overlap",
          std::string(which) + " entries " + std::to_string(rs[i - 1].idx) + " and " + std::to_string(rs[i].idx) + " overlap");
}

}  // namespace
}  // namespace pe

Status inspect_gpt(BlockDevice& dev, uint32_t ss, GptInspection* out) {
  if (!out) return Status::Make(Errc::InvalidArgument);
  *out = GptInspection{};
  if (ss < 512 || ss > 65536 || (ss & (ss - 1))) return Status::Make(Errc::InvalidArgument);
  GptInspection& g = *out;
  g.sector_size = ss;
  g.disk_sectors = dev.size() / ss;
  if (g.disk_sectors < 1) return Status::Ok();
  const uint64_t last = g.disk_sectors - 1;

  // 保護 MBR
  {
    std::vector<uint8_t> m(ss);
    Status st = dev.read_at(0, m.data(), ss);
    if (!st.ok()) return st;
    g.mbr_present = m[510] == 0x55 && m[511] == 0xAA;
    if (g.mbr_present) {
      for (int i = 0; i < 4; i++) {
        const uint8_t* e = m.data() + 446 + 16 * i;
        if (e[4] == 0xEE) {
          g.mbr_protective = true;
          uint64_t want = std::min<uint64_t>(last, 0xFFFFFFFFull);
          if (le32(e + 8) == 1 && le32(e + 12) == want) g.mbr_protective_ok = true;
        } else if (e[4] != 0) {
          g.mbr_hybrid = true;
        }
      }
    }
  }
  if (g.disk_sectors < 3) return Status::Ok();

  Status st = examine(dev, ss, 1, g.disk_sectors, &g.primary);
  if (!st.ok()) return st;

  // バックアップ候補: プライマリが示す位置 → ディスク末尾
  std::vector<uint64_t> cand;
  if (g.primary.header_valid && g.primary.alt_lba > 1 && g.primary.alt_lba < g.disk_sectors) cand.push_back(g.primary.alt_lba);
  if (std::find(cand.begin(), cand.end(), last) == cand.end()) cand.push_back(last);
  bool have = false;
  for (uint64_t lba : cand) {
    GptCopyInfo c;
    st = examine(dev, ss, lba, g.disk_sectors, &c);
    if (!st.ok()) return st;
    if (!c.present) continue;
    auto rank = [](const GptCopyInfo& x) { return x.usable() ? 3 : x.header_valid ? 2 : 1; };
    if (!have || rank(c) > rank(g.backup)) { g.backup = std::move(c); have = true; }
    if (g.backup.usable()) break;
  }
  if (!have) g.backup.lba = last;

  const GptCopyInfo &P = g.primary, &B = g.backup;
  g.backup_at_end = B.present && B.lba == last;
  g.disk_grew = B.header_valid && B.lba < last;
  g.backup_beyond_disk = (P.header_valid && P.alt_lba >= g.disk_sectors) || (P.truncated && P.entries_valid);
  g.copies_agree = P.usable() && B.usable() && P.disk_guid == B.disk_guid && P.first_usable == B.first_usable &&
                   P.last_usable == B.last_usable && P.entries_crc == B.entries_crc && P.num_entries == B.num_entries &&
                   P.entry_size == B.entry_size && P.alt_lba == B.lba && B.alt_lba == 1;

  if (!P.present && !B.present) return Status::Ok();

  // ---- 診断
  if (P.present && !P.header_valid) add(&g, Severity::Error, "gpt-primary-header-invalid", "primary GPT header invalid: " + P.reason);
  if (!P.present && B.present) add(&g, Severity::Warning, "gpt-primary-header-missing", "primary GPT header missing");
  if (B.present && !B.header_valid) add(&g, Severity::Warning, "gpt-backup-header-invalid", "backup GPT header invalid: " + B.reason);
  if (!B.present && P.header_valid) add(&g, Severity::Warning, "gpt-backup-header-missing", "backup GPT header not found");
  if (P.header_valid && !P.entries_valid) add(&g, Severity::Error, "gpt-primary-entries-crc", "primary GPT entry array CRC mismatch");
  if (B.header_valid && !B.entries_valid) add(&g, Severity::Warning, "gpt-backup-entries-crc", "backup GPT entry array CRC mismatch");
  if (!P.usable() && !B.usable()) add(&g, Severity::Error, "gpt-unusable", "no valid GPT header and entry array");
  if (P.truncated) add(&g, Severity::Warning, "gpt-truncated", "the primary GPT describes more space than the device has (image truncated or disk shrunk)");
  if (P.usable() && B.usable() && !g.copies_agree) {
    std::string why;
    if (P.disk_guid != B.disk_guid) why += " disk-guid";
    if (P.first_usable != B.first_usable || P.last_usable != B.last_usable) why += " usable-range";
    if (P.entries_crc != B.entries_crc) why += " entries";
    if (P.alt_lba != B.lba) why += " alt-lba";
    add(&g, Severity::Warning, "gpt-backup-mismatch", "primary and backup GPT differ:" + why);
  }
  if (g.disk_grew) add(&g, Severity::Warning, "gpt-backup-not-at-end", "backup GPT is not at the last sector (disk was resized?)");
  if (g.backup_beyond_disk) add(&g, Severity::Warning, "gpt-backup-beyond-disk", "primary GPT points to a backup location beyond the end of the device (image truncated?)");
  for (const GptCopyInfo* c : {&P, &B}) {
    if (!c->header_valid) continue;
    const char* nm = c == &P ? "primary" : "backup";
    uint64_t arr = (static_cast<uint64_t>(c->num_entries) * c->entry_size + ss - 1) / ss;
    if (c->entries_lba <= c->last_usable && c->entries_lba + arr > c->first_usable)
      add(&g, Severity::Error, "gpt-entry-array-overlaps-usable", std::string(nm) + " entry array overlaps the usable range");
    if (all_zero(c->disk_guid.data(), 16)) add(&g, Severity::Warning, "gpt-disk-guid-zero", std::string(nm) + " GPT disk GUID is all zero");
    if (c->revision != 0x00010000) add(&g, Severity::Info, "gpt-revision", std::string(nm) + " GPT has an unusual revision");
    uint32_t hsize = le32(c->header_sector.data() + 12);
    if (hsize > 92 && !all_zero(c->header_sector.data() + 92, std::min<size_t>(hsize, c->header_sector.size()) - 92))
      add(&g, Severity::Info, "gpt-header-trailing-data", std::string(nm) + " GPT header has non-zero bytes after the defined fields");
    if (c->usable()) check_entries(&g, *c, nm);
  }
  if (g.mbr_present && g.mbr_hybrid && g.mbr_protective) add(&g, Severity::Warning, "hybrid-mbr", "hybrid MBR detected; GPT is authoritative");
  if (!g.mbr_present) add(&g, Severity::Warning, "gpt-no-protective-mbr", "no protective MBR");
  else if (!g.mbr_protective) add(&g, Severity::Warning, "gpt-no-protective-mbr", "no protective MBR entry");
  else if (!g.mbr_protective_ok) add(&g, Severity::Warning, "gpt-protective-mbr-bad-size", "protective MBR entry does not span the disk");

  // ---- 修復の提案
  const GptCopyInfo* src = P.usable() ? &P : B.usable() ? &B : nullptr;
  if (!src && P.truncated && P.entries_valid) g.suggested_repairs.push_back(GptRepairKind::MoveBackupToEnd);
  if (src) {
    uint64_t src_backup_lba = src == &P ? P.alt_lba : B.lba;
    if (src_backup_lba != last) g.suggested_repairs.push_back(GptRepairKind::MoveBackupToEnd);
    else if (src == &P && !B.usable()) g.suggested_repairs.push_back(GptRepairKind::CopyPrimaryToBackup);
    else if (src == &B && !P.usable()) g.suggested_repairs.push_back(GptRepairKind::CopyBackupToPrimary);
    else if (!g.copies_agree) g.suggested_repairs.push_back(GptRepairKind::CopyPrimaryToBackup);
    if (!g.mbr_protective_ok && !g.mbr_hybrid) g.suggested_repairs.push_back(GptRepairKind::RebuildProtectiveMbr);
  }
  return Status::Ok();
}

namespace pe {

Status collect_table_sectors(BlockDevice& dev, uint32_t ss, const GptInspection& gi, std::vector<TableSector>* out) {
  std::map<uint64_t, TableSector> m;
  uint64_t disk_sectors = dev.size() / ss;
  auto grab = [&](uint64_t lba, SectorKind k) -> Status {
    if (lba >= disk_sectors || m.count(lba)) return Status::Ok();
    TableSector t{lba, k, std::vector<uint8_t>(ss)};
    Status st = dev.read_at(lba * ss, t.data.data(), ss);
    if (!st.ok()) return st;
    m.emplace(lba, std::move(t));
    return Status::Ok();
  };
  Status st = grab(0, kMbr);
  if (!st.ok()) return st;
  std::vector<uint8_t> s0 = m[0].data;

  if (gi.sector_size == ss) {
    for (int pass = 0; pass < 2; pass++) {
      const GptCopyInfo& c = pass == 0 ? gi.primary : gi.backup;
      if (!c.present) continue;
      st = grab(c.lba, pass == 0 ? kGptPrimaryHeader : kGptBackupHeader);
      if (!st.ok()) return st;
      if (c.header_valid) {
        uint64_t arr = (static_cast<uint64_t>(c.num_entries) * c.entry_size + ss - 1) / ss;
        for (uint64_t i = 0; i < arr; i++) {
          st = grab(c.entries_lba + i, pass == 0 ? kGptPrimaryEntries : kGptBackupEntries);
          if (!st.ok()) return st;
        }
      }
    }
  }
  bool gpt_usable = gi.primary.usable() || gi.backup.usable();
  if (!gpt_usable && s0[510] == 0x55 && s0[511] == 0xAA) {
    uint64_t ext_start = 0;
    bool have = false;
    for (int i = 0; i < 4; i++) {
      const uint8_t* e = s0.data() + 446 + 16 * i;
      if ((e[4] == 0x05 || e[4] == 0x0F || e[4] == 0x85) && le32(e + 12) != 0) { ext_start = le32(e + 8); have = true; break; }
    }
    if (have) {
      std::set<uint64_t> seen;
      uint64_t ebr = ext_start;
      for (int n = 0; n < 256 && ebr < disk_sectors && seen.insert(ebr).second; n++) {
        st = grab(ebr, kEbr);
        if (!st.ok()) return st;
        const std::vector<uint8_t>& d = m[ebr].data;
        if (d[510] != 0x55 || d[511] != 0xAA) break;
        const uint8_t* e1 = d.data() + 446 + 16;
        if (e1[4] == 0 || le32(e1 + 12) == 0 || !(e1[4] == 0x05 || e1[4] == 0x0F || e1[4] == 0x85)) break;
        ebr = ext_start + le32(e1 + 8);
      }
    }
  }
  out->clear();
  for (auto& kv : m) out->push_back(std::move(kv.second));
  return Status::Ok();
}

}  // namespace pe
}  // namespace usbmem
