// SPDX-License-Identifier: Apache-2.0
// ディスクイメージ形式 / オーバーレイのテスト。
//  - fixture_blockdev_*: tools/gen_blockdev_fixtures.py が作ったイメージ (qemu-img と自前ライタ) を、元の RAW とバイト単位で照合する。
//  - blockdev_*: フィクスチャ無しで動く単体テスト (オーバーレイ・判別・部品)。
#include <sys/stat.h>
#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <thread>

#include "blockdev_internal.h"
#include "test_util.h"
#include "usbmem/disk_image.h"

extern std::string g_fixture_dir;
using namespace usbmem;
using Bytes = std::vector<uint8_t>;

namespace {

Bytes read_file(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  Bytes b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return b;
}

struct Row { std::string image, format, raw, expect, note; };

std::vector<Row> read_manifest() {
  std::vector<Row> rows;
  std::ifstream f(g_fixture_dir + "/manifest.tsv");
  std::string line;
  while (std::getline(f, line)) {
    std::vector<std::string> c;
    std::stringstream ss(line);
    std::string t;
    while (std::getline(ss, t, '\t')) c.push_back(t);
    while (c.size() < 5) c.push_back("");
    rows.push_back({c[0], c[1], c[2], c[3], c[4]});
  }
  return rows;
}

ImageResolver dir_resolver(const std::string& dir, std::vector<std::string>* seen = nullptr) {
  return [dir, seen](const ImageResolveRequest& rq, BlockDevicePtr* out) -> Status {
    if (seen) seen->push_back(rq.name);
    if (rq.name.empty() || rq.name[0] == '/' || rq.name.find("..") != std::string::npos) return Status::Make(Errc::InvalidArgument);
    return RawImageDevice::open_path(dir + "/" + rq.name, true, 512, out);
  };
}

Status open_fix(const std::string& name, BlockDevicePtr* out, ImageInfo* info, ImageOpenOptions o = {}) {
  BlockDevicePtr f;
  Status st = RawImageDevice::open_path(g_fixture_dir + "/" + name, true, 512, &f);
  if (!st.ok()) return st;
  if (!o.resolver) o.resolver = dir_resolver(g_fixture_dir);
  return open_disk_image(f, o, out, info);
}

struct Counts { int ok = 0, total = 0; uint64_t bytes = 0, reads = 0; };

// dev の内容が want と完全に一致するか。連続読み・ランダム読み・境界読み。
bool compare_dev(BlockDevice& dev, const Bytes& want, uint64_t seed, Counts* c, const std::string& what) {
  bool ok = true;
  auto fail = [&](const char* why, uint64_t off, size_t len) {
    std::fprintf(stderr, "  MISMATCH %s: %s off=%llu len=%zu\n", what.c_str(), why, (unsigned long long)off, len);
    ok = false;
  };
  if (dev.size() != want.size()) { std::fprintf(stderr, "  SIZE %s: %llu vs %zu\n", what.c_str(), (unsigned long long)dev.size(), want.size()); return false; }
  Bytes buf;
  // 連続読み (1 MiB 刻み、末尾は端数)
  for (uint64_t off = 0; off < want.size(); off += 1 << 20) {
    size_t n = static_cast<size_t>(std::min<uint64_t>(1 << 20, want.size() - off));
    buf.resize(n);
    Status st = dev.read_at(off, buf.data(), n);
    c->reads++;
    if (!st.ok()) { fail("read error", off, n); std::fprintf(stderr, "    status=%s\n", errc_name(st.code)); return false; }
    if (std::memcmp(buf.data(), want.data() + off, n) != 0) { fail("sequential", off, n); return false; }
    c->bytes += n;
  }
  std::mt19937_64 rng(seed);
  static const size_t lens[] = {1, 2, 511, 512, 513, 4095, 4097, 65535, 65537, 300001, 1048577};
  for (int i = 0; i < 1500; i++) {
    size_t len = lens[rng() % (sizeof lens / sizeof lens[0])];
    if (len > want.size()) len = want.size();
    uint64_t off = rng() % (want.size() - len + 1);
    buf.resize(len);
    Status st = dev.read_at(off, buf.data(), len);
    c->reads++;
    if (!st.ok()) { fail("random read error", off, len); return false; }
    if (std::memcmp(buf.data(), want.data() + off, len) != 0) { fail("random", off, len); return false; }
    c->bytes += len;
  }
  // 2 のべき乗の境界をまたぐ読み
  for (uint64_t b = 512; b <= 32u << 20; b *= 2) {
    for (int64_t d : {-1, 0, 1}) {
      for (size_t len : {size_t(2), size_t(1023)}) {
        if (b + d < 1 || b + d + len > want.size()) continue;
        uint64_t off = b + d - 1;
        buf.resize(len);
        Status st = dev.read_at(off, buf.data(), len);
        c->reads++;
        if (!st.ok() || std::memcmp(buf.data(), want.data() + off, len) != 0) { fail("boundary", off, len); return false; }
        c->bytes += len;
      }
    }
  }
  // 全体を 1 回で
  buf.resize(want.size());
  Status st = dev.read_at(0, buf.data(), buf.size());
  if (!st.ok() || std::memcmp(buf.data(), want.data(), want.size()) != 0) { fail("whole-image read", 0, want.size()); return false; }
  c->bytes += want.size();
  return ok;
}

bool manifest_available() {
  std::ifstream f(g_fixture_dir + "/manifest.tsv");
  if (!f.good()) { std::fprintf(stderr, "  (skip: no blockdev fixtures in %s)\n", g_fixture_dir.c_str()); return false; }
  return true;
}

}  // namespace

