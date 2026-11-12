// SPDX-License-Identifier: Apache-2.0
// DiskTools (fscheck / mkfs) の単体テスト。外部ツールとの突き合わせは tools/check_disktools.py。
#include <atomic>
#include <cstdint>
#include <functional>

#include "test_util.h"
#include "usbmem/c_api.h"
#include "usbmem/c_api_disktools.h"
#include "usbmem/fscheck.h"
#include "usbmem/mkfs.h"

using namespace usbmem;

namespace {

// 書き込みと flush の回数を数える / 失敗させるラッパー
class SpyDevice final : public BlockDevice {
 public:
  explicit SpyDevice(std::shared_ptr<MemoryDevice> inner) : in_(std::move(inner)) {}
  Status read_at(uint64_t o, void* b, size_t n, const IoOptions& io = {}) override { return in_->read_at(o, b, n, io); }
  Status write_at(uint64_t o, const void* b, size_t n, const IoOptions& io = {}) override {
    writes++;
    if (on_write) on_write();
    if (fail_at >= 0 && writes == fail_at) return Status::Make(Errc::Io);   // その 1 回だけ失敗
    return in_->write_at(o, b, n, io);
  }
  Status flush() override { flushes++; return in_->flush(); }
  Status close() override { return in_->close(); }
  uint64_t size() const override { return in_->size(); }
  uint32_t logical_block_size() const override { return in_->logical_block_size(); }
  bool is_read_only() const override { return in_->is_read_only(); }
  std::string describe() const override { return in_->describe(); }
  std::vector<uint8_t>& bytes() { return in_->bytes(); }
  int writes = 0, flushes = 0, fail_at = -1;
  std::function<void()> on_write;

 private:
  std::shared_ptr<MemoryDevice> in_;
};

std::shared_ptr<SpyDevice> make_dev(size_t size, uint8_t fill = 0, bool ro = false) {
  return std::make_shared<SpyDevice>(std::make_shared<MemoryDevice>(std::vector<uint8_t>(size, fill), ro));
}

struct Case { FsType fs; size_t size; const char* name; };
const Case kCases[] = {{FsType::Fat32, 40u << 20, "fat32"}, {FsType::Exfat, 8u << 20, "exfat"}, {FsType::Ext2, 4u << 20, "ext2"}};

MkfsOptions opts(FsType fs) {
  MkfsOptions o;
  o.fs = fs;
  o.seed = 42;
  o.timestamp = 1700000000;
  return o;
}

std::string token_of(const MkfsPlan& p) { return Mkfs::make_confirm_token(p, p.confirm_phrase); }

MkfsApplyOptions with_backup(std::vector<uint8_t>* keep = nullptr) {
  MkfsApplyOptions a;
  a.backup_sink = [keep](const std::vector<uint8_t>& b) { if (keep) *keep = b; return Status::Ok(); };
  return a;
}

}  // namespace

TEST(disktools_mkfs_roundtrip_all) {
  for (auto& c : kCases) {
    auto dev = make_dev(c.size, 0xA5);
    MkfsPlan plan;
    MkfsResult pr = Mkfs::plan(*dev, opts(c.fs), &plan);
    CHECK(pr.ok());
    CHECK(plan.ok);
    CHECK(plan.requires_force);                                  // 0xA5 の中身 = 何かが入っている
    CHECK(plan.confirm_phrase.compare(0, 6, "ERASE ") == 0);
    CHECK_EQ(dev->writes, 0);                                    // 計画は書かない
    MkfsResult r = Mkfs::apply(*dev, plan, token_of(plan), with_backup());
    CHECK(r.ok());
    CHECK(r.verified);
    CHECK(r.check_verdict == "clean");
    Status io;
    CHECK(probe_filesystem(*dev, 0, dev->size(), &io) == c.fs);
    FsCheckReport rep;
    FsCheckOptions fo;
    CHECK(check_filesystem(*dev, 0, dev->size(), c.fs, fo, &rep).ok());
    CHECK(rep.verdict == FsVerdict::Clean);
    CHECK_EQ(rep.error_count + rep.warning_count, 0);
  }
}

TEST(disktools_mkfs_blank_device_uses_format_phrase) {
  for (auto& c : kCases) {
    auto dev = make_dev(c.size, 0);
    MkfsPlan plan;
    CHECK(Mkfs::plan(*dev, opts(c.fs), &plan).ok());
    CHECK(plan.ok);
    CHECK(!plan.requires_force);
    CHECK(plan.confirm_phrase.compare(0, 7, "FORMAT ") == 0);
  }
}

