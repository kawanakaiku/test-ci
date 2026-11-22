// SPDX-License-Identifier: Apache-2.0
// パーティション探索 (partition_scan)・復元案 (partition_recover)・C API のテスト。
// 本物の mkfs / sfdisk / testdisk との照合は tools/check_partscan_tools.py が usbmem_partscan_cli を通して行う。ここは手組みの合成イメージ・乱数・破損入力。
#include <fcntl.h>
#include <sys/stat.h>

#include <algorithm>
#include <fstream>

#include "partscan_test_util.h"
#include "usbmem/c_api_partscan.h"
#include "usbmem/partition_recover.h"
#include "usbmem/partition_scan.h"

using namespace usbmem;
using namespace ps_test;

namespace {

constexpr uint64_t kMiB = 1u << 20;

ScanResult scan_img(const Bytes& img, ScanLevel lv, uint32_t bs = 512, int min_conf = 40) {
  MemoryDevice dev(img, true, bs);
  ScanOptions o;
  o.level = lv;
  o.min_confidence = min_conf;
  ScanResult r;
  Status st = scan_partitions(dev, o, &r);
  CHECK(st.ok());
  return r;
}

const ScanCandidate* find_at(const ScanResult& r, uint64_t start_bytes) {
  for (const auto& c : r.candidates)
    if (c.start_bytes == start_bytes) return &c;
  return nullptr;
}

bool has_note(const ScanCandidate& c, const char* n) {
  return std::find(c.notes.begin(), c.notes.end(), std::string(n)) != c.notes.end();
}

using Range = std::pair<uint64_t, uint64_t>;

std::vector<Range> ranges_of(const Bytes& img, uint32_t ss = 512) {
  PartitionTable t;
  CHECK(tb::parse(img, &t, ss).ok());
  std::vector<Range> v;
  for (const auto& p : t.partitions)
    if (!p.protective && !p.extended_container) v.push_back({p.start_lba, p.sector_count});
  std::sort(v.begin(), v.end());
  return v;
}

std::shared_ptr<pt::WDev> wdev(Bytes img, uint32_t bs = 512) { return std::make_shared<pt::WDev>(std::move(img), bs); }

// 提案 id の案を適用する (バックアップを取り、確認トークンを通す)。成功したら true
bool apply_proposal(const std::shared_ptr<pt::WDev>& d, const std::vector<RecoveryProposal>& props, const char* id, Bytes* backup = nullptr) {
  const RecoveryProposal* p = nullptr;
  for (const auto& x : props)
    if (x.id == id) p = &x;
  if (!p) { std::fprintf(stderr, "  no proposal %s\n", id); return false; }
  EditorOptions eo;
  eo.rng_seed = 9;
  std::unique_ptr<PartitionEditor> ed;
  if (!PartitionEditor::open(d, eo, &ed).ok()) return false;
  EditResult r = queue_recovery_proposal(*ed, *p);
  if (!r.ok()) { std::fprintf(stderr, "  queue failed %s %s\n", r.code.c_str(), r.message.c_str()); return false; }
  EditPlan plan;
  if (!ed->plan(&plan).ok() || !plan.ok) {
    for (auto& pr : plan.problems) std::fprintf(stderr, "  problem %s %s\n", pr.code.c_str(), pr.message.c_str());
    return false;
  }
  ApplyOptions ao;
  ao.backup_sink = [&](const Bytes& b) { if (backup) *backup = b; return Status::Ok(); };
  r = ed->apply(plan, PartitionEditor::make_confirm_token(plan, plan.confirm_phrase), ao);
  if (!r.ok()) std::fprintf(stderr, "  apply failed %s %s\n", r.code.c_str(), r.message.c_str());
  return r.ok() && r.verified;
}

// 4 パーティションの GPT ディスク (NTFS 16M / ext4 24M / exFAT 16M / XFS 16M)。total 120 MiB
struct Gpt4 {
  Bytes img;
  std::vector<Range> truth;
};
Gpt4 make_gpt4() {
  Gpt4 g;
  uint64_t total = 245760;
  Bytes ntfs = make_ntfs(16 * kMiB), ext4 = make_ext(24 * kMiB, 1), exfat = make_exfat(16 * kMiB), xfs = make_xfs(16 * kMiB);
  g.img = tb::make_gpt_disk(512, total, {{0x28, 2048, 2048 + 32768 - 1, u"a"}, {0x28, 34816, 34816 + 49152 - 1, u"b"}, {0x28, 83968, 83968 + 32768 - 1, u"c"}, {0x28, 116736, 116736 + 32768 - 1, u"d"}});
  place(g.img, 2048ull * 512, ntfs);
  place(g.img, 34816ull * 512, ext4);
  place(g.img, 83968ull * 512, exfat);
  place(g.img, 116736ull * 512, xfs);
  g.truth = {{2048, 32768}, {34816, 49152}, {83968, 32768}, {116736, 32768}};
  return g;
}

void wipe_gpt(Bytes& img, bool primary, bool backup) {
  size_t total = img.size() / 512;
  if (primary) std::memset(img.data(), 0, 34 * 512);
  if (backup) std::memset(&img[(total - 33) * 512], 0, 33 * 512);
}

// 乱数の破損をパッチとして持つデバイス (巨大なコピーを避ける)
class PatchDev final : public BlockDevice {
 public:
  PatchDev(const Bytes& base, uint32_t bs = 512) : base_(base), size_(base.size()), bs_(bs) {}
  struct Patch { uint64_t off; Bytes data; };
  std::vector<Patch> patches;
  uint64_t fail_from = UINT64_MAX;  // この位置以降の読み取りは Io
  Status read_at(uint64_t o, void* b, size_t n, const IoOptions& = {}) override {
    if (!range_ok(size_, o, n)) return Status::Make(Errc::OutOfRange);
    if (o + n > fail_from) return Status::Make(Errc::Io, 5);
    std::memcpy(b, &base_[o], n);
    uint8_t* d = static_cast<uint8_t*>(b);
    for (const auto& p : patches) {
      uint64_t lo = std::max<uint64_t>(o, p.off), hi = std::min<uint64_t>(o + n, p.off + p.data.size());
      if (lo < hi) std::memcpy(d + (lo - o), &p.data[lo - p.off], hi - lo);
    }
    return Status::Ok();
  }
  Status write_at(uint64_t, const void*, size_t, const IoOptions& = {}) override { return Status::Make(Errc::ReadOnly); }
  Status flush() override { return Status::Ok(); }
  Status close() override { return Status::Ok(); }
  uint64_t size() const override { return size_; }
  uint32_t logical_block_size() const override { return bs_; }
  bool is_read_only() const override { return true; }
  std::string describe() const override { return "patch"; }
  void truncate(uint64_t n) { size_ = std::min<uint64_t>(n, base_.size()); }

