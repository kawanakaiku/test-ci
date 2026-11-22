// SPDX-License-Identifier: Apache-2.0
// パーティション探索の走査部。docs/partition-scan.md
//
// Quick: 典型的な位置 (先頭の数十セクタ・1 MiB 境界・旧 CHS 境界・既存の表のエントリの前後) と、見つけた FS の直後を調べる。
// Full:  すべてのセクタを 1 回読む。どちらも、読んだ 512 バイトごとにシグネチャ (アンカー) を探し、見つかった位置を partition_scan_detect.cpp で検証する。
#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <tuple>

#include "partition_scan_internal.h"

namespace usbmem {

const char* scan_relation_name(ScanRelation r) {
  switch (r) {
    case ScanRelation::NotInTable: return "not-in-table";
    case ScanRelation::Match: return "match";
    case ScanRelation::Overlap: return "overlap";
    case ScanRelation::Contained: return "contained";
  }
  return "not-in-table";
}

namespace {

using namespace ps;

constexpr uint64_t kMiB = 1ull << 20;
constexpr uint64_t kFullBlock = 4 * kMiB;

uint64_t align_up(uint64_t v, uint64_t a) { return a ? (v + a - 1) / a * a : v; }

struct Scanner {
  BlockDevice& dev;
  const ScanOptions& opt;
  Reader rd;
  uint32_t ss;
  uint64_t lo, hi;
  std::set<std::tuple<uint64_t, int, uint64_t>> tried;
  std::map<std::pair<uint64_t, int>, Hit> best;
  std::vector<uint64_t> new_ends;  // 今回見つかった FS の終わり (Quick の追跡用)

  Scanner(BlockDevice& d, const ScanOptions& o, uint32_t sector, uint64_t l, uint64_t h) : dev(d), opt(o), rd(d, o.cancel), ss(sector), lo(l), hi(h) {}

  void chunk(uint64_t addr, const uint8_t* c) {
    std::vector<Hyp> hs;
    match_anchors(c, addr, ss, &hs);
    for (const Hyp& y : hs) {
      if (y.start < lo || y.start >= hi) continue;
      if (!tried.insert({y.start, static_cast<int>(y.kind), y.aux}).second) continue;
      Hit hit;
      if (!verify(rd, y, ss, &hit)) continue;
      auto key = std::make_pair(hit.start, family_of(hit.fs));
      auto it = best.find(key);
      if (it == best.end()) {
        if (hit.size_known && hit.size) new_ends.push_back(hit.start + hit.size);
        best.emplace(key, std::move(hit));
      } else if (hit.score > it->second.score) {
        it->second = std::move(hit);
      }
    }
  }

  // addr から n 個の 512 バイトチャンクを読んで処理する (読めなければ何もしない)
  void read_chunks(uint64_t addr, unsigned n) {
    if (addr < lo || addr >= hi) return;
    uint8_t buf[2048];
    if (n > 4) n = 4;
    if (!rd.read(addr, buf, 512ull * n)) {
      if (n == 1) return;
      for (unsigned i = 0; i < n; i++) read_chunks(addr + 512ull * i, 1);  // 一部だけ読めないとき
      return;
    }
    for (unsigned i = 0; i < n; i++) chunk(addr + 512ull * i, buf + 512 * i);
  }

