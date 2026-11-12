// SPDX-License-Identifier: Apache-2.0
// パーティションエディタ本体 (操作の積み上げ → 計画 → 確認トークン付き適用)。設計は docs/partition-edit.md。
#include "usbmem/partition_edit.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <mutex>
#include <set>

#include "partition_edit_internal.h"
#include "usbmem/sha256.h"

namespace usbmem {
using namespace pe;

namespace {

// 書き込み可能なエディタの排他 (デバイスごとに 1 つ)
std::mutex g_reg_mu;
std::set<const void*> g_reg_devs;

const std::array<uint8_t, 16> kLinuxFsGuid = {0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
                                              0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4};

Diagnostic D(Severity s, const char* code, std::string msg) { return Diagnostic{s, code, std::move(msg)}; }

EditResult fail(Errc c, const char* code, std::string msg) {
  EditResult r;
  r.status = Status::Make(c);
  r.code = code;
  r.message = std::move(msg);
  return r;
}
EditResult success(const char* code = "ok") {
  EditResult r;
  r.code = code;
  return r;
}

// ------------------------------------------------------------------ モデル

enum Dirty : uint32_t { kDType = 1, kDUid = 2, kDRange = 4, kDAttrs = 8, kDName = 16 };

struct MPart {
  uint32_t handle = 0;
  bool created = false;
  bool logical = false, extended = false;
  uint64_t start = 0, count = 0;
  uint8_t mbr_type = 0;
  bool bootable = false;
  std::array<uint8_t, 16> type_guid{}, unique_guid{};
  uint64_t attrs = 0;
  std::string name;
  int slot = -1;             // GPT: エントリ位置 / MBR 主: 0..3
  uint32_t dirty = 0;
};

struct Model {
  TableKind kind = TableKind::None;
  bool unusable = false;            // GPT が壊れていて編集の元にできない
  bool fresh = false;               // new_table
  bool overwrite_fs = false;
  bool rebuild_mbr = false;         // 保護 MBR を作り直す
  bool has_repair = false;
  GptRepairKind repair = GptRepairKind::CopyPrimaryToBackup;
  std::vector<MPart> parts;
  std::array<uint8_t, 16> disk_guid{};
  uint32_t mbr_sig = 0;
  GptGeo geo;
  std::vector<uint8_t> entries;     // GPT エントリ配列 (元の生バイト。fresh ならゼロ)
  std::vector<std::pair<uint32_t, int>> orig_slots;  // 元の (handle, slot)
  uint32_t next_handle = 5;
};

struct Span { uint64_t lo, hi; };  // [lo, hi)
Span span_of(const MPart& p) { return {p.logical && p.start > 0 ? p.start - 1 : p.start, p.start + p.count}; }

uint64_t align_up(uint64_t v, uint64_t a) { return a <= 1 ? v : (v + a - 1) / a * a; }

bool is_ext_type(uint8_t t) { return t == 0x05 || t == 0x0F || t == 0x85; }

const MPart* find_part(const Model& m, uint32_t handle) {
  for (auto& p : m.parts)
    if (p.handle == handle) return &p;
  return nullptr;
}
MPart* find_part(Model& m, uint32_t handle) { return const_cast<MPart*>(find_part(static_cast<const Model&>(m), handle)); }
const MPart* find_ext(const Model& m, uint32_t skip = 0) {
  for (auto& p : m.parts)
    if (p.extended && p.handle != skip) return &p;
  return nullptr;
}

// 候補 c が表に収まるか。収まれば空、そうでなければ理由コード。skip = 比較から除く既存パーティション (自分自身)
std::string check_fit(const Model& m, uint64_t N, const MPart& c, uint32_t skip) {
  if (c.count == 0) return "zero-size";
  if (c.count > UINT64_MAX - c.start) return "overflow";
  uint64_t end = c.start + c.count;  // 排他的
  if (end > N) return "out-of-range";
  if (m.kind == TableKind::Gpt) {
    if (c.start < m.geo.first_usable || end - 1 > m.geo.last_usable) return "outside-usable";
  } else {
    if (c.start == 0) return "overlaps-table";
    if (c.start > 0xFFFFFFFFull || c.count > 0xFFFFFFFFull || end > (1ull << 32)) return "mbr-lba-limit";
    if (c.logical) {
      const MPart* e = find_ext(m, skip);
      if (!e) return "no-extended";
      if (c.start < 1 || span_of(c).lo < e->start || end > e->start + e->count) return "outside-extended";
    } else if (c.extended) {
      for (auto& p : m.parts) {
        if (!p.logical || p.handle == skip) continue;
        Span s = span_of(p);
        if (s.lo < c.start || s.hi > end) return "extended-excludes-logical";
      }
    }
  }
  Span cs = span_of(c);
  for (auto& p : m.parts) {
    if (p.handle == skip) continue;
    if (m.kind == TableKind::Mbr && p.logical != c.logical) continue;  // 主/拡張と論理は別の階層
    Span s = span_of(p);
    if (cs.lo < s.hi && s.lo < cs.hi) return "overlap";
  }
  return "";
}

// 空き領域を探す。want_start == 0 なら自動配置。成功なら "" を返し *start / *count を設定する
std::string find_space(const Model& m, uint64_t N, uint64_t align, bool logical, uint64_t want_start, uint64_t want_count,
                       uint64_t* start, uint64_t* count) {
  uint64_t lo, hi;
  if (m.kind == TableKind::Gpt) {
    lo = m.geo.first_usable;
    hi = m.geo.last_usable + 1;
  } else if (logical) {
    const MPart* e = find_ext(m);
    if (!e) return "no-extended";
    lo = e->start;
    hi = e->start + e->count;
  } else {
    lo = 1;
    hi = std::min<uint64_t>(N, 1ull << 32);
  }
  std::vector<Span> used;
  for (auto& p : m.parts) {
    if (m.kind == TableKind::Mbr && p.logical != logical) continue;
    used.push_back(span_of(p));
  }
  std::sort(used.begin(), used.end(), [](const Span& a, const Span& b) { return a.lo < b.lo; });

  if (want_start) {
    if (logical && want_start < 1) return "out-of-range";
    uint64_t cand_lo = logical ? want_start - 1 : want_start;
    uint64_t limit = hi;
    for (auto& s : used) {
      if (s.lo <= cand_lo && s.hi > cand_lo) return "overlap";
      if (s.lo > cand_lo) limit = std::min(limit, s.lo);
    }
    *start = want_start;
    if (want_count) { *count = want_count; return ""; }
    if (limit <= want_start) return "no-space";
    *count = limit - want_start;
    return "";
  }
  for (int pass = 0; pass < 2; pass++) {
    uint64_t al = pass == 0 ? align : 1;
    bool found = false;
    uint64_t best_s = 0, best_c = 0, prev = lo;
    for (size_t i = 0; i <= used.size(); i++) {
      uint64_t gap_hi = i < used.size() ? used[i].lo : hi;
      uint64_t cand = align_up(logical ? prev + 1 : prev, al);
      if (gap_hi > cand) {
        uint64_t avail = gap_hi - cand;
        if (want_count) {
          if (avail >= want_count) { best_s = cand; best_c = want_count; found = true; break; }
        } else if (avail > best_c) {
          best_s = cand; best_c = avail; found = true;
        }
      }
      if (i < used.size()) prev = std::max(prev, used[i].hi);
    }
    if (found) { *start = best_s; *count = best_c; return ""; }
  }
  return "no-space";
}

// ------------------------------------------------------------------ 直列化 (モデル → セクタ)

struct Planned {
  uint64_t lba;
  std::vector<uint8_t> data;
  std::string what;
  int order;
};

void chs(uint64_t lba, uint8_t* o) {
  if (lba >= 1023ull * 255 * 63) { o[0] = 0xFE; o[1] = 0xFF; o[2] = 0xFF; return; }
  uint32_t c = static_cast<uint32_t>(lba / (255 * 63)), t = static_cast<uint32_t>(lba % (255 * 63));
  uint32_t h = t / 63, s = t % 63 + 1;
  o[0] = static_cast<uint8_t>(h);
  o[1] = static_cast<uint8_t>(s | ((c >> 2) & 0xC0));
  o[2] = static_cast<uint8_t>(c & 0xFF);
}

void mbr_entry(uint8_t* e, bool boot, uint8_t type, uint64_t start, uint64_t count) {
  std::memset(e, 0, 16);
  e[0] = boot ? 0x80 : 0x00;
  chs(start, e + 1);
  e[4] = type;
  chs(count ? start + count - 1 : start, e + 5);
  put32(e + 8, static_cast<uint32_t>(start));
  put32(e + 12, static_cast<uint32_t>(count));
}

enum Order { kOrdBackupEntries = 0, kOrdBackupHeader, kOrdPrimaryEntries, kOrdPrimaryHeader, kOrdMbr, kOrdStale };

void chunk_entries(std::vector<Planned>* out, const std::vector<uint8_t>& ent, uint64_t lba, uint32_t ss, const char* what,
                   int order) {
  size_t secs = (ent.size() + ss - 1) / ss;
  for (size_t i = 0; i < secs; i++) {
    Planned p{lba + i, std::vector<uint8_t>(ss, 0), what, order};
    size_t n = std::min<size_t>(ss, ent.size() - i * ss);
    std::memcpy(p.data.data(), ent.data() + i * ss, n);
    out->push_back(std::move(p));
  }
}

std::vector<uint8_t> protective_mbr(const std::vector<uint8_t>& s0, uint32_t ss, uint64_t disk_sectors) {
  std::vector<uint8_t> s(ss, 0);
  if (s0.size() == ss && s0[510] == 0x55 && s0[511] == 0xAA) std::memcpy(s.data(), s0.data(), 446);  // ブートコードとシグネチャを残す
  uint8_t* e = s.data() + 446;
  e[0] = 0;
  e[1] = 0; e[2] = 2; e[3] = 0;
  e[4] = 0xEE;
  if (disk_sectors - 1 >= 1023ull * 255 * 63) { e[5] = 0xFF; e[6] = 0xFF; e[7] = 0xFF; }  // UEFI 仕様: 表せないときは 0xFFFFFF
  else chs(disk_sectors - 1, e + 5);
  put32(e + 8, 1);
  put32(e + 12, static_cast<uint32_t>(std::min<uint64_t>(disk_sectors - 1, 0xFFFFFFFFull)));
  s[510] = 0x55; s[511] = 0xAA;
  return s;
}

std::vector<uint8_t> text_to_utf16_field(const std::string& name) {
  std::vector<uint8_t> u;
  utf8_to_utf16le(name, 36, &u);
  u.resize(72, 0);
  return u;
}

// GPT のエントリ配列を作る (編集後のモデル)
std::vector<uint8_t> build_entries(const Model& m) {
  std::vector<uint8_t> ent = m.entries;
  size_t esz = m.geo.entry_size;
  ent.resize(static_cast<size_t>(m.geo.num_entries) * esz, 0);
  for (auto& os : m.orig_slots) {  // 削除されたもの
    bool alive = false;
    for (auto& p : m.parts)
      if (!p.created && p.handle == os.first) alive = true;
    if (!alive && os.second >= 0 && static_cast<uint32_t>(os.second) < m.geo.num_entries)
      std::memset(ent.data() + static_cast<size_t>(os.second) * esz, 0, esz);
  }
  for (auto& p : m.parts) {
    if (p.slot < 0 || static_cast<uint32_t>(p.slot) >= m.geo.num_entries) continue;
    uint8_t* e = ent.data() + static_cast<size_t>(p.slot) * esz;
    uint32_t d = p.created ? 0xFFFFFFFFu : p.dirty;
    if (p.created) std::memset(e, 0, esz);
    if (d & kDType) std::memcpy(e, p.type_guid.data(), 16);
    if (d & kDUid) std::memcpy(e + 16, p.unique_guid.data(), 16);
    if (d & kDRange) { put64(e + 32, p.start); put64(e + 40, p.start + p.count - 1); }
    if (d & kDAttrs) put64(e + 48, p.attrs);
    if (d & kDName) { auto u = text_to_utf16_field(p.name); std::memcpy(e + 56, u.data(), 72); }
  }
  return ent;
}

void serialize_gpt(const Model& m, uint32_t ss, uint64_t N, const std::vector<uint8_t>& s0, bool write_protective,
                   std::vector<Planned>* out) {
  std::vector<uint8_t> ent = build_entries(m);
  uint32_t crc = crc32(ent.data(), ent.size());
  chunk_entries(out, ent, m.geo.b_ent_lba, ss, "gpt-backup-entries", kOrdBackupEntries);
  out->push_back({m.geo.alt_lba, build_gpt_header(m.geo, false, m.disk_guid, crc), "gpt-backup-header", kOrdBackupHeader});
  chunk_entries(out, ent, m.geo.p_ent_lba, ss, "gpt-primary-entries", kOrdPrimaryEntries);
  out->push_back({1, build_gpt_header(m.geo, true, m.disk_guid, crc), "gpt-primary-header", kOrdPrimaryHeader});
  if (write_protective) out->push_back({0, protective_mbr(s0, ss, N), "protective-mbr", kOrdMbr});
}

// MBR の直列化。論理パーティションの EBR 位置は「最初は拡張の先頭、以降は start - 1」
bool serialize_mbr(const Model& m, uint32_t ss, const std::vector<uint8_t>& s0, std::vector<Planned>* out, std::string* why) {
  std::vector<uint8_t> s(ss, 0);
  if (s0.size() == ss && s0[510] == 0x55 && s0[511] == 0xAA) std::memcpy(s.data(), s0.data(), 446);
  else if (s0.size() == ss) std::memcpy(s.data(), s0.data(), 440);
  put32(s.data() + 440, m.mbr_sig);
  std::vector<const MPart*> logicals;
  const MPart* ext = nullptr;
  for (auto& p : m.parts) {
    if (p.logical) { logicals.push_back(&p); continue; }
    if (p.slot < 0 || p.slot > 3) { *why = "primary-slot"; return false; }
    mbr_entry(s.data() + 446 + 16 * p.slot, p.bootable, p.mbr_type, p.start, p.count);
    if (p.extended) ext = &p;
  }
  // 使っていない主スロットはゼロ (memset 済みでないので明示的に消す)
  for (int i = 0; i < 4; i++) {
    bool used = false;
    for (auto& p : m.parts)
      if (!p.logical && p.slot == i) used = true;
    if (!used) std::memset(s.data() + 446 + 16 * i, 0, 16);
  }
  s[510] = 0x55; s[511] = 0xAA;
  out->push_back({0, std::move(s), "mbr", 1});

  if (ext) {
    std::sort(logicals.begin(), logicals.end(), [](const MPart* a, const MPart* b) { return a->start < b->start; });
    std::vector<uint64_t> ebr;
    for (size_t i = 0; i < logicals.size(); i++) ebr.push_back(i == 0 ? ext->start : logicals[i]->start - 1);
    if (logicals.empty()) ebr.push_back(ext->start);
    uint8_t ext_type = is_ext_type(ext->mbr_type) ? ext->mbr_type : 0x05;
    for (size_t i = 0; i < std::max<size_t>(1, logicals.size()); i++) {
      std::vector<uint8_t> e(ss, 0);
      if (i < logicals.size()) {
        const MPart* l = logicals[i];
        mbr_entry(e.data() + 446, l->bootable, l->mbr_type, l->start - ebr[i], l->count);
        // CHS は絶対 LBA から計算する
        chs(l->start, e.data() + 447);
        chs(l->start + l->count - 1, e.data() + 451);
        if (i + 1 < logicals.size()) {
          const MPart* n = logicals[i + 1];
          uint64_t rel = ebr[i + 1] - ext->start, len = n->start + n->count - ebr[i + 1];
          mbr_entry(e.data() + 446 + 16, false, ext_type, rel, len);
          chs(ebr[i + 1], e.data() + 446 + 16 + 1);
          chs(n->start + n->count - 1, e.data() + 446 + 16 + 5);
        }
      }
      e[510] = 0x55; e[511] = 0xAA;
      out->push_back({ebr[i], std::move(e), "ebr", 0});
    }
  }
  return true;
}

// ------------------------------------------------------------------ 表示用

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
      c = 0xFFFD;
    }
    if (c < 0x80) out += static_cast<char>(c);
    else if (c < 0x800) { out += static_cast<char>(0xC0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { out += static_cast<char>(0xE0 | (c >> 12)); out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); out += static_cast<char>(0x80 | (c & 0x3F)); }
    else { out += static_cast<char>(0xF0 | (c >> 18)); out += static_cast<char>(0x80 | ((c >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); out += static_cast<char>(0x80 | (c & 0x3F)); }
  }
  return out;
}

// 意味的に同じ表か (作って消した、同じ値を設定した、など)。同じなら書き込みは不要
bool models_equal(const Model& a, const Model& b) {
  if (a.kind != b.kind || a.unusable != b.unusable || b.fresh || b.has_repair || b.rebuild_mbr) return false;
  if (a.disk_guid != b.disk_guid || a.mbr_sig != b.mbr_sig) return false;
  if (a.kind == TableKind::Gpt && (a.geo.alt_lba != b.geo.alt_lba || a.geo.first_usable != b.geo.first_usable ||
                                   a.geo.last_usable != b.geo.last_usable || a.geo.b_ent_lba != b.geo.b_ent_lba ||
                                   a.geo.p_ent_lba != b.geo.p_ent_lba))
    return false;
  if (a.parts.size() != b.parts.size()) return false;
  for (auto& q : b.parts) {
    const MPart* p = find_part(a, q.handle);
    if (!p) return false;
    if (p->start != q.start || p->count != q.count || p->mbr_type != q.mbr_type || p->bootable != q.bootable ||
        p->type_guid != q.type_guid || p->unique_guid != q.unique_guid || p->attrs != q.attrs || p->name != q.name ||
        p->logical != q.logical || p->extended != q.extended || p->slot != q.slot)
      return false;
  }
  return true;
}

std::string fmt_size(uint64_t sectors, uint32_t ss) {
  char b[48];
  double bytes = static_cast<double>(sectors) * ss;
  if (bytes >= 1099511627776.0) std::snprintf(b, sizeof b, "%.2f TiB", bytes / 1099511627776.0);
  else if (bytes >= 1073741824.0) std::snprintf(b, sizeof b, "%.2f GiB", bytes / 1073741824.0);
  else if (bytes >= 1048576.0) std::snprintf(b, sizeof b, "%.2f MiB", bytes / 1048576.0);
  else std::snprintf(b, sizeof b, "%.1f KiB", bytes / 1024.0);
  return b;
}

std::string describe_part(const MPart& p, TableKind kind, uint32_t ss) {
  char b[256];
  std::snprintf(b, sizeof b, "#%u start=%" PRIu64 " sectors=%" PRIu64 " (%s)", p.handle, p.start, p.count, fmt_size(p.count, ss).c_str());
  std::string s = b;
  if (kind == TableKind::Gpt) {
    std::string tn = gpt_type_name(p.type_guid);
    s += " type=" + (tn.empty() ? guid_to_string(p.type_guid) : tn);
    if (!p.name.empty()) s += " name=\"" + p.name + "\"";
  } else {
    std::snprintf(b, sizeof b, " type=0x%02X%s%s", p.mbr_type, p.logical ? " logical" : "", p.extended ? " extended" : "");
    s += b;
  }
  return s;
}

}  // namespace

// ------------------------------------------------------------------ Impl

struct PartitionEditor::Impl {
  BlockDevicePtr dev;
  EditorOptions opt;
  uint32_t ss = 512;
  uint64_t N = 0;
  uint64_t align = 2048;
  bool writable = false;
  bool registered = false;
  bool in_use = false;
  PartitionTable table;
  GptInspection gi;
  Model base, work;
  std::vector<std::string> log;
  uint64_t rng_counter = 0;
  mutable std::mutex mu;

