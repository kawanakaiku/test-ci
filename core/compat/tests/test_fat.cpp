// SPDX-License-Identifier: GPL-2.0-or-later
// FAT12/16/32 + vfat (Linux fs/fat を LinuxCompat 上で動かしたもの) の読み取りテスト。
// 使い方: usbmem_fat_tests <fixture_dir>            tools/gen_fat_fixture.py が作った *.img / *.manifest を全部検証
//         usbmem_fat_tests --corrupt <fixture_dir>  壊れたボリューム (boot/チェックサム/サイズ) のマウントが、クラッシュせず失敗すること
//         usbmem_fat_tests --ioerr <fixture_dir>    マウント・走査中のデバイス I/O エラーで、クラッシュせず失敗すること
//         usbmem_fat_tests --fuzz <fixture_dir> <iterations> <seed>
#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "usbmem/devices.h"
#include "usbmem/fat_c.h"
#include "usbmem/ntfs_c.h"   // usbmem_bdev_wrap / free
#include "usbmem/fs_probe.h"
#include "usbmem/exfat_c.h"
#include "usbmem/ext2_c.h"

using namespace usbmem;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static std::vector<uint8_t> pattern(uint32_t seed, size_t n) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; i++) v[i] = static_cast<uint8_t>((i * 31 + seed * 7 + (i >> 8)) & 0xFF);
  return v;
}

// 読み取り専用デバイス。write_at が 1 度でも呼ばれたら数える (読み取り専用マウントが書かないことの検査)。
// fail_after >= 0 なら、その回数の read_at の後はすべて EIO を返す。mut は読み出し時に差し替えるバイト。
class SpyDevice final : public BlockDevice {
 public:
  SpyDevice(BlockDevicePtr base, long fail_after = -1, std::map<uint64_t, uint8_t> mut = {}, uint64_t size_override = 0)
      : base_(std::move(base)), fail_after_(fail_after), mut_(std::move(mut)), size_(size_override ? size_override : base_->size()) {}
  Status read_at(uint64_t off, void* buf, size_t len, const IoOptions& o = {}) override {
    reads++;
    if (fail_after_ >= 0 && reads > fail_after_) return Status::Make(Errc::Io, EIO);
    if (off > size_ || len > size_ - off) return Status::Make(Errc::OutOfRange);
    Status st = base_->read_at(off, buf, len, o);
    if (!st.ok()) return st;
    for (auto it = mut_.lower_bound(off); it != mut_.end() && it->first < off + len; ++it) static_cast<uint8_t*>(buf)[it->first - off] = it->second;
    return st;
  }
  Status write_at(uint64_t, const void*, size_t, const IoOptions& = {}) override { writes++; return Status::Make(Errc::ReadOnly); }
  Status flush() override { flushes++; return Status::Ok(); }
  Status close() override { return Status::Ok(); }
  uint64_t size() const override { return size_; }
  uint32_t logical_block_size() const override { return 512; }
  bool is_read_only() const override { return true; }
  std::string describe() const override { return "spy"; }
  std::atomic<long> reads{0}, writes{0}, flushes{0};
 private:
  BlockDevicePtr base_; long fail_after_; std::map<uint64_t, uint8_t> mut_; uint64_t size_;
};

struct Mounted {
  std::shared_ptr<SpyDevice> dev; usbmem_bdev* bd = nullptr; usbmem_fat* v = nullptr; int err = 0;
  ~Mounted() { if (v) usbmem_fat_unmount(v); if (bd) usbmem_bdev_free(bd); }
};
static std::unique_ptr<Mounted> mount_dev(std::shared_ptr<SpyDevice> dev) {
  auto m = std::make_unique<Mounted>();
  m->dev = dev;
  BlockDevicePtr p = dev;
  m->bd = usbmem_bdev_wrap(&p);
  m->err = usbmem_fat_mount(m->bd, 1, &m->v);
  if (m->err) m->v = nullptr;
  return m;
}
static BlockDevicePtr open_img(const std::string& path) {
  BlockDevicePtr d;
  if (!RawImageDevice::open_path(path, true, 512, &d).ok()) return nullptr;
  return d;
}

