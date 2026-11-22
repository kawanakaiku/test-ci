// SPDX-License-Identifier: Apache-2.0
// 失われたパーティション表の復元案。docs/partition-scan.md
#include <algorithm>
#include <cstdio>

#include "partition_scan_internal.h"
#include "usbmem/partition_recover.h"

namespace usbmem {
namespace {

using namespace ps;

constexpr uint32_t kMax32 = 0xFFFFFFFFu;

// 読み取り専用のビュー (検証用の PartitionEditor が排他を取らないように、書き込めないデバイスとして見せる)
class RoView final : public BlockDevice {
 public:
  explicit RoView(BlockDevice& d) : d_(d) {}
  Status read_at(uint64_t o, void* b, size_t n, const IoOptions& io = {}) override { return d_.read_at(o, b, n, io); }
  Status write_at(uint64_t, const void*, size_t, const IoOptions& = {}) override { return Status::Make(Errc::ReadOnly); }
  Status flush() override { return Status::Ok(); }
  Status close() override { return Status::Ok(); }
  uint64_t size() const override { return d_.size(); }
  uint32_t logical_block_size() const override { return d_.logical_block_size(); }
  bool is_read_only() const override { return true; }
  std::string describe() const override { return "ro-view"; }

 private:
  BlockDevice& d_;
};

std::array<uint8_t, 16> guid_of(const char* s) {
  std::array<uint8_t, 16> g{};
  parse_guid(s, &g);
  return g;
}

void type_of(FsType fs, uint64_t sectors, uint8_t* mbr, std::array<uint8_t, 16>* guid) {
  static const auto kBasic = guid_of("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7");
  static const auto kLinux = guid_of("0FC63DAF-8483-4772-8E79-3D69D8477DE4");
  static const auto kLuks = guid_of("CA7D7CCB-63ED-4C53-861C-1742536059CC");
  *mbr = 0x83;
  *guid = kLinux;
  switch (fs) {
    case FsType::Fat12: *mbr = 0x01; *guid = kBasic; break;
    case FsType::Fat16: *mbr = sectors < 65536 ? 0x04 : 0x06; *guid = kBasic; break;
    case FsType::Fat32: *mbr = 0x0C; *guid = kBasic; break;
    case FsType::Exfat: case FsType::Ntfs: case FsType::BitLocker: *mbr = 0x07; *guid = kBasic; break;
    case FsType::Iso9660: case FsType::Udf: *mbr = 0x07; *guid = kBasic; break;
    case FsType::Luks: *mbr = 0x83; *guid = kLuks; break;
    default: break;  // ext / xfs / btrfs / f2fs / squashfs / erofs: Linux filesystem (0x83)
  }
}

struct Limits {
  uint64_t first = 0, last_excl = 0;  // 使える範囲 [first, last_excl) (セクタ)
};

Limits limits_for(TableKind k, uint32_t ss, uint64_t disk_sectors) {
  Limits l;
  if (k == TableKind::Gpt) {
    uint64_t arr = (128ull * 128 + ss - 1) / ss;
    l.first = 2 + arr;
    l.last_excl = disk_sectors > 2 + arr ? disk_sectors - 1 - arr : 0;  // 最後の使用可能 LBA = disk - 2 - arr。排他的な上限はその +1
  } else {
    l.first = 1;
    l.last_excl = std::min<uint64_t>(disk_sectors, 1ull << 32);
  }
  return l;
}

// 候補 → 提案のエントリ (大きさは後で決める)
struct Pick {
  int32_t cand;
  uint64_t start;  // セクタ
  uint64_t size;   // セクタ (0 = 不明)
  bool exact;      // FS のヘッダの大きさをそのまま使う
};

// 重ならない候補の部分集合を信頼度の高い順に選ぶ
std::vector<Pick> select_candidates(const ScanResult& scan, const RecoveryOptions& opt, uint64_t first, uint64_t last_excl) {
  std::vector<int32_t> order;
  for (size_t i = 0; i < scan.candidates.size(); i++) {
    const ScanCandidate& c = scan.candidates[i];
    if (c.nested_in >= 0 || c.confidence < opt.min_confidence || c.fs == FsType::Unknown) continue;
    if (c.start_lba < first || c.start_lba >= last_excl) continue;
    order.push_back(static_cast<int32_t>(i));
  }
  std::stable_sort(order.begin(), order.end(), [&](int32_t a, int32_t b) { return scan.candidates[a].confidence > scan.candidates[b].confidence; });
  std::vector<Pick> picks;
  for (int32_t i : order) {
    const ScanCandidate& c = scan.candidates[i];
    uint64_t sz = c.size_known ? c.sector_count : 0;
    uint64_t end = c.start_lba + (sz ? sz : 1);
    bool clash = false;
    for (const Pick& p : picks) {
      uint64_t pe = p.start + (p.size ? p.size : 1);
      if (c.start_lba < pe && end > p.start) { clash = true; break; }
    }
    if (!clash) picks.push_back({i, c.start_lba, sz, c.size_known && !c.size_minimum});
  }
  std::sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) { return a.start < b.start; });
  return picks;
}