  ~Impl() {
    if (registered) {
      std::lock_guard<std::mutex> l(g_reg_mu);
      g_reg_devs.erase(dev.get());
    }
  }

  std::array<uint8_t, 16> new_guid() {
    return make_random_guid(opt.rng_seed ? opt.rng_seed * 1000003ull + (++rng_counter) : 0);
  }

  Status refresh();
  Model load_model() const;
  Model model_from_copy(const GptCopyInfo& c, bool primary) const;
  std::string device_id(uint32_t use_ss = 0) const;
  EditResult check_ready(bool need_table) const;
  EditResult validate(const Model& m, std::vector<Diagnostic>* out) const;
  EditResult build_edit_plan(EditPlan* out) const;
  Status finish_plan(std::vector<Planned>& pl, EditPlan* out, const std::vector<std::pair<std::string, int>>& stage_names,
                     bool allow_fs_overwrite, uint32_t pss, uint64_t pN) const;
  std::string compute_hash(const EditPlan& p) const;
  void describe_changes(EditPlan* out) const;
};

Status PartitionEditor::Impl::refresh() {
  ParseOptions po;
  po.sector_size = opt.sector_size;
  po.read_labels = false;
  Status st = read_partition_table(*dev, &table, po);
  if (!st.ok()) return st;
  ss = table.sector_size;
  N = dev->size() / ss;
  st = inspect_gpt(*dev, ss, &gi);
  if (!st.ok()) return st;
  align = opt.alignment_sectors ? opt.alignment_sectors : std::max<uint64_t>(1, (1u << 20) / ss);
  base = load_model();
  work = base;
  log.clear();
  return Status::Ok();
}

Model PartitionEditor::Impl::model_from_copy(const GptCopyInfo& c, bool primary) const {
  Model m;
  m.kind = TableKind::Gpt;
  const GptCopyInfo& P = gi.primary;
  const GptCopyInfo& B = gi.backup;
  m.geo.ss = ss;
  m.geo.disk_sectors = N;
  m.geo.first_usable = c.first_usable;
  m.geo.last_usable = c.last_usable;
  m.geo.num_entries = c.num_entries;
  m.geo.entry_size = c.entry_size;
  uint64_t arr = m.geo.arr_sectors();
  if (primary) {
    m.geo.alt_lba = c.alt_lba;
    m.geo.p_ent_lba = c.entries_lba;
    m.geo.b_ent_lba = B.header_valid ? B.entries_lba : (c.alt_lba > arr ? c.alt_lba - arr : 0);
  } else {
    m.geo.alt_lba = c.lba;
    m.geo.b_ent_lba = c.entries_lba;
    m.geo.p_ent_lba = P.header_valid ? P.entries_lba : 2;
  }
  m.disk_guid = c.disk_guid;
  m.entries = c.entries;
  return m;
}

Model PartitionEditor::Impl::load_model() const {
  Model m;
  m.kind = table.kind;
  if (table.kind == TableKind::Gpt) {
    const GptCopyInfo* src = gi.primary.usable() ? &gi.primary : gi.backup.usable() ? &gi.backup : nullptr;
    if (!src) { m.unusable = true; return m; }
    m = model_from_copy(*src, src == &gi.primary);
    for (auto& p : table.partitions) {
      MPart q;
      q.handle = p.index;
      q.slot = static_cast<int>(p.index) - 1;
      q.start = p.start_lba;
      q.count = p.sector_count;
      q.type_guid = p.type_guid;
      q.unique_guid = p.unique_guid;
      q.attrs = p.gpt_attributes;
      q.name = p.name;
      m.parts.push_back(q);
      m.orig_slots.push_back({q.handle, q.slot});
    }
  } else if (table.kind == TableKind::Mbr) {
    m.mbr_sig = table.mbr_disk_signature;
    for (auto& p : table.partitions) {
      MPart q;
      q.handle = p.index;
      q.logical = p.logical;
      q.extended = p.extended_container;
      q.start = p.start_lba;
      q.count = p.sector_count;
      q.mbr_type = p.mbr_type;
      q.bootable = p.bootable;
      q.slot = p.logical ? -1 : static_cast<int>(p.index) - 1;
      m.parts.push_back(q);
      m.next_handle = std::max<uint32_t>(m.next_handle, p.index + 1);
    }
  }
  return m;
}

std::string PartitionEditor::Impl::device_id(uint32_t use_ss) const {
  std::string id = dev->describe() + "|bytes=" + std::to_string(dev->size()) + "|ss=" + std::to_string(use_ss ? use_ss : ss);
  if (table.kind == TableKind::Gpt) id += "|gpt=" + table.disk_guid;
  else if (table.kind == TableKind::Mbr) id += "|mbr=" + table.mbr_disk_id;
  return id;
}

EditResult PartitionEditor::Impl::check_ready(bool need_table) const {
  if (need_table) {
    if (work.kind == TableKind::None) return fail(Errc::InvalidArgument, "no-table", "the device has no partition table; create one first");
    if (work.unusable) return fail(Errc::InvalidArgument, "gpt-unusable", "the GPT is not usable as a base for editing; create a new table or restore a backup");
  }
  return success();
}

// ------------------------------------------------------------------ 検証

EditResult PartitionEditor::Impl::validate(const Model& m, std::vector<Diagnostic>* out) const {
  auto err = [&](const char* c, std::string msg) { out->push_back(D(Severity::Error, c, std::move(msg))); };
  auto warn = [&](const char* c, std::string msg) { out->push_back(D(Severity::Warning, c, std::move(msg))); };
  if (m.kind == TableKind::None) return success();
  if (m.unusable) { err("gpt-unusable", "GPT is not usable"); return success(); }

  if (m.kind == TableKind::Gpt) {
    const GptGeo& g = m.geo;
    uint64_t arr = g.arr_sectors();
    bool geo_ok = g.num_entries >= 1 && g.num_entries <= kMaxGptEntries && g.entry_size >= 128 && g.first_usable <= g.last_usable &&
                  g.last_usable < N && g.alt_lba < N && g.alt_lba > g.last_usable && g.p_ent_lba >= 2 && g.b_ent_lba >= 2 &&
                  g.p_ent_lba + arr <= g.first_usable && g.b_ent_lba > g.last_usable && g.b_ent_lba + arr <= g.alt_lba &&
                  g.first_usable >= 2;
    if (!geo_ok) err("gpt-geometry-invalid", "GPT header / entry array placement is inconsistent with the disk");
    std::set<std::string> uids;
    std::set<int> slots;
    for (auto& p : m.parts) {
      if (p.slot < 0 || static_cast<uint32_t>(p.slot) >= g.num_entries) err("gpt-no-free-slot", "partition " + std::to_string(p.handle) + " has no entry slot");
      else if (!slots.insert(p.slot).second) err("gpt-slot-conflict", "two partitions use entry slot " + std::to_string(p.slot + 1));
      if (all_zero(p.type_guid.data(), 16)) err("gpt-type-zero", "partition " + std::to_string(p.handle) + " has an all-zero type GUID");
      if (all_zero(p.unique_guid.data(), 16)) err("gpt-unique-zero", "partition " + std::to_string(p.handle) + " has an all-zero unique GUID");
      else if (!uids.insert(std::string(reinterpret_cast<const char*>(p.unique_guid.data()), 16)).second)
        err("gpt-unique-duplicate", "partition " + std::to_string(p.handle) + " duplicates another unique GUID");
      std::vector<uint8_t> u;
      if (!utf8_to_utf16le(p.name, 36, &u)) err("gpt-name-invalid", "partition " + std::to_string(p.handle) + " name is invalid or longer than 36 UTF-16 units");
    }
    if (all_zero(m.disk_guid.data(), 16)) err("gpt-disk-guid-zero", "disk GUID is all zero");
  } else {
    int prim = 0, ext = 0;
    std::set<int> slots;
    std::set<uint32_t> hs;
    for (auto& p : m.parts) {
      if (!hs.insert(p.handle).second) err("handle-duplicate", "duplicate partition number");
      if (p.logical) continue;
      prim++;
      if (p.extended) ext++;
      if (p.slot < 0 || p.slot > 3 || !slots.insert(p.slot).second) err("mbr-slot-conflict", "primary slot conflict");
    }
    if (prim > 4) err("mbr-too-many-primary", "more than 4 primary partitions");
    if (ext > 1) err("mbr-multiple-extended", "more than one extended partition");
    for (auto& p : m.parts) {
      if (p.mbr_type == 0) err("mbr-type-zero", "partition " + std::to_string(p.handle) + " has type 0x00");
      if (p.mbr_type == 0xEE) err("mbr-type-protective", "type 0xEE is reserved for the GPT protective MBR");
      if (p.extended != is_ext_type(p.mbr_type)) err("mbr-extended-type", "partition " + std::to_string(p.handle) + ": extended type and role disagree");
      if (p.logical && !find_ext(m)) err("no-extended", "logical partition without an extended partition");
      if (p.logical && p.extended) err("mbr-extended-logical", "a logical partition cannot be extended");
    }
    int boots = 0;
    for (auto& p : m.parts) boots += p.bootable ? 1 : 0;
    if (boots > 1) warn("mbr-multiple-active", "more than one partition is marked active");
  }
  // 範囲・重なり
  for (auto& p : m.parts) {
    std::string r = check_fit(m, N, p, p.handle);
    if (!r.empty()) err(r == "overlap" ? "partitions-overlap" : "partition-range-invalid", "partition " + std::to_string(p.handle) + ": " + r);
    if ((p.created || (p.dirty & kDRange)) && p.start % align != 0 && !p.extended)
      warn("partition-unaligned", "partition " + std::to_string(p.handle) + " does not start on a " + std::to_string(align) + "-sector boundary");
  }
  return success();
}

// ------------------------------------------------------------------ 計画の共通仕上げ

std::string PartitionEditor::Impl::compute_hash(const EditPlan& p) const {
  Sha256 h;
  std::string head = "usbmem-plan-v1";
  h.update(head.data(), head.size() + 1);  // NUL を含める
  h.update(p.device_id.data(), p.device_id.size() + 1);
  uint8_t b[8];
  put32(b, p.sector_size);
  h.update(b, 4);
  put32(b, p.requires_force ? 1 : 0);
  h.update(b, 4);
  for (auto& st : p.stages) {
    h.update(st.name.data(), st.name.size() + 1);
    put32(b, static_cast<uint32_t>(st.writes.size()));
    h.update(b, 4);
    for (auto& w : st.writes) {
      put64(b, w.lba);
      h.update(b, 8);
      put32(b, static_cast<uint32_t>(w.data.size()));
      h.update(b, 4);
      h.update(w.data.data(), w.data.size());
      put32(b, static_cast<uint32_t>(w.before.size()));
      h.update(b, 4);
      h.update(w.before.data(), w.before.size());
    }
  }
  return hex_encode(h.finish());
}

static bool plan_tables_equal(const PartitionTable& a, const PartitionTable& b, std::vector<Diagnostic>* diffs) {
  bool eq = true;
  auto d = [&](const std::string& m) { eq = false; if (diffs) diffs->push_back(D(Severity::Error, "verify-mismatch", m)); };
  if (a.kind != b.kind) d("table kind differs");
  if (a.kind == TableKind::Gpt && a.disk_guid != b.disk_guid) d("disk GUID differs");
  if (a.kind == TableKind::Mbr && a.mbr_disk_signature != b.mbr_disk_signature) d("MBR disk signature differs");
  if (a.partitions.size() != b.partitions.size()) { d("partition count differs"); return false; }
  for (size_t i = 0; i < a.partitions.size(); i++) {
    const Partition &x = a.partitions[i], &y = b.partitions[i];
    if (x.index != y.index || x.start_lba != y.start_lba || x.sector_count != y.sector_count || x.mbr_type != y.mbr_type ||
        x.type_guid != y.type_guid || x.unique_guid != y.unique_guid || x.gpt_attributes != y.gpt_attributes || x.name != y.name ||
        x.bootable != y.bootable || x.logical != y.logical)
      d("partition " + std::to_string(x.index) + " differs");
  }
  return eq;
}

Status PartitionEditor::Impl::finish_plan(std::vector<Planned>& pl, EditPlan* out,
                                          const std::vector<std::pair<std::string, int>>& stage_names,
                                          bool allow_fs_overwrite, uint32_t ss, uint64_t N) const {
  std::stable_sort(pl.begin(), pl.end(), [](const Planned& a, const Planned& b) {
    if (a.order != b.order) return a.order < b.order;
    return a.lba < b.lba;
  });
  // 同じ LBA の重複 (後勝ち) を除く
  std::map<uint64_t, size_t> last_idx;
  for (size_t i = 0; i < pl.size(); i++) last_idx[pl[i].lba] = i;
  std::map<uint64_t, std::vector<uint8_t>> overlay;
  for (auto& st : stage_names) {
    EditStage es;
    es.name = st.first;
    for (size_t i = 0; i < pl.size(); i++) {
      if (pl[i].order != st.second || last_idx[pl[i].lba] != i) continue;
      Planned& p = pl[i];
      if (p.lba >= N) {
        out->problems.push_back(D(Severity::Error, "write-out-of-range", "planned write at LBA " + std::to_string(p.lba) + " is outside the disk"));
        continue;
      }
      SectorWrite w;
      w.lba = p.lba;
      w.what = p.what;
      w.before.assign(ss, 0);
      Status rs = dev->read_at(p.lba * ss, w.before.data(), ss);
      if (!rs.ok()) return rs;
      if (w.before == p.data) continue;  // 変更なし
      w.data = std::move(p.data);
      overlay[w.lba] = w.data;
      es.writes.push_back(std::move(w));
    }
    if (!es.writes.empty()) out->stages.push_back(std::move(es));
  }
  out->sector_size = ss;
  out->device_id = device_id(ss);
  out->before = table;

  if (!out->stages.empty()) {
    OverlayDevice ov(*dev, ss, overlay);
    ParseOptions po;
    po.sector_size = ss;
    po.probe_filesystems = false;
    po.read_labels = false;
    Status st = read_partition_table(ov, &out->after, po);
    if (!st.ok()) return st;
    for (auto& d : out->after.diagnostics) {
      bool known = false;
      for (auto& b : table.diagnostics)
        if (b.code == d.code) known = true;
      if (known || d.severity == Severity::Info) continue;
      out->problems.push_back(D(d.severity, ("after-" + d.code).c_str(), "after applying: " + d.message));
    }
    // 書き込みがパーティションのデータ領域に触れないこと
    auto hits = [&](const PartitionTable& t, uint64_t lba) {
      for (auto& p : t.partitions) {
        if (p.extended_container || p.protective) continue;
        if (t.whole_device_volume && allow_fs_overwrite) continue;
        if (lba >= p.start_lba && lba - p.start_lba < p.sector_count) return true;
      }
      return false;
    };
    for (auto& s : out->stages)
      for (auto& w : s.writes)
        if (hits(out->before, w.lba) || hits(out->after, w.lba)) {
          std::string why = out->before.whole_device_volume ? "the device holds a filesystem without a partition table" : "partition data";
          out->problems.push_back(D(Severity::Error, "write-hits-data", "write at LBA " + std::to_string(w.lba) + " would overwrite " + why));
        }
  }
  return Status::Ok();
}

// ------------------------------------------------------------------ 差分

void PartitionEditor::Impl::describe_changes(EditPlan* out) const {
  const Model& b = base;
  const Model& w = work;
  auto add = [&](std::string s) { out->changes.push_back(std::move(s)); };
  bool new_kind = w.fresh;
  if (new_kind) {
    add(std::string("create a new empty ") + (w.kind == TableKind::Gpt ? "GPT" : "MBR") + " partition table (existing table is discarded)");
    if (table.whole_device_volume) add("WARNING: the device holds a filesystem without a partition table; its boot sector will be overwritten");
  }
  if (w.has_repair) add(std::string("repair GPT: ") + gpt_repair_name(w.repair));
  if (w.kind == TableKind::Gpt && !new_kind && b.kind == TableKind::Gpt && !b.unusable && w.disk_guid != b.disk_guid)
    add("disk GUID " + guid_to_string(b.disk_guid) + " -> " + guid_to_string(w.disk_guid));
  if (w.kind == TableKind::Mbr && !new_kind && b.kind == TableKind::Mbr && w.mbr_sig != b.mbr_sig) {
    char s[64]; std::snprintf(s, sizeof s, "disk signature %08x -> %08x", b.mbr_sig, w.mbr_sig); add(s);
  }
  if (w.kind == TableKind::Gpt && w.geo.alt_lba != b.geo.alt_lba && !new_kind)
    add("backup GPT header moves from LBA " + std::to_string(b.geo.alt_lba) + " to LBA " + std::to_string(w.geo.alt_lba) + "; usable range becomes " +
        std::to_string(w.geo.first_usable) + ".." + std::to_string(w.geo.last_usable));
  if (w.rebuild_mbr) add("rewrite the protective MBR");
  if (!new_kind) {
    for (auto& bp : b.parts) {
      const MPart* wp = nullptr;
      for (auto& p : w.parts)
        if (!p.created && p.handle == bp.handle) wp = &p;
      if (!wp) add("delete partition " + describe_part(bp, b.kind, ss));
    }
  }
  for (auto& p : w.parts) {
    if (p.created) { add("create partition " + describe_part(p, w.kind, ss)); continue; }
    if (new_kind) continue;
    const MPart* bp = find_part(b, p.handle);
    if (!bp) continue;
    std::string d;
    if (p.start != bp->start) d += " start " + std::to_string(bp->start) + "->" + std::to_string(p.start) + ";";
    if (p.count != bp->count) d += " size " + std::to_string(bp->count) + "->" + std::to_string(p.count) + " sectors;";
    if (p.mbr_type != bp->mbr_type) { char s[48]; std::snprintf(s, sizeof s, " type 0x%02X->0x%02X;", bp->mbr_type, p.mbr_type); d += s; }
    if (p.type_guid != bp->type_guid) d += " type GUID " + guid_to_string(bp->type_guid) + "->" + guid_to_string(p.type_guid) + ";";
    if (p.unique_guid != bp->unique_guid) d += " unique GUID changed;";
    if (p.attrs != bp->attrs) d += " attributes " + std::to_string(bp->attrs) + "->" + std::to_string(p.attrs) + ";";
    if (p.name != bp->name) d += " name \"" + bp->name + "\"->\"" + p.name + "\";";
    if (p.bootable != bp->bootable) d += p.bootable ? " set active;" : " clear active;";
    if (!d.empty()) add("modify partition " + std::to_string(p.handle) + ":" + d);
  }
}

// ------------------------------------------------------------------ 計画 (編集)

EditResult PartitionEditor::Impl::build_edit_plan(EditPlan* out) const {
  *out = EditPlan{};
  out->sector_size = ss;
  out->device_id = device_id();
  std::vector<Diagnostic> probs;
  const Model& w = work;
  if (w.kind == TableKind::None) {
    out->problems.push_back(D(Severity::Info, "no-changes", "no partition table and no operations"));
    out->before = table;
    return success();
  }
  validate(w, &probs);
  out->problems = probs;

  if (models_equal(base, w)) {
    out->before = table;
    out->problems.push_back(D(Severity::Info, "no-changes", "the planned table is identical to the one on the device"));
    out->plan_hash = compute_hash(*out);
    return success();
  }

  // セクタ 0 の現状 (ブートコードを残すため)
  std::vector<uint8_t> s0(ss, 0);
  Status rs = dev->read_at(0, s0.data(), ss);
  if (!rs.ok()) { EditResult r; r.status = rs; r.code = "io-error"; r.message = "failed to read sector 0"; return r; }

  std::vector<Planned> pl;
  std::vector<std::pair<std::string, int>> stage_names;
  bool has_error = false;
  for (auto& d : probs) if (d.severity == Severity::Error) has_error = true;

  if (!has_error) {
    if (w.kind == TableKind::Gpt) {
      bool prot = w.fresh || w.rebuild_mbr;
      serialize_gpt(w, ss, N, s0, prot, &pl);
      stage_names = {{"gpt-backup-entries", kOrdBackupEntries}, {"gpt-backup-header", kOrdBackupHeader},
                     {"gpt-primary-entries", kOrdPrimaryEntries}, {"gpt-primary-header", kOrdPrimaryHeader},
                     {"protective-mbr", kOrdMbr}, {"gpt-erase-stale", kOrdStale}};
      // 古いバックアップの位置が変わるとき (移動) は古いヘッダと配列を消す
      if (gi.backup.present && gi.backup.lba != w.geo.alt_lba && gi.backup.lba > 1) {
        pl.push_back({gi.backup.lba, std::vector<uint8_t>(ss, 0), "gpt-erase-stale-header", kOrdStale});
        if (gi.backup.header_valid && gi.backup.entries_lba != w.geo.b_ent_lba && gi.backup.entries_lba != w.geo.p_ent_lba) {
          uint64_t arr = (static_cast<uint64_t>(gi.backup.num_entries) * gi.backup.entry_size + ss - 1) / ss;
          for (uint64_t i = 0; i < arr; i++) {
            uint64_t l = gi.backup.entries_lba + i;
            bool inside_new = (l >= w.geo.b_ent_lba && l < w.geo.b_ent_lba + w.geo.arr_sectors()) ||
                              (l >= w.geo.p_ent_lba && l < w.geo.p_ent_lba + w.geo.arr_sectors());
            if (!inside_new) pl.push_back({l, std::vector<uint8_t>(ss, 0), "gpt-erase-stale-entries", kOrdStale});
          }
        }
      }
    } else {
      std::string why;
      if (!serialize_mbr(w, ss, s0, &pl, &why)) out->problems.push_back(D(Severity::Error, "mbr-serialize", why));
      stage_names = {{"ebr", 0}, {"mbr", 1}, {"gpt-erase-backup-header", 2}, {"gpt-erase-primary-header", 3}};
      if (table.kind == TableKind::Gpt) {  // GPT → MBR: 古い GPT ヘッダを消す (MBR を書いた後)
        if (gi.backup.present && gi.backup.lba > 1) pl.push_back({gi.backup.lba, std::vector<uint8_t>(ss, 0), "gpt-erase-backup-header", 2});
        if (gi.primary.present) pl.push_back({1, std::vector<uint8_t>(ss, 0), "gpt-erase-primary-header", 3});
      }
    }
  }
  Status st = has_error ? Status::Ok() : finish_plan(pl, out, stage_names, w.overwrite_fs, ss, N);
  if (!st.ok()) { EditResult r; r.status = st; r.code = "io-error"; r.message = "I/O error while planning"; return r; }
  if (has_error) { out->before = table; }

  // 内部整合: 適用後の表がモデルと一致するか
  if (!out->stages.empty() && !has_error) {
    PartitionTable expect;
    expect.kind = w.kind;
    std::vector<MPart> ps = w.parts;
    for (auto& p : ps) {
      Partition q;
      q.index = p.handle;
      q.start_lba = p.start; q.sector_count = p.count;
      q.mbr_type = p.mbr_type; q.type_guid = p.type_guid; q.unique_guid = p.unique_guid;
      q.gpt_attributes = p.attrs; q.name = p.name; q.bootable = p.bootable; q.logical = p.logical;
      expect.partitions.push_back(q);
    }
    PartitionTable got = out->after;
    auto key = [&](const Partition& p) { return std::make_pair(p.logical ? 1 : 0, p.start_lba); };
    (void)key;
    // 比較は (start, count, type...) の集合で行う (番号は再割り当てされうる)
    auto norm = [](PartitionTable& t) {
      std::sort(t.partitions.begin(), t.partitions.end(), [](const Partition& a, const Partition& b) {
        return a.start_lba != b.start_lba ? a.start_lba < b.start_lba : a.sector_count < b.sector_count;
      });
      for (auto& p : t.partitions) p.index = 0;
    };
    for (auto& p : got.partitions) if (p.protective) p.protective = false;
    norm(expect); norm(got);
    bool same = expect.partitions.size() == got.partitions.size() && got.kind == w.kind;
    for (size_t i = 0; same && i < got.partitions.size(); i++) {
      const Partition &x = expect.partitions[i], &y = got.partitions[i];
      same = x.start_lba == y.start_lba && x.sector_count == y.sector_count && x.logical == y.logical &&
             (w.kind == TableKind::Gpt ? (x.type_guid == y.type_guid && x.unique_guid == y.unique_guid && x.gpt_attributes == y.gpt_attributes && x.name == y.name)
                                        : (x.mbr_type == y.mbr_type && x.bootable == y.bootable));
    }
    if (!same) out->problems.push_back(D(Severity::Error, "internal-mismatch", "the planned sectors do not parse back to the intended table"));
  }

  describe_changes(out);
  bool has_err = false;
  for (auto& d : out->problems) if (d.severity == Severity::Error) has_err = true;
  // 破壊的か
  out->destructive = w.fresh && (table.kind != TableKind::None || table.whole_device_volume);
  if (!w.fresh) {
    for (auto& bp : base.parts) {
      const MPart* wp = nullptr;
      for (auto& p : w.parts) if (!p.created && p.handle == bp.handle) wp = &p;
      if (!wp || wp->start != bp.start || wp->count < bp.count) out->destructive = true;
    }
  }
  if (out->stages.empty() && !has_err) {
    out->problems.push_back(D(Severity::Info, "no-changes", "the planned table is identical to the one on the device"));
  }
  out->ok = !has_err && !out->stages.empty();
  out->plan_hash = compute_hash(*out);
  out->confirm_phrase = std::string(out->requires_force ? "FORCE " : "WRITE ") + [&] {
    std::string s = out->plan_hash.substr(0, 6);
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
  }();
  return success();
}

// ------------------------------------------------------------------ PartitionEditor (公開)

size_t EditPlan::sector_count() const {
  size_t n = 0;
  for (auto& s : stages) n += s.writes.size();
  return n;
}

EditResult PartitionEditor::open(BlockDevicePtr dev, const EditorOptions& opt, std::unique_ptr<PartitionEditor>* out) {
  if (!dev || !out) return fail(Errc::InvalidArgument, "bad-argument", "null device or output");
  if (opt.sector_size && (opt.sector_size < 512 || opt.sector_size > 65536 || (opt.sector_size & (opt.sector_size - 1))))
    return fail(Errc::InvalidArgument, "bad-sector-size", "sector size must be a power of two between 512 and 65536");
  auto impl = std::make_unique<Impl>();
  impl->dev = dev;
  impl->opt = opt;
  impl->in_use = opt.in_use;
  impl->writable = !dev->is_read_only();
  if (impl->writable) {
    std::lock_guard<std::mutex> l(g_reg_mu);
    if (!g_reg_devs.insert(dev.get()).second) return fail(Errc::InvalidArgument, "busy", "another writable editor is already open on this device");
    impl->registered = true;
  }
  Status st = impl->refresh();
  if (!st.ok()) {
    EditResult r;
    r.status = st;
    r.code = "io-error";
    r.message = "failed to read the partition table";
    return r;
  }
  out->reset(new PartitionEditor(std::move(impl)));
  return success();
}

PartitionEditor::PartitionEditor(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
PartitionEditor::~PartitionEditor() = default;

const PartitionTable& PartitionEditor::table() const { return impl_->table; }
const GptInspection& PartitionEditor::gpt_inspection() const { return impl_->gi; }
bool PartitionEditor::writable() const { return impl_->writable; }
void PartitionEditor::set_in_use(bool v) { std::lock_guard<std::mutex> l(impl_->mu); impl_->in_use = v; }
const std::vector<std::string>& PartitionEditor::op_log() const { return impl_->log; }
void PartitionEditor::clear_ops() {
  std::lock_guard<std::mutex> l(impl_->mu);
  impl_->work = impl_->base;
  impl_->log.clear();
}

#define LOCK std::lock_guard<std::mutex> lk_(impl_->mu)
#define NEED_TABLE { EditResult rr_ = impl_->check_ready(true); if (!rr_.ok()) return rr_; }

EditResult PartitionEditor::create(const PartitionSpec& spec, uint32_t* handle) {
  LOCK;
  NEED_TABLE;
  Model& m = impl_->work;
  MPart c;
  c.created = true;
  c.attrs = spec.attrs;
  c.name = spec.name;
  c.bootable = spec.bootable;
  bool logical = false, extended = false;
  if (m.kind == TableKind::Mbr) {
    PartRole role = spec.role;
    int used = 0;
    std::set<int> slots;
    for (auto& p : m.parts) if (!p.logical) { used++; slots.insert(p.slot); }
    if (role == PartRole::Auto) {
      if (used < 4) role = PartRole::Primary;
      else if (find_ext(m)) role = PartRole::Logical;
      else return fail(Errc::InvalidArgument, "no-slot", "all 4 primary slots are used and there is no extended partition");
    }
    logical = role == PartRole::Logical;
    extended = role == PartRole::Extended;
    c.mbr_type = spec.mbr_type;
    if (extended && !is_ext_type(c.mbr_type)) {
      if (spec.mbr_type == 0x83) c.mbr_type = 0x05;
      else return fail(Errc::InvalidArgument, "mbr-extended-type", "an extended partition needs type 0x05, 0x0F or 0x85");
    }
    if (!extended && is_ext_type(c.mbr_type)) return fail(Errc::InvalidArgument, "mbr-extended-type", "use role Extended to create an extended partition");
    if (c.mbr_type == 0) return fail(Errc::InvalidArgument, "mbr-type-zero", "partition type 0x00 is not allowed");
    if (c.mbr_type == 0xEE) return fail(Errc::InvalidArgument, "mbr-type-protective", "type 0xEE is reserved for GPT");
    if (extended && find_ext(m)) return fail(Errc::InvalidArgument, "mbr-multiple-extended", "an extended partition already exists");
    if (logical && !find_ext(m)) return fail(Errc::InvalidArgument, "no-extended", "create an extended partition first");
    if (!logical) {
      if (used >= 4) return fail(Errc::InvalidArgument, "no-slot", "all 4 primary slots are used");
      for (int i = 0; i < 4; i++) if (!slots.count(i)) { c.slot = i; break; }
      c.handle = static_cast<uint32_t>(c.slot) + 1;
      // 同じ番号の元のパーティションが生きていると衝突する (削除済みなら再利用可)
      if (find_part(m, c.handle)) return fail(Errc::InvalidArgument, "no-slot", "slot already used");
    } else {
      c.handle = m.next_handle++;
    }
  } else {
    c.type_guid = all_zero(spec.type_guid.data(), 16) ? kLinuxFsGuid : spec.type_guid;
    c.unique_guid = all_zero(spec.unique_guid.data(), 16) ? impl_->new_guid() : spec.unique_guid;
    // 空きエントリ
    std::vector<bool> taken(m.geo.num_entries, false);
    for (auto& p : m.parts) if (p.slot >= 0 && static_cast<uint32_t>(p.slot) < taken.size()) taken[p.slot] = true;
    size_t esz = m.geo.entry_size;
    for (uint32_t i = 0; i < m.geo.num_entries && i < taken.size(); i++) {
      if (taken[i]) continue;
      bool freed = false;  // 元の配列で使われていた (= 無効な範囲などで解析から漏れた) スロットは使わない
      for (auto& os : m.orig_slots) if (os.second == static_cast<int>(i)) freed = true;
      if (!freed && (i + 1) * esz <= m.entries.size() && !all_zero(m.entries.data() + i * esz, 16)) continue;
      c.slot = static_cast<int>(i);
      break;
    }
    if (c.slot < 0) return fail(Errc::InvalidArgument, "gpt-no-free-slot", "the GPT entry array is full");
    c.handle = static_cast<uint32_t>(c.slot) + 1;
    if (find_part(m, c.handle)) return fail(Errc::InvalidArgument, "gpt-no-free-slot", "slot already used");
    c.dirty = 0xFF;
    std::vector<uint8_t> u;
    if (!utf8_to_utf16le(c.name, 36, &u)) return fail(Errc::InvalidArgument, "gpt-name-invalid", "name is not valid UTF-8 or longer than 36 UTF-16 units");
  }
  c.logical = logical;
  c.extended = extended;
  uint64_t s = 0, n = 0;
  std::string r = find_space(m, impl_->N, impl_->align, logical, spec.start, spec.count, &s, &n);
  if (!r.empty()) return fail(Errc::InvalidArgument, r.c_str(), "no room for the partition: " + r);
  c.start = s;
  c.count = n;
  r = check_fit(m, impl_->N, c, 0);
  if (!r.empty()) return fail(Errc::InvalidArgument, r.c_str(), "the partition does not fit: " + r);
  m.parts.push_back(c);
  if (handle) *handle = c.handle;
  impl_->log.push_back("create " + describe_part(c, m.kind, impl_->ss));
  return success();
}

EditResult PartitionEditor::remove(uint32_t handle) {
  LOCK;
  NEED_TABLE;
  Model& m = impl_->work;
  MPart* p = find_part(m, handle);
  if (!p) return fail(Errc::InvalidArgument, "no-such-partition", "no such partition");
  if (p->extended)
    for (auto& q : m.parts) if (q.logical) return fail(Errc::InvalidArgument, "extended-not-empty", "delete the logical partitions first");
  impl_->log.push_back("remove #" + std::to_string(handle));
  m.parts.erase(m.parts.begin() + (p - m.parts.data()));
  return success();
}

EditResult PartitionEditor::set_mbr_type(uint32_t handle, uint8_t type) {
  LOCK;
  NEED_TABLE;
  Model& m = impl_->work;
  if (m.kind != TableKind::Mbr) return fail(Errc::InvalidArgument, "wrong-table", "not an MBR table");
  MPart* p = find_part(m, handle);
  if (!p) return fail(Errc::InvalidArgument, "no-such-partition", "no such partition");
  if (type == 0) return fail(Errc::InvalidArgument, "mbr-type-zero", "type 0x00 is not allowed");
  if (type == 0xEE) return fail(Errc::InvalidArgument, "mbr-type-protective", "type 0xEE is reserved for GPT");
  if (p->extended != is_ext_type(type)) return fail(Errc::InvalidArgument, "mbr-extended-type", "cannot change between extended and data types");
  p->mbr_type = type;
  impl_->log.push_back("set-type #" + std::to_string(handle));
  return success();
}

EditResult PartitionEditor::set_gpt_type(uint32_t handle, const std::array<uint8_t, 16>& guid) {
  LOCK;
  NEED_TABLE;
  Model& m = impl_->work;
  if (m.kind != TableKind::Gpt) return fail(Errc::InvalidArgument, "wrong-table", "not a GPT table");
  MPart* p = find_part(m, handle);
  if (!p) return fail(Errc::InvalidArgument, "no-such-partition", "no such partition");
  if (all_zero(guid.data(), 16)) return fail(Errc::InvalidArgument, "gpt-type-zero", "the all-zero type GUID marks an unused entry");
  p->type_guid = guid;
  p->dirty |= kDType;
  impl_->log.push_back("set-type #" + std::to_string(handle));
  return success();
}

EditResult PartitionEditor::set_name(uint32_t handle, const std::string& utf8) {
  LOCK;
  NEED_TABLE;
  Model& m = impl_->work;
  if (m.kind != TableKind::Gpt) return fail(Errc::InvalidArgument, "wrong-table", "not a GPT table");
  MPart* p = find_part(m, handle);
  if (!p) return fail(Errc::InvalidArgument, "no-such-partition", "no such partition");
  std::vector<uint8_t> u;
  if (!utf8_to_utf16le(utf8, 36, &u)) return fail(Errc::InvalidArgument, "gpt-name-invalid", "name is not valid UTF-8 or longer than 36 UTF-16 units");
  p->name = utf8;
  p->dirty |= kDName;
  impl_->log.push_back("set-name #" + std::to_string(handle));
  return success();
}

EditResult PartitionEditor::set_attrs(uint32_t handle, uint64_t attrs) {
  LOCK;
  NEED_TABLE;
  Model& m = impl_->work;
  if (m.kind != TableKind::Gpt) return fail(Errc::InvalidArgument, "wrong-table", "not a GPT table");
  MPart* p = find_part(m, handle);
  if (!p) return fail(Errc::InvalidArgument, "no-such-partition", "no such partition");
  p->attrs = attrs;
  p->dirty |= kDAttrs;
  impl_->log.push_back("set-attrs #" + std::to_string(handle));
  return success();
}

EditResult PartitionEditor::set_bootable(uint32_t handle, bool bootable) {
  LOCK;
  NEED_TABLE;
  Model& m = impl_->work;
  if (m.kind != TableKind::Mbr) return fail(Errc::InvalidArgument, "wrong-table", "not an MBR table");
  MPart* p = find_part(m, handle);
  if (!p) return fail(Errc::InvalidArgument, "no-such-partition", "no such partition");
  if (p->extended) return fail(Errc::InvalidArgument, "mbr-extended-active", "an extended partition cannot be active");
  p->bootable = bootable;
  impl_->log.push_back("set-bootable #" + std::to_string(handle));
  return success();
}

EditResult PartitionEditor::set_unique_guid(uint32_t handle, const std::array<uint8_t, 16>& guid) {
  LOCK;
  NEED_TABLE;
  Model& m = impl_->work;
  if (m.kind != TableKind::Gpt) return fail(Errc::InvalidArgument, "wrong-table", "not a GPT table");
  MPart* p = find_part(m, handle);
  if (!p) return fail(Errc::InvalidArgument, "no-such-partition", "no such partition");
  if (all_zero(guid.data(), 16)) return fail(Errc::InvalidArgument, "gpt-unique-zero", "the unique GUID must not be all zero");
  for (auto& q : m.parts) if (q.handle != handle && q.unique_guid == guid) return fail(Errc::InvalidArgument, "gpt-unique-duplicate", "another partition already has this GUID");
  p->unique_guid = guid;
  p->dirty |= kDUid;
  impl_->log.push_back("set-unique-guid #" + std::to_string(handle));
  return success();
}

EditResult PartitionEditor::resize(uint32_t handle, uint64_t new_count) {
  LOCK;
  NEED_TABLE;
  Model& m = impl_->work;
  MPart* p = find_part(m, handle);
  if (!p) return fail(Errc::InvalidArgument, "no-such-partition", "no such partition");
  MPart c = *p;
  c.count = new_count;
  std::string r = check_fit(m, impl_->N, c, handle);
  if (!r.empty()) return fail(Errc::InvalidArgument, r.c_str(), "cannot resize: " + r);
  p->count = new_count;
  p->dirty |= kDRange;
  impl_->log.push_back("resize #" + std::to_string(handle));
  return success();
}

EditResult PartitionEditor::move(uint32_t handle, uint64_t new_start) {
  LOCK;
  NEED_TABLE;
  Model& m = impl_->work;
  MPart* p = find_part(m, handle);
  if (!p) return fail(Errc::InvalidArgument, "no-such-partition", "no such partition");
  MPart c = *p;
  c.start = new_start;
  std::string r = check_fit(m, impl_->N, c, handle);
  if (!r.empty()) return fail(Errc::InvalidArgument, r.c_str(), "cannot move: " + r);
  p->start = new_start;
  p->dirty |= kDRange;
  impl_->log.push_back("move #" + std::to_string(handle));
  return success();
}

EditResult PartitionEditor::set_disk_id(const std::string& id) {
  LOCK;
  NEED_TABLE;
  Model& m = impl_->work;
  if (m.kind == TableKind::Gpt) {
    std::array<uint8_t, 16> g;
    if (!parse_guid(id, &g)) return fail(Errc::InvalidArgument, "bad-guid", "not a GUID");
    if (all_zero(g.data(), 16)) return fail(Errc::InvalidArgument, "gpt-disk-guid-zero", "the disk GUID must not be all zero");
    m.disk_guid = g;
  } else {
    std::string s = id;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
    if (s.empty() || s.size() > 8) return fail(Errc::InvalidArgument, "bad-signature", "expected up to 8 hex digits");
    uint32_t v = 0;
    for (char ch : s) {
      int d = (ch >= '0' && ch <= '9') ? ch - '0' : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10 : (ch >= 'A' && ch <= 'F') ? ch - 'A' + 10 : -1;
      if (d < 0) return fail(Errc::InvalidArgument, "bad-signature", "expected hex digits");
      v = v * 16 + static_cast<uint32_t>(d);
    }
    m.mbr_sig = v;
  }
  impl_->log.push_back("set-disk-id");
  return success();
}

EditResult PartitionEditor::new_table(TableKind kind, uint32_t gpt_entries, bool overwrite_fs) {
  LOCK;
  if (kind != TableKind::Gpt && kind != TableKind::Mbr) return fail(Errc::InvalidArgument, "bad-kind", "kind must be GPT or MBR");
  Model m;
  m.kind = kind;
  m.fresh = true;
  m.overwrite_fs = overwrite_fs;
  if (kind == TableKind::Gpt) {
    if (gpt_entries < 128 || gpt_entries > kMaxGptEntries) return fail(Errc::InvalidArgument, "gpt-entries", "entry count must be 128..8192");
    if (!GptGeo::standard(impl_->ss, impl_->N, gpt_entries, 128, &m.geo)) return fail(Errc::InvalidArgument, "disk-too-small", "the disk is too small for a GPT");
    m.entries.assign(static_cast<size_t>(gpt_entries) * 128, 0);
    m.disk_guid = impl_->new_guid();
  } else {
    if (impl_->N < 2) return fail(Errc::InvalidArgument, "disk-too-small", "the disk is too small for an MBR");
    auto g = impl_->new_guid();
    m.mbr_sig = le32(g.data());
    if (m.mbr_sig == 0) m.mbr_sig = 1;
  }
  impl_->work = std::move(m);
  impl_->log.clear();
  impl_->log.push_back(std::string("new-table ") + (kind == TableKind::Gpt ? "gpt" : "mbr"));
  return success();
}

EditResult PartitionEditor::repair_gpt(GptRepairKind kind) {
  LOCK;
  Impl& I = *impl_;
  if (!I.log.empty()) return fail(Errc::InvalidArgument, "repair-not-first", "a GPT repair must be the first operation");
  if (I.base.kind != TableKind::Gpt && !(I.gi.any_present())) return fail(Errc::InvalidArgument, "wrong-table", "the device has no GPT");
  const GptInspection& g = I.gi;
  Model m;
  switch (kind) {
    case GptRepairKind::RebuildProtectiveMbr:
      if (!g.primary.usable() && !g.backup.usable()) return fail(Errc::InvalidArgument, "gpt-unusable", "no usable GPT copy");
      m = I.base;
      m.rebuild_mbr = true;
      break;
    case GptRepairKind::CopyPrimaryToBackup:
      if (!g.primary.usable()) return fail(Errc::InvalidArgument, "gpt-primary-unusable", "the primary GPT is not usable");
      m = I.model_from_copy(g.primary, true);
      break;
    case GptRepairKind::CopyBackupToPrimary:
      if (!g.backup.usable()) return fail(Errc::InvalidArgument, "gpt-backup-unusable", "the backup GPT is not usable");
      m = I.model_from_copy(g.backup, false);
      break;
    case GptRepairKind::MoveBackupToEnd: {
      const GptCopyInfo* src = g.primary.usable() ? &g.primary : (g.primary.truncated && g.primary.entries_valid) ? &g.primary : g.backup.usable() ? &g.backup : nullptr;
      if (!src) return fail(Errc::InvalidArgument, "gpt-unusable", "no usable GPT copy");
      m = I.model_from_copy(*src, src == &g.primary);
      uint64_t arr = m.geo.arr_sectors();
      if (I.N < 2 * arr + 5) return fail(Errc::InvalidArgument, "disk-too-small", "the disk is too small");
      m.geo.alt_lba = I.N - 1;
      m.geo.b_ent_lba = I.N - 1 - arr;
      m.geo.last_usable = m.geo.b_ent_lba - 1;
      if (!g.mbr_protective_ok && !g.mbr_hybrid) m.rebuild_mbr = true;
      break;
    }
    default: return fail(Errc::InvalidArgument, "bad-repair", "unknown repair");
  }
  // パーティションは「選んだコピー」の内容から作り直す
  if (kind != GptRepairKind::RebuildProtectiveMbr) {
    m.parts.clear();
    m.orig_slots.clear();
    const GptCopyInfo& c = (kind == GptRepairKind::CopyBackupToPrimary) ? g.backup
                           : (kind == GptRepairKind::CopyPrimaryToBackup) ? g.primary
                                                                           : ((g.primary.usable() || g.primary.truncated) ? g.primary : g.backup);
    for (uint32_t i = 0; i < c.num_entries; i++) {
      const uint8_t* e = c.entries.data() + static_cast<size_t>(i) * c.entry_size;
      if (all_zero(e, 16)) continue;
      uint64_t a = le64(e + 32), b = le64(e + 40);
      MPart q;
      q.handle = i + 1;
      q.slot = static_cast<int>(i);
      q.start = a;
      q.count = a <= b && b - a < UINT64_MAX ? b - a + 1 : 0;  // 壊れた範囲は 0 にして検証で弾く (remove で消せる)
      std::memcpy(q.type_guid.data(), e, 16);
      std::memcpy(q.unique_guid.data(), e + 16, 16);
      q.attrs = le64(e + 48);
      q.name = utf16le_to_utf8(e + 56, 36);
      m.parts.push_back(q);
      m.orig_slots.push_back({q.handle, q.slot});
    }
  }
  m.has_repair = true;
  m.repair = kind;
  I.work = std::move(m);
  I.log.push_back(std::string("repair ") + gpt_repair_name(kind));
  return success();
}

EditResult PartitionEditor::plan(EditPlan* out) const {
  if (!out) return fail(Errc::InvalidArgument, "bad-argument", "null plan");
  std::lock_guard<std::mutex> l(impl_->mu);
  return impl_->build_edit_plan(out);
}

std::string PartitionEditor::make_confirm_token(const EditPlan& plan, const std::string& typed) {
  Sha256 h;
  std::string head = "usbmem-confirm-v1";
  h.update(head.data(), head.size() + 1);
  h.update(plan.plan_hash.data(), plan.plan_hash.size() + 1);
  h.update(plan.device_id.data(), plan.device_id.size() + 1);
  h.update(typed.data(), typed.size());
  return hex_encode(h.finish());
}

EditResult PartitionEditor::apply(const EditPlan& plan, const std::string& token, const ApplyOptions& ao) {
  std::lock_guard<std::mutex> l(impl_->mu);
  Impl& I = *impl_;
  if (!I.writable) return fail(Errc::ReadOnly, "device-read-only", "the device is opened read-only");
  if (I.in_use) return fail(Errc::InvalidArgument, "in-use", "the device is in use (mounted); unmount it before editing the partition table");
  if (!plan.ok) return fail(Errc::InvalidArgument, "plan-invalid", "the plan has errors or no changes");
  // 計画の内容から計算し直す (構造体が改ざんされていても検出する)
  std::string h = I.compute_hash(plan);
  if (h != plan.plan_hash) return fail(Errc::InvalidArgument, "plan-tampered", "the plan was modified after it was created");
  EditPlan canon = plan;
  std::string expect_phrase = std::string(plan.requires_force ? "FORCE " : "WRITE ") + [&] {
    std::string s = h.substr(0, 6);
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
  }();
  canon.confirm_phrase = expect_phrase;
  if (token != make_confirm_token(canon, expect_phrase)) return fail(Errc::InvalidArgument, "bad-token", "the confirmation token does not match this plan");
  const uint32_t pss = plan.sector_size;
  if (pss < 512 || pss > 65536 || (pss & (pss - 1))) return fail(Errc::InvalidArgument, "plan-invalid", "bad sector size in the plan");
  uint64_t pN = I.dev->size() / pss;
  if (plan.device_id.compare(0, I.dev->describe().size(), I.dev->describe()) != 0 ||
      plan.device_id.find("|bytes=" + std::to_string(I.dev->size())) == std::string::npos)
    return fail(Errc::InvalidArgument, "wrong-device", "the plan was made for a different device");
  for (auto& st : plan.stages)
    for (auto& w : st.writes)
      if (w.lba >= pN || w.data.size() != pss || w.before.size() != pss) return fail(Errc::InvalidArgument, "plan-invalid", "malformed write in the plan");

  // ディスクの内容が計画時のままか
  {
    std::vector<uint8_t> cur(pss);
    for (auto& st : plan.stages)
      for (auto& w : st.writes) {
        Status rs = I.dev->read_at(w.lba * pss, cur.data(), pss);
        if (!rs.ok()) { EditResult r; r.status = rs; r.code = "io-error"; r.message = "failed to re-read a target sector"; return r; }
        if (cur != w.before) return fail(Errc::InvalidArgument, "device-changed", "the device changed after the plan was made; create a new plan");
      }
  }

  // 自動バックアップ (書き込み前に必ず)
  if (ao.require_backup && !ao.backup_sink) return fail(Errc::InvalidArgument, "backup-required", "a backup sink is required before writing");
  if (ao.backup_sink) {
    BackupFile bf;
    bf.sector_size = pss;
    bf.disk_bytes = I.dev->size();
    bf.timestamp = ao.timestamp;
    bf.table_kind = I.table.kind == TableKind::Gpt ? 2 : I.table.kind == TableKind::Mbr ? 1 : 0;
    if (I.table.kind == TableKind::Gpt) { std::array<uint8_t, 16> g{}; parse_guid(I.table.disk_guid, &g); bf.disk_id = g; }
    else if (I.table.kind == TableKind::Mbr) put32(bf.disk_id.data(), I.table.mbr_disk_signature);
    std::vector<TableSector> ts;
    GptInspection gi2;
    Status rs = inspect_gpt(*I.dev, pss, &gi2);
    if (rs.ok()) rs = collect_table_sectors(*I.dev, pss, gi2, &ts);
    if (!rs.ok()) { EditResult r; r.status = rs; r.code = "io-error"; r.message = "failed to collect the current table for the backup"; return r; }
    std::map<uint64_t, TableSector> merged;
    for (auto& t : ts) merged[t.lba] = t;
    for (auto& st : plan.stages)
      for (auto& w : st.writes)
        if (!merged.count(w.lba)) merged[w.lba] = TableSector{w.lba, kind_of_what(w.what), w.before};
    for (auto& kv : merged) bf.sectors.push_back(kv.second);
    Status bs = ao.backup_sink(serialize_backup(bf));
    if (!bs.ok()) { EditResult r; r.status = bs; r.code = "backup-failed"; r.message = "the backup could not be saved; nothing was written"; return r; }
  }

  // 書き込み (段階ごとに flush)
  EditResult res;
  std::vector<const SectorWrite*> done;
  Status fs = Status::Ok();
  const char* fcode = "write-failed";
  const SectorWrite* failed_at = nullptr;
  for (auto& st : plan.stages) {
    for (auto& w : st.writes) {
      Status ws = I.dev->write_at(w.lba * pss, w.data.data(), pss);
      if (!ws.ok()) { fs = ws; failed_at = &w; break; }
      done.push_back(&w);
      res.sectors_written++;
    }
    if (!fs.ok()) break;
    Status fl = I.dev->flush();
    if (!fl.ok()) { fs = fl; fcode = "flush-failed"; break; }
  }
  if (!fs.ok()) {
    res.status = fs;
    res.code = fcode;
    res.message = std::string("write failed in stage; ") + (ao.rollback_on_error ? "rolling back" : "not rolled back");
    if (ao.rollback_on_error) {
      bool all = true;
      std::vector<const SectorWrite*> undo(done.rbegin(), done.rend());
      if (failed_at) undo.insert(undo.begin(), failed_at);  // 部分的に書かれた可能性があるので戻す
      for (auto* w : undo) {
        Status us = I.dev->write_at(w->lba * pss, w->before.data(), pss);
        if (!us.ok()) all = false;
      }
      if (!I.dev->flush().ok()) all = false;
      res.rolled_back = all;
      res.message += all ? ": restored the original sectors" : ": ROLLBACK INCOMPLETE; restore the backup";
    }
    I.refresh();
    return res;
  }

  // 検証: 読み戻し + 再パース
  {
    std::vector<uint8_t> cur(pss);
    for (auto& st : plan.stages)
      for (auto& w : st.writes) {
        Status rs = I.dev->read_at(w.lba * pss, cur.data(), pss);
        if (!rs.ok() || cur != w.data) {
          res.status = rs.ok() ? Status::Make(Errc::Io) : rs;
          res.code = "verify-failed";
          res.message = "read-back differs at LBA " + std::to_string(w.lba);
          I.refresh();
          return res;
        }
      }
    PartitionTable t;
    ParseOptions po;
    po.sector_size = pss;
    po.probe_filesystems = false;
    po.read_labels = false;
    Status ps = read_partition_table(*I.dev, &t, po);
    if (!ps.ok()) { res.status = ps; res.code = "verify-failed"; res.message = "could not re-read the table"; I.refresh(); return res; }
    if (!plan_tables_equal(plan.after, t, &res.verify_problems)) {
      res.status = Status::Make(Errc::Io);
      res.code = "verify-failed";
      res.message = "the table read back differs from the plan";
      I.refresh();
      return res;
    }
  }
  res.verified = true;
  res.code = "ok";
  Status rf = I.refresh();
  if (!rf.ok()) { res.status = rf; res.code = "io-error"; res.message = "applied, but failed to re-read the table"; }
  return res;
}

// ------------------------------------------------------------------ バックアップ / 復元 / sfdisk

EditResult PartitionEditor::backup(uint64_t timestamp, std::vector<uint8_t>* out) const {
  if (!out) return fail(Errc::InvalidArgument, "bad-argument", "null output");
  std::lock_guard<std::mutex> l(impl_->mu);
  Impl& I = *impl_;
  BackupFile bf;
  bf.sector_size = I.ss;
  bf.disk_bytes = I.dev->size();
  bf.timestamp = timestamp;
  bf.table_kind = I.table.kind == TableKind::Gpt ? 2 : I.table.kind == TableKind::Mbr ? 1 : 0;
  if (I.table.kind == TableKind::Gpt) { std::array<uint8_t, 16> g{}; parse_guid(I.table.disk_guid, &g); bf.disk_id = g; }
  else if (I.table.kind == TableKind::Mbr) put32(bf.disk_id.data(), I.table.mbr_disk_signature);
  Status st = collect_table_sectors(*I.dev, I.ss, I.gi, &bf.sectors);
  if (!st.ok()) { EditResult r; r.status = st; r.code = "io-error"; r.message = "failed to read the table sectors"; return r; }
  *out = serialize_backup(bf);
  return success();
}

EditResult PartitionEditor::export_sfdisk(std::string* out, const std::string& device_name) const {
  if (!out) return fail(Errc::InvalidArgument, "bad-argument", "null output");
  std::lock_guard<std::mutex> l(impl_->mu);
  if (impl_->table.kind == TableKind::None) return fail(Errc::InvalidArgument, "no-table", "the device has no partition table");
  *out = sfdisk_dump(impl_->table, impl_->gi, device_name);
  return success();
}

EditResult PartitionEditor::plan_restore(const std::vector<uint8_t>& file, EditPlan* out) const {
  if (!out) return fail(Errc::InvalidArgument, "bad-argument", "null plan");
  std::lock_guard<std::mutex> l(impl_->mu);
  Impl& I = *impl_;
  *out = EditPlan{};
  BackupFile bf;
  Status ps = parse_backup(file, &bf);
  if (!ps.ok()) return fail(ps.code, ps.code == Errc::Unsupported ? "backup-version" : "backup-corrupt", "the backup file is damaged or not a backup of this program");

  const uint32_t bss = bf.sector_size;
  const uint64_t bN = I.dev->size() / bss;
  std::vector<Diagnostic>& pr = out->problems;
  if (bss != I.ss) {
    out->requires_force = true;
    pr.push_back(D(Severity::Warning, "restore-sector-size-mismatch", "the backup was taken with " + std::to_string(bss) + "-byte sectors but the device is read with " + std::to_string(I.ss)));
  }
  if (bf.disk_bytes != I.dev->size()) {
    out->requires_force = true;
    pr.push_back(D(Severity::Warning, "restore-disk-size-mismatch", "the backup was taken from a disk of " + std::to_string(bf.disk_bytes) + " bytes; this device has " + std::to_string(I.dev->size())));
  }
  {
    std::array<uint8_t, 16> cur{};
    if (I.table.kind == TableKind::Gpt) parse_guid(I.table.disk_guid, &cur);
    else if (I.table.kind == TableKind::Mbr) put32(cur.data(), I.table.mbr_disk_signature);
    uint32_t cur_kind = I.table.kind == TableKind::Gpt ? 2 : I.table.kind == TableKind::Mbr ? 1 : 0;
    size_t cmp = bf.table_kind == 2 ? 16 : 4;
    if (cur_kind != bf.table_kind || (bf.table_kind != 0 && std::memcmp(cur.data(), bf.disk_id.data(), cmp) != 0))
      pr.push_back(D(Severity::Warning, "restore-disk-id-mismatch", "the backup comes from a different table identity (disk GUID / signature) than the current table"));
  }
  for (auto& s : bf.sectors)
    if (s.lba >= bN) pr.push_back(D(Severity::Error, "backup-sector-out-of-range", "the backup contains LBA " + std::to_string(s.lba) + " which is outside this device"));

  std::vector<Planned> pl;
  auto ord = [&](SectorKind k) -> int {
    switch (k) {
      case kOther: return 0;
      case kGptBackupEntries: return 1;
      case kGptBackupHeader: return 2;
      case kGptPrimaryEntries: return 3;
      case kGptPrimaryHeader: return 4;
      case kEbr: return 5;
      case kMbr: return 6;
    }
    return 0;
  };
  static const char* names[] = {"other", "gpt-backup-entries", "gpt-backup-header", "gpt-primary-entries", "gpt-primary-header", "ebr", "mbr"};
  static const char* whats[] = {"other", "gpt-backup-entries", "gpt-backup-header", "gpt-primary-entries", "gpt-primary-header", "ebr", "mbr"};
  std::set<uint64_t> restored;
  for (auto& s : bf.sectors) {
    int o = ord(s.kind);
    pl.push_back({s.lba, s.data, whats[o], o});
    restored.insert(s.lba);
  }
  // 現在のディスクに GPT があり、復元後は GPT でない / 別の位置のとき、古いヘッダを消す
  {
    GptInspection cur;
    Status gs = inspect_gpt(*I.dev, bss, &cur);
    if (!gs.ok()) { EditResult r; r.status = gs; r.code = "io-error"; r.message = "I/O error"; return r; }
    std::vector<std::pair<uint64_t, const char*>> stale;
    if (cur.primary.present && !restored.count(1)) stale.push_back({1, "gpt-erase-primary-header"});
    if (cur.backup.present && cur.backup.lba > 1 && !restored.count(cur.backup.lba)) stale.push_back({cur.backup.lba, "gpt-erase-backup-header"});
    for (auto& s : stale) pl.push_back({s.first, std::vector<uint8_t>(bss, 0), s.second, 7});
  }
  std::vector<std::pair<std::string, int>> stage_names;
  for (int i = 0; i < 7; i++) stage_names.push_back({names[i], i});
  stage_names.push_back({"gpt-erase-stale", 7});

  Status fst = I.finish_plan(pl, out, stage_names, true, bss, bN);
  if (!fst.ok()) { EditResult r; r.status = fst; r.code = "io-error"; r.message = "I/O error while planning the restore"; return r; }
  out->sector_size = bss;
  out->destructive = true;
  out->changes.push_back("restore the partition table from a backup (" + std::to_string(bf.sectors.size()) + " sectors; " +
                         (bf.table_kind == 2 ? "GPT" : bf.table_kind == 1 ? "MBR" : "no table") + ")");
  bool has_err = false;
  for (auto& d : out->problems) if (d.severity == Severity::Error) has_err = true;
  if (out->stages.empty() && !has_err) out->problems.push_back(D(Severity::Info, "no-changes", "the device already holds this table"));
  out->ok = !has_err && !out->stages.empty();
  out->plan_hash = I.compute_hash(*out);
  std::string ph = out->plan_hash.substr(0, 6);
  for (auto& c : ph) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  out->confirm_phrase = std::string(out->requires_force ? "FORCE " : "WRITE ") + ph;
  return success();
}

}  // namespace usbmem