TEST(disktools_mkfs_existing_fs_needs_erase_phrase) {
  auto dev = make_dev(8u << 20, 0);
  MkfsPlan p1;
  CHECK(Mkfs::plan(*dev, opts(FsType::Ext2), &p1).ok());
  CHECK(Mkfs::apply(*dev, p1, token_of(p1), with_backup()).ok());
  MkfsPlan p2;
  CHECK(Mkfs::plan(*dev, opts(FsType::Exfat), &p2).ok());
  CHECK(p2.ok);
  CHECK(p2.requires_force);
  CHECK(p2.existing_fs == "ext2");
  bool warned = false;
  for (auto& d : p2.problems) if (d.code == "overwrites-filesystem") warned = true;
  CHECK(warned);
}

TEST(disktools_mkfs_refusals_write_nothing) {
  auto dev = make_dev(8u << 20, 0x5A);
  MkfsPlan plan;
  CHECK(Mkfs::plan(*dev, opts(FsType::Exfat), &plan).ok());
  CHECK(plan.ok);
  const std::vector<uint8_t> before = dev->bytes();
  const std::string good = token_of(plan);

  { MkfsApplyOptions a = with_backup(); a.in_use = true; MkfsResult r = Mkfs::apply(*dev, plan, good, a); CHECK(r.code == "in-use"); CHECK(!r.ok()); }
  { MkfsResult r = Mkfs::apply(*dev, plan, "deadbeef", with_backup()); CHECK(r.code == "bad-token"); }
  { MkfsResult r = Mkfs::apply(*dev, plan, "", with_backup()); CHECK(r.code == "bad-token"); }
  { MkfsResult r = Mkfs::apply(*dev, plan, Mkfs::make_confirm_token(plan, "FORMAT 000000"), with_backup()); CHECK(r.code == "bad-token"); }   // 別の確認文字列
  { MkfsApplyOptions a; a.require_backup = true; MkfsResult r = Mkfs::apply(*dev, plan, good, a); CHECK(r.code == "backup-required"); }
  { MkfsApplyOptions a; a.backup_sink = [](const std::vector<uint8_t>&) { return Status::Make(Errc::Io); }; MkfsResult r = Mkfs::apply(*dev, plan, good, a); CHECK(r.code == "backup-failed"); }
  {
    MkfsPlan t = plan;   // 改ざん: 書く内容を 1 バイト変える
    for (auto& st : t.stages) for (auto& rg : st.regions) if (!rg.data.empty()) { rg.data[0] ^= 1; goto done; }
  done:
    MkfsResult r = Mkfs::apply(*dev, t, good, with_backup());
    CHECK(r.code == "plan-tampered");
  }
  {
    MkfsPlan t = plan;   // 範囲外の領域
    t.stages.back().regions.back().offset = t.length;
    MkfsResult r = Mkfs::apply(*dev, t, good, with_backup());
    CHECK(r.code == "plan-invalid");
  }
  {
    auto other = make_dev(9u << 20, 0x5A);   // 別のデバイス (サイズ違い)
    MkfsResult r = Mkfs::apply(*other, plan, good, with_backup());
    CHECK(r.code == "wrong-device");
  }
  {
    auto ro = make_dev(8u << 20, 0x5A, true);
    MkfsPlan rp;
    CHECK(Mkfs::plan(*ro, opts(FsType::Exfat), &rp).ok());
    CHECK(rp.ok);   // 読み取り専用でも計画は作れる (Warning 付き)
    MkfsResult r = Mkfs::apply(*ro, rp, token_of(rp), with_backup());
    CHECK(r.code == "device-read-only");
  }
  {
    MkfsOptions o = opts(FsType::Exfat);
    o.in_use = true;
    MkfsPlan up;
    CHECK(Mkfs::plan(*dev, o, &up).ok());
    CHECK(!up.ok);   // マウント中は計画の段階で不可
  }
  {
    dev->bytes()[100] ^= 0xFF;   // 計画のあとでディスクの内容が変わった (上書きされる領域の中)
    MkfsResult r = Mkfs::apply(*dev, plan, good, with_backup());
    CHECK(r.code == "device-changed");
    dev->bytes()[100] ^= 0xFF;
  }
  CHECK_EQ(dev->writes, 0);
  CHECK(dev->bytes() == before);
  // 正しいトークンなら通る
  MkfsResult ok = Mkfs::apply(*dev, plan, good, with_backup());
  CHECK(ok.ok());
  CHECK(ok.verified);
}