// 提案のエントリの大きさを決める (FS のヘッダの大きさ。不明 / 下限のものは次の開始の手前まで、または 1 MiB に切り上げ)
std::vector<ProposedPartition> build_entries(const ScanResult& scan, std::vector<Pick> picks, uint64_t align, uint64_t limit_excl, bool* estimated) {
  std::vector<ProposedPartition> v;
  for (size_t k = 0; k < picks.size(); k++) {
    const Pick& p = picks[k];
    const ScanCandidate& c = scan.candidates[static_cast<size_t>(p.cand)];
    uint64_t next = k + 1 < picks.size() ? picks[k + 1].start : limit_excl;
    uint64_t end = p.start + p.size;
    bool est = false;
    bool aligned_start = align && p.start % align == 0;
    if (p.size == 0) {
      end = next;  // LUKS など大きさが分からない: 次の候補の手前まで
      est = true;
    } else if (!p.exact || c.fs == FsType::F2fs) {
      // 下限 (squashfs など) / 内部で切り捨てる FS (F2FS): 開始が揃っているなら 1 MiB に切り上げる
      if (aligned_start) {
        uint64_t r = (end + align - 1) / align * align;
        if (r <= next) end = r;
      }
      est = true;
    }
    if (end > next) end = next;
    if (end <= p.start) end = std::min(p.start + 1, next);
    ProposedPartition pp;
    pp.start_lba = p.start;
    pp.sector_count = end - p.start;
    pp.fs = c.fs;
    type_of(c.fs, pp.sector_count, &pp.mbr_type, &pp.type_guid);
    pp.source = "scan";
    pp.candidate = p.cand;
    pp.confidence = c.confidence;
    pp.extent_estimated = est;
    if (est) *estimated = true;
    v.push_back(pp);
  }
  return v;
}

// MBR: 4 つまでは主、それより多いときは先頭の 3 つを主、残りを拡張 + 論理にする
bool assign_mbr_roles(std::vector<ProposedPartition>* v, std::vector<std::string>* notes) {
  if (v->size() <= 4) {
    for (auto& p : *v) p.role = PartRole::Primary;
    return true;
  }
  for (size_t i = 0; i < v->size(); i++) (*v)[i].role = i < 3 ? PartRole::Primary : PartRole::Logical;
  ProposedPartition ext;
  ext.role = PartRole::Extended;
  ext.mbr_type = 0x0F;
  ext.start_lba = (*v)[3].start_lba - 1;  // 先頭の EBR の位置
  ext.sector_count = (*v).back().start_lba + (*v).back().sector_count - ext.start_lba;
  ext.source = "scan";
  ext.confidence = 0;
  v->insert(v->begin() + 3, ext);
  notes->push_back("mbr-needs-extended");
  return true;
}

std::string json_str(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += static_cast<char>(c);
  }
  return o + "\"";
}

}  // namespace

EditResult queue_recovery_proposal(PartitionEditor& ed, const RecoveryProposal& p) {
  ed.clear_ops();
  EditResult r;
  if (p.action == RecoveryAction::RepairGpt) return ed.repair_gpt(p.repair);
  if (p.action == RecoveryAction::NewTable) {
    r = ed.new_table(p.table, 128, false);
    if (!r.ok()) return r;
  }
  for (const auto& e : p.partitions) {
    if (e.source != "scan") continue;
    PartitionSpec s;
    s.role = e.role;
    s.start = e.start_lba;
    s.count = e.sector_count;
    s.mbr_type = e.mbr_type;
    s.type_guid = e.type_guid;
    s.unique_guid = e.unique_guid;
    s.name = e.name;
    s.attrs = e.attrs;
    s.bootable = e.bootable;
    r = ed.create(s, nullptr);
    if (!r.ok()) { ed.clear_ops(); return r; }
  }
  r = EditResult{};
  return r;
}

