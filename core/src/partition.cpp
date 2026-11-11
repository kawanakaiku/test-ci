// SPDX-License-Identifier: Apache-2.0
#include "usbmem/partition.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>

#include "usbmem/crc32.h"

namespace usbmem {
namespace {

uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t le64(const uint8_t* p) { return le32(p) | (static_cast<uint64_t>(le32(p + 4)) << 32); }

constexpr size_t kMaxGptEntriesBytes = 4u << 20;  // 異常に大きい配列を拒否 (標準は 16 KiB)
constexpr uint32_t kMaxGptEntries = 8192;
constexpr int kMaxLogicalPartitions = 256;

void diag(PartitionTable& t, Severity s, const char* code, std::string msg) {
  t.diagnostics.push_back({s, code, std::move(msg)});
}

bool all_zero(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (p[i]) return false;
  return true;
}

void fill_extent(Partition& p, const PartitionTable& t) {
  // 呼び出し側で start_lba/sector_count が disk 内か検証済み、あるいは超過を許容して表示する。
  // バイト換算は 64bit でオーバーフローしうるので飽和させる。
  auto mul = [&](uint64_t v) {
    return v > UINT64_MAX / t.sector_size ? UINT64_MAX : v * t.sector_size;
  };
  p.start_bytes = mul(p.start_lba);
  p.size_bytes = mul(p.sector_count);
}

// ------------------------------------------------------------------ GPT

struct GptHeader {
  uint32_t revision, header_size;
  uint64_t my_lba, alt_lba, first_usable, last_usable;
  std::array<uint8_t, 16> disk_guid;
  uint64_t entries_lba;
  uint32_t num_entries, entry_size, entries_crc;
};

// ヘッダ 1 本を検証する。失敗理由を reason に入れる。
bool parse_gpt_header(const uint8_t* s, size_t ss, uint64_t lba, uint64_t disk_sectors, GptHeader* h,
                      std::string* reason) {
  if (std::memcmp(s, "EFI PART", 8) != 0) { *reason = "bad signature"; return false; }
  h->header_size = le32(s + 12);
  if (h->header_size < 92 || h->header_size > ss) { *reason = "bad header size"; return false; }
  uint32_t stored = le32(s + 16);
  std::vector<uint8_t> tmp(s, s + h->header_size);
  std::memset(tmp.data() + 16, 0, 4);
  if (crc32(tmp.data(), tmp.size()) != stored) { *reason = "header CRC mismatch"; return false; }
  h->revision = le32(s + 8);
  h->my_lba = le64(s + 24);
  h->alt_lba = le64(s + 32);
  h->first_usable = le64(s + 40);
  h->last_usable = le64(s + 48);
  std::memcpy(h->disk_guid.data(), s + 56, 16);
  h->entries_lba = le64(s + 72);
  h->num_entries = le32(s + 80);
  h->entry_size = le32(s + 84);
  h->entries_crc = le32(s + 88);
  if (h->my_lba != lba) { *reason = "MyLBA does not match location"; return false; }
  if (h->entry_size < 128 || (h->entry_size & (h->entry_size - 1)) != 0 || h->entry_size > 4096) {
    *reason = "bad entry size"; return false;
  }
  if (h->num_entries == 0 || h->num_entries > kMaxGptEntries) { *reason = "bad entry count"; return false; }
  if (static_cast<uint64_t>(h->num_entries) * h->entry_size > kMaxGptEntriesBytes) {
    *reason = "entry array too large"; return false;
  }
  if (h->first_usable > h->last_usable || h->last_usable >= disk_sectors) {
    *reason = "bad usable range"; return false;
  }
  uint64_t arr_secs = (static_cast<uint64_t>(h->num_entries) * h->entry_size + ss - 1) / ss;
  if (h->entries_lba >= disk_sectors || arr_secs > disk_sectors - h->entries_lba) {
    *reason = "entry array outside disk"; return false;
  }
  return true;
}

Status read_gpt_entries(BlockDevice& dev, const GptHeader& h, uint32_t ss, std::vector<uint8_t>* out,
                        bool* crc_ok) {
  size_t bytes = static_cast<size_t>(h.num_entries) * h.entry_size;
  out->assign(bytes, 0);
  Status st = dev.read_at(h.entries_lba * ss, out->data(), bytes);
  if (!st.ok()) return st;
  *crc_ok = crc32(out->data(), bytes) == h.entries_crc;
  return Status::Ok();
}

std::string utf16le_to_utf8(const uint8_t* p, size_t units) {
  std::string out;
  for (size_t i = 0; i < units; i++) {
    uint32_t c = le16(p + 2 * i);
    if (c == 0) break;
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < units) {
      uint32_t d = le16(p + 2 * (i + 1));
      if (d >= 0xDC00 && d < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); i++; }
      else c = 0xFFFD;
    } else if (c >= 0xD800 && c < 0xE000) {
      c = 0xFFFD;  // 孤立サロゲート
    }
    if (c < 0x80) out += static_cast<char>(c);
    else if (c < 0x800) { out += static_cast<char>(0xC0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) {
      out += static_cast<char>(0xE0 | (c >> 12)); out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (c >> 18)); out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); out += static_cast<char>(0x80 | (c & 0x3F));
    }
  }
  return out;
}