TEST(disktools_mkfs_busy_when_concurrent) {
  auto dev = make_dev(8u << 20, 0);
  MkfsPlan plan;
  CHECK(Mkfs::plan(*dev, opts(FsType::Exfat), &plan).ok());
  std::string nested;
  bool once = false;
  dev->on_write = [&] {
    if (once) return;
    once = true;
    MkfsPlan p2;
    Mkfs::plan(*dev, opts(FsType::Exfat), &p2);
    nested = Mkfs::apply(*dev, p2, token_of(p2), with_backup()).code;
  };
  MkfsResult r = Mkfs::apply(*dev, plan, token_of(plan), with_backup());
  CHECK(r.ok());
  CHECK(nested == "busy" || nested == "device-changed");   // 2 つ目は始まらない
}

TEST(disktools_mkfs_unsupported_and_invalid_options) {
  auto dev = make_dev(64u << 20, 0);
  MkfsPlan p;
  MkfsOptions o = opts(FsType::Ntfs);
  CHECK(Mkfs::plan(*dev, o, &p).ok());
  CHECK(!p.ok);
  o = opts(FsType::Ext4);
  CHECK(Mkfs::plan(*dev, o, &p).ok());
  CHECK(!p.ok);
  o = opts(FsType::Fat32);
  o.label = "bad*label";
  CHECK(Mkfs::plan(*dev, o, &p).ok());
  CHECK(!p.ok);
  o.label = "123456789012";   // 12 バイト
  CHECK(Mkfs::plan(*dev, o, &p).ok());
  CHECK(!p.ok);
  o = opts(FsType::Fat32);
  o.cluster_size = 3000;
  CHECK(Mkfs::plan(*dev, o, &p).ok());
  CHECK(!p.ok);
  o = opts(FsType::Fat32);
  o.offset = 100;   // セクタ境界でない
  CHECK(Mkfs::plan(*dev, o, &p).ok());
  CHECK(!p.ok);
  o = opts(FsType::Fat32);
  o.offset = dev->size() + 512;
  CHECK(Mkfs::plan(*dev, o, &p).ok());
  CHECK(!p.ok);
  auto tiny = make_dev(4u << 20, 0);
  CHECK(Mkfs::plan(*tiny, opts(FsType::Fat32), &p).ok());
  CHECK(!p.ok);   // 小さすぎて FAT32 にならない
  o = opts(FsType::Exfat);
  o.label = std::string("\xff\xfe");   // 不正な UTF-8
  CHECK(Mkfs::plan(*dev, o, &p).ok());
  CHECK(!p.ok);
  o = opts(FsType::Ext2);
  o.label = "12345678901234567";   // 17 バイト
  CHECK(Mkfs::plan(*dev, o, &p).ok());
  CHECK(!p.ok);
}

TEST(disktools_mkfs_partition_table_is_flagged) {
  // GPT の保護 MBR + ヘッダがあるデバイスの先頭からの作成は、表を壊すので強い警告
  auto dev = make_dev(40u << 20, 0);
  auto& b = dev->bytes();
  b[510] = 0x55; b[511] = 0xAA;
  b[446 + 4] = 0xEE; b[446 + 8] = 1; b[446 + 12] = 0xFF; b[446 + 13] = 0xFF;
  std::memcpy(&b[512], "EFI PART", 8);
  MkfsPlan p;
  CHECK(Mkfs::plan(*dev, opts(FsType::Fat32), &p).ok());
  bool w = false;
  for (auto& d : p.problems) if (d.code == "overwrites-partition-table") w = true;
  CHECK(w);
  CHECK(p.requires_force);
}

TEST(disktools_mkfs_cancel_and_failure_roll_back) {
  for (int mode = 0; mode < 2; mode++) {
    auto dev = make_dev(8u << 20, 0x3C);
    const std::vector<uint8_t> before = dev->bytes();
    MkfsPlan plan;
    CHECK(Mkfs::plan(*dev, opts(FsType::Exfat), &plan).ok());
    MkfsApplyOptions a = with_backup();
    CancelToken ct;
    if (mode == 0) {
      // 3 回目の書き込みでキャンセル
      dev->on_write = [&] { if (dev->writes == 3) ct.cancel(); };
      a.cancel = &ct;
    } else {
      dev->fail_at = 5;   // 5 回目の書き込みが失敗する
    }
    MkfsResult r = Mkfs::apply(*dev, plan, token_of(plan), a);
    CHECK(!r.ok());
    CHECK(r.code == (mode == 0 ? "cancelled" : "write-failed"));
    CHECK(r.rolled_back);
    dev->fail_at = -1;
    dev->on_write = nullptr;
    // 小さい領域 (ブート領域・ビットマップなど) は元に戻る。大きなゼロ埋め領域 (FAT) は戻らない
    bool same_head = std::equal(before.begin(), before.begin() + 12 * 512 * 2, dev->bytes().begin());
    CHECK(same_head);
  }
}