  // Quick の 1 位置: 先頭 2 KiB (ブートセクタ・ext/erofs/f2fs のスーパーブロック)、ISO / btrfs の位置、直前のセクタ (NTFS のバックアップ)。
  // deep: 先頭が壊れた FS を見つけるためにバックアップの位置も読む。
  void probe_position(uint64_t p, bool deep) {
    read_chunks(p, 4);
    read_chunks(p + 0x8000, 1);
    read_chunks(p + 0x8800, 1);
    read_chunks(p + 0x10000, 1);
    if (p >= ss) read_chunks(p - ss, 1);
    bool found_here = false;
    for (auto it = best.lower_bound({p, 0}); it != best.end() && it->first.first == p; ++it) found_here = true;
    if (!deep || found_here) return;
    // FAT32 (セクタ 6) / exFAT (12) のバックアップ、ext のグループ 1 の予備スーパーブロック、F2FS の 2 つ目、btrfs のミラー、LUKS2 の 2 つ目のヘッダ
    static const uint32_t secs[] = {6, 12};
    for (uint32_t k : secs) {
      read_chunks(p + 512ull * k, 1);
      if (ss != 512) read_chunks(p + static_cast<uint64_t>(ss) * k, 1);
    }
    for (uint64_t bs = 1024; bs <= 4096; bs <<= 1) read_chunks(p + (8 * bs + (bs == 1024 ? 1 : 0)) * bs, 1);
    read_chunks(p + 4096 + 1024, 1);
    read_chunks(p + 0x4000000, 1);
    for (uint64_t h = 16384; h <= 65536; h <<= 1) read_chunks(p + h, 1);
  }
};

void add_cylinder_seeds(std::set<uint64_t>* pos, uint32_t ss, uint64_t lo, uint64_t hi, uint64_t limit) {
  if (ss != 512) return;
  for (uint64_t k = 0; k < limit; k++) {
    for (uint64_t s : {k * 16065, k * 16065 + 63}) {
      uint64_t b = s * 512;
      if (b >= lo && b < hi) pos->insert(b);
    }
  }
}

void add_table_seeds(const ScanResult& r, uint32_t ss, uint64_t lo, uint64_t hi, std::set<uint64_t>* pos) {
  auto add = [&](uint64_t lba) {
    uint64_t b = lba * ss;
    if (lba < (1ull << 52) && b >= lo && b < hi) pos->insert(b);
  };
  for (const auto& p : r.table.partitions) {
    add(p.start_lba);
    add(p.start_lba + p.sector_count);
    if (p.start_lba) add(p.start_lba - 1);
  }
  for (const GptCopyInfo* c : {&r.gpt.primary, &r.gpt.backup}) {
    if (!c->entries_valid || !c->entry_size || c->entry_size < 128) continue;
    for (size_t o = 0; o + c->entry_size <= c->entries.size() && o < (1u << 20); o += c->entry_size) {
      const uint8_t* e = c->entries.data() + o;
      bool used = false;
      for (int i = 0; i < 16; i++) used = used || e[i];
      if (!used) continue;
      uint64_t first = le64(e + 32), last = le64(e + 40);
      add(first);
      if (last < (1ull << 52)) add(last + 1);
    }
  }
}

void add_follow_ups(uint64_t end, uint32_t ss, uint64_t lo, uint64_t hi, std::map<uint64_t, bool>* pending, const std::set<uint64_t>& done) {
  auto add = [&](uint64_t b) {
    if (b % ss || b < lo || b >= hi || done.count(b)) return;
    (*pending)[b] = true;
  };
  add(end);
  add(align_up(end, kMiB));
  add(align_up(end, kMiB) + 1ull * ss);
  for (uint64_t i = 1; i <= 64; i++) add(end + i * ss);  // 論理パーティションの前の EBR・数セクタの隙間
  uint64_t cyl = 16065ull * 512;
  uint64_t c = align_up(end, cyl);
  add(c);
  add(c + 63 * 512);
}

void progress(const ScanOptions& o, const char* phase, uint64_t done, uint64_t total) {
  if (o.progress) o.progress(phase, done, total);
}

}  // namespace

Status scan_partitions(BlockDevice& dev, const ScanOptions& opt, ScanResult* out) {
  if (!out) return Status::Make(Errc::InvalidArgument);
  *out = ScanResult{};
  ScanResult& res = *out;
  res.level = opt.level;
  res.disk_bytes = dev.size();
  uint32_t ss = opt.sector_size ? opt.sector_size : dev.logical_block_size();
  if (ss < 512 || ss > 65536 || !is_pow2(ss)) ss = 512;
  if (opt.sector_size && (opt.sector_size < 512 || opt.sector_size > 65536 || !is_pow2(opt.sector_size))) return Status::Make(Errc::InvalidArgument);

  progress(opt, "table", 0, 1);
  // 既存の表と GPT の状態 (probe なし)
  {
    ParseOptions po;
    po.sector_size = opt.sector_size;
    po.probe_filesystems = false;
    po.read_labels = false;
    Status st = read_partition_table(dev, &res.table, po);
    if (!st.ok()) res.table = PartitionTable{};  // 読めない / 不正: 表なしで続ける (読めないセクタは走査でも数える)
    if (res.table.kind != TableKind::None && !opt.sector_size) ss = res.table.sector_size;
    Status gs = inspect_gpt(dev, ss, &res.gpt);
    if (!gs.ok()) res.gpt = GptInspection{};
  }
  res.sector_size = ss;
  if (opt.cancel && opt.cancel->cancelled()) { res.cancelled = true; return Status::Make(Errc::Cancelled); }

  uint64_t lo = opt.range_start, hi = opt.range_end ? std::min(opt.range_end, res.disk_bytes) : res.disk_bytes;
  lo = lo / 512 * 512;
  if (lo >= hi) return Status::Ok();
  Scanner sc(dev, opt, ss, lo, hi);

  if (opt.level == ScanLevel::Full) {
    uint64_t total = hi - lo, done = 0;
    std::vector<uint8_t> buf(kFullBlock);
    for (uint64_t a = lo; a < hi && !sc.rd.stopped(); a += kFullBlock) {
      uint64_t n = std::min<uint64_t>(kFullBlock, hi - a) / 512 * 512;
      if (n == 0) break;
      if (sc.rd.read(a, buf.data(), n)) {
        for (uint64_t o = 0; o < n; o += 512) sc.chunk(a + o, buf.data() + o);
      } else if (!sc.rd.stopped()) {
        for (uint64_t o = 0; o < n && !sc.rd.stopped(); o += 512) sc.read_chunks(a + o, 1);  // 読めない塊は 512 バイトずつ読み直し、読めない所だけ飛ばす
      }
      done += n;
      res.positions_probed += n / 512;
      progress(opt, "scan", done, total);
    }
  } else {
    std::set<uint64_t> base;
    for (uint64_t s = 0; s < 128; s++) {
      uint64_t b = s * ss;
      if (b >= lo && b < hi) base.insert(b);
    }
    // 先頭の 1 MiB 境界から max_positions 個
    for (uint64_t b = align_up(lo, kMiB), n = 0; b < hi && n < opt.max_positions; b += kMiB, n++) base.insert(b);
    add_cylinder_seeds(&base, ss, lo, hi, std::min<uint64_t>(2048, opt.max_positions / 4 + 1));
    std::set<uint64_t> table_seeds;
    add_table_seeds(res, ss, lo, hi, &table_seeds);
    for (uint64_t s : table_seeds) base.insert(s);
    // ディスクの終わり (最後の区画の NTFS のバックアップ・GPT のバックアップ付近)
    std::set<uint64_t> tails;
    for (uint64_t k = 1; k <= 16; k++)
      if (hi >= k * ss && (hi - k * ss) >= lo) tails.insert(hi - k * ss);

    std::map<uint64_t, bool> pending;  // 位置 → deep
    for (uint64_t b : base) pending[b] = b < 64ull * kMiB || table_seeds.count(b) > 0;
    std::set<uint64_t> done;
    const uint64_t limit = opt.max_positions * 4 + 1024;
    uint64_t processed = 0;
    while (!pending.empty() && !sc.rd.stopped() && processed < limit) {
      auto it = pending.begin();
      uint64_t p = it->first;
      bool deep = it->second;
      pending.erase(it);
      if (!done.insert(p).second) continue;
      sc.new_ends.clear();
      sc.probe_position(p, deep);
      processed++;
      for (uint64_t e : sc.new_ends) add_follow_ups(e, ss, lo, hi, &pending, done);
      if ((processed & 63) == 0) progress(opt, "scan", processed, processed + pending.size());
    }
    for (uint64_t t : tails) {
      if (sc.rd.stopped()) break;
      sc.read_chunks(t / 512 * 512, 1);
    }
    res.positions_probed = processed;
    progress(opt, "scan", processed, processed);
  }
  res.bytes_read = sc.rd.bytes_read;
  res.read_errors = sc.rd.io_errors;
  if (!sc.rd.fatal.ok()) {
    res.cancelled = false;
  }
  bool cancelled = opt.cancel && opt.cancel->cancelled();

  // 候補にまとめる
  progress(opt, "verify", 0, 1);
  // 別の FS 自身のバックアップ構造を、独立した FS と取り違えたものを外す:
  //  - 「バックアップだけ」の仮説 B で、B のバックアップ位置が、同じ系統の有効な先頭構造 A そのものであるもの (exFAT: +12 セクタ、F2FS: +4 KiB)
  //  - NTFS の最終セクタのバックアップブートセクタを先頭と見なしたもの (別の NTFS の「開始 + 大きさ - 1 セクタ」)
  std::set<std::pair<uint64_t, int>> copies;
  for (const auto& kb : sc.best) {
    if (sc.best.size() > 4096) break;  // 乱数領域などで仮説が膨らんだときは省く (後段の信頼度の足切りに任せる)
    const Hit& b = kb.second;
    for (const auto& ka : sc.best) {
      const Hit& a = ka.second;
      if (&a == &b || ka.first.second != kb.first.second || a.start <= b.start) continue;
      uint64_t d = a.start - b.start;
      bool exf = b.fs == FsType::Exfat && (d == 12ull * 512 || d == 12ull * 1024 || d == 12ull * 2048 || d == 12ull * 4096);
      bool f2 = b.fs == FsType::F2fs && d == 4096;
      bool f32 = b.fs == FsType::Fat32 && (d == 6ull * 512 || d == 6ull * ss);
      if ((exf || f2 || f32) && a.fs == b.fs && a.primary_ok) {
        // A が B のバックアップの位置にある。A 自身のバックアップが無ければ A は B のバックアップの写しにすぎない。
        // A のバックアップも有効で B の先頭が壊れている (B は A を手前にずらした誤仮説) なら、本物は A
        if (b.backup_ok && !a.backup_ok) copies.insert(ka.first);
        else if (a.backup_ok && !b.primary_ok) copies.insert(kb.first);
      }
      if (a.fs == FsType::Ntfs && b.fs == FsType::Ntfs && b.size_known && b.start + b.size >= ss && a.start == b.start + b.size - ss && (b.primary_ok || b.backup_ok))
        copies.insert(b.score > a.score ? ka.first : kb.first);  // 点の高い方 (MFT まで確かめられた方) が本物
    }
  }
  std::vector<ScanCandidate> cands;
  for (auto& kv : sc.best) {
    const Hit& h = kv.second;
    if (copies.count(kv.first)) continue;
    if (h.score < opt.min_confidence) continue;
    ScanCandidate c;
    c.fs = h.fs;
    c.variant = h.variant;
    c.sector_size = ss;
    c.start_bytes = h.start;
    c.start_lba = h.start / ss;
    c.size_known = h.size_known;
    c.size_minimum = h.size_min;
    c.size_bytes = h.size;
    c.sector_count = (h.size + ss - 1) / ss;
    c.confidence = std::min(h.score, 100);
    c.primary_ok = h.primary_ok;
    c.backup_ok = h.backup_ok;
    c.notes = h.notes;
    c.fits_device = h.size == 0 || (h.start <= res.disk_bytes && h.size <= res.disk_bytes - h.start);
    if (!c.fits_device) {
      c.notes.push_back("exceeds-device");
      if (c.confidence > 5) c.confidence -= 5;
    }
    cands.push_back(std::move(c));
  }
  std::sort(cands.begin(), cands.end(), [](const ScanCandidate& a, const ScanCandidate& b) {
    return a.start_bytes != b.start_bytes ? a.start_bytes < b.start_bytes : a.confidence > b.confidence;
  });
  if (cands.size() > opt.max_candidates) {
    cands.resize(opt.max_candidates);
    res.truncated_candidates = true;
  }

  // 入れ子 (別の FS の内側にある FS = イメージファイルなど)
  for (size_t i = 0; i < cands.size(); i++) {
    int best_j = -1;
    for (size_t j = 0; j < cands.size(); j++) {
      if (i == j || !cands[j].size_known || cands[j].size_minimum || cands[j].size_bytes == 0) continue;
      const auto& a = cands[i];
      const auto& b = cands[j];
      if (a.start_bytes > b.start_bytes && a.start_bytes < b.end_bytes() && (a.size_bytes == 0 || a.end_bytes() <= b.end_bytes())) {
        if (best_j < 0 || cands[j].size_bytes < cands[static_cast<size_t>(best_j)].size_bytes) best_j = static_cast<int>(j);
      }
    }
    cands[i].nested_in = best_j;
    if (best_j >= 0) cands[i].notes.push_back("nested");
  }

  // 既存の表との関係
  std::vector<bool> entry_has_fs(res.table.partitions.size(), false);
  for (auto& c : cands) {
    uint64_t cend = c.size_bytes ? c.end_bytes() : c.start_bytes + 1;
    if (res.table.whole_device_volume && c.start_bytes == 0) {
      c.relation = ScanRelation::Match;
      c.table_index = 0;
      continue;
    }
    for (size_t k = 0; k < res.table.partitions.size(); k++) {
      const Partition& p = res.table.partitions[k];
      if (p.protective || p.extended_container) continue;
      uint64_t ps_ = p.start_bytes, pe = p.start_bytes + p.size_bytes;
      if (c.start_bytes == ps_) {
        bool fit = !c.size_known || c.size_minimum || c.size_bytes <= p.size_bytes || !c.fits_device;
        c.relation = fit ? ScanRelation::Match : ScanRelation::Overlap;
        c.table_index = static_cast<int32_t>(p.index);
        entry_has_fs[k] = true;
        break;
      }
      if (c.start_bytes < pe && cend > ps_) {
        bool inside = c.start_bytes > ps_ && cend <= pe;
        ScanRelation r = inside ? ScanRelation::Contained : ScanRelation::Overlap;
        if (c.table_index < 0 || r == ScanRelation::Contained) { c.relation = r; c.table_index = static_cast<int32_t>(p.index); }
      }
    }
  }
  for (size_t k = 0; k < res.table.partitions.size(); k++) {
    const Partition& p = res.table.partitions[k];
    if (p.protective || p.extended_container || entry_has_fs[k]) continue;
    res.table_entries_without_fs.push_back(p.index);
  }

  res.candidates = std::move(cands);
  if (cancelled) {
    res.cancelled = true;
    return Status::Make(Errc::Cancelled);
  }
  if (!sc.rd.fatal.ok()) return sc.rd.fatal;
  return Status::Ok();
}

namespace {

void js(std::string& o, const std::string& s) {
  o += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      default:
        if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += static_cast<char>(c);
    }
  }
  o += '"';
}
std::string u64(uint64_t v) { return "\"" + std::to_string(v) + "\""; }
const char* boolean(bool b) { return b ? "true" : "false"; }

}  // namespace