struct Entry { std::string name; uint64_t ino; unsigned type; };
static int list_dir(usbmem_fat* v, uint64_t dir, std::vector<Entry>* out) {
  out->clear();
  return usbmem_fat_readdir(v, dir, [](void* u, const char* n, int len, uint64_t ino, unsigned t) {
    static_cast<std::vector<Entry>*>(u)->push_back({std::string(n, (size_t)len), ino, t}); return 0; }, out);
}
static int resolve(usbmem_fat* v, const std::string& path, uint64_t* ino) {
  uint64_t cur = USBMEM_FAT_ROOT_INO; size_t i = 0;
  while (i < path.size()) {
    size_t j = path.find('/', i); if (j == std::string::npos) j = path.size();
    if (j > i) { std::string part = path.substr(i, j - i); int rc = usbmem_fat_lookup(v, cur, part.data(), part.size(), &cur); if (rc) return rc; }
    i = j + 1;
  }
  *ino = cur; return 0;
}

struct MFile { std::string path; uint64_t size; uint32_t seed; int64_t mtime; std::string attrs; };
struct Manifest { std::vector<std::string> dirs; std::vector<MFile> files; };
static bool load_manifest(const std::string& path, Manifest* m) {
  std::ifstream f(path);
  if (!f) return false;
  std::string line;
  while (std::getline(f, line)) {
    std::vector<std::string> c; std::stringstream ss(line); std::string t;
    while (std::getline(ss, t, '\t')) c.push_back(t);
    if (c.size() == 2 && c[0] == "D") m->dirs.push_back(c[1]);
    else if (c.size() == 6 && c[0] == "F") m->files.push_back({c[1], std::stoull(c[2]), (uint32_t)std::stoul(c[3]), std::stoll(c[4]), c[5]});
  }
  return !m->files.empty();
}
static std::string parent_of(const std::string& p) { size_t k = p.rfind('/'); return k == std::string::npos ? "" : p.substr(0, k); }
static std::string base_of(const std::string& p) { size_t k = p.rfind('/'); return k == std::string::npos ? p : p.substr(k + 1); }