TEST(disktools_mkfs_backup_roundtrip_and_corruption) {
  auto dev = make_dev(8u << 20, 0);
  MkfsPlan p1;
  CHECK(Mkfs::plan(*dev, opts(FsType::Ext2), &p1).ok());
  CHECK(Mkfs::apply(*dev, p1, token_of(p1), with_backup()).ok());
  const std::vector<uint8_t> ext_img = dev->bytes();
  MkfsPlan p2;
  CHECK(Mkfs::plan(*dev, opts(FsType::Exfat), &p2).ok());
  std::vector<uint8_t> bk;
  MkfsResult r = Mkfs::apply(*dev, p2, token_of(p2), with_backup(&bk));
  CHECK(r.ok());
  MkfsBackupInfo info;
  CHECK(parse_mkfs_backup(bk, &info).ok());
  CHECK(info.regions > 0);
  CHECK_EQ(info.range_length, 8u << 20);
  // 破損の検出
  std::vector<uint8_t> bad = bk;
  bad[bad.size() / 2] ^= 1;
  CHECK(parse_mkfs_backup(bad, &info).code == Errc::Corrupt);
  bad = bk;
  bad.resize(bad.size() - 1);
  CHECK(parse_mkfs_backup(bad, &info).code == Errc::Corrupt);
  CHECK(parse_mkfs_backup({}, &info).code == Errc::Corrupt);
  // 復元: 元の ext2 が検査を通る (inode テーブルはゼロ埋めで消えていないため、元の内容が戻る)
  CHECK(mkfs_restore_backup(*dev, bk).ok());
  FsCheckReport rep;
  CHECK(check_filesystem(*dev, 0, dev->size(), FsType::Ext2, FsCheckOptions{}, &rep).ok());
  CHECK(rep.verdict == FsVerdict::Clean);
  CHECK(dev->bytes() == ext_img);   // exFAT が触った領域はすべて小さい領域なので、完全に元へ戻る
  auto ro = make_dev(8u << 20, 0, true);
  CHECK(mkfs_restore_backup(*ro, bk).code == Errc::ReadOnly);
}

TEST(disktools_check_never_writes) {
  for (auto& c : kCases) {
    auto dev = make_dev(c.size, 0);
    MkfsPlan plan;
    CHECK(Mkfs::plan(*dev, opts(c.fs), &plan).ok());
    CHECK(Mkfs::apply(*dev, plan, token_of(plan), with_backup()).ok());
    dev->writes = dev->flushes = 0;
    FsCheckReport rep;
    CHECK(check_filesystem(*dev, 0, dev->size(), c.fs, FsCheckOptions{}, &rep).ok());
    CHECK_EQ(dev->writes, 0);
    CHECK_EQ(dev->flushes, 0);
    // 種別を省略 (Unknown) したら検出して検査する
    FsCheckReport rep2;
    CHECK(check_filesystem(*dev, 0, dev->size(), FsType::Unknown, FsCheckOptions{}, &rep2).ok());
    CHECK(rep2.fs == c.fs);
    CHECK(rep2.verdict == FsVerdict::Clean);
    CHECK_EQ(dev->writes, 0);
    // 読み取り専用で開いたデバイスでも検査できる
    auto ro = std::make_shared<MemoryDevice>(dev->bytes(), true);
    FsCheckReport rep3;
    CHECK(check_filesystem(*ro, 0, ro->size(), c.fs, FsCheckOptions{}, &rep3).ok());
    CHECK(rep3.verdict == FsVerdict::Clean);
  }
}