 private:
  const Bytes& base_;
  uint64_t size_;
  uint32_t bs_;
};

}  // namespace

// ------------------------------------------------------------------ 検出器

TEST(partscan_each_fs_found_quick_and_full) {
  for (const auto& m : all_makers()) {
    Bytes fs = m.make(m.size);
    Bytes disk(2 * kMiB + fs.size() + kMiB, 0);
    place(disk, kMiB, fs);
    for (ScanLevel lv : {ScanLevel::Quick, ScanLevel::Full}) {
      ScanResult r = scan_img(disk, lv);
      const ScanCandidate* c = find_at(r, kMiB);
      if (!c) { std::fprintf(stderr, "  %s level %d: not found (%zu candidates)\n", m.name, static_cast<int>(lv), r.candidates.size()); g_failures++; continue; }
      if (r.candidates.size() != 1) {
        std::fprintf(stderr, "  %s level %d: %zu candidates\n", m.name, static_cast<int>(lv), r.candidates.size());
        for (const auto& x : r.candidates) std::fprintf(stderr, "    %s start %llu size %llu conf %d primary %d backup %d\n", fs_type_name(x.fs), (unsigned long long)x.start_bytes, (unsigned long long)x.size_bytes, x.confidence, x.primary_ok, x.backup_ok);
        g_failures++;
      }
      if (c->fs != m.fs) { std::fprintf(stderr, "  %s: fs %s\n", m.name, fs_type_name(c->fs)); g_failures++; }
      CHECK(c->confidence >= 60);
      CHECK(c->primary_ok);
      CHECK(c->relation == ScanRelation::NotInTable);
      CHECK(c->fits_device);
      if (m.exact) CHECK_EQ(c->size_bytes, m.size);
      if (c->size_known && !m.exact) CHECK(c->size_bytes <= m.size);
      CHECK_EQ(c->start_lba, kMiB / 512);
    }
  }
}

TEST(partscan_unaligned_start_found_by_full) {
  Bytes fs = make_ext(8 * kMiB, 1);
  Bytes disk(12 * kMiB, 0);
  place(disk, kMiB + 512 * 7, fs);
  ScanResult r = scan_img(disk, ScanLevel::Full);
  const ScanCandidate* c = find_at(r, kMiB + 512 * 7);
  CHECK(c != nullptr);
  CHECK_EQ(r.candidates.size(), 1u);
}