// 1 つのイメージの全検証
static void verify_image(const std::string& dir, const std::string& name) {
  std::fprintf(stderr, "== %s\n", name.c_str());
  Manifest man;
  CHECK(load_manifest(dir + "/" + name + ".manifest", &man));
  BlockDevicePtr base = open_img(dir + "/" + name + ".img");
  CHECK(base != nullptr);
  if (!base) return;
  auto spy = std::make_shared<SpyDevice>(base);
  auto m = mount_dev(spy);
  CHECK(m->err == 0 && m->v);
  if (!m->v) { std::fprintf(stderr, "mount failed: %d\n", m->err); return; }
  usbmem_fat* v = m->v;

  // 期待するディレクトリの内容 (パス → 名前の集合)
  std::map<std::string, std::set<std::string>> expect;
  expect[""];
  for (auto& d : man.dirs) { expect[parent_of(d)].insert(base_of(d)); expect[d]; }
  for (auto& f : man.files) expect[parent_of(f.path)].insert(base_of(f.path));
  for (auto& kv : expect) {
    uint64_t dino = 0;
    CHECK(resolve(v, kv.first, &dino) == 0);
    std::vector<Entry> ents;
    CHECK(list_dir(v, dino, &ents) == 0);
    std::set<std::string> got; for (auto& e : ents) got.insert(e.name);
    CHECK(got.size() == ents.size());      // 重複なし
    if (got != kv.second) {
      std::fprintf(stderr, "dir '%s': expected %zu entries, got %zu\n", kv.first.c_str(), kv.second.size(), got.size());
      for (auto& n : kv.second) if (!got.count(n)) std::fprintf(stderr, "  missing: %s\n", n.c_str());
      for (auto& n : got) if (!kv.second.count(n)) std::fprintf(stderr, "  extra: %s\n", n.c_str());
      g_fail++;
    }
    for (auto& e : ents) {   // readdir が返した ino で、そのまま stat できる。種別が一致する
      usbmem_fat_stat st;
      CHECK(usbmem_fat_stat_ino(v, e.ino, &st) == 0);
      bool is_dir = expect.count(kv.first.empty() ? e.name : kv.first + "/" + e.name) > 0;
      CHECK(is_dir == (e.type == 4 /*DT_DIR*/));
      CHECK(((st.mode & 0170000) == 0040000) == is_dir);
    }
  }
  // 全ファイル: サイズ・内容 (パターン)・更新時刻 (FAT は 2 秒精度のローカル時刻。ここでは UTC として解釈される)・属性・複数の読み方
  for (auto& f : man.files) {
    uint64_t ino = 0;
    int rc = resolve(v, f.path, &ino);
    CHECK(rc == 0);
    if (rc) { std::fprintf(stderr, "resolve %s: %d\n", f.path.c_str(), rc); continue; }
    usbmem_fat_stat st;
    CHECK(usbmem_fat_stat_ino(v, ino, &st) == 0);
    CHECK(st.size == f.size);
    CHECK((st.mode & 0170000) == 0100000);
    if (st.mtime_sec != f.mtime) std::fprintf(stderr, "mtime %s: got %lld want %lld\n", f.path.c_str(), (long long)st.mtime_sec, (long long)f.mtime);
    CHECK(st.mtime_sec == f.mtime);
    CHECK(((st.mode & 0222) == 0) == (f.attrs.find('r') != std::string::npos));   // 読み取り専用属性 → 書き込みビットなし
    CHECK(st.nlink == 1);
    std::vector<uint8_t> want = pattern(f.seed, (size_t)f.size);
    std::vector<uint8_t> got(f.size + 16, 0xAB);
    int64_t n = usbmem_fat_read(v, ino, 0, got.data(), got.size());   // 要求が EOF を超えても、残りだけ返す
    CHECK(n == (int64_t)f.size);
    if (n == (int64_t)f.size && f.size) CHECK(std::memcmp(got.data(), want.data(), (size_t)f.size) == 0);
    CHECK(usbmem_fat_read(v, ino, f.size, got.data(), 10) == 0);        // ちょうど EOF
    CHECK(usbmem_fat_read(v, ino, f.size + 4096, got.data(), 10) == 0);  // EOF より先
    if (f.size > 3) {   // 不揃いな位置・長さ
      for (uint64_t off : {uint64_t(1), uint64_t(f.size / 2), uint64_t(f.size - 1)}) {
        size_t len = (size_t)std::min<uint64_t>(f.size - off, 4099);
        std::vector<uint8_t> part(len);
        CHECK(usbmem_fat_read(v, ino, off, part.data(), len) == (int64_t)len);
        CHECK(std::memcmp(part.data(), want.data() + off, len) == 0);
      }
    }
  }
  // 名前の扱い
  uint64_t a = 0, b = 0, c = 0;
  CHECK(resolve(v, "MiXeD.CaSe", &a) == 0);
  CHECK(resolve(v, "mixed.case", &b) == 0);   // FAT は大文字小文字を区別しない (ASCII)
  CHECK(a == b);
  CHECK(resolve(v, "small.txt", &a) == 0 && resolve(v, "SMALL.TXT", &b) == 0 && a == b);
  CHECK(resolve(v, "LOWER.TXT", &a) == 0 && resolve(v, "lower.txt", &b) == 0 && a == b);
  CHECK(resolve(v, "upper.txt", &a) == 0 && resolve(v, "UPPER.TXT", &b) == 0 && a == b);
  CHECK(resolve(v, "SUB/DEEP/LEAF.BIN", &a) == 0 && resolve(v, "sub/deep/leaf.bin", &b) == 0 && a == b);
  CHECK(resolve(v, "日本語ファイル名.txt", &c) == 0);
  CHECK(resolve(v, "emoji_\xF0\x9F\x98\x80.txt", &c) == 0);      // U+1F600 (サロゲートペア)
  CHECK(resolve(v, "nonexistent", &c) == -ENOENT);
  CHECK(resolve(v, "sub/nonexistent", &c) == -ENOENT);
  CHECK(resolve(v, "small.txt/x", &c) == -ENOTDIR);
  uint64_t sub = 0; CHECK(resolve(v, "sub", &sub) == 0);
  std::vector<uint8_t> tmp(16);
  CHECK(usbmem_fat_read(v, sub, 0, tmp.data(), 16) == -EISDIR);
  CHECK(usbmem_fat_stat_ino(v, 999999999, nullptr) == -ENOENT);
  CHECK(usbmem_fat_lookup(v, 999999999, "x", 1, &c) == -ENOENT);
  // 読み取り専用: 書き込み・マウント要求
  BlockDevicePtr p2 = spy; usbmem_bdev* bd2 = usbmem_bdev_wrap(&p2); usbmem_fat* v2 = nullptr;
  // 書き込みを要求: 書き込みを無効にしたビルドは -EROFS。USBMEM_NTFS_WRITE=ON のビルドでは成功する (その場合も、書き込みを始めるまでメディアには何も書かない = 下の writes / flushes == 0。docs/fat-write.md)
  int rw_rc = usbmem_fat_mount(bd2, 0, &v2);
  CHECK(rw_rc == -EROFS || rw_rc == 0);
  if (rw_rc == 0) usbmem_fat_unmount(v2);
  usbmem_bdev_free(bd2);
  CHECK(spy->writes == 0);
  CHECK(spy->flushes == 0);
  // 2 回目のマウント (別インスタンス) も同じ結果 (モジュール init の再実行などで壊れない)
  auto m2 = mount_dev(std::make_shared<SpyDevice>(base));
  CHECK(m2->err == 0);
  if (m2->v) { uint64_t x = 0; CHECK(resolve(m2->v, "sub/deep/leaf.bin", &x) == 0); }
  std::fprintf(stderr, "   %zu files, %zu dirs, device reads %ld\n", man.files.size(), man.dirs.size(), (long)spy->reads);
}