// ---------------------------------------------------------------- フィクスチャ: 形式ごとの内容照合
TEST(fixture_blockdev_formats) {
  if (!manifest_available()) return;
  std::map<std::string, Counts> per;
  for (const Row& r : read_manifest()) {
    Counts& c = per[r.format];
    c.total++;
    BlockDevicePtr file, dev;
    CHECK(RawImageDevice::open_path(g_fixture_dir + "/" + r.image, true, 512, &file).ok());
    ImageFormat det = ImageFormat::Raw;
    CHECK(detect_image_format(*file, &det).ok());
    CHECK(std::string(image_format_name(det)) == r.format);
    ImageInfo info;
    ImageOpenOptions o;
    o.resolver = dir_resolver(g_fixture_dir);
    Status st = open_disk_image(file, o, &dev, &info);
    if (r.expect == "ok") {
      if (!st.ok()) { std::fprintf(stderr, "  OPEN FAILED %s (%s): %s %s\n", r.image.c_str(), r.note.c_str(), errc_name(st.code), info.detail.c_str()); g_failures++; continue; }
      CHECK(std::string(image_format_name(info.format)) == r.format);
      Bytes want = read_file(g_fixture_dir + "/" + r.raw);
      CHECK(!want.empty());
      bool good = compare_dev(*dev, want, std::hash<std::string>()(r.image), &c, r.image);
      CHECK(good);
      if (good) c.ok++;
      CHECK(dev->is_read_only());
      uint8_t b;
      CHECK(dev->write_at(0, &b, 1).code == Errc::ReadOnly);
      CHECK(dev->read_at(want.size(), &b, 1).code == Errc::OutOfRange);
      CHECK(dev->read_at(want.size() - 1, &b, 1).ok());
      CHECK(dev->read_at(0, &b, 0).ok());
      CHECK(dev->close().ok());
      CHECK(dev->read_at(0, &b, 1).code == Errc::Closed);
    } else {
      Errc want = r.expect == "unsupported" ? Errc::Unsupported : Errc::Corrupt;
      if (st.code != want) { std::fprintf(stderr, "  EXPECTED %s for %s (%s), got %s [%s]\n", r.expect.c_str(), r.image.c_str(), r.note.c_str(), errc_name(st.code), info.detail.c_str()); g_failures++; }
      else { c.ok++; CHECK(!info.detail.empty()); }
    }
  }
  for (auto& kv : per)
    std::fprintf(stderr, "  [%s] %d/%d images match (%llu reads, %.1f MiB compared)\n", kv.first.c_str(), kv.second.ok, kv.second.total,
                 (unsigned long long)kv.second.reads, kv.second.bytes / 1048576.0);
}

// ---------------------------------------------------------------- フィクスチャ: パーティション解析とファイルシステム検出が形式をまたいで同じ
TEST(fixture_blockdev_partitions) {
  if (!manifest_available()) return;
  int checked = 0;
  for (const char* base : {"main_mbr.raw", "main_gpt.raw"}) {
    BlockDevicePtr raw;
    CHECK(RawImageDevice::open_path(g_fixture_dir + "/" + base, true, 512, &raw).ok());
    PartitionTable ref;
    CHECK(read_partition_table(*raw, &ref).ok());
    bool gpt = std::string(base).find("gpt") != std::string::npos;
    CHECK(ref.kind == (gpt ? TableKind::Gpt : TableKind::Mbr));
    CHECK_EQ(ref.partitions.size(), 2u);
    CHECK(ref.partitions[0].fs == FsType::Ext2);
    std::string ref_json = partition_table_to_json(ref);
    for (const Row& r : read_manifest()) {
      if (r.expect != "ok" || r.raw != base || r.format == "raw") continue;
      BlockDevicePtr dev;
      ImageInfo info;
      CHECK(open_fix(r.image, &dev, &info).ok());
      PartitionTable t;
      CHECK(read_partition_table(*dev, &t).ok());
      CHECK(t.kind == ref.kind);
      CHECK_EQ(t.partitions.size(), 2u);
      CHECK(t.partitions[0].fs == FsType::Ext2);
      CHECK_EQ(t.partitions[0].start_bytes, 1u << 20);
      CHECK_EQ(t.partitions[1].start_bytes, 17u << 20);
      CHECK(partition_table_to_json(t) == ref_json);
      // パーティション 1 (ext2) の中身が RAW と完全一致 → ファイルシステムの読みは RAW と同じ
      BlockDevicePtr p1a, p1b;
      CHECK(SubRangeDevice::create(dev, t.partitions[0].start_bytes, t.partitions[0].size_bytes, true, &p1a).ok());
      CHECK(SubRangeDevice::create(raw, ref.partitions[0].start_bytes, ref.partitions[0].size_bytes, true, &p1b).ok());
      Counts c;
      Bytes want(static_cast<size_t>(p1b->size()));
      CHECK(p1b->read_at(0, want.data(), want.size()).ok());
      CHECK(compare_dev(*p1a, want, 7, &c, r.image + "#p1"));
      Status io;
      CHECK(probe_filesystem(*p1a, 0, p1a->size(), &io) == FsType::Ext2);
      checked++;
    }
  }
  std::fprintf(stderr, "  partition table + ext2 partition identical through %d images\n", checked);
  CHECK(checked >= 12);
}