TEST(partscan_primary_damaged_found_by_backup) {
  struct Case { const char* name; Bytes (*make)(uint64_t); uint64_t size; FsType fs; size_t zero_off, zero_len; };
  const Case cases[] = {
      {"fat32", make_fat32, 40u << 20, FsType::Fat32, 0, 3 * 512},
      {"ntfs", make_ntfs, 16u << 20, FsType::Ntfs, 0, 4096},
      {"exfat", make_exfat, 16u << 20, FsType::Exfat, 0, 12 * 512},
      {"ext2", mk_ext2, 24u << 20, FsType::Ext2, 0, 4096},
      {"ext4", mk_ext4, 24u << 20, FsType::Ext4, 0, 4096},
      {"f2fs", make_f2fs, 16u << 20, FsType::F2fs, 0, 4096},
      {"btrfs", make_btrfs, 100u << 20, FsType::Btrfs, 0x10000, 4096},
      {"luks2", make_luks2, 32u << 20, FsType::Luks, 0, 16384},
  };
  for (const auto& cs : cases) {
    Bytes fs = cs.make(cs.size);
    Bytes intact(2 * kMiB + fs.size() + kMiB, 0);
    place(intact, kMiB, fs);
    const ScanCandidate* ic = nullptr;
    ScanResult ir = scan_img(intact, ScanLevel::Quick);
    ic = find_at(ir, kMiB);
    CHECK(ic != nullptr);
    int intact_conf = ic ? ic->confidence : 100;
    std::memset(&fs[cs.zero_off], 0, cs.zero_len);
    Bytes disk(2 * kMiB + fs.size() + kMiB, 0);
    place(disk, kMiB, fs);
    for (ScanLevel lv : {ScanLevel::Quick, ScanLevel::Full}) {
      ScanResult r = scan_img(disk, lv);
      const ScanCandidate* c = find_at(r, kMiB);
      if (!c) { std::fprintf(stderr, "  %s level %d: backup-only FS not found\n", cs.name, static_cast<int>(lv)); g_failures++; continue; }
      CHECK(c->fs == cs.fs);
      CHECK(!c->primary_ok);
      CHECK(c->backup_ok);
      CHECK(has_note(*c, "primary-damaged"));
      if (c->confidence >= intact_conf || r.candidates.size() != 1) {
        std::fprintf(stderr, "  %s level %d: conf %d, %zu candidates\n", cs.name, static_cast<int>(lv), c->confidence, r.candidates.size());
        for (const auto& x : r.candidates) std::fprintf(stderr, "    %s start %llu size %llu conf %d primary %d backup %d\n", fs_type_name(x.fs), (unsigned long long)x.start_bytes, (unsigned long long)x.size_bytes, x.confidence, x.primary_ok, x.backup_ok);
        g_failures++;
      }
    }
  }
}

TEST(partscan_zero_and_random_regions_have_no_candidates) {
  Bytes zero(16 * kMiB, 0);
  CHECK_EQ(scan_img(zero, ScanLevel::Quick).candidates.size(), 0u);
  CHECK_EQ(scan_img(zero, ScanLevel::Full).candidates.size(), 0u);
  Bytes ones(4 * kMiB, 0xFF);
  CHECK_EQ(scan_img(ones, ScanLevel::Full).candidates.size(), 0u);
  size_t fp = 0;
  for (uint32_t seed = 1; seed <= 60; seed++) {
    Bytes rnd = random_bytes(2 * kMiB, seed);
    fp += scan_img(rnd, ScanLevel::Full).candidates.size();
    fp += scan_img(rnd, ScanLevel::Quick).candidates.size();
  }
  Bytes big = random_bytes(80 * kMiB, 4242);
  fp += scan_img(big, ScanLevel::Quick).candidates.size();
  fp += scan_img(big, ScanLevel::Full).candidates.size();
  CHECK_EQ(fp, 0u);
}

TEST(partscan_ascii_text_and_mbr_boot_code_are_not_filesystems) {
  Bytes text(4 * kMiB, ' ');
  for (size_t i = 0; i < text.size(); i++) text[i] = static_cast<uint8_t>('a' + (i * 7) % 26);
  CHECK_EQ(scan_img(text, ScanLevel::Full).candidates.size(), 0u);
  // 55 AA と EB xx 90 だけを持つブートコードのようなセクタ (BPB の値が不整合)
  Bytes code(2 * kMiB, 0);
  for (size_t o = 0; o < code.size(); o += 4096) {
    code[o] = 0xEB; code[o + 1] = 0x3C; code[o + 2] = 0x90;
    tb::le16(code, o + 11, 512); code[o + 13] = 3; code[o + 16] = 2;
    tb::sign55aa(*reinterpret_cast<Bytes*>(&code));  // 何もしない (下で直接書く)
    code[o + 510] = 0x55; code[o + 511] = 0xAA;
  }
  CHECK_EQ(scan_img(code, ScanLevel::Full).candidates.size(), 0u);
}

TEST(partscan_nested_image_is_flagged) {
  // FAT32 のデータ領域の中に ext4 のイメージを置く
  Bytes outer = make_fat32(48 * kMiB);
  Bytes inner = make_ext(8 * kMiB, 1);
  Bytes disk(50 * kMiB, 0);
  place(disk, kMiB, outer);
  place(disk, 20 * kMiB, inner);
  ScanResult r = scan_img(disk, ScanLevel::Full);
  const ScanCandidate* o = find_at(r, kMiB);
  const ScanCandidate* i = find_at(r, 20 * kMiB);
  CHECK(o != nullptr && i != nullptr);
  if (o && i) {
    CHECK(o->nested_in < 0);
    CHECK(i->nested_in >= 0);
    CHECK(has_note(*i, "nested"));
  }
}

TEST(partscan_4k_sector_device) {
  Bytes fs = make_ext(8 * kMiB, 1);
  Bytes disk(12 * kMiB, 0);
  place(disk, kMiB, fs);
  for (ScanLevel lv : {ScanLevel::Quick, ScanLevel::Full}) {
    ScanResult r = scan_img(disk, lv, 4096);
    const ScanCandidate* c = find_at(r, kMiB);
    CHECK(c != nullptr);
    if (c) { CHECK_EQ(c->start_lba, kMiB / 4096); CHECK_EQ(c->sector_size, 4096u); }
    CHECK_EQ(r.sector_size, 4096u);
  }
}