static std::vector<std::string> list_images(const std::string& dir) {
  std::vector<std::string> out;
  for (const char* n : {"fat12_f1440", "fat12_c4k", "fat16_c2k", "fat16_c512", "fat32_c4k", "fat32_c512", "fat32_s4k", "fat32_c8k"}) if (access((dir + "/" + n + ".img").c_str(), R_OK) == 0) out.push_back(n);
  return out;
}

// 全体を歩く (fuzz / ioerr 用)。クラッシュ・ハングしないことだけを見る。budget で打ち切り。
static void walk(usbmem_fat* v, uint64_t dir, int depth, int& budget) {
  if (depth > 12 || budget <= 0) return;
  std::vector<Entry> ents;
  if (list_dir(v, dir, &ents)) return;
  for (auto& e : ents) {
    if (--budget <= 0) return;
    usbmem_fat_stat st;
    if (usbmem_fat_stat_ino(v, e.ino, &st)) continue;
    if (e.type == 4) walk(v, e.ino, depth + 1, budget);
    else { std::vector<uint8_t> buf(65536); uint64_t sz = std::min<uint64_t>(st.size, 1u << 20); for (uint64_t o = 0; o < sz && budget > 0; o += buf.size(), budget--) if (usbmem_fat_read(v, e.ino, o, buf.data(), buf.size()) <= 0) break; }
  }
}