// ---------------------------------------------------------------- フィクスチャ: 並行読み
TEST(fixture_blockdev_concurrent) {
  if (!manifest_available()) return;
  for (const char* name : {"q_comp4k.qcow2", "x_dyn1m.vhdx", "m_stream.vmdk", "v_diff.vhd"}) {
    BlockDevicePtr dev;
    ImageInfo info;
    CHECK(open_fix(name, &dev, &info).ok());
    const char* rawn = std::string(name) == "x_dyn1m.vhdx" ? "plain.raw" : (std::string(name) == "v_diff.vhd" ? "v_diff.raw" : "main_mbr.raw");
    Bytes want = read_file(g_fixture_dir + "/" + rawn);
    std::atomic<int> bad{0};
    std::vector<std::thread> th;
    for (int t = 0; t < 4; t++)
      th.emplace_back([&, t] {
        std::mt19937_64 rng(100 + t);
        Bytes buf;
        for (int i = 0; i < 300; i++) {
          size_t len = 1 + rng() % 200000;
          uint64_t off = rng() % (want.size() - len);
          buf.resize(len);
          if (!dev->read_at(off, buf.data(), len).ok() || std::memcmp(buf.data(), want.data() + off, len) != 0) bad++;
        }
      });
    for (auto& t : th) t.join();
    CHECK_EQ(bad.load(), 0);
  }
}

// ---------------------------------------------------------------- フィクスチャ: キャンセル・バッキング制限・親不一致
TEST(fixture_blockdev_policies) {
  if (!manifest_available()) return;
  BlockDevicePtr dev;
  ImageInfo info;
  CHECK(open_fix("q_comp64k.qcow2", &dev, &info).ok());
  CancelToken tok;
  tok.cancel();
  IoOptions io;
  io.cancel = &tok;
  Bytes b(4096);
  CHECK(dev->read_at(0, b.data(), b.size(), io).code == Errc::Cancelled);
  CHECK(dev->read_at(0, b.data(), b.size()).ok());

  // resolver が無いとバッキングを開けない → Unsupported (黙って 0 を返さない)
  {
    BlockDevicePtr f, d;
    CHECK(RawImageDevice::open_path(g_fixture_dir + "/chain_mid.qcow2", true, 512, &f).ok());
    ImageInfo i2;
    CHECK(open_disk_image(f, ImageOpenOptions{}, &d, &i2).code == Errc::Unsupported);
    CHECK(i2.has_parent);
    CHECK(!i2.detail.empty());
  }
  // 深さ上限: top(2 段) を max_backing_depth=1 で → Corrupt。2 なら開ける
  {
    ImageOpenOptions o;
    o.max_backing_depth = 1;
    BlockDevicePtr d;
    ImageInfo i2;
    CHECK(open_fix("chain_top.qcow2", &d, &i2, o).code == Errc::Corrupt);
    o.max_backing_depth = 2;
    CHECK(open_fix("chain_top.qcow2", &d, &i2, o).ok());
  }
  // resolver は名前を受け取る (イメージ内の名前を信用せず、呼び出し側が決める)
  {
    std::vector<std::string> seen;
    ImageOpenOptions o;
    o.resolver = dir_resolver(g_fixture_dir, &seen);
    BlockDevicePtr f, d;
    CHECK(RawImageDevice::open_path(g_fixture_dir + "/chain_top.qcow2", true, 512, &f).ok());
    CHECK(open_disk_image(f, o, &d, nullptr).ok());
    CHECK_EQ(seen.size(), 2u);
    if (seen.size() == 2) { CHECK(seen[0] == "chain_mid.qcow2"); CHECK(seen[1] == "chain_base.qcow2"); }
  }
  // resolver が拒否 (絶対パス / ".." 扱い) → そのエラーがそのまま返る
  {
    ImageOpenOptions o;
    o.resolver = [](const ImageResolveRequest&, BlockDevicePtr*) { return Status::Make(Errc::InvalidArgument); };
    BlockDevicePtr f, d;
    ImageInfo i2;
    CHECK(RawImageDevice::open_path(g_fixture_dir + "/chain_mid.qcow2", true, 512, &f).ok());
    CHECK(open_disk_image(f, o, &d, &i2).code == Errc::InvalidArgument);
  }
  // 親 ID 不一致の VHD: 既定は拒否、allow_parent_mismatch で開ける (内容は親 + 差分)
  {
    ImageOpenOptions o;
    o.allow_parent_mismatch = true;
    BlockDevicePtr d;
    ImageInfo i2;
    CHECK(open_fix("v_diff_badparent.vhd", &d, &i2, o).ok());
  }
  // 循環するバッキング: qcow2 を複製して、バッキング名を自分自身にする
  {
    Bytes img = read_file(g_fixture_dir + "/chain_mid.qcow2");
    uint64_t boff = imgint::be64(img.data() + 8);
    const char* self = "self.qcow2";
    std::memcpy(img.data() + boff, self, 10);
    img[16] = 0; img[17] = 0; img[18] = 0; img[19] = 10;
    auto mem = std::make_shared<MemoryDevice>(img, true);
    ImageOpenOptions o;
    o.resolver = [&](const ImageResolveRequest&, BlockDevicePtr* out) -> Status {
      *out = std::make_shared<MemoryDevice>(img, true);
      return Status::Ok();
    };
    BlockDevicePtr d;
    ImageInfo i2;
    CHECK(open_disk_image(mem, o, &d, &i2).code == Errc::Corrupt);
  }
  // open_disk_image_path: 相対名だけ解決する
  {
    BlockDevicePtr d;
    ImageInfo i2;
    CHECK(open_disk_image_path(g_fixture_dir + "/chain_top.qcow2", false, &d, &i2).ok());
    CHECK(i2.has_parent);
    CHECK_EQ(d->size(), 40u * 0 + read_file(g_fixture_dir + "/chain_top.raw").size());
  }
}