TEST(partscan_truncated_image_keeps_the_cut_partition) {
  Gpt4 g = make_gpt4();
  PatchDev dev(g.img);
  dev.truncate(130000ull * 512);  // 最後の XFS (116736 〜) の途中で切る
  ScanOptions o;
  ScanResult r;
  CHECK(scan_partitions(dev, o, &r).ok());
  const ScanCandidate* x = find_at(r, 116736ull * 512);
  CHECK(x != nullptr);
  if (x) { CHECK(!x->fits_device); CHECK(has_note(*x, "exceeds-device")); CHECK_EQ(x->size_bytes, 16 * kMiB); }
  CHECK(find_at(r, 2048ull * 512) != nullptr);
  CHECK(find_at(r, 34816ull * 512) != nullptr);
  CHECK(find_at(r, 83968ull * 512) != nullptr);
}

// ------------------------------------------------------------------ 既存の表との関係

TEST(partscan_relations_to_existing_table) {
  Gpt4 g = make_gpt4();
  ScanResult r = scan_img(g.img, ScanLevel::Quick);
  CHECK_EQ(r.candidates.size(), 4u);
  CHECK(r.table.kind == TableKind::Gpt);
  for (size_t i = 0; i < r.candidates.size(); i++) {
    CHECK(r.candidates[i].relation == ScanRelation::Match);
    CHECK_EQ(r.candidates[i].table_index, static_cast<int32_t>(i + 1));
  }
  CHECK(r.table_entries_without_fs.empty());
  // 1 つ目のエントリを消す → その FS は「表に無い」
  Bytes cut = g.img;
  std::memset(&cut[2 * 512], 0, 128);  // エントリ配列の 1 件目
  // 配列の CRC が合わなくなる → バックアップが使われる。バックアップも同じに消す
  size_t total = cut.size() / 512;
  std::memset(&cut[(total - 33) * 512], 0, 128);
  ScanResult r2 = scan_img(cut, ScanLevel::Quick);
  const ScanCandidate* first = find_at(r2, 2048ull * 512);
  CHECK(first != nullptr);
  // 表の残りが読めなければ表なし → 全部 NotInTable。読めれば 1 つ目だけ NotInTable
  if (first) CHECK(first->relation == ScanRelation::NotInTable);
  // 表の中に FS が無いエントリ
  Bytes empty_ent = tb::make_gpt_disk(512, 20480, {{0x28, 2048, 4095, u"x"}});
  ScanResult r3 = scan_img(empty_ent, ScanLevel::Quick);
  CHECK_EQ(r3.candidates.size(), 0u);
  CHECK_EQ(r3.table_entries_without_fs.size(), 1u);
}

TEST(partscan_whole_device_filesystem_matches_table_none) {
  Bytes fs = make_fat32(40 * kMiB);
  ScanResult r = scan_img(fs, ScanLevel::Quick);
  CHECK_EQ(r.candidates.size(), 1u);
  CHECK(r.table.whole_device_volume);
  CHECK(r.candidates[0].relation == ScanRelation::Match);
}

// ------------------------------------------------------------------ 復元案

TEST(partscan_recover_gpt_from_scratch_when_both_copies_are_gone) {
  Gpt4 g = make_gpt4();
  Bytes broken = g.img;
  wipe_gpt(broken, true, true);
  auto d = wdev(broken);
  ScanResult r;
  CHECK(scan_partitions(*d, ScanOptions{}, &r).ok());
  CHECK_EQ(r.candidates.size(), 4u);
  for (const auto& c : r.candidates) CHECK(c.relation == ScanRelation::NotInTable);
  std::vector<RecoveryProposal> props;
  CHECK(propose_table_recovery(*d, r, RecoveryOptions{}, &props).ok());
  bool saw_gpt = false, saw_mbr = false;
  for (const auto& p : props) {
    if (p.id == "rebuild-gpt") { saw_gpt = true; CHECK(p.plan_ok); CHECK_EQ(p.partitions.size(), 4u); }
    if (p.id == "rebuild-mbr") { saw_mbr = true; CHECK(p.plan_ok); CHECK_EQ(p.partitions.size(), 4u); }
  }
  CHECK(saw_gpt && saw_mbr);
  CHECK(props.size() >= 2);
  // 案は信頼度の高い順
  for (size_t i = 1; i < props.size(); i++) CHECK(props[i - 1].plan_ok != props[i].plan_ok ? props[i - 1].plan_ok : props[i - 1].confidence >= props[i].confidence);
  Bytes backup;
  CHECK(apply_proposal(d, props, "rebuild-gpt", &backup));
  CHECK(ranges_of(pt::read_all(*d)) == g.truth);
  // 書き込み前のバックアップ (壊れた表) が作られた
  BackupInfo bi;
  CHECK(parse_table_backup(backup, &bi).ok());
  // 復元後は探索の結果が表と一致する
  ScanResult r2;
  CHECK(scan_partitions(*d, ScanOptions{}, &r2).ok());
  for (const auto& c : r2.candidates) CHECK(c.relation == ScanRelation::Match);
  // FS のデータは 1 バイトも変わっていない
  Bytes after = pt::read_all(*d);
  CHECK(std::memcmp(&after[2048ull * 512], &g.img[2048ull * 512], 100ull * kMiB) == 0);
}