static void test_corrupt(const std::string& dir) {
  for (auto& name : list_images(dir)) {
    BlockDevicePtr base = open_img(dir + "/" + name + ".img");
    CHECK(base);
    // 正常
    CHECK(mount_dev(std::make_shared<SpyDevice>(base))->err == 0);
    struct Case { const char* what; std::map<uint64_t, uint8_t> mut; uint64_t size; bool must_fail; };
    // BPB の位置 (FAT12/16/32 共通): 0x0B 論理セクタ長 / 0x0D クラスタあたりセクタ / 0x0E 予約セクタ / 0x10 FAT 数 / 0x15 メディア / 0x16 FAT16 の FAT 長 / 0x24 FAT32 の FAT 長
    std::vector<Case> cases = {
      {"bytes per sector = 0", {{0x0B, 0}, {0x0C, 0}}, 0, true},
      {"bytes per sector = 1000 (not a power of 2)", {{0x0B, 0xE8}, {0x0C, 0x03}}, 0, true},
      {"bytes per sector = 8192", {{0x0B, 0x00}, {0x0C, 0x20}}, 0, true},
      {"sectors per cluster = 3", {{0x0D, 3}}, 0, true},
      {"sectors per cluster = 0", {{0x0D, 0}}, 0, true},
      {"number of FATs = 0", {{0x10, 0}}, 0, true},
      {"media = 0x00", {{0x15, 0x00}}, 0, true},
      {"reserved sectors = 0", {{0x0E, 0}, {0x0F, 0}}, 0, true},
      {"FAT length = 0 (FAT16 and FAT32 fields)", {{0x16, 0}, {0x17, 0}, {0x24, 0}, {0x25, 0}, {0x26, 0}, {0x27, 0}}, 0, true},
      {"root entries huge", {{0x11, 0xFF}, {0x12, 0xFF}}, 0, false},
      {"total sectors huge", {{0x13, 0xFF}, {0x14, 0xFF}, {0x20, 0xFF}, {0x21, 0xFF}, {0x22, 0xFF}, {0x23, 0x7F}}, 0, false},
      {"FAT32 root cluster = 0", {{0x2C, 0}, {0x2D, 0}, {0x2E, 0}, {0x2F, 0}}, 0, false},
      {"FAT32 root cluster huge", {{0x2C, 0xFF}, {0x2D, 0xFF}, {0x2E, 0xFF}, {0x2F, 0x0F}}, 0, false},
      {"boot signature", {{510, 0}, {511, 0}}, 0, false},
      {"device truncated to 64 KiB", {}, 65536, false},
      {"device truncated to 1 MiB", {}, 1u << 20, false},
      {"zeroed boot sector", {}, 0, true},
    };
    for (int i = 0; i < 512; i++) cases.back().mut[(uint64_t)i] = 0;
    for (auto& c : cases) {
      alarm(30);
      auto m = mount_dev(std::make_shared<SpyDevice>(base, -1, c.mut, c.size));
      if (m->v) { int b = 400; walk(m->v, USBMEM_FAT_ROOT_INO, 0, b); }
      bool failed = m->err != 0;
      if (c.must_fail && !failed) { std::fprintf(stderr, "%s: [%s] mount unexpectedly succeeded\n", name.c_str(), c.what); g_fail++; }
      std::fprintf(stderr, "   %-12s %-44s -> mount %s (%d)\n", name.c_str(), c.what, m->err ? "failed" : "ok", m->err);
      alarm(0);
    }
  }
}

static void test_ioerr(const std::string& dir) {
  for (auto& name : list_images(dir)) {
    BlockDevicePtr base = open_img(dir + "/" + name + ".img");
    CHECK(base);
    // 何回目の read から失敗させるか: マウント中の各段階を順に潰す。後半は走査中の失敗。
    int mount_fail = 0, ok = 0;
    for (long n = 0; n < 60; n++) {
      alarm(30);
      auto m = mount_dev(std::make_shared<SpyDevice>(base, n));
      if (m->v) { ok++; int b = 400; walk(m->v, USBMEM_FAT_ROOT_INO, 0, b); } else mount_fail++;
      alarm(0);
    }
    for (long n = 60; n < 400; n += 7) {
      alarm(30);
      auto m = mount_dev(std::make_shared<SpyDevice>(base, n));
      if (m->v) { int b = 400; walk(m->v, USBMEM_FAT_ROOT_INO, 0, b); }
      alarm(0);
    }
    std::fprintf(stderr, "   %s: read-failure injection: mount failed %d, mounted-then-failed %d (no crash)\n", name.c_str(), mount_fail, ok);
    CHECK(mount_fail > 0);   // 最初の read から失敗するなら、マウントは失敗するはず
  }
}