void check_overlaps(PartitionTable& t) {
  std::vector<const Partition*> v;
  for (auto& p : t.partitions)
    if (!p.extended_container && !p.protective) v.push_back(&p);
  std::sort(v.begin(), v.end(), [](auto* a, auto* b) { return a->start_lba < b->start_lba; });
  for (size_t i = 1; i < v.size(); i++) {
    const Partition* a = v[i - 1];
    const Partition* b = v[i];
    if (b->start_lba - a->start_lba < a->sector_count) {  // start 昇順なので差は非負
      diag(t, Severity::Warning, "partitions-overlap",
           "partitions " + std::to_string(a->index) + " and " + std::to_string(b->index) + " overlap");
    }
  }
}

// GPT を読む。signature が見つからない場合 *present=false。
// 戻り値の Status は I/O エラーのみ。
Status parse_gpt(BlockDevice& dev, PartitionTable& t, bool* present) {
  const uint32_t ss = t.sector_size;
  *present = false;
  std::vector<uint8_t> sec(ss);
  const uint64_t last_lba = t.disk_sectors - 1;

  GptHeader prim{}, back{};
  std::string prim_reason, back_reason;
  bool prim_ok = false, back_ok = false, prim_sig = false, back_sig = false;

  if (t.disk_sectors >= 2) {
    Status st = dev.read_at(1ull * ss, sec.data(), ss);
    if (!st.ok()) return st;
    prim_sig = std::memcmp(sec.data(), "EFI PART", 8) == 0;
    if (prim_sig) prim_ok = parse_gpt_header(sec.data(), ss, 1, t.disk_sectors, &prim, &prim_reason);
  }

  // バックアップ: プライマリの AlternateLBA 優先、なければディスク末尾
  std::set<uint64_t> cand;
  if (prim_ok && prim.alt_lba < t.disk_sectors) cand.insert(prim.alt_lba);
  if (t.disk_sectors >= 3) cand.insert(last_lba);
  for (uint64_t lba : cand) {
    if (lba <= 1) continue;
    Status st = dev.read_at(lba * ss, sec.data(), ss);
    if (!st.ok()) return st;
    bool sig = std::memcmp(sec.data(), "EFI PART", 8) == 0;
    if (!sig) continue;
    back_sig = true;
    GptHeader h{};
    std::string why;
    if (parse_gpt_header(sec.data(), ss, lba, t.disk_sectors, &h, &why)) { back = h; back_ok = true; back_reason.clear(); break; }
    back_reason = why;
  }

  if (!prim_sig && !back_sig) return Status::Ok();
  *present = true;
  t.kind = TableKind::Gpt;

  if (prim_sig && !prim_ok) diag(t, Severity::Error, "gpt-primary-header-invalid", "primary GPT header invalid: " + prim_reason);
  if (!prim_sig && back_sig) diag(t, Severity::Warning, "gpt-primary-header-missing", "primary GPT header missing");
  if (back_sig && !back_ok) diag(t, Severity::Warning, "gpt-backup-header-invalid", "backup GPT header invalid: " + back_reason);
  if (!back_sig && prim_ok) diag(t, Severity::Warning, "gpt-backup-header-missing", "backup GPT header not found");

  // 採用するヘッダを決める。エントリ配列の CRC まで通ったものだけ採用。
  std::vector<uint8_t> prim_ent, back_ent;
  bool prim_ent_ok = false, back_ent_ok = false;
  if (prim_ok) {
    Status st = read_gpt_entries(dev, prim, ss, &prim_ent, &prim_ent_ok);
    if (!st.ok()) return st;
    if (!prim_ent_ok) diag(t, Severity::Error, "gpt-primary-entries-crc", "primary GPT entry array CRC mismatch");
  }
  if (back_ok) {
    Status st = read_gpt_entries(dev, back, ss, &back_ent, &back_ent_ok);
    if (!st.ok()) return st;
    if (!back_ent_ok) diag(t, Severity::Warning, "gpt-backup-entries-crc", "backup GPT entry array CRC mismatch");
  }

  const GptHeader* use = nullptr;
  const std::vector<uint8_t>* ents = nullptr;
  if (prim_ok && prim_ent_ok) { use = &prim; ents = &prim_ent; }
  else if (back_ok && back_ent_ok) {
    use = &back; ents = &back_ent; t.used_backup_gpt = true;
    diag(t, Severity::Warning, "gpt-using-backup", "using backup GPT because the primary is unusable");
  } else {
    diag(t, Severity::Error, "gpt-unusable", "no valid GPT header and entry array");
    return Status::Ok();
  }

  if (prim_ok && prim_ent_ok && back_ok && back_ent_ok) {
    if (prim.entries_crc != back.entries_crc || prim.disk_guid != back.disk_guid ||
        prim.first_usable != back.first_usable || prim.last_usable != back.last_usable)
      diag(t, Severity::Warning, "gpt-backup-mismatch", "primary and backup GPT differ");
  }
  if (use->alt_lba != last_lba && !t.used_backup_gpt)
    diag(t, Severity::Warning, "gpt-backup-not-at-end",
         "backup GPT is not at the last sector (disk was resized or image truncated?)");
  if (use->revision != 0x00010000) diag(t, Severity::Info, "gpt-revision", "unusual GPT revision");

  t.disk_guid = guid_to_string(use->disk_guid);
  for (uint32_t i = 0; i < use->num_entries; i++) {
    const uint8_t* e = ents->data() + static_cast<size_t>(i) * use->entry_size;
    if (all_zero(e, 16)) continue;  // 未使用
    Partition p;
    p.index = i + 1;
    std::memcpy(p.type_guid.data(), e, 16);
    std::memcpy(p.unique_guid.data(), e + 16, 16);
    uint64_t first = le64(e + 32), last = le64(e + 40);
    p.gpt_attributes = le64(e + 48);
    p.name = utf16le_to_utf8(e + 56, 36);
    p.type_name = gpt_type_name(p.type_guid);
    if (first > last || last >= t.disk_sectors) {
      diag(t, Severity::Error, "gpt-entry-invalid-range",
           "GPT entry " + std::to_string(i + 1) + " has an invalid or out-of-disk range; skipped");
      continue;
    }
    if (first < use->first_usable || last > use->last_usable)
      diag(t, Severity::Warning, "gpt-entry-outside-usable",
           "GPT entry " + std::to_string(i + 1) + " is outside the usable range");
    p.start_lba = first;
    p.sector_count = last - first + 1;
    fill_extent(p, t);
    t.partitions.push_back(std::move(p));
  }
  check_overlaps(t);
  return Status::Ok();
}