TEST(partscan_recover_mbr_from_scratch) {
  Gpt4 g = make_gpt4();
  Bytes broken = g.img;
  wipe_gpt(broken, true, true);
  auto d = wdev(broken);
  ScanResult r;
  CHECK(scan_partitions(*d, ScanOptions{}, &r).ok());
  std::vector<RecoveryProposal> props;
  CHECK(propose_table_recovery(*d, r, RecoveryOptions{}, &props).ok());
  CHECK(apply_proposal(d, props, "rebuild-mbr"));
  PartitionTable t;
  CHECK(tb::parse(pt::read_all(*d), &t).ok());
  CHECK(t.kind == TableKind::Mbr);
  CHECK(!t.has_errors());
  CHECK(ranges_of(pt::read_all(*d)) == g.truth);
  // 種別は FS から推定: NTFS / exFAT = 0x07、ext4 / XFS = 0x83
  CHECK_EQ(t.partitions[0].mbr_type, 0x07);
  CHECK_EQ(t.partitions[1].mbr_type, 0x83);
  CHECK_EQ(t.partitions[2].mbr_type, 0x07);
  CHECK_EQ(t.partitions[3].mbr_type, 0x83);
}

TEST(partscan_recover_gpt_from_backup_copy) {
  Gpt4 g = make_gpt4();
  Bytes broken = g.img;
  wipe_gpt(broken, true, false);  // プライマリだけ消す。バックアップは生きている
  auto d = wdev(broken);
  ScanResult r;
  CHECK(scan_partitions(*d, ScanOptions{}, &r).ok());
  CHECK(r.gpt.backup.usable());
  CHECK(!r.gpt.primary.usable());
  std::vector<RecoveryProposal> props;
  CHECK(propose_table_recovery(*d, r, RecoveryOptions{}, &props).ok());
  CHECK(!props.empty());
  CHECK(props[0].id == "gpt-copy-backup-to-primary");
  CHECK(props[0].plan_ok);
  CHECK_EQ(props[0].partitions.size(), 4u);
  CHECK(apply_proposal(d, props, "gpt-copy-backup-to-primary"));
  pt::Snap before = pt::snap(*wdev(g.img)), after = pt::snap(*d);
  CHECK(before == after);
  CHECK(pt::snap(*d).gpt_usable);
}

TEST(partscan_recover_mbr_with_logical_partitions) {
  // 主 3 + 拡張 (論理 3) の MBR ディスクをエディタで作り、FS を置いてから表を消す
  uint64_t total = 184320;
  Bytes img(total * 512, 0);
  auto d = wdev(img);
  {
    EditorOptions eo;
    eo.rng_seed = 3;
    std::unique_ptr<PartitionEditor> ed;
    CHECK(PartitionEditor::open(d, eo, &ed).ok());
    CHECK(ed->new_table(TableKind::Mbr).ok());
    auto mk = [&](PartRole role, uint64_t start, uint64_t count) {
      PartitionSpec s;
      s.role = role; s.start = start; s.count = count;
      CHECK(ed->create(s, nullptr).ok());
    };
    mk(PartRole::Primary, 2048, 32768);
    mk(PartRole::Primary, 34816, 32768);
    mk(PartRole::Primary, 67584, 16384);
    mk(PartRole::Extended, 83968, 0);
    mk(PartRole::Logical, 86016, 49152);
    mk(PartRole::Logical, 137216, 32768);
    mk(PartRole::Logical, 172032, 8192);
    EditPlan plan;
    CHECK(ed->plan(&plan).ok() && plan.ok);
    ApplyOptions ao;
    ao.backup_sink = [](const Bytes&) { return Status::Ok(); };
    CHECK(ed->apply(plan, PartitionEditor::make_confirm_token(plan, plan.confirm_phrase), ao).ok());
  }
  std::vector<Range> truth = {{2048, 32768}, {34816, 32768}, {67584, 16384}, {86016, 49152}, {137216, 32768}, {172032, 8192}};
  ps_test::place(d->bytes(), 2048ull * 512, make_ntfs(16 * kMiB));
  ps_test::place(d->bytes(), 34816ull * 512, make_exfat(16 * kMiB));
  ps_test::place(d->bytes(), 67584ull * 512, make_iso(8 * kMiB));
  ps_test::place(d->bytes(), 86016ull * 512, make_ext(24 * kMiB, 1));
  ps_test::place(d->bytes(), 137216ull * 512, make_xfs(16 * kMiB));
  ps_test::place(d->bytes(), 172032ull * 512, make_erofs(4 * kMiB));
  CHECK(ranges_of(d->bytes()) == truth);
  // 表を消す: セクタ 0 と EBR (拡張の先頭と各論理の直前)
  for (uint64_t lba : {0ull, 83968ull, 86015ull, 137215ull, 172031ull}) std::memset(&d->bytes()[lba * 512], 0, 512);
  ScanResult r;
  CHECK(scan_partitions(*d, ScanOptions{}, &r).ok());
  CHECK_EQ(r.candidates.size(), 6u);
  std::vector<RecoveryProposal> props;
  CHECK(propose_table_recovery(*d, r, RecoveryOptions{}, &props).ok());
  const RecoveryProposal* mbr = nullptr;
  for (const auto& p : props) if (p.id == "rebuild-mbr") mbr = &p;
  CHECK(mbr != nullptr);
  if (mbr) {
    CHECK(mbr->plan_ok);
    size_t ext = 0, logical = 0;
    for (const auto& e : mbr->partitions) { ext += e.role == PartRole::Extended; logical += e.role == PartRole::Logical; }
    CHECK_EQ(ext, 1u);
    CHECK_EQ(logical, 3u);
    CHECK(std::find(mbr->notes.begin(), mbr->notes.end(), "mbr-needs-extended") != mbr->notes.end());
  }
  CHECK(apply_proposal(d, props, "rebuild-mbr"));
  CHECK(ranges_of(d->bytes()) == truth);
  PartitionTable t;
  CHECK(tb::parse(d->bytes(), &t).ok());
  CHECK(!t.has_errors());
}