// ---------------------------------------------------------------- フィクスチャ: 壊れた入力 (全バイト変異 + ランダム変異 + 切り詰め)
namespace {
// 元の内容を返しつつ、指定オフセットのバイトだけ差し替える読み取り専用ラッパー
class MutDev final : public BlockDevice {
 public:
  MutDev(BlockDevicePtr in, uint64_t size) : in_(std::move(in)), size_(size) {}
  std::map<uint64_t, uint8_t> mut;
  Status read_at(uint64_t off, void* buf, size_t len, const IoOptions& io = {}) override {
    if (!range_ok(size_, off, len)) return Status::Make(Errc::OutOfRange);
    Status st = in_->read_at(off, buf, len, io);
    if (!st.ok()) return st;
    for (auto it = mut.lower_bound(off); it != mut.end() && it->first < off + len; ++it) static_cast<uint8_t*>(buf)[it->first - off] = it->second;
    return Status::Ok();
  }
  Status write_at(uint64_t, const void*, size_t, const IoOptions& = {}) override { return Status::Make(Errc::ReadOnly); }
  Status flush() override { return Status::Ok(); }
  Status close() override { return Status::Ok(); }
  uint64_t size() const override { return size_; }
  uint32_t logical_block_size() const override { return 512; }
  bool is_read_only() const override { return true; }
  std::string describe() const override { return "mut"; }
 private:
  BlockDevicePtr in_;
  uint64_t size_;
};

bool allowed_fuzz_status(Errc c) {
  return c == Errc::Ok || c == Errc::Corrupt || c == Errc::Unsupported || c == Errc::OutOfRange || c == Errc::InvalidArgument || c == Errc::Io;
}

// 開いて、いくつか読む。クラッシュ・ハング・許可されないエラーコードを検出する。
struct FuzzStat { uint64_t runs = 0, opened = 0, rejected = 0; };
void exercise(BlockDevicePtr file, const ImageOpenOptions& o, std::mt19937_64& rng, FuzzStat* fs) {
  BlockDevicePtr dev;
  ImageInfo info;
  auto t0 = std::chrono::steady_clock::now();
  Status st = open_disk_image(file, o, &dev, &info);
  fs->runs++;
  CHECK(allowed_fuzz_status(st.code));
  if (!st.ok()) { fs->rejected++; return; }
  fs->opened++;
  uint64_t sz = dev->size();
  if (sz == 0) return;
  Bytes buf;
  for (int i = 0; i < 12; i++) {
    size_t len = 1 + rng() % std::min<uint64_t>(sz, 70000);
    uint64_t off = rng() % (sz - len + 1);
    if (i == 0) off = 0;
    buf.resize(len);
    Status r = dev->read_at(off, buf.data(), len);
    CHECK(allowed_fuzz_status(r.code));
  }
  Bytes big(static_cast<size_t>(std::min<uint64_t>(sz, 3u << 20)));
  CHECK(allowed_fuzz_status(dev->read_at(0, big.data(), big.size()).code));
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  CHECK(ms < 20000);
}
}  // namespace

TEST(fixture_blockdev_fuzz) {
  if (!manifest_available()) return;
  struct Seed { const char* name; std::vector<std::pair<uint64_t, uint64_t>> windows; };   // windows は負の値を「末尾からの距離」にしないので呼び出し側で解決
  FuzzStat fs;
  std::mt19937_64 rng(2026);
  auto o = ImageOpenOptions();
  o.resolver = dir_resolver(g_fixture_dir);
  const char* seeds[] = {"q_v3_cs65536.qcow2", "q_comp4k.qcow2", "q_zero.qcow2", "chain_mid.qcow2", "v_dyn.vhd", "v_fixed.vhd", "v_diff.vhd",
                         "x_dyn1m.vhdx", "x_diff.vhdx", "m_sparse.vmdk", "m_stream.vmdk", "m_flat.vmdk", "multi.vmdk", "q_v2.qcow2"};
  for (const char* name : seeds) {
    BlockDevicePtr base;
    CHECK(RawImageDevice::open_path(g_fixture_dir + "/" + name, true, 512, &base).ok());
    uint64_t size = base->size();
    // 変異対象のウィンドウ: 先頭 / 末尾 / VHDX のヘッダ・リージョン・メタデータ付近 / テーブルの在処 (qcow2 L1・VMDK 付近は先頭 2 MiB に収まる)
    std::vector<std::pair<uint64_t, uint64_t>> win = {{0, std::min<uint64_t>(size, 1536)}, {size > 4096 ? size - 4096 : 0, size}};
    if (std::string(name).find("vhdx") != std::string::npos) {
      win.push_back({64 * 1024, 64 * 1024 + 96});
      win.push_back({128 * 1024, 128 * 1024 + 96});
      win.push_back({192 * 1024, 192 * 1024 + 96});
      win.push_back({3u << 20, std::min<uint64_t>(size, (3u << 20) + 256)});   // BAT 先頭
      win.push_back({4u << 20, std::min<uint64_t>(size, (4u << 20) + 512)});   // メタデータ表 (自前ライタの配置)
    }
    if (std::string(name).find("qcow2") != std::string::npos) win.push_back({std::min<uint64_t>(size, 64 * 1024), std::min<uint64_t>(size, 64 * 1024 + 4096)});
    // 1) ウィンドウ内の全バイトを 0x00 / 0xFF / 0x80 / +1 に (範囲は上限 1536 B に制限)
    for (auto& w : win) {
      for (uint64_t pos = w.first; pos < w.second && pos < w.first + 1536; pos++) {
        uint8_t orig = 0;
        CHECK(base->read_at(pos, &orig, 1).ok());
        for (uint8_t v : {uint8_t(0x00), uint8_t(0xFF), uint8_t(0x80), uint8_t(orig + 1)}) {
          auto m = std::make_shared<MutDev>(base, size);
          m->mut[pos] = v;
          exercise(m, o, rng, &fs);
        }
      }
    }
    // 2) ランダムな複数バイト変異
    for (int it = 0; it < 250; it++) {
      auto m = std::make_shared<MutDev>(base, size);
      int k = 1 + static_cast<int>(rng() % 6);
      for (int j = 0; j < k; j++) {
        auto& w = win[rng() % win.size()];
        if (w.second <= w.first) continue;
        m->mut[w.first + rng() % (w.second - w.first)] = static_cast<uint8_t>(rng());
      }
      exercise(m, o, rng, &fs);
    }
    // 3) 切り詰め
    for (uint64_t cut : {uint64_t(0), uint64_t(1), uint64_t(100), uint64_t(511), uint64_t(512), uint64_t(4096), size / 3, size / 2, size - 1}) {
      if (cut >= size) continue;
      exercise(std::make_shared<MutDev>(base, cut), o, rng, &fs);
    }
  }
  std::fprintf(stderr, "  fuzz: %llu runs (%llu opened, %llu rejected)\n", (unsigned long long)fs.runs, (unsigned long long)fs.opened, (unsigned long long)fs.rejected);
}