TEST(disktools_check_options_limits) {
  auto dev = make_dev(8u << 20, 0);
  MkfsPlan plan;
  CHECK(Mkfs::plan(*dev, opts(FsType::Exfat), &plan).ok());
  CHECK(Mkfs::apply(*dev, plan, token_of(plan), with_backup()).ok());
  // キャンセル済み: 途中で止まり Incomplete
  CancelToken ct;
  ct.cancel();
  FsCheckOptions fo;
  fo.cancel = &ct;
  FsCheckReport rep;
  CHECK(check_filesystem(*dev, 0, dev->size(), FsType::Exfat, fo, &rep).code == Errc::Cancelled);   // 報告は途中までの分が残る
  CHECK(rep.verdict == FsVerdict::Incomplete);
  // メモリ上限が極端に小さい
  FsCheckOptions lo;
  lo.memory_limit = 16;
  FsCheckReport rep2;
  CHECK(check_filesystem(*dev, 0, dev->size(), FsType::Exfat, lo, &rep2).ok());
  CHECK(rep2.verdict == FsVerdict::Incomplete || rep2.verdict == FsVerdict::Errors);
  // 範囲外の指定
  FsCheckReport rep3;
  CHECK(!check_filesystem(*dev, dev->size() + 1, 512, FsType::Exfat, FsCheckOptions{}, &rep3).ok());
  // findings の上限: 壊した FAT で大量に出ても max_findings で打ち切る (件数は数え続ける)
  for (size_t i = 0; i < 4096; i++) dev->bytes()[7u * 512 * 1024 / 8 + i] = 0;   // 乱暴に壊す (どこでもよい)
  FsCheckOptions mo;
  mo.max_findings = 3;
  FsCheckReport rep4;
  CHECK(check_filesystem(*dev, 0, dev->size(), FsType::Exfat, mo, &rep4).ok());
  CHECK(rep4.findings.size() <= 3);
  // JSON は常に出せる
  CHECK(!fscheck_report_to_json(rep4).empty());
}

// 破損入力で落ちない・止まる (ASan/UBSan と組み合わせる)
TEST(disktools_fuzz_checkers_do_not_crash) {
  uint64_t x = 88172645463325252ull;
  auto rnd = [&] { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
  for (auto& c : kCases) {
    auto base = make_dev(c.size, 0);
    MkfsPlan plan;
    CHECK(Mkfs::plan(*base, opts(c.fs), &plan).ok());
    CHECK(Mkfs::apply(*base, plan, token_of(plan), with_backup()).ok());
    // 書かれた領域 (メタデータ) のうち、小さい領域を壊す対象にする
    std::vector<std::pair<uint64_t, uint64_t>> targets;
    for (auto& st : plan.stages) for (auto& r : st.regions) if (!r.data.empty() && st.name != "invalidate-old") targets.push_back({r.offset, r.length});
    CHECK(!targets.empty());
    const std::vector<uint8_t> orig = base->bytes();
    int iters = c.fs == FsType::Fat32 ? 60 : 150;
    for (int it = 0; it < iters; it++) {
      auto dev = std::make_shared<MemoryDevice>(orig, false);
      int flips = 1 + static_cast<int>(rnd() % 6);
      for (int k = 0; k < flips; k++) {
        auto t = targets[rnd() % targets.size()];
        uint64_t pos = t.first + rnd() % t.second;
        uint64_t mode = rnd() % 4;
        if (mode == 0) dev->bytes()[pos] = static_cast<uint8_t>(rnd());
        else if (mode == 1) dev->bytes()[pos] ^= static_cast<uint8_t>(1u << (rnd() % 8));
        else if (mode == 2) dev->bytes()[pos] = 0xFF;
        else { for (int q = 0; q < 8 && pos + static_cast<uint64_t>(q) < dev->bytes().size(); q++) dev->bytes()[pos + static_cast<uint64_t>(q)] = static_cast<uint8_t>(rnd()); }
      }
      FsCheckReport rep;
      FsCheckOptions fo;
      fo.max_findings = 50;
      fo.max_objects = 100000;
      Status st = check_filesystem(*dev, 0, dev->size(), c.fs, fo, &rep);
      CHECK(st.ok() || st.code == Errc::Io || st.code == Errc::OutOfRange);
      CHECK(!fscheck_report_to_json(rep).empty());
    }
  }
}

// ---- C API
namespace {
std::string take(char* p) {
  std::string s = p ? p : "";
  usbmem_free(p);
  return s;
}
bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }
}  // namespace