TEST(partscan_add_missing_keeps_existing_entries) {
  Gpt4 g = make_gpt4();
  // 4 つ目のエントリだけを消す (配列と CRC を作り直さず、エントリを書き換えてヘッダの CRC も直す)
  Bytes img = tb::make_gpt_disk(512, 245760, {{0x28, 2048, 2048 + 32768 - 1, u"a"}, {0x28, 34816, 34816 + 49152 - 1, u"b"}, {0x28, 83968, 83968 + 32768 - 1, u"c"}});
  place(img, 2048ull * 512, make_ntfs(16 * kMiB));
  place(img, 34816ull * 512, make_ext(24 * kMiB, 1));
  place(img, 83968ull * 512, make_exfat(16 * kMiB));
  place(img, 116736ull * 512, make_xfs(16 * kMiB));  // 表に無い
  auto d = wdev(img);
  ScanResult r;
  CHECK(scan_partitions(*d, ScanOptions{}, &r).ok());
  CHECK_EQ(r.candidates.size(), 4u);
  CHECK(r.candidates[3].relation == ScanRelation::NotInTable);
  std::vector<RecoveryProposal> props;
  CHECK(propose_table_recovery(*d, r, RecoveryOptions{}, &props).ok());
  const RecoveryProposal* add = nullptr;
  for (const auto& p : props) if (p.id == "add-missing") add = &p;
  CHECK(add != nullptr);
  if (add) { CHECK(add->plan_ok); CHECK_EQ(add->partitions.size(), 4u); }
  pt::Snap before = pt::snap(*d);
  CHECK(apply_proposal(d, props, "add-missing"));
  CHECK(ranges_of(d->bytes()) == g.truth);
  pt::Snap after = pt::snap(*d);
  // 既存の 3 つは種別・GUID・名前までそのまま
  for (size_t i = 0; i < 3; i++) CHECK(before.parts[i] == after.parts[i]);
  CHECK(after.id == before.id);
}

TEST(partscan_no_proposal_for_whole_device_filesystem) {
  Bytes fs = make_ext(24 * kMiB, 1);
  auto d = wdev(fs);
  ScanResult r;
  CHECK(scan_partitions(*d, ScanOptions{}, &r).ok());
  std::vector<RecoveryProposal> props;
  CHECK(propose_table_recovery(*d, r, RecoveryOptions{}, &props).ok());
  CHECK(props.empty());
}

TEST(partscan_proposals_json_is_wellformed) {
  Gpt4 g = make_gpt4();
  Bytes broken = g.img;
  wipe_gpt(broken, true, true);
  auto d = wdev(broken);
  ScanResult r;
  CHECK(scan_partitions(*d, ScanOptions{}, &r).ok());
  std::vector<RecoveryProposal> props;
  CHECK(propose_table_recovery(*d, r, RecoveryOptions{}, &props).ok());
  std::string j = recovery_proposals_to_json(props), s = scan_result_to_json(r);
  CHECK(j.size() > 100 && j.front() == '[' && j.back() == ']');
  CHECK(s.find("\"candidates\"") != std::string::npos && s.find("\"relation\":\"not-in-table\"") != std::string::npos);
  int depth = 0;
  bool in_str = false;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\\' && in_str) { i++; continue; }
    if (s[i] == '"') in_str = !in_str;
    if (!in_str) depth += (s[i] == '{' || s[i] == '[') - (s[i] == '}' || s[i] == ']');
  }
  CHECK_EQ(depth, 0);
}

// ------------------------------------------------------------------ 進捗・取り消し・読み取りエラー