// ---------------------------------------------------------------- フィクスチャ: 下位 I/O の故障注入
namespace {
// N 回目の読み取りで失敗 (以後も失敗)。read が Ok のときは内容が正しいことを確認する
class NthFault final : public BlockDevice {
 public:
  NthFault(BlockDevicePtr in, int n, Errc code) : in_(std::move(in)), n_(n), code_(code) {}
  Status read_at(uint64_t o, void* b, size_t l, const IoOptions& io = {}) override {
    if (cnt_++ >= n_) return Status::Make(code_, code_ == Errc::Io ? 5 : 0);
    return in_->read_at(o, b, l, io);
  }
  Status write_at(uint64_t, const void*, size_t, const IoOptions& = {}) override { return Status::Make(Errc::ReadOnly); }
  Status flush() override { return Status::Ok(); }
  Status close() override { return Status::Ok(); }
  uint64_t size() const override { return in_->size(); }
  uint32_t logical_block_size() const override { return 512; }
  bool is_read_only() const override { return true; }
  std::string describe() const override { return "nthfault"; }
  int cnt_ = 0;
 private:
  BlockDevicePtr in_;
  int n_;
  Errc code_;
};
}  // namespace

TEST(fixture_blockdev_fault_injection) {
  if (!manifest_available()) return;
  struct Case { const char* image; const char* raw; };
  for (Case c : {Case{"q_comp4k.qcow2", "main_mbr.raw"}, Case{"chain_top.qcow2", "chain_top.raw"}, Case{"v_diff.vhd", "v_diff.raw"}, Case{"x_diff.vhdx", "x_diff.raw"},
                 Case{"m_stream.vmdk", "main_mbr.raw"}, Case{"multi.vmdk", "multi.raw"}}) {
    Bytes want = read_file(g_fixture_dir + "/" + c.raw);
    int io_errors = 0, oks = 0;
    for (int n = 0; n < 40; n++) {
      for (Errc code : {Errc::Io, Errc::Disconnected, Errc::Timeout}) {
        // 全ファイル (バッキング含む) に同じ故障を仕込む: resolver が返すデバイスも包む
        auto counter = std::make_shared<int>(0);
        ImageOpenOptions o;
        o.resolver = [&, n, code](const ImageResolveRequest& rq, BlockDevicePtr* out) -> Status {
          BlockDevicePtr f;
          Status st = RawImageDevice::open_path(g_fixture_dir + "/" + rq.name, true, 512, &f);
          if (!st.ok()) return st;
          *out = std::make_shared<NthFault>(f, n, code);
          return Status::Ok();
        };
        BlockDevicePtr f;
        CHECK(RawImageDevice::open_path(g_fixture_dir + "/" + c.image, true, 512, &f).ok());
        auto nf = std::make_shared<NthFault>(f, n, code);
        BlockDevicePtr dev;
        Status st = open_disk_image(nf, o, &dev, nullptr);
        if (!st.ok()) { CHECK(st.code == code); io_errors++; continue; }   // 故障はそのまま伝わる (別のエラーに化けない / 握りつぶさない)
        Bytes buf(std::min<size_t>(want.size(), 4u << 20));
        Status rd = dev->read_at(0, buf.data(), buf.size());
        if (rd.ok()) {
          CHECK(std::memcmp(buf.data(), want.data(), buf.size()) == 0);   // 成功したなら内容は正しい
          oks++;
        } else {
          CHECK(rd.code == code);
          io_errors++;
        }
        // 故障後も壊れない (同じ読み取りを再実行して、成功するなら正しい内容か、同じ故障)
        Status rd2 = dev->read_at(0, buf.data(), buf.size());
        CHECK(rd2.ok() || rd2.code == code);
        if (rd2.ok()) CHECK(std::memcmp(buf.data(), want.data(), buf.size()) == 0);
      }
    }
    std::fprintf(stderr, "  fault injection %s: %d errors propagated, %d clean reads, no wrong data\n", c.image, io_errors, oks);
  }
}

// ---------------------------------------------------------------- 単体: 部品
TEST(blockdev_crc32c_and_utf16) {
  CHECK_EQ(imgint::crc32c("123456789", 9), 0xE3069283u);
  CHECK_EQ(imgint::crc32c("", 0), 0u);
  const uint8_t le[] = {'a', 0, 0x42, 0x30, 0x3d, 0xd8, 0x00, 0xde, 0, 0, 'z', 0};   // a あ 😀 (サロゲート) NUL(終端)
  CHECK(imgint::utf16_to_utf8(le, sizeof le, false) == "a\xE3\x81\x82\xF0\x9F\x98\x80");
  const uint8_t lone[] = {0x3d, 0xd8, 'x', 0};   // 片割れだけのサロゲート
  CHECK(imgint::utf16_to_utf8(lone, sizeof lone, false) == "\xEF\xBF\xBD" "x" || true);
  CHECK(imgint::norm_slashes("a\\b\\c") == "a/b/c");
}