// ------------------------------------------------------------------ MBR

bool is_extended_type(uint8_t t) { return t == 0x05 || t == 0x0F || t == 0x85; }

void parse_mbr(BlockDevice& dev, PartitionTable& t, const uint8_t* mbr, Status* io) {
  const uint32_t ss = t.sector_size;
  t.kind = TableKind::Mbr;
  t.mbr_disk_signature = le32(mbr + 440);
  uint64_t ext_start = 0, ext_len = 0;
  bool ext_seen = false;

  for (int i = 0; i < 4; i++) {
    const uint8_t* e = mbr + 446 + 16 * i;
    if (all_zero(e, 16)) continue;
    if (e[0] != 0x00 && e[0] != 0x80)
      diag(t, Severity::Warning, "mbr-bad-status", "MBR entry " + std::to_string(i + 1) + " has an invalid status byte");
    uint8_t type = e[4];
    uint64_t start = le32(e + 8), count = le32(e + 12);
    if (type == 0 || count == 0) continue;
    Partition p;
    p.index = i + 1;
    p.mbr_type = type;
    p.bootable = e[0] == 0x80;
    p.start_lba = start;
    p.sector_count = count;
    p.protective = type == 0xEE;
    p.extended_container = is_extended_type(type);
    p.type_name = mbr_type_name(type);
    fill_extent(p, t);
    if (start >= t.disk_sectors || count > t.disk_sectors - start)
      diag(t, Severity::Warning, "mbr-partition-exceeds-disk",
           "MBR entry " + std::to_string(i + 1) + " extends past the end of the device");
    if (p.extended_container) {
      if (ext_seen) diag(t, Severity::Warning, "mbr-multiple-extended", "multiple extended partitions");
      else { ext_seen = true; ext_start = start; ext_len = count; }
    }
    t.partitions.push_back(std::move(p));
  }

  if (!ext_seen) return;
  // EBR チェーン: 論理パーティションの start は「その EBR」からの相対、
  // 次の EBR の位置は「拡張パーティション先頭」からの相対。
  std::set<uint64_t> visited;
  uint64_t ebr = ext_start;
  std::vector<uint8_t> sec(ss);
  for (int n = 0; n < kMaxLogicalPartitions; n++) {
    if (ebr >= t.disk_sectors) {
      diag(t, Severity::Warning, "ebr-outside-disk", "EBR lies outside the device; chain truncated");
      return;
    }
    if (!visited.insert(ebr).second) {
      diag(t, Severity::Error, "ebr-loop", "EBR chain loops; truncated");
      return;
    }
    Status st = dev.read_at(ebr * ss, sec.data(), ss);
    if (!st.ok()) { *io = st; return; }
    if (sec[510] != 0x55 || sec[511] != 0xAA) {
      diag(t, Severity::Warning, "ebr-bad-signature", "EBR has no 0x55AA signature; chain truncated");
      return;
    }
    const uint8_t* e0 = sec.data() + 446;
    const uint8_t* e1 = sec.data() + 446 + 16;
    if (e0[4] != 0 && le32(e0 + 12) != 0) {
      Partition p;
      p.index = 5 + static_cast<uint32_t>(n);
      p.logical = true;
      p.mbr_type = e0[4];
      p.bootable = e0[0] == 0x80;
      p.start_lba = ebr + le32(e0 + 8);  // 64bit なのでオーバーフローしない
      p.sector_count = le32(e0 + 12);
      p.type_name = mbr_type_name(p.mbr_type);
      fill_extent(p, t);
      if (p.start_lba < ext_start || p.start_lba + p.sector_count > ext_start + ext_len)
        diag(t, Severity::Warning, "logical-outside-extended",
             "logical partition " + std::to_string(p.index) + " is outside its extended partition");
      if (p.start_lba >= t.disk_sectors || p.sector_count > t.disk_sectors - p.start_lba)
        diag(t, Severity::Warning, "mbr-partition-exceeds-disk",
             "logical partition " + std::to_string(p.index) + " extends past the end of the device");
      t.partitions.push_back(std::move(p));
    }
    if (e1[4] == 0 || le32(e1 + 12) == 0 || !is_extended_type(e1[4])) return;  // チェーン終端
    uint64_t next = ext_start + le32(e1 + 8);
    ebr = next;
  }
  diag(t, Severity::Warning, "ebr-too-many", "too many logical partitions; chain truncated");
}

}  // namespace