static int fuzz(const std::string& dir, int iters, unsigned seed) {
  auto images = list_images(dir);
  uint64_t x = seed * 2654435761u + 1;
  auto rnd = [&]() { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
  int mounted = 0, total = 0;
  for (auto& name : images) {
    BlockDevicePtr base = open_img(dir + "/" + name + ".img");
    if (!base) { g_fail++; continue; }
    uint8_t bs[512] = {0};
    base->read_at(0, bs, 512);
    auto le16 = [&](int o) { return (uint32_t)(bs[o] | (bs[o + 1] << 8)); };
    auto le32 = [&](int o) { return (uint64_t)le16(o) | ((uint64_t)le16(o + 2) << 16); };
    uint32_t sector = le16(0x0B), cluster = sector * bs[0x0D];
    uint64_t fat_off = (uint64_t)le16(0x0E) * sector;
    uint64_t fat_len = (uint64_t)(le16(0x16) ? le16(0x16) : le32(0x24)) * sector;
    uint64_t root_off = fat_off + bs[0x10] * fat_len, root_len = (uint64_t)le16(0x11) * 32;
    uint64_t data = root_off + root_len;   // FAT32 はここがクラスタ 2 (ルートディレクトリ)
    if (!sector || !cluster || !fat_len) { g_fail++; continue; }
    for (int it = 0; it < iters; it++) {
      std::map<uint64_t, uint8_t> mut;
      int nmut = 1 + (int)(rnd() % 24);
      unsigned region = (unsigned)(rnd() % 7);   // 1 反復では 1 つの領域だけを壊す
      for (int k = 0; k < nmut; k++) {
        uint64_t off;
        switch (region) {
          case 0: off = rnd() % 90; break;                                              // BPB (先頭 90 バイト)
          case 1: off = fat_off + rnd() % std::min<uint64_t>(fat_len, 16384); break;    // FAT の先頭
          case 2: off = root_len ? root_off + rnd() % root_len : data + rnd() % (4u * cluster); break;   // ルートディレクトリ
          case 3: off = data + rnd() % std::min<uint64_t>(base->size() - data, 512u << 10); break;      // ディレクトリ・データ
          case 4: off = rnd() % base->size(); break;
          case 5: off = sector + rnd() % 512; break;                                    // FSInfo (FAT32)
          default: off = rnd() % 512; break;
        }
        mut[off] = (uint8_t)(rnd() % 3 == 0 ? 0xFF : rnd());
      }
      if (const char* lp = std::getenv("USBMEM_FUZZ_LAST")) {
        if (FILE* lf = std::fopen(lp, "w")) { std::fprintf(lf, "%s\n", name.c_str()); for (auto& kv : mut) std::fprintf(lf, "%llu %u\n", (unsigned long long)kv.first, kv.second); std::fclose(lf); }
      }
      alarm(30);   // ハング検出
      auto m = mount_dev(std::make_shared<SpyDevice>(base, -1, mut));
      total++;
      if (m->v) { mounted++; int budget = 1500; walk(m->v, USBMEM_FAT_ROOT_INO, 0, budget); }
      CHECK(m->dev->writes == 0);
      alarm(0);
    }
  }
  std::fprintf(stderr, "fuzz: %d iterations (seed %u), %d mounted, no crash/hang\n", total, seed, mounted);
  return g_fail ? 1 : 0;
}

// FAT・exFAT・ext2 のコアを同一プロセスで同時に使う (互換層のグローバル状態・NLS 登録・モジュール init の共存)
static std::vector<uint8_t> read_all_fat(usbmem_fat* v, const char* path, size_t n) {
  uint64_t ino = 0; std::vector<uint8_t> b(n, 0);
  if (resolve(v, path, &ino) != 0 || usbmem_fat_read(v, ino, 0, b.data(), n) != (int64_t)n) b.clear();
  return b;
}
static int coexist(const std::string& dfat, const std::string& dexfat, const std::string& dext2) {
  BlockDevicePtr bf = open_img(dfat + "/fat32_c4k.img"), bx = open_img(dexfat + "/c4k.img"), b2 = open_img(dext2 + "/b4k.img");
  CHECK(bf && bx && b2);
  if (!bf || !bx || !b2) return 1;
  usbmem_bdev* df = usbmem_bdev_wrap(&bf); usbmem_bdev* dx = usbmem_bdev_wrap(&bx); usbmem_bdev* d2 = usbmem_bdev_wrap(&b2);
  usbmem_fat* vf = nullptr; usbmem_exfat* vx = nullptr; usbmem_ext2* v2 = nullptr;
  CHECK(usbmem_fat_mount(df, 1, &vf) == 0);
  CHECK(usbmem_exfat_mount(dx, 1, &vx) == 0);
  CHECK(usbmem_ext2_mount(d2, 1, &v2) == 0);
  CHECK(vf && vx && v2);
  if (vf && vx && v2) {
    std::vector<uint8_t> f0 = read_all_fat(vf, "SMALL.TXT", 13), f_multi = read_all_fat(vf, "multi.bin", 5 * 4096 + 17);
    CHECK(f0.size() == 13 && f_multi.size() == 5 * 4096 + 17);
    std::vector<Entry> ents0; CHECK(list_dir(vf, USBMEM_FAT_ROOT_INO, &ents0) == 0 && ents0.size() > 20);
    uint64_t ix = 0, i2 = 0; std::vector<uint8_t> a(11), b(11), a0, b0;
    CHECK(usbmem_exfat_lookup(vx, USBMEM_EXFAT_ROOT_INO, "small.txt", 9, &ix) == 0);
    CHECK(usbmem_ext2_lookup(v2, USBMEM_EXT2_ROOT_INO, "small.txt", 9, &i2) == 0);
    CHECK(usbmem_exfat_read(vx, ix, 0, a.data(), 11) == 11);
    CHECK(usbmem_ext2_read(v2, i2, 0, b.data(), 11) == 11);
    CHECK(a != std::vector<uint8_t>(11, 0) && b != std::vector<uint8_t>(11, 0));
    a0 = a; b0 = b;                                    // 以後の交互操作で内容が変わらないこと (exFAT と ext2 は生成器が違うので a と b は別物)
    // 交互にアンマウント・再マウントして、残った側が影響を受けないこと
    usbmem_exfat_unmount(vx); vx = nullptr;
    CHECK(read_all_fat(vf, "SMALL.TXT", 13) == f0);
    CHECK(usbmem_ext2_read(v2, i2, 0, b.data(), 11) == 11 && b == b0);
    usbmem_fat_unmount(vf); vf = nullptr;
    CHECK(usbmem_ext2_read(v2, i2, 0, b.data(), 11) == 11 && b == b0);
    CHECK(usbmem_fat_mount(df, 1, &vf) == 0);
    CHECK(vf && read_all_fat(vf, "SMALL.TXT", 13) == f0 && read_all_fat(vf, "multi.bin", 5 * 4096 + 17) == f_multi);
    CHECK(usbmem_exfat_mount(dx, 1, &vx) == 0);
    CHECK(vx && usbmem_exfat_lookup(vx, USBMEM_EXFAT_ROOT_INO, "small.txt", 9, &ix) == 0);
    CHECK(vx && usbmem_exfat_read(vx, ix, 0, a.data(), 11) == 11 && a == a0);
    std::vector<Entry> ents; CHECK(vf && list_dir(vf, USBMEM_FAT_ROOT_INO, &ents) == 0 && ents.size() == ents0.size());
  }
  if (vf) usbmem_fat_unmount(vf);
  if (vx) usbmem_exfat_unmount(vx);
  if (v2) usbmem_ext2_unmount(v2);
  usbmem_bdev_free(df); usbmem_bdev_free(dx); usbmem_bdev_free(d2);
  std::fprintf(stderr, g_fail ? "coexist: FAILED\n" : "coexist: FAT + exFAT + ext2 in one process OK\n");
  return g_fail ? 1 : 0;
}

// probe_filesystem が FAT12/16/32 を取り違えず、exFAT / NTFS / ext2 のイメージを FAT と誤認しないこと (既存の判定優先順位は変えていない)
static int probe_check(const std::string& dfat, const std::string& dexfat, const std::string& dntfs, const std::string& dext2) {
  using usbmem::FsType;
  auto probe = [&](const std::string& path) {
    BlockDevicePtr d = open_img(path);
    if (!d) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); g_fail++; return FsType::Unknown; }
    return usbmem::probe_filesystem(*d, 0, d->size());
  };
  struct { const char* n; FsType t; } fats[] = {
    {"fat12_f1440", FsType::Fat12}, {"fat12_c4k", FsType::Fat12}, {"fat16_c2k", FsType::Fat16}, {"fat16_c512", FsType::Fat16},
    {"fat32_c4k", FsType::Fat32}, {"fat32_c512", FsType::Fat32}, {"fat32_s4k", FsType::Fat32}, {"fat32_c8k", FsType::Fat32}};
  for (auto& f : fats) {
    FsType t = probe(dfat + "/" + f.n + ".img");
    if (t != f.t) std::fprintf(stderr, "probe %s: got %s want %s\n", f.n, usbmem::fs_type_name(t), usbmem::fs_type_name(f.t));
    CHECK(t == f.t);
  }
  for (const char* n : {"c4k", "c512", "s4k_c32k"}) CHECK(probe(dexfat + "/" + n + ".img") == FsType::Exfat);
  CHECK(probe(dntfs + "/ntfs_basic.img") == FsType::Ntfs);
  CHECK(probe(dext2 + "/b4k.img") == FsType::Ext2);
  // 破損 BPB を持つ FAT は FAT と判定しない (FAT32 の FAT 長 0 / セクタ長不正)
  {
    BlockDevicePtr b = open_img(dfat + "/fat32_c4k.img");
    if (b) {
      std::vector<uint8_t> head(4096); b->read_at(0, head.data(), 4096);
      auto base = head; base[0x0B] = 0; base[0x0C] = 0;
      CHECK(usbmem::probe_boot_sector(base.data(), 4096, b->size()) == FsType::Unknown);
      base = head; base[0x24] = base[0x25] = base[0x26] = base[0x27] = 0;
      CHECK(usbmem::probe_boot_sector(base.data(), 4096, b->size()) == FsType::Unknown);
      CHECK(usbmem::probe_boot_sector(head.data(), 4096, b->size()) == FsType::Fat32);
    }
  }
  std::fprintf(stderr, g_fail ? "probe: FAILED\n" : "probe: FAT12/16/32 distinguished from exFAT/NTFS/ext2 OK\n");
  return g_fail ? 1 : 0;
}