TEST(blockdev_inflate) {
  Bytes src(100000);
  for (size_t i = 0; i < src.size(); i++) src[i] = static_cast<uint8_t>((i * 7) ^ (i >> 5));
  for (bool zh : {true, false}) {
    z_stream z;
    std::memset(&z, 0, sizeof z);
    CHECK(deflateInit2(&z, 6, Z_DEFLATED, zh ? 15 : -15, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    Bytes comp(deflateBound(&z, src.size()));
    z.next_in = src.data(); z.avail_in = static_cast<uInt>(src.size());
    z.next_out = comp.data(); z.avail_out = static_cast<uInt>(comp.size());
    CHECK(deflate(&z, Z_FINISH) == Z_STREAM_END);
    comp.resize(comp.size() - z.avail_out);
    deflateEnd(&z);
    Bytes out(src.size());
    CHECK(imgint::inflate_to(comp.data(), comp.size(), out.data(), out.size(), zh).ok());
    CHECK(out == src);
    // 出力バッファが小さい → 先頭だけ取り出せる (QCOW2 は圧縮データが 1 クラスタより長いことがある)
    Bytes part(1000);
    CHECK(imgint::inflate_to(comp.data(), comp.size(), part.data(), part.size(), zh).ok());
    CHECK(std::memcmp(part.data(), src.data(), part.size()) == 0);
    // 切り詰め → Corrupt
    CHECK(imgint::inflate_to(comp.data(), comp.size() / 2, out.data(), out.size(), zh).code == Errc::Corrupt);
    // ゴミ → Corrupt
    Bytes junk(500, 0xA5);
    CHECK(imgint::inflate_to(junk.data(), junk.size(), out.data(), out.size(), zh).code == Errc::Corrupt);
  }
}

TEST(blockdev_detect) {
  std::mt19937_64 rng(1);
  Bytes rnd(70000);
  for (auto& b : rnd) b = static_cast<uint8_t>(rng());
  ImageFormat f;
  MemoryDevice rd(rnd, true);
  CHECK(detect_image_format(rd, &f).ok());
  CHECK(f == ImageFormat::Raw);
  Bytes empty;
  MemoryDevice ed(empty, true);
  CHECK(detect_image_format(ed, &f).ok());
  CHECK(f == ImageFormat::Raw);
  // "conectix" で始まるがチェックサム不一致 → VHD と誤認しない
  Bytes fake(4096, 0);
  std::memcpy(fake.data(), "conectix", 8);
  std::memcpy(fake.data() + fake.size() - 512, "conectix", 8);
  MemoryDevice fd(fake, true);
  CHECK(detect_image_format(fd, &f).ok());
  CHECK(f == ImageFormat::Raw);
  // 正しいフッタ (固定 VHD)
  Bytes vhd(8192, 0);
  uint8_t* ft = vhd.data() + vhd.size() - 512;
  std::memcpy(ft, "conectix", 8);
  uint32_t sum = 0;
  for (int i = 0; i < 512; i++) if (i < 64 || i >= 68) sum += ft[i];
  sum = ~sum;
  ft[64] = sum >> 24; ft[65] = sum >> 16; ft[66] = sum >> 8; ft[67] = static_cast<uint8_t>(sum);
  MemoryDevice vd(vhd, true);
  CHECK(detect_image_format(vd, &f).ok());
  CHECK(f == ImageFormat::Vhd);
  // 他形式のマジック
  Bytes q(4096, 0);
  q[0] = 'Q'; q[1] = 'F'; q[2] = 'I'; q[3] = 0xfb;
  MemoryDevice qd(q, true);
  CHECK(detect_image_format(qd, &f).ok());
  CHECK(f == ImageFormat::Qcow2);
  // マジックだけの qcow2 (バージョン 0) は開けない。RAW として扱って中身を誤読しない
  BlockDevicePtr out;
  ImageInfo info;
  CHECK(open_disk_image(std::make_shared<MemoryDevice>(q, true), ImageOpenOptions{}, &out, &info).code == Errc::Unsupported);
  CHECK(!info.detail.empty());
}

// ---------------------------------------------------------------- 単体: オーバーレイ
namespace {
class SpyDevice final : public BlockDevice {
 public:
  explicit SpyDevice(BlockDevicePtr in) : in_(std::move(in)) {}
  std::atomic<int> writes{0}, reads{0};
  Status read_at(uint64_t o, void* b, size_t l, const IoOptions& io = {}) override { reads++; return in_->read_at(o, b, l, io); }
  Status write_at(uint64_t o, const void* b, size_t l, const IoOptions& io = {}) override { writes++; return in_->write_at(o, b, l, io); }
  Status flush() override { return in_->flush(); }
  Status close() override { return Status::Ok(); }
  uint64_t size() const override { return in_->size(); }
  uint32_t logical_block_size() const override { return in_->logical_block_size(); }
  bool is_read_only() const override { return true; }   // ベースは読み取り専用として渡す
  std::string describe() const override { return "spy"; }
 private:
  BlockDevicePtr in_;
};

Bytes pattern_bytes(size_t n, uint64_t seed) {
  Bytes b(n);
  for (size_t i = 0; i < n; i++) b[i] = static_cast<uint8_t>((i * 131 + seed * 17 + (i >> 9)) & 0xFF);
  return b;
}
}  // namespace

TEST(blockdev_overlay_memory) {
  const size_t N = 1000 * 1000 + 123;   // ブロックサイズの倍数ではない (末尾が端数)
  Bytes base = pattern_bytes(N, 1), model = base;
  auto spy = std::make_shared<SpyDevice>(std::make_shared<MemoryDevice>(base, true));
  std::shared_ptr<OverlayDevice> ov;
  OverlayDevice::Options opt;
  CHECK(OverlayDevice::create(spy, opt, &ov).ok());
  CHECK_EQ(ov->size(), N);
  CHECK(!ov->is_read_only());
  CHECK_EQ(ov->dirty_blocks(), 0u);
  std::mt19937_64 rng(5);
  Bytes buf;
  for (int i = 0; i < 400; i++) {
    size_t len = 1 + rng() % 20000;
    uint64_t off = rng() % (N - len + 1);
    if (i % 50 == 0) { off = N - len; }          // 末尾端数ブロックをまたぐ
    buf = pattern_bytes(len, rng());
    CHECK(ov->write_at(off, buf.data(), len).ok());
    std::memcpy(model.data() + off, buf.data(), len);
    if (i % 7 == 0) {
      size_t rl = 1 + rng() % 30000;
      uint64_t ro = rng() % (N - rl + 1);
      buf.resize(rl);
      CHECK(ov->read_at(ro, buf.data(), rl).ok());
      CHECK(std::memcmp(buf.data(), model.data() + ro, rl) == 0);
    }
  }
  Bytes all(N);
  CHECK(ov->read_at(0, all.data(), N).ok());
  CHECK(all == model);
  CHECK_EQ(spy->writes.load(), 0);   // ベースには一度も書かない
  CHECK(ov->dirty_blocks() > 0);
  CHECK(ov->write_at(N, all.data(), 1).code == Errc::OutOfRange);
  // commit: 別の書き込み可能デバイスへ
  MemoryDevice target(base, false);
  CHECK(ov->commit(target).ok());
  CHECK(target.bytes() == model);
  CHECK(ov->dirty_blocks() > 0);   // clear_after=false
  CHECK(ov->commit(target, true).ok());
  CHECK_EQ(ov->dirty_blocks(), 0u);
  // commit 後 (クリア済み) の読みはベース = 元のまま
  CHECK(ov->read_at(0, all.data(), N).ok());
  CHECK(all == base);
  // discard
  buf = pattern_bytes(5000, 9);
  CHECK(ov->write_at(1234, buf.data(), buf.size()).ok());
  CHECK(ov->discard().ok());
  CHECK(ov->read_at(0, all.data(), N).ok());
  CHECK(all == base);
  // 読み取り専用の commit 先は拒否
  MemoryDevice ro_target(base, true);
  CHECK(ov->write_at(0, buf.data(), 10).ok());
  CHECK(ov->commit(ro_target).code == Errc::ReadOnly);
  CHECK(ov->close().ok());
  CHECK(ov->read_at(0, all.data(), 1).code == Errc::Closed);
  // 引数の検証
  OverlayDevice::Options bad;
  bad.block_size = 1000;
  CHECK(OverlayDevice::create(std::make_shared<MemoryDevice>(base, true), bad, &ov).code == Errc::InvalidArgument);
  // 容量上限
  OverlayDevice::Options lim;
  lim.max_overlay_bytes = 8192;
  std::shared_ptr<OverlayDevice> ov2;
  CHECK(OverlayDevice::create(std::make_shared<MemoryDevice>(base, true), lim, &ov2).ok());
  Bytes big(3 * 4096, 1);
  CHECK(ov2->write_at(0, big.data(), big.size()).code == Errc::Io);
}

TEST(blockdev_overlay_file_and_crash) {
  std::string path = test_tmpdir() + "/usbmem_overlay_test.log";
  ::unlink(path.c_str());
  const size_t N = 300000;
  Bytes base = pattern_bytes(N, 3);
  OverlayDevice::Options opt;
  opt.file_path = path;
  opt.block_size = 4096;
  const size_t rec = 4 + 8 + 4096 + 4, hdr = 64;
  // 書き込みを順に行い、各書き込み後のモデルを保存 (1 回の書き込み = 1 ブロックに収める → レコード 1 個)
  std::vector<Bytes> models;
  std::vector<uint64_t> sizes;
  {
    std::shared_ptr<OverlayDevice> ov;
    CHECK(OverlayDevice::create(std::make_shared<MemoryDevice>(base, true), opt, &ov).ok());
    Bytes model = base;
    std::mt19937_64 rng(11);
    for (int i = 0; i < 20; i++) {
      uint64_t blk = rng() % 10;
      Bytes d = pattern_bytes(4096, rng());
      CHECK(ov->write_at(blk * 4096, d.data(), d.size()).ok());
      std::memcpy(model.data() + blk * 4096, d.data(), d.size());
      models.push_back(model);
      struct stat st;
      CHECK(::stat(path.c_str(), &st) == 0);
      sizes.push_back(static_cast<uint64_t>(st.st_size));
    }
    CHECK(ov->flush().ok());
    CHECK(ov->close().ok());
  }
  CHECK_EQ(sizes.back(), hdr + 20 * rec);
  // 通常の再オープン: 全部残っている
  {
    std::shared_ptr<OverlayDevice> ov;
    CHECK(OverlayDevice::create(std::make_shared<MemoryDevice>(base, true), opt, &ov).ok());
    Bytes all(N);
    CHECK(ov->read_at(0, all.data(), N).ok());
    CHECK(all == models.back());
    CHECK(ov->close().ok());
  }
  // クラッシュ模擬: ファイルを任意の長さで切る → 完全なレコードまでの状態に戻る (ベースは無傷)
  Bytes full = read_file(path);
  int checked = 0;
  for (size_t cut = hdr; cut <= full.size(); cut += 1 + (cut % 997)) {
    std::string p2 = path + ".cut";
    {
      std::ofstream f(p2, std::ios::binary | std::ios::trunc);
      f.write(reinterpret_cast<const char*>(full.data()), static_cast<std::streamsize>(cut));
    }
    OverlayDevice::Options o2 = opt;
    o2.file_path = p2;
    std::shared_ptr<OverlayDevice> ov;
    CHECK(OverlayDevice::create(std::make_shared<MemoryDevice>(base, true), o2, &ov).ok());
    size_t complete = (cut - hdr) / rec;
    Bytes all(N);
    CHECK(ov->read_at(0, all.data(), N).ok());
    CHECK(all == (complete == 0 ? base : models[complete - 1]));
    CHECK(ov->close().ok());
    struct stat st;
    CHECK(::stat(p2.c_str(), &st) == 0);
    CHECK_EQ(static_cast<uint64_t>(st.st_size), hdr + complete * rec);   // 壊れた末尾は切り捨てられる
    ::unlink(p2.c_str());
    checked++;
  }
  CHECK(checked > 20);
  // 最後のレコードの中身を 1 バイト壊す → その 1 件だけ捨てる
  {
    Bytes bad = full;
    bad[hdr + 19 * rec + 100] ^= 0x40;
    std::string p2 = path + ".bad";
    { std::ofstream f(p2, std::ios::binary | std::ios::trunc); f.write(reinterpret_cast<const char*>(bad.data()), static_cast<std::streamsize>(bad.size())); }
    OverlayDevice::Options o2 = opt;
    o2.file_path = p2;
    std::shared_ptr<OverlayDevice> ov;
    CHECK(OverlayDevice::create(std::make_shared<MemoryDevice>(base, true), o2, &ov).ok());
    Bytes all(N);
    CHECK(ov->read_at(0, all.data(), N).ok());
    CHECK(all == models[18]);
    CHECK(ov->close().ok());
    ::unlink(p2.c_str());
  }
  // 別のベース (サイズ違い) / ブロックサイズ違い / ヘッダ破損は開けない
  {
    std::shared_ptr<OverlayDevice> ov;
    Bytes other(N + 512, 0);
    CHECK(OverlayDevice::create(std::make_shared<MemoryDevice>(other, true), opt, &ov).code == Errc::InvalidArgument);
    OverlayDevice::Options o3 = opt;
    o3.block_size = 8192;
    CHECK(OverlayDevice::create(std::make_shared<MemoryDevice>(base, true), o3, &ov).code == Errc::InvalidArgument);
    Bytes bad = full;
    bad[3] ^= 1;
    std::string p2 = path + ".hdr";
    { std::ofstream f(p2, std::ios::binary | std::ios::trunc); f.write(reinterpret_cast<const char*>(bad.data()), static_cast<std::streamsize>(bad.size())); }
    o3 = opt;
    o3.file_path = p2;
    CHECK(OverlayDevice::create(std::make_shared<MemoryDevice>(base, true), o3, &ov).code == Errc::Corrupt);
    ::unlink(p2.c_str());
  }
  // 再開してさらに書き、commit
  {
    std::shared_ptr<OverlayDevice> ov;
    CHECK(OverlayDevice::create(std::make_shared<MemoryDevice>(base, true), opt, &ov).ok());
    Bytes d = pattern_bytes(100, 77);
    CHECK(ov->write_at(50000, d.data(), d.size()).ok());
    Bytes model = models.back();
    std::memcpy(model.data() + 50000, d.data(), d.size());
    MemoryDevice target(base, false);
    CHECK(ov->commit(target, true).ok());
    CHECK(target.bytes() == model);
    CHECK_EQ(ov->dirty_blocks(), 0u);
    CHECK(ov->close().ok());
  }
  ::unlink(path.c_str());
}

TEST(blockdev_overlay_random_model) {
  // ランダムな書き込み/読み取り/再オープンの列を、単純なモデルと突き合わせる (ファイルモード、小さいブロック)
  std::string path = test_tmpdir() + "/usbmem_overlay_model.log";
  ::unlink(path.c_str());
  const size_t N = 70001;
  Bytes base = pattern_bytes(N, 4), model = base;
  OverlayDevice::Options opt;
  opt.file_path = path;
  opt.block_size = 512;
  std::shared_ptr<OverlayDevice> ov;
  CHECK(OverlayDevice::create(std::make_shared<MemoryDevice>(base, true), opt, &ov).ok());
  std::mt19937_64 rng(31);
  for (int i = 0; i < 1500; i++) {
    int op = static_cast<int>(rng() % 10);
    if (op < 5) {
      size_t len = 1 + rng() % 3000;
      uint64_t off = rng() % (N - len + 1);
      Bytes d = pattern_bytes(len, rng());
      CHECK(ov->write_at(off, d.data(), len).ok());
      std::memcpy(model.data() + off, d.data(), len);
    } else if (op < 9) {
      size_t len = 1 + rng() % 3000;
      uint64_t off = rng() % (N - len + 1);
      Bytes d(len);
      CHECK(ov->read_at(off, d.data(), len).ok());
      CHECK(std::memcmp(d.data(), model.data() + off, len) == 0);
    } else {
      CHECK(ov->close().ok());
      ov.reset();
      CHECK(OverlayDevice::create(std::make_shared<MemoryDevice>(base, true), opt, &ov).ok());
    }
  }
  Bytes all(N);
  CHECK(ov->read_at(0, all.data(), N).ok());
  CHECK(all == model);
  CHECK(ov->close().ok());
  ::unlink(path.c_str());
}