Status propose_table_recovery(BlockDevice& dev, const ScanResult& scan, const RecoveryOptions& opt, std::vector<RecoveryProposal>* out) {
  if (!out) return Status::Make(Errc::InvalidArgument);
  out->clear();
  const uint32_t ss = scan.sector_size ? scan.sector_size : 512;
  const uint64_t disk_sectors = scan.disk_bytes / ss;
  const uint64_t align = opt.alignment_sectors ? opt.alignment_sectors : std::max<uint64_t>(1, (1u << 20) / ss);
  std::vector<RecoveryProposal> props;

  // 1) GPT 自体の修復 (primary / backup のどちらかが生きている)。エディタが提案する修復をそのまま案にする
  for (GptRepairKind k : scan.gpt.suggested_repairs) {
    RecoveryProposal p;
    p.id = std::string("gpt-") + gpt_repair_name(k);
    p.action = RecoveryAction::RepairGpt;
    p.repair = k;
    p.table = TableKind::Gpt;
    const GptCopyInfo* src = scan.gpt.primary.usable() ? &scan.gpt.primary : scan.gpt.backup.usable() ? &scan.gpt.backup : nullptr;
    switch (k) {
      case GptRepairKind::CopyBackupToPrimary: p.title = "Rebuild the primary GPT from the backup GPT"; src = &scan.gpt.backup; p.confidence = 95; break;
      case GptRepairKind::CopyPrimaryToBackup: p.title = "Rebuild the backup GPT from the primary GPT"; src = &scan.gpt.primary; p.confidence = 95; break;
      case GptRepairKind::MoveBackupToEnd: p.title = "Move the backup GPT to the end of the disk"; p.confidence = 90; break;
      case GptRepairKind::RebuildProtectiveMbr: p.title = "Rebuild the protective MBR"; p.confidence = 90; break;
    }
    if (src && src->usable() && src->entry_size >= 128) {
      for (size_t o = 0; o + src->entry_size <= src->entries.size() && o < (1u << 20); o += src->entry_size) {
        const uint8_t* e = src->entries.data() + o;
        if (all_zero(e, 16)) continue;
        ProposedPartition pp;
        std::memcpy(pp.type_guid.data(), e, 16);
        std::memcpy(pp.unique_guid.data(), e + 16, 16);
        pp.start_lba = le64(e + 32);
        uint64_t last = le64(e + 40);
        pp.sector_count = last >= pp.start_lba ? last - pp.start_lba + 1 : 0;
        pp.attrs = le64(e + 48);
        pp.source = "gpt-backup";
        pp.confidence = p.confidence;
        p.partitions.push_back(pp);
      }
    }
    // 探索で見つけた FS が、残っているエントリのどれとも対応しなければ注記
    for (const auto& c : scan.candidates)
      if (c.nested_in < 0 && c.confidence >= 70 && c.relation == ScanRelation::NotInTable) { p.notes.push_back("candidates-not-in-gpt"); p.confidence -= 10; break; }
    props.push_back(std::move(p));
  }

  // 2) 既存の表が正しく読めるなら、表に無い候補だけを足す案
  const bool table_ok = scan.table.kind != TableKind::None && !scan.table.has_errors() && !scan.table.whole_device_volume;
  bool whole_fs = false;
  for (const auto& c : scan.candidates)
    if (c.start_bytes == 0 && c.nested_in < 0 && c.confidence >= opt.min_confidence && c.size_known && c.size_bytes > scan.disk_bytes / 2) whole_fs = true;
  if (table_ok && !whole_fs) {
    Limits lim = limits_for(scan.table.kind, ss, disk_sectors);
    std::vector<Pick> picks;
    for (auto& pk : select_candidates(scan, opt, lim.first, lim.last_excl)) {
      const ScanCandidate& c = scan.candidates[static_cast<size_t>(pk.cand)];
      if (c.relation != ScanRelation::NotInTable) continue;
      picks.push_back(pk);
    }
    // 既存のエントリと重ならないか (NotInTable なら重ならない)。残すエントリの終わり・次の開始で大きさを抑える
    if (!picks.empty()) {
      RecoveryProposal p;
      p.id = "add-missing";
      p.title = "Keep the existing partition table and add the partitions that were found on the disk";
      p.action = RecoveryAction::AddMissing;
      p.table = scan.table.kind;
      std::vector<uint64_t> obstacles;
      for (const auto& q : scan.table.partitions) {
        if (q.protective) continue;
        ProposedPartition pp;
        pp.start_lba = q.start_lba;
        pp.sector_count = q.sector_count;
        pp.fs = q.fs;
        pp.source = "table";
        pp.role = q.extended_container ? PartRole::Extended : q.logical ? PartRole::Logical : PartRole::Primary;
        pp.mbr_type = q.mbr_type;
        pp.type_guid = q.type_guid;
        pp.unique_guid = q.unique_guid;
        pp.name = q.name;
        pp.confidence = 100;
        p.partitions.push_back(pp);
        obstacles.push_back(q.start_lba);
      }
      // 次の既存エントリの開始を超えないようにするため、擬似的な候補として大きさを決める
      std::vector<Pick> bounded;
      bool est = false;
      std::vector<ProposedPartition> built;
      for (size_t k = 0; k < picks.size(); k++) {
        uint64_t next = lim.last_excl;
        for (uint64_t o : obstacles) if (o > picks[k].start && o < next) next = o;
        if (k + 1 < picks.size() && picks[k + 1].start < next) next = picks[k + 1].start;
        std::vector<Pick> one = {picks[k]};
        auto b = build_entries(scan, one, align, next, &est);
        built.insert(built.end(), b.begin(), b.end());
      }
      for (auto& e : built) {
        if (scan.table.kind == TableKind::Mbr) e.role = PartRole::Auto;
        p.partitions.push_back(e);
      }
      std::sort(p.partitions.begin(), p.partitions.end(), [](const ProposedPartition& a, const ProposedPartition& b) { return a.start_lba < b.start_lba; });
      int sum = 0, n = 0;
      for (const auto& e : built) { sum += e.confidence; n++; }
      p.confidence = n ? sum / n : 0;
      p.notes.push_back("types-guessed");
      if (est) p.notes.push_back("sizes-estimated");
      props.push_back(std::move(p));
    }
  }

  // 3) 候補だけから表を作り直す案 (GPT / MBR)
  if (!whole_fs) {
    bool gpt_remnant = scan.gpt.primary.present || scan.gpt.backup.present;
    bool mbr_remnant = false;
    {
      uint8_t s0[512];
      if (dev.read_at(0, s0, 512).ok() && s0[510] == 0x55 && s0[511] == 0xAA) {
        for (int i = 0; i < 4; i++) {
          const uint8_t* e = s0 + 446 + 16 * i;
          if (e[4] != 0 && e[4] != 0xEE) mbr_remnant = true;
        }
      }
    }
    for (TableKind kind : {TableKind::Gpt, TableKind::Mbr}) {
      Limits lim = limits_for(kind, ss, disk_sectors);
      if (lim.last_excl <= lim.first) continue;
      std::vector<Pick> picks = select_candidates(scan, opt, lim.first, lim.last_excl);
      if (picks.empty()) continue;
      bool est = false;
      std::vector<ProposedPartition> v = build_entries(scan, picks, align, lim.last_excl, &est);
      RecoveryProposal p;
      p.action = RecoveryAction::NewTable;
      p.table = kind;
      p.id = kind == TableKind::Gpt ? "rebuild-gpt" : "rebuild-mbr";
      p.title = kind == TableKind::Gpt ? "Create a new GPT from the partitions found on the disk" : "Create a new MBR partition table from the partitions found on the disk";
      bool fits = true;
      if (kind == TableKind::Mbr) {
        for (const auto& e : v)
          if (e.start_lba > kMax32 || e.sector_count > kMax32 || e.start_lba + e.sector_count > (1ull << 32)) fits = false;
        if (!fits) { p.notes.push_back("mbr-cannot-represent"); }
        if (ss != 512) p.notes.push_back("mbr-non-512-sector");
        assign_mbr_roles(&v, &p.notes);
      }
      p.partitions = v;
      int sum = 0, n = 0;
      for (const auto& e : v) if (e.confidence) { sum += e.confidence; n++; }
      int avg = n ? sum / n : 0;
      int evidence = kind == TableKind::Gpt ? (gpt_remnant ? 12 : 0) : (mbr_remnant ? 12 : 0);
      // 手がかりが無いとき: 2 TiB 以下・512 バイトセクタ・MBR で表せるなら MBR をやや優先 (USB メモリの標準)。そうでなければ GPT
      if (!gpt_remnant && !mbr_remnant) evidence = (kind == TableKind::Mbr && fits && ss == 512) ? 3 : 1;
      p.confidence = std::max(0, std::min(100, avg * 85 / 100 + evidence));
      if (kind == TableKind::Mbr && !fits) p.confidence = std::min(p.confidence, 10);
      p.notes.push_back("types-guessed");
      if (est) p.notes.push_back("sizes-estimated");
      if (gpt_remnant && kind == TableKind::Gpt) p.notes.push_back("gpt-remnants");
      if (mbr_remnant && kind == TableKind::Mbr) p.notes.push_back("mbr-remnants");
      props.push_back(std::move(p));
    }
  }

  // 検証: 読み取り専用のビューの上でエディタに計画を作らせる
  if (opt.validate) {
    RoView ro(dev);
    for (auto& p : props) {
      std::unique_ptr<PartitionEditor> ed;
      EditorOptions eo;
      eo.sector_size = ss;
      eo.rng_seed = opt.rng_seed;
      EditResult r = PartitionEditor::open(std::shared_ptr<BlockDevice>(&ro, [](BlockDevice*) {}), eo, &ed);
      if (!r.ok()) { p.problems.push_back({Severity::Error, r.code, r.message}); continue; }
      r = queue_recovery_proposal(*ed, p);
      if (!r.ok()) { p.problems.push_back({Severity::Error, r.code, r.message}); continue; }
      EditPlan plan;
      r = ed->plan(&plan);
      p.plan_ok = r.ok() && plan.ok;
      p.problems = plan.problems;
      if (!r.ok() && p.problems.empty()) p.problems.push_back({Severity::Error, r.code, r.message});
      p.changes = plan.changes;
      p.confirm_phrase = plan.confirm_phrase;
    }
  }
  std::stable_sort(props.begin(), props.end(), [](const RecoveryProposal& a, const RecoveryProposal& b) {
    if (a.plan_ok != b.plan_ok && !a.plan_ok) return false;
    if (a.plan_ok != b.plan_ok) return true;
    return a.confidence > b.confidence;
  });
  *out = std::move(props);
  return Status::Ok();
}