int main(int argc, char** argv) {
  std::signal(SIGALRM, [](int) { std::fprintf(stderr, "TIMEOUT (hang)\n"); std::abort(); });
  if (argc < 2) { std::fprintf(stderr, "usage: %s <fixture_dir> | --corrupt <dir> | --ioerr <dir> | --fuzz <dir> <iters> <seed> | --coexist <fat_dir> <exfat_dir> <ext2_dir>\n", argv[0]); return 2; }
  std::string mode = argv[1];
  if (mode == "--fuzz" && argc >= 5) return fuzz(argv[2], std::atoi(argv[3]), (unsigned)std::atoi(argv[4]));
  if (mode == "--probe" && argc >= 6) return probe_check(argv[2], argv[3], argv[4], argv[5]);
  if (mode == "--coexist" && argc >= 5) return coexist(argv[2], argv[3], argv[4]);
  if (mode == "--corrupt" && argc >= 3) { test_corrupt(argv[2]); return g_fail ? 1 : 0; }
  if (mode == "--ioerr" && argc >= 3) { test_ioerr(argv[2]); return g_fail ? 1 : 0; }
  auto images = list_images(argv[1]);
  CHECK(!images.empty());
  for (auto& n : images) verify_image(argv[1], n);
  if (g_fail) { std::fprintf(stderr, "FAILED: %d checks\n", g_fail); return 1; }
  std::fprintf(stderr, "fat: all checks passed (%zu images)\n", images.size());
  return 0;
}