TEST(partscan_cancel_and_progress) {
  Bytes disk = random_bytes(24 * kMiB, 5);
  MemoryDevice dev(disk, true, 512);
  CancelToken tok;
  ScanOptions o;
  o.level = ScanLevel::Full;
  o.cancel = &tok;
  std::vector<std::string> phases;
  uint64_t last_done = 0;
  o.progress = [&](const char* ph, uint64_t done, uint64_t total) {
    if (phases.empty() || phases.back() != ph) phases.push_back(ph);
    if (std::string(ph) == "scan") {
      CHECK(done <= total);
      CHECK(done >= last_done);
      last_done = done;
      if (done >= 8 * kMiB) tok.cancel();  // 途中で取り消す
    }
  };
  ScanResult r;
  Status st = scan_partitions(dev, o, &r);
  CHECK(st.code == Errc::Cancelled);
  CHECK(r.cancelled);
  CHECK(last_done >= 8 * kMiB && last_done < 24 * kMiB);
  CHECK(phases.size() >= 2 && phases[0] == "table");
  // 取り消し済みのトークンなら何も読まない
  CancelToken pre;
  pre.cancel();
  ScanOptions o2;
  o2.cancel = &pre;
  ScanResult r2;
  CHECK(scan_partitions(dev, o2, &r2).code == Errc::Cancelled);
  CHECK(r2.candidates.empty());
}

TEST(partscan_never_writes) {
  Gpt4 g = make_gpt4();
  auto d = wdev(g.img);
  ScanResult r;
  CHECK(scan_partitions(*d, ScanOptions{}, &r).ok());
  ScanOptions full;
  full.level = ScanLevel::Full;
  CHECK(scan_partitions(*d, full, &r).ok());
  std::vector<RecoveryProposal> props;
  CHECK(propose_table_recovery(*d, r, RecoveryOptions{}, &props).ok());
  CHECK(d->writes.empty());
  CHECK(d->flush_at.empty());
  CHECK(d->bytes() == g.img);
}

TEST(partscan_read_errors_are_skipped_and_counted) {
  Gpt4 g = make_gpt4();
  PatchDev dev(g.img);
  dev.fail_from = 60000ull * 512;  // 後半は読めない
  ScanOptions o;
  o.level = ScanLevel::Full;
  ScanResult r;
  Status st = scan_partitions(dev, o, &r);
  CHECK(st.ok());
  CHECK(r.read_errors > 0);
  CHECK(find_at(r, 2048ull * 512) != nullptr);
  CHECK(find_at(r, 34816ull * 512) != nullptr);
  CHECK(find_at(r, 116736ull * 512) == nullptr);
}

// ------------------------------------------------------------------ 破損入力 (ASan / UBSan 用)

TEST(partscan_fuzz_corrupt_inputs_do_not_crash) {
  Gpt4 g = make_gpt4();
  // 速くするため小さなディスク: 先頭 40 MiB (NTFS + ext4 の途中まで) と、各種の FS を並べたもの
  Bytes base(48 * kMiB, 0);
  place(base, kMiB, make_fat32(8 * kMiB));
  place(base, 10 * kMiB, make_ntfs(6 * kMiB));
  place(base, 17 * kMiB, make_exfat(6 * kMiB));
  place(base, 24 * kMiB, make_ext(8 * kMiB, 1));
  place(base, 33 * kMiB, make_xfs(6 * kMiB));
  place(base, 40 * kMiB, make_erofs(4 * kMiB));
  Bytes gpt = tb::make_gpt_disk(512, base.size() / 512, {{0x28, 2048, 18431, u"a"}, {0x28, 20480, 32767, u"b"}});
  std::memcpy(base.data(), gpt.data(), 34 * 512);
  std::memcpy(&base[base.size() - 33 * 512], &gpt[gpt.size() - 33 * 512], 33 * 512);
  std::mt19937 rng(20261123);
  auto rnd = [&](uint64_t n) { return n ? rng() % n : 0; };
  size_t total_candidates = 0;
  for (int it = 0; it < 2000; it++) {
    PatchDev dev(base);
    int kind = static_cast<int>(rnd(5));
    int n = 1 + static_cast<int>(rnd(8));
    for (int k = 0; k < n; k++) {
      PatchDev::Patch p;
      // 構造のある場所 (先頭 2 MiB と各 FS の先頭付近) を中心に壊す
      uint64_t anchors[] = {0, kMiB, 10 * kMiB, 17 * kMiB, 24 * kMiB, 24 * kMiB + 8 * kMiB + 1024, 33 * kMiB, 40 * kMiB, base.size() - 20 * 512};
      uint64_t off = anchors[rnd(9)] + rnd(70000);
      if (off >= base.size()) off = base.size() - 1;
      size_t len = 1 + rnd(kind == 0 ? 4 : 600);
      if (off + len > base.size()) len = base.size() - off;
      p.off = off;
      p.data.resize(len);
      for (auto& b : p.data) b = static_cast<uint8_t>(kind == 1 ? 0 : kind == 2 ? 0xFF : rng());
      dev.patches.push_back(std::move(p));
    }
    if (rnd(8) == 0) dev.truncate(rnd(base.size()) / 512 * 512 + 512);
    if (rnd(40) == 0) dev.fail_from = rnd(base.size());
    ScanOptions o;
    o.level = (it % 8 == 0) ? ScanLevel::Full : ScanLevel::Quick;
    ScanResult r;
    Status st = scan_partitions(dev, o, &r);
    CHECK(st.ok());
    total_candidates += r.candidates.size();
    for (size_t i = 0; i < r.candidates.size(); i++) {
      const auto& c = r.candidates[i];
      CHECK(c.start_bytes < dev.size());
      CHECK(c.confidence >= 0 && c.confidence <= 100);
      CHECK(c.nested_in < static_cast<int32_t>(r.candidates.size()));
      if (i) CHECK(r.candidates[i - 1].start_bytes <= c.start_bytes);
    }
    std::vector<RecoveryProposal> props;
    if (it % 4 == 0) CHECK(propose_table_recovery(dev, r, RecoveryOptions{}, &props).ok());
    (void)scan_result_to_json(r);
    (void)recovery_proposals_to_json(props);
  }
  CHECK(total_candidates > 0);
}