// ------------------------------------------------------------------ 公開 API

bool PartitionTable::has_errors() const {
  for (auto& d : diagnostics)
    if (d.severity == Severity::Error) return true;
  return false;
}

std::string guid_to_string(const std::array<uint8_t, 16>& g) {
  char b[40];
  std::snprintf(b, sizeof b, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", g[3],
                g[2], g[1], g[0], g[5], g[4], g[7], g[6], g[8], g[9], g[10], g[11], g[12], g[13], g[14],
                g[15]);
  return b;
}

const char* mbr_type_name(uint8_t t) {
  switch (t) {
    case 0x01: return "FAT12";
    case 0x04: case 0x06: case 0x0E: return "FAT16";
    case 0x05: case 0x0F: case 0x85: return "Extended";
    case 0x07: return "NTFS/exFAT/HPFS";
    case 0x0B: case 0x0C: return "FAT32";
    case 0x11: case 0x14: case 0x16: case 0x1B: case 0x1C: case 0x1E: return "Hidden FAT";
    case 0x27: return "Windows RE";
    case 0x82: return "Linux swap";
    case 0x83: return "Linux";
    case 0x8E: return "Linux LVM";
    case 0xA5: return "FreeBSD";
    case 0xAF: return "HFS/HFS+";
    case 0xEE: return "GPT protective";
    case 0xEF: return "EFI System";
    case 0xFD: return "Linux RAID";
  }
  return "";
}