std::string scan_result_to_json(const ScanResult& r) {
  std::string o = "{\"sectorSize\":" + std::to_string(r.sector_size) + ",\"diskBytes\":" + u64(r.disk_bytes) + ",\"level\":";
  js(o, r.level == ScanLevel::Full ? "full" : "quick");
  o += ",\"positionsProbed\":" + u64(r.positions_probed) + ",\"bytesRead\":" + u64(r.bytes_read) + ",\"readErrors\":" + u64(r.read_errors);
  o += std::string(",\"cancelled\":") + boolean(r.cancelled) + ",\"truncatedCandidates\":" + boolean(r.truncated_candidates);
  const char* kinds[] = {"none", "mbr", "gpt"};
  o += std::string(",\"table\":{\"kind\":\"") + kinds[static_cast<int>(r.table.kind)] + "\",\"partitions\":" + std::to_string(r.table.partitions.size()) +
       ",\"wholeDeviceVolume\":" + boolean(r.table.whole_device_volume) + ",\"usedBackupGpt\":" + boolean(r.table.used_backup_gpt) +
       ",\"hasErrors\":" + boolean(r.table.has_errors()) + "}";
  o += ",\"tableEntriesWithoutFs\":[";
  for (size_t i = 0; i < r.table_entries_without_fs.size(); i++) o += (i ? "," : "") + std::to_string(r.table_entries_without_fs[i]);
  o += "],\"candidates\":[";
  for (size_t i = 0; i < r.candidates.size(); i++) {
    const ScanCandidate& c = r.candidates[i];
    if (i) o += ',';
    o += "{\"index\":" + std::to_string(i) + ",\"fs\":";
    js(o, fs_type_name(c.fs));
    o += ",\"variant\":";
    js(o, c.variant);
    o += ",\"startLba\":" + u64(c.start_lba) + ",\"sectorCount\":" + u64(c.sector_count) + ",\"startBytes\":" + u64(c.start_bytes) + ",\"sizeBytes\":" + u64(c.size_bytes);
    o += std::string(",\"sizeKnown\":") + boolean(c.size_known) + ",\"sizeMinimum\":" + boolean(c.size_minimum) + ",\"fitsDevice\":" + boolean(c.fits_device);
    o += ",\"confidence\":" + std::to_string(c.confidence) + ",\"primaryOk\":" + boolean(c.primary_ok) + ",\"backupOk\":" + boolean(c.backup_ok);
    o += ",\"relation\":";
    js(o, scan_relation_name(c.relation));
    o += ",\"tableIndex\":" + std::to_string(c.table_index) + ",\"nestedIn\":" + std::to_string(c.nested_in) + ",\"notes\":[";
    for (size_t k = 0; k < c.notes.size(); k++) { if (k) o += ','; js(o, c.notes[k]); }
    o += "]}";
  }
  o += "]}";
  return o;
}

}  // namespace usbmem