// ------------------------------------------------------------------ C API

namespace {

std::string make_image(const char* name, size_t bytes) {
  std::string p = test_tmpdir() + "/usbmem_pscan_" + name + ".img";
  ::unlink(p.c_str());
  int fd = ::open(p.c_str(), O_CREAT | O_RDWR, 0600);
  if (fd >= 0) { (void)!::ftruncate(fd, static_cast<off_t>(bytes)); ::close(fd); }
  return p;
}

void write_file(const std::string& p, const Bytes& b, uint64_t off = 0) {
  int fd = ::open(p.c_str(), O_WRONLY);
  if (fd >= 0) { (void)!::pwrite(fd, b.data(), b.size(), static_cast<off_t>(off)); ::close(fd); }
}

std::string take(char* s) {
  std::string r = s ? s : "";
  usbmem_free(s);
  return r;
}

}  // namespace

TEST(partscan_capi_scan_propose_apply) {
  Gpt4 g = make_gpt4();
  Bytes broken = g.img;
  wipe_gpt(broken, true, true);
  std::string img = make_image("capi", broken.size());
  write_file(img, broken);
  usbmem_device* dev = nullptr;
  CHECK_EQ(usbmem_open_path(img.c_str(), 0, &dev), 0);
  usbmem_pjob* job = usbmem_pjob_new();
  usbmem_partscan* h = nullptr;
  CHECK_EQ(usbmem_partscan_run(dev, 0, 0, 0, job, &h), 0);
  CHECK(h != nullptr);
  char ph[32] = {};
  unsigned long long done = 0, tot = 0;
  usbmem_pjob_progress(job, ph, sizeof ph, &done, &tot);
  CHECK(std::string(ph) == "verify");
  char* js = nullptr;
  CHECK_EQ(usbmem_partscan_result_json(h, &js), 0);
  std::string res = take(js);
  CHECK(res.find("\"fs\":\"ntfs\"") != std::string::npos);
  CHECK(res.find("\"fs\":\"xfs\"") != std::string::npos);
  CHECK_EQ(usbmem_partscan_proposals_json(h, &js), 0);
  std::string props = take(js);
  size_t k = props.find("\"id\":\"rebuild-gpt\"");
  CHECK(k != std::string::npos);
  int idx = -1;
  {
    // 案の添字を JSON から取る (rebuild-gpt の直前の "index")
    size_t p = props.rfind("\"index\":", k);
    idx = std::atoi(props.c_str() + p + 8);
  }
  usbmem_partedit* ed = nullptr;
  CHECK_EQ(usbmem_partedit_open(dev, 0, 0, 5, &ed), 0);
  CHECK_EQ(usbmem_partscan_queue_proposal(h, idx, ed), 0);
  CHECK_EQ(usbmem_partedit_plan_json(ed, 0, &js), 0);
  std::string plan = take(js);
  CHECK(plan.find("\"ok\":true") != std::string::npos);
  std::string phrase = plan.substr(plan.find("\"confirmPhrase\":\"") + 17);
  phrase = phrase.substr(0, phrase.find('"'));
  char* tok = nullptr;
  CHECK_EQ(usbmem_partedit_make_token(ed, phrase.c_str(), &tok), 0);
  std::string token = take(tok), bk = img + ".bak";
  CHECK_EQ(usbmem_partedit_apply(ed, token.c_str(), bk.c_str(), 1700000000ull, &js), 0);
  std::string ap = take(js);
  CHECK(ap.find("\"ok\":true") != std::string::npos && ap.find("\"verified\":true") != std::string::npos);
  struct stat sb;
  CHECK(::stat(bk.c_str(), &sb) == 0 && sb.st_size > 0);
  usbmem_partedit_close(ed);
  usbmem_partscan_close(h);
  // 取り消し
  usbmem_pjob_cancel(job);
  usbmem_partscan* h2 = nullptr;
  CHECK_EQ(usbmem_partscan_run(dev, 0, 1, 0, job, &h2), static_cast<int>(Errc::Cancelled));
  CHECK(h2 != nullptr);
  usbmem_partscan_close(h2);
  usbmem_pjob_free(job);
  usbmem_close(dev);
  ::unlink(img.c_str());
  ::unlink(bk.c_str());
}