std::string recovery_proposals_to_json(const std::vector<RecoveryProposal>& v) {
  std::string o = "[";
  for (size_t i = 0; i < v.size(); i++) {
    const RecoveryProposal& p = v[i];
    if (i) o += ',';
    o += "{\"index\":" + std::to_string(i) + ",\"id\":" + json_str(p.id) + ",\"title\":" + json_str(p.title);
    o += std::string(",\"table\":\"") + (p.table == TableKind::Gpt ? "gpt" : p.table == TableKind::Mbr ? "mbr" : "none") + "\"";
    o += ",\"action\":" + json_str(p.action == RecoveryAction::RepairGpt ? "repair-gpt" : p.action == RecoveryAction::NewTable ? "new-table" : "add-missing");
    o += ",\"confidence\":" + std::to_string(p.confidence) + ",\"planOk\":" + (p.plan_ok ? "true" : "false") + ",\"confirmPhrase\":" + json_str(p.confirm_phrase);
    o += ",\"notes\":[";
    for (size_t k = 0; k < p.notes.size(); k++) { if (k) o += ','; o += json_str(p.notes[k]); }
    o += "],\"problems\":[";
    for (size_t k = 0; k < p.problems.size(); k++) {
      if (k) o += ',';
      o += "{\"severity\":" + json_str(p.problems[k].severity == Severity::Error ? "error" : p.problems[k].severity == Severity::Warning ? "warning" : "info") +
           ",\"code\":" + json_str(p.problems[k].code) + ",\"message\":" + json_str(p.problems[k].message) + "}";
    }
    o += "],\"changes\":[";
    for (size_t k = 0; k < p.changes.size(); k++) { if (k) o += ','; o += json_str(p.changes[k]); }
    o += "],\"partitions\":[";
    for (size_t k = 0; k < p.partitions.size(); k++) {
      const auto& e = p.partitions[k];
      if (k) o += ',';
      const char* role = e.role == PartRole::Primary ? "primary" : e.role == PartRole::Extended ? "extended" : e.role == PartRole::Logical ? "logical" : "auto";
      o += "{\"startLba\":\"" + std::to_string(e.start_lba) + "\",\"sectorCount\":\"" + std::to_string(e.sector_count) + "\",\"fs\":" + json_str(fs_type_name(e.fs)) +
           ",\"source\":" + json_str(e.source) + ",\"candidate\":" + std::to_string(e.candidate) + ",\"confidence\":" + std::to_string(e.confidence) +
           ",\"role\":" + json_str(role) + ",\"mbrType\":" + std::to_string(e.mbr_type) + ",\"typeGuid\":" + json_str(guid_to_string(e.type_guid)) +
           ",\"extentEstimated\":" + (e.extent_estimated ? "true" : "false") + "}";
    }
    o += "]}";
  }
  return o + "]";
}

}  // namespace usbmem