const char* gpt_type_name(const std::array<uint8_t, 16>& raw) {
  static const struct { const char* guid; const char* name; } kTypes[] = {
      {"C12A7328-F81F-11D2-BA4B-00A0C93EC93B", "EFI System"},
      {"024DEE41-33E7-11D3-9D69-0008C781F39F", "MBR partition scheme"},
      {"21686148-6449-6E6F-744E-656564454649", "BIOS boot"},
      {"E3C9E316-0B5C-4DB8-817D-F92DF00215AE", "Microsoft reserved"},
      {"EBD0A0A2-B9E5-4433-87C0-68B6B72699C7", "Microsoft basic data"},
      {"DE94BBA4-06D1-4D40-A16A-BFD50179D6AC", "Windows recovery"},
      {"5808C8AA-7E8F-42E0-85D2-E1E90434CFB3", "LDM metadata"},
      {"AF9B60A0-1431-4F62-BC68-3311714A69AD", "LDM data"},
      {"0FC63DAF-8483-4772-8E79-3D69D8477DE4", "Linux filesystem"},
      {"0657FD6D-A4AB-43C4-84E5-0933C84B4F4F", "Linux swap"},
      {"A19D880F-05FC-4D3B-A006-743F0F84911E", "Linux RAID"},
      {"E6D6D379-F507-44C2-A23C-238F2A3DF928", "Linux LVM"},
      {"933AC7E1-2EB4-4F13-B844-0E14E2AEF915", "Linux /home"},
      {"48465300-0000-11AA-AA11-00306543ECAC", "Apple HFS+"},
      {"7C3457EF-0000-11AA-AA11-00306543ECAC", "APFS"},
  };
  std::string s = guid_to_string(raw);
  for (auto& t : kTypes)
    if (s == t.guid) return t.name;
  return "";
}