TEST(disktools_capi_mkfs_and_check) {
  std::string path = test_tmpdir() + "/disktools_capi_" + std::to_string(::getpid()) + ".img";
  {
    FILE* f = std::fopen(path.c_str(), "wb");
    CHECK(f != nullptr);
    if (!f) return;
    std::vector<uint8_t> z(1 << 20, 0);
    for (int i = 0; i < 8; i++) std::fwrite(z.data(), 1, z.size(), f);
    std::fclose(f);
  }
  usbmem_device* dev = nullptr;
  CHECK_EQ(usbmem_open_path(path.c_str(), 0, &dev), 0);
  usbmem_mkfs* mk = nullptr;
  CHECK_EQ(usbmem_mkfs_open(dev, "exfat", 0, 0, "CAPI", 0, 0, 0, 0, 0, 5, 1700000000, &mk), 0);
  char* pj = nullptr;
  CHECK_EQ(usbmem_mkfs_plan_json(mk, &pj), 0);
  std::string plan = take(pj);
  CHECK(has(plan, "\"ok\":true"));
  CHECK(has(plan, "\"confirmPhrase\":\"FORMAT "));
  // 拒否: 間違ったトークン / マウント中
  char* rj = nullptr;
  CHECK(usbmem_mkfs_apply(mk, "nope", nullptr, 0, 0, nullptr, &rj) != 0);
  CHECK(has(take(rj), "\"code\":\"bad-token\""));
  // 確認文字列を JSON から取り出す
  size_t k = plan.find("\"confirmPhrase\":\"") + 17;
  std::string phrase = plan.substr(k, plan.find('"', k) - k);
  char* tok = nullptr;
  CHECK_EQ(usbmem_mkfs_make_token(mk, phrase.c_str(), &tok), 0);
  std::string token = take(tok);
  CHECK(usbmem_mkfs_apply(mk, token.c_str(), nullptr, 0, 1, nullptr, &rj) != 0);   // in_use
  CHECK(has(take(rj), "\"code\":\"in-use\""));
  usbmem_job* job = usbmem_job_new();
  CHECK_EQ(usbmem_mkfs_apply(mk, token.c_str(), nullptr, 0, 0, job, &rj), 0);
  std::string res = take(rj);
  CHECK(has(res, "\"verified\":true"));
  char ph[32];
  unsigned long long done = 0, total = 0;
  usbmem_job_progress(job, ph, sizeof ph, &done, &total);
  CHECK(done > 0);
  usbmem_job_free(job);
  // 適用後は計画が使えない
  CHECK(usbmem_mkfs_apply(mk, token.c_str(), nullptr, 0, 0, nullptr, &rj) != 0);
  CHECK(has(take(rj), "\"code\":\"no-plan\""));
  unsigned char* bk = nullptr;
  size_t bl = 0;
  CHECK_EQ(usbmem_mkfs_last_backup(mk, &bk, &bl), 0);
  char* ij = nullptr;
  CHECK_EQ(usbmem_mkfs_backup_info_json(bk, bl, &ij), 0);
  CHECK(has(take(ij), "\"regions\""));
  usbmem_free(bk);
  usbmem_mkfs_close(mk);
  // 検査
  char* cj = nullptr;
  CHECK_EQ(usbmem_fscheck_run(dev, 0, 0, nullptr, 0, 0, nullptr, &cj), 0);
  std::string rep = take(cj);
  CHECK(has(rep, "\"verdict\":\"clean\""));
  CHECK(has(rep, "\"fs\":\"exfat\""));
  CHECK(usbmem_fscheck_supported("ntfs") == 1);
  CHECK(usbmem_fscheck_supported("btrfs") == 0);
  CHECK(usbmem_fscheck_run(dev, 0, 0, "nonsense", 0, 0, nullptr, &cj) != 0);
  // キャンセル
  usbmem_job* j2 = usbmem_job_new();
  usbmem_job_cancel(j2);
  int rc = usbmem_fscheck_run(dev, 0, 0, "exfat", 0, 0, j2, &cj);
  CHECK_EQ(rc, static_cast<int>(Errc::Cancelled));
  CHECK(has(take(cj), "\"verdict\":\"incomplete\""));
  usbmem_job_free(j2);
  // 不正な引数
  CHECK(usbmem_mkfs_open(dev, "ntfs", 0, 0, nullptr, 0, 0, 0, 0, 0, 0, 0, &mk) != 0);
  CHECK(usbmem_mkfs_open(nullptr, "ext2", 0, 0, nullptr, 0, 0, 0, 0, 0, 0, 0, &mk) != 0);
  usbmem_close(dev);
  ::unlink(path.c_str());
}