Status read_partition_table(BlockDevice& dev, PartitionTable* out, const ParseOptions& opt) {
  if (!out) return Status::Make(Errc::InvalidArgument);
  *out = PartitionTable{};
  PartitionTable& t = *out;
  t.disk_bytes = dev.size();

  std::vector<uint32_t> sizes;
  if (opt.sector_size) {
    if (opt.sector_size < 512 || opt.sector_size > 65536 || (opt.sector_size & (opt.sector_size - 1)))
      return Status::Make(Errc::InvalidArgument);
    sizes = {opt.sector_size};
  } else {
    for (uint32_t s : {dev.logical_block_size(), 512u, 4096u})
      if (std::find(sizes.begin(), sizes.end(), s) == sizes.end()) sizes.push_back(s);
  }

  if (t.disk_bytes < 512) {
    t.sector_size = sizes[0];
    diag(t, Severity::Info, "device-too-small", "device is smaller than one sector");
    return Status::Ok();
  }

  // 1) GPT (サイズ候補ごとに LBA1 を調べる)
  for (uint32_t ss : sizes) {
    if (t.disk_bytes < 2ull * ss) continue;
    PartitionTable cand;
    cand.sector_size = ss;
    cand.disk_bytes = t.disk_bytes;
    cand.disk_sectors = t.disk_bytes / ss;
    bool present = false;
    Status st = parse_gpt(dev, cand, &present);
    if (!st.ok()) return st;
    if (present) {
      *out = std::move(cand);
      break;
    }
  }
  if (t.kind == TableKind::Gpt) {
    // 保護 MBR / ハイブリッド MBR の確認(情報提供のみ)
    std::vector<uint8_t> mbr(512);
    Status st = dev.read_at(0, mbr.data(), 512);
    if (!st.ok()) return st;
    if (mbr[510] == 0x55 && mbr[511] == 0xAA) {
      bool prot = false, other = false;
      for (int i = 0; i < 4; i++) {
        const uint8_t* e = mbr.data() + 446 + 16 * i;
        if (e[4] == 0xEE) prot = true;
        else if (e[4] != 0) other = true;
      }
      if (!prot) diag(t, Severity::Warning, "gpt-no-protective-mbr", "no protective MBR entry");
      else if (other) diag(t, Severity::Warning, "hybrid-mbr", "hybrid MBR detected; GPT is authoritative");
    } else {
      diag(t, Severity::Warning, "gpt-no-protective-mbr", "no protective MBR");
    }
  } else {
    t.sector_size = sizes[0];
    t.disk_sectors = t.disk_bytes / t.sector_size;
    std::vector<uint8_t> s0(t.sector_size);
    Status st = dev.read_at(0, s0.data(), s0.size());
    if (!st.ok()) return st;

    FsType vbr = probe_boot_sector(s0.data(), s0.size(), t.disk_bytes);
    if (vbr != FsType::Unknown) {
      // パーティションを持たない"スーパーフロッピー"
      t.whole_device_volume = true;
      t.whole_device_fs = vbr;
      Partition p;
      p.index = 1;
      p.start_lba = 0;
      p.sector_count = t.disk_sectors;
      fill_extent(p, t);
      p.fs = vbr;
      t.partitions.push_back(std::move(p));
      diag(t, Severity::Info, "superfloppy", "no partition table; the whole device is one volume");
      return Status::Ok();
    }
    if (s0[510] == 0x55 && s0[511] == 0xAA) {
      Status io;
      parse_mbr(dev, t, s0.data(), &io);
      if (!io.ok()) return io;
      for (auto& p : t.partitions)
        if (p.protective)
          diag(t, Severity::Error, "gpt-protective-without-gpt",
               "protective MBR present but no valid GPT was found");
      check_overlaps(t);
      if (t.partitions.empty()) diag(t, Severity::Info, "mbr-empty", "MBR contains no partitions");
    } else {
      Status io;
      FsType fs = probe_filesystem(dev, 0, t.disk_bytes, &io);
      if (!io.ok()) return io;
      if (fs != FsType::Unknown) {
        t.whole_device_volume = true;
        t.whole_device_fs = fs;
        Partition p;
        p.index = 1;
        p.sector_count = t.disk_sectors;
        fill_extent(p, t);
        p.fs = fs;
        t.partitions.push_back(std::move(p));
        diag(t, Severity::Info, "superfloppy", "no partition table; the whole device is one volume");
      } else {
        bool blank = all_zero(s0.data(), s0.size());
        diag(t, Severity::Info, blank ? "blank-device" : "no-partition-table",
             blank ? "first sector is all zero" : "no recognizable partition table or filesystem");
      }
      return Status::Ok();
    }
  }

  if (opt.probe_filesystems) {
    for (auto& p : t.partitions) {
      if (p.extended_container || p.protective || p.fs != FsType::Unknown) continue;
      if (!range_ok(t.disk_bytes, p.start_bytes, p.size_bytes)) continue;
      Status io;
      p.fs = probe_filesystem(dev, p.start_bytes, p.size_bytes, &io);
      if (!io.ok()) return io;
    }
  }
  return Status::Ok();
}

// ------------------------------------------------------------------ JSON

namespace {
void json_str(std::string& o, const std::string& s) {
  o += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += static_cast<char>(c);
    }
  }
  o += '"';
}
const char* kind_name(TableKind k) { return k == TableKind::Gpt ? "gpt" : k == TableKind::Mbr ? "mbr" : "none"; }
const char* sev_name(Severity s) { return s == Severity::Error ? "error" : s == Severity::Warning ? "warning" : "info"; }
}  // namespace

std::string partition_table_to_json(const PartitionTable& t) {
  // 64bit 値は JSON number の精度を超えうる(2^53)ため、サイズ・LBA は文字列で出す。
  std::string o = "{\"kind\":";
  json_str(o, kind_name(t.kind));
  o += ",\"sectorSize\":" + std::to_string(t.sector_size);
  o += ",\"diskBytes\":\"" + std::to_string(t.disk_bytes) + "\"";
  o += ",\"diskGuid\":"; json_str(o, t.disk_guid);
  o += ",\"usedBackupGpt\":"; o += t.used_backup_gpt ? "true" : "false";
  o += ",\"wholeDeviceVolume\":"; o += t.whole_device_volume ? "true" : "false";
  o += ",\"partitions\":[";
  for (size_t i = 0; i < t.partitions.size(); i++) {
    const Partition& p = t.partitions[i];
    if (i) o += ',';
    o += "{\"index\":" + std::to_string(p.index);
    o += ",\"startLba\":\"" + std::to_string(p.start_lba) + "\"";
    o += ",\"sectorCount\":\"" + std::to_string(p.sector_count) + "\"";
    o += ",\"startBytes\":\"" + std::to_string(p.start_bytes) + "\"";
    o += ",\"sizeBytes\":\"" + std::to_string(p.size_bytes) + "\"";
    o += ",\"logical\":"; o += p.logical ? "true" : "false";
    o += ",\"extendedContainer\":"; o += p.extended_container ? "true" : "false";
    o += ",\"protective\":"; o += p.protective ? "true" : "false";
    o += ",\"bootable\":"; o += p.bootable ? "true" : "false";
    o += ",\"mbrType\":" + std::to_string(p.mbr_type);
    o += ",\"typeGuid\":"; json_str(o, t.kind == TableKind::Gpt ? guid_to_string(p.type_guid) : "");
    o += ",\"uniqueGuid\":"; json_str(o, t.kind == TableKind::Gpt ? guid_to_string(p.unique_guid) : "");
    o += ",\"name\":"; json_str(o, p.name);
    o += ",\"typeName\":"; json_str(o, p.type_name);
    o += ",\"fs\":"; json_str(o, fs_type_name(p.fs));
    o += '}';
  }
  o += "],\"diagnostics\":[";
  for (size_t i = 0; i < t.diagnostics.size(); i++) {
    if (i) o += ',';
    o += "{\"severity\":"; json_str(o, sev_name(t.diagnostics[i].severity));
    o += ",\"code\":"; json_str(o, t.diagnostics[i].code);
    o += ",\"message\":"; json_str(o, t.diagnostics[i].message);
    o += '}';
  }
  o += "]}";
  return o;
}

}  // namespace usbmem
