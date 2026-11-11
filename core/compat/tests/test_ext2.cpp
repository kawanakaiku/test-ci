// SPDX-License-Identifier: GPL-2.0-or-later
// ext2 (Linux fs/ext2 を LinuxCompat 上で動かしたもの) の読み取りテスト。
// 使い方: usbmem_ext2_tests <fixture_dir>            tools/gen_ext2_fixture.py が作った *.img / *.manifest を全部検証
//         usbmem_ext2_tests --corrupt <fixture_dir>  壊れたボリューム (スーパーブロック/サイズ/ext4 機能) のマウントが、クラッシュせず失敗すること
//         usbmem_ext2_tests --ioerr <fixture_dir>    マウント・走査中のデバイス I/O エラーで、クラッシュせず失敗すること
//         usbmem_ext2_tests --fuzz <fixture_dir> <iterations> <seed>
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
#include "usbmem/ext2_c.h"
#include "usbmem/ntfs_c.h"   // usbmem_bdev_wrap / free

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
  std::shared_ptr<SpyDevice> dev; usbmem_bdev* bd = nullptr; usbmem_ext2* v = nullptr; int err = 0;
  ~Mounted() { if (v) usbmem_ext2_unmount(v); if (bd) usbmem_bdev_free(bd); }
};
static std::unique_ptr<Mounted> mount_dev(std::shared_ptr<SpyDevice> dev) {
  auto m = std::make_unique<Mounted>();
  m->dev = dev;
  BlockDevicePtr p = dev;
  m->bd = usbmem_bdev_wrap(&p);
  m->err = usbmem_ext2_mount(m->bd, 1, &m->v);
  if (m->err) m->v = nullptr;
  return m;
}
static BlockDevicePtr open_img(const std::string& path) {
  BlockDevicePtr d;
  if (!RawImageDevice::open_path(path, true, 512, &d).ok()) return nullptr;
  return d;
}

struct Entry { std::string name; uint64_t ino; unsigned type; };
static int list_dir(usbmem_ext2* v, uint64_t dir, std::vector<Entry>* out) {
  out->clear();
  return usbmem_ext2_readdir(v, dir, [](void* u, const char* n, int len, uint64_t ino, unsigned t) {
    static_cast<std::vector<Entry>*>(u)->push_back({std::string(n, (size_t)len), ino, t}); return 0; }, out);
}
static int resolve(usbmem_ext2* v, const std::string& path, uint64_t* ino) {
  uint64_t cur = USBMEM_EXT2_ROOT_INO; size_t i = 0;
  while (i < path.size()) {
    size_t j = path.find('/', i); if (j == std::string::npos) j = path.size();
    if (j > i) { std::string part = path.substr(i, j - i); int rc = usbmem_ext2_lookup(v, cur, part.data(), part.size(), &cur); if (rc) return rc; }
    i = j + 1;
  }
  *ino = cur; return 0;
}

// manifest (tools/gen_ext2_fixture.py): D path mtime / F path size mtime nlink spec / L path target
struct Region { char kind; uint64_t off, len; uint32_t seed; };   // 'd' = pattern(seed) を (ファイルオフセット) から、'z' = ゼロ
struct MFile { std::string path; uint64_t size; int64_t mtime; uint32_t nlink; std::vector<Region> regions; };
struct MLink { std::string path, target; };
struct Manifest { std::vector<std::pair<std::string, int64_t>> dirs; std::vector<MFile> files; std::vector<MLink> links; };
static bool load_manifest(const std::string& path, Manifest* m) {
  std::ifstream f(path);
  if (!f) return false;
  std::string line;
  while (std::getline(f, line)) {
    std::vector<std::string> c; std::stringstream ss(line); std::string t;
    while (std::getline(ss, t, '\t')) c.push_back(t);
    if (c.size() == 3 && c[0] == "D") m->dirs.push_back({c[1], std::stoll(c[2])});
    else if (c.size() == 3 && c[0] == "L") m->links.push_back({c[1], c[2]});
    else if (c[0] == "F" && c.size() >= 5) {
      MFile mf{c[1], std::stoull(c[2]), std::stoll(c[3]), (uint32_t)std::stoul(c[4]), {}};
      if (c.size() >= 6 && !c[5].empty()) {
        std::stringstream rs(c[5]); std::string it;
        while (std::getline(rs, it, ',')) {
          std::vector<std::string> p; std::stringstream is(it); std::string q;
          while (std::getline(is, q, ':')) p.push_back(q);
          if (p[0] == "d") mf.regions.push_back({'d', std::stoull(p[1]), std::stoull(p[2]), (uint32_t)std::stoul(p[3])});
          else mf.regions.push_back({'z', std::stoull(p[1]), std::stoull(p[2]), 0});
        }
      }
      m->files.push_back(mf);
    }
  }
  return !m->files.empty();
}
static std::string parent_of(const std::string& p) { size_t k = p.rfind('/'); return k == std::string::npos ? "" : p.substr(0, k); }
static std::string base_of(const std::string& p) { size_t k = p.rfind('/'); return k == std::string::npos ? p : p.substr(k + 1); }
static std::vector<uint8_t> pattern_at(uint32_t seed, uint64_t off, size_t n) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; i++) { uint64_t o = off + i; v[i] = static_cast<uint8_t>((o * 31 + seed * 7 + (o >> 8)) & 0xFF); }
  return v;
}

// 1 つのファイルの内容検査。小さいファイルは全体を 1 回で読んで比較。大きな疎ファイルは「データ領域 (全部) + 穴の先頭・末尾・途中」を読む。
static void verify_file(usbmem_ext2* v, const MFile& f, uint64_t ino) {
  usbmem_ext2_stat st;
  CHECK(usbmem_ext2_stat_ino(v, ino, &st) == 0);
  CHECK(st.size == f.size);
  CHECK((st.mode & 0170000) == 0100000);
  CHECK(st.mtime_sec == f.mtime);
  CHECK(st.nlink == f.nlink);
  std::vector<uint8_t> tmp(16, 0xAB);
  CHECK(usbmem_ext2_read(v, ino, f.size, tmp.data(), 10) == 0);          // ちょうど EOF
  CHECK(usbmem_ext2_read(v, ino, f.size + 4096, tmp.data(), 10) == 0);   // EOF より先
  if (f.size <= (16u << 20)) {
    std::vector<uint8_t> want(f.size, 0);
    for (auto& r : f.regions) if (r.kind == 'd') { auto p = pattern_at(r.seed, r.off, (size_t)r.len); std::memcpy(want.data() + r.off, p.data(), p.size()); }
    std::vector<uint8_t> got(f.size + 16, 0xAB);
    int64_t n = usbmem_ext2_read(v, ino, 0, got.data(), got.size());     // 要求が EOF を超えても、残りだけ返す
    CHECK(n == (int64_t)f.size);
    if (n == (int64_t)f.size && f.size && std::memcmp(got.data(), want.data(), (size_t)f.size) != 0) {
      size_t k = 0; while (k < f.size && got[k] == want[k]) k++;
      std::fprintf(stderr, "content mismatch %s: size %llu first diff at %zu (got %u want %u)\n", f.path.c_str(), (unsigned long long)f.size, k, got[k], want[k]);
      g_fail++;
    }
    if (f.size > 3) {   // 不揃いな位置・長さ
      for (uint64_t off : {uint64_t(1), uint64_t(f.size / 2), uint64_t(f.size - 1)}) {
        size_t len = (size_t)std::min<uint64_t>(f.size - off, 4099);
        std::vector<uint8_t> part(len);
        CHECK(usbmem_ext2_read(v, ino, off, part.data(), len) == (int64_t)len);
        CHECK(std::memcmp(part.data(), want.data() + off, len) == 0);
      }
    }
    return;
  }
  // 巨大な疎ファイル
  auto check_range = [&](uint64_t off, uint64_t len, const std::vector<uint8_t>* want_or_zero_seed_data, uint32_t seed, bool zero) {
    (void)want_or_zero_seed_data;
    std::vector<uint8_t> got((size_t)len);
    int64_t n = usbmem_ext2_read(v, ino, off, got.data(), got.size());
    CHECK(n == (int64_t)len);
    if (n != (int64_t)len) return;
    if (zero) { for (size_t i = 0; i < got.size(); i++) if (got[i] != 0) { std::fprintf(stderr, "%s: nonzero at %llu\n", f.path.c_str(), (unsigned long long)(off + i)); g_fail++; break; } }
    else { auto w = pattern_at(seed, off, (size_t)len); CHECK(std::memcmp(got.data(), w.data(), w.size()) == 0); }
  };
  for (auto& r : f.regions) {
    if (r.kind == 'd') check_range(r.off, r.len, nullptr, r.seed, false);
    else {
      uint64_t head = std::min<uint64_t>(r.len, 8192);
      check_range(r.off, head, nullptr, 0, true);
      if (r.len > head) { check_range(r.off + r.len - head, head, nullptr, 0, true); }
      if (r.len > 3 * head) check_range(r.off + r.len / 2, head, nullptr, 0, true);
    }
  }
}

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
  usbmem_ext2* v = m->v;

  // 期待するディレクトリの内容 (パス → 名前の集合)。ルートには mke2fs が作る lost+found がある
  std::map<std::string, std::set<std::string>> expect;
  expect[""].insert("lost+found"); expect["lost+found"];
  for (auto& d : man.dirs) { expect[parent_of(d.first)].insert(base_of(d.first)); expect[d.first]; }
  for (auto& f : man.files) expect[parent_of(f.path)].insert(base_of(f.path));
  for (auto& l : man.links) expect[parent_of(l.path)].insert(base_of(l.path));
  for (auto& kv : expect) {
    uint64_t dino = 0;
    CHECK(resolve(v, kv.first, &dino) == 0);
    std::vector<Entry> ents;
    CHECK(list_dir(v, dino, &ents) == 0);
    std::set<std::string> got; int dots = 0, dotdots = 0; size_t nonspecial = 0;
    for (auto& e : ents) {
      if (e.name == ".") { dots++; CHECK(e.ino == dino); continue; }
      if (e.name == "..") { dotdots++; if (kv.first.empty()) CHECK(e.ino == USBMEM_EXT2_ROOT_INO); continue; }
      got.insert(e.name); nonspecial++;
    }
    CHECK(dots == 1 && dotdots == 1);
    CHECK(got.size() == nonspecial);      // 重複なし
    if (got != kv.second) {
      std::fprintf(stderr, "dir '%s': expected %zu entries, got %zu\n", kv.first.c_str(), kv.second.size(), got.size());
      for (auto& n : kv.second) if (!got.count(n)) std::fprintf(stderr, "  missing: %s\n", n.c_str());
      for (auto& n : got) if (!kv.second.count(n)) std::fprintf(stderr, "  extra: %s\n", n.c_str());
      g_fail++;
    }
    for (auto& e : ents) {   // readdir が返した ino で、そのまま stat できる。種別が一致する
      if (e.name == "." || e.name == "..") continue;
      usbmem_ext2_stat st;
      CHECK(usbmem_ext2_stat_ino(v, e.ino, &st) == 0);
      std::string full = kv.first.empty() ? e.name : kv.first + "/" + e.name;
      bool is_dir = expect.count(full) > 0;
      bool is_link = false; for (auto& l : man.links) if (l.path == full) is_link = true;
      CHECK(is_dir == (e.type == 4 /*DT_DIR*/));
      CHECK(is_link == (e.type == 10 /*DT_LNK*/));
      CHECK(((st.mode & 0170000) == 0040000) == is_dir);
      CHECK(((st.mode & 0170000) == 0120000) == is_link);
      if (is_dir) CHECK(st.nlink >= 2);
    }
  }
  for (auto& d : man.dirs) {   // ディレクトリの mtime
    uint64_t ino = 0; usbmem_ext2_stat st;
    CHECK(resolve(v, d.first, &ino) == 0 && usbmem_ext2_stat_ino(v, ino, &st) == 0);
    CHECK(st.mtime_sec == d.second);
  }
  // 全ファイル: サイズ・内容・mtime・nlink
  for (auto& f : man.files) {
    uint64_t ino = 0;
    int rc = resolve(v, f.path, &ino);
    CHECK(rc == 0);
    if (rc) { std::fprintf(stderr, "resolve %s: %d\n", f.path.c_str(), rc); continue; }
    verify_file(v, f, ino);
  }
  // シンボリックリンク: stat の mode は S_IFLNK、read でリンク先の文字列が読める (高速/低速/ぶら下がり)
  for (auto& l : man.links) {
    uint64_t ino = 0; usbmem_ext2_stat st;
    CHECK(resolve(v, l.path, &ino) == 0);       // lookup はリンクを辿らない (リンク自体の inode)
    CHECK(usbmem_ext2_stat_ino(v, ino, &st) == 0);
    CHECK((st.mode & 0170000) == 0120000);
    CHECK(st.size == l.target.size());
    std::vector<char> b(4096, 'x');
    int64_t n = usbmem_ext2_read(v, ino, 0, b.data(), b.size());
    CHECK(n == (int64_t)l.target.size());
    if (n == (int64_t)l.target.size()) CHECK(std::memcmp(b.data(), l.target.data(), l.target.size()) == 0);
  }
  CHECK(!man.links.empty());
  // 名前の扱い
  uint64_t a = 0, b = 0, c = 0;
  CHECK(resolve(v, "small.txt", &a) == 0);
  CHECK(resolve(v, "hardlink_to_small.txt", &b) == 0);
  CHECK(a == b);                                // ハードリンクは同じ inode
  CHECK(resolve(v, "SMALL.TXT", &c) == -ENOENT);   // ext2 は大文字小文字を区別する
  CHECK(resolve(v, "日本語ファイル名.txt", &c) == 0);
  CHECK(resolve(v, std::string(255, 'n'), &c) == 0);
  CHECK(resolve(v, std::string(256, 'n'), &c) != 0);    // 長すぎる名前
  CHECK(resolve(v, "nonexistent", &c) == -ENOENT);
  CHECK(resolve(v, "d1/nonexistent", &c) == -ENOENT);
  CHECK(resolve(v, "small.txt/x", &c) == -ENOTDIR);
  uint64_t d1 = 0; CHECK(resolve(v, "d1", &d1) == 0);
  std::vector<uint8_t> tmp(16);
  CHECK(usbmem_ext2_read(v, d1, 0, tmp.data(), 16) == -EISDIR);
  // 不正な ino
  CHECK(usbmem_ext2_stat_ino(v, 0, nullptr) < 0);
  CHECK(usbmem_ext2_stat_ino(v, 1, nullptr) < 0);              // 不良ブロック inode
  CHECK(usbmem_ext2_stat_ino(v, 999999999, nullptr) < 0);
  CHECK(usbmem_ext2_stat_ino(v, 1ull << 40, nullptr) < 0);
  CHECK(usbmem_ext2_stat_ino(v, 20000, nullptr) < 0);          // 範囲内だが未使用の inode
  CHECK(usbmem_ext2_lookup(v, 999999999, "x", 1, &c) < 0);
  // 読み取り専用: 書き込み・マウント要求
  BlockDevicePtr p2 = spy; usbmem_bdev* bd2 = usbmem_bdev_wrap(&p2); usbmem_ext2* v2 = nullptr;
  // 書き込みを要求: 書き込みを無効にしたビルドは -EROFS。USBMEM_NTFS_WRITE=ON のビルドでは成功しうる (その場合も、書き込みを始めるまでメディアには何も書かない = 下の writes / flushes == 0)
  int rw_rc = usbmem_ext2_mount(bd2, 0, &v2);
  CHECK(rw_rc == -EROFS || rw_rc == 0);
  if (rw_rc == 0) usbmem_ext2_unmount(v2);
  usbmem_bdev_free(bd2);
  CHECK(spy->writes == 0);
  CHECK(spy->flushes == 0);
  // 2 回目のマウント (別インスタンス) も同じ結果 (モジュール init の再実行などで壊れない)
  auto m2 = mount_dev(std::make_shared<SpyDevice>(base));
  CHECK(m2->err == 0);
  if (m2->v) { uint64_t x = 0; CHECK(resolve(m2->v, "d1/d2/d3/d4/d5/d6/d7/d8/leaf.txt", &x) == 0); }
  std::fprintf(stderr, "   %zu files, %zu dirs, %zu links, device reads %ld\n", man.files.size(), man.dirs.size(), man.links.size(), (long)spy->reads);
}

static std::vector<std::string> list_images(const std::string& dir) {
  std::vector<std::string> out;
  for (const char* n : {"b1k", "b2k", "b4k"}) if (access((dir + "/" + n + ".img").c_str(), R_OK) == 0) out.push_back(n);
  return out;
}

// 全体を歩く (fuzz / ioerr 用)。クラッシュ・ハングしないことだけを見る。budget で打ち切り。
static void walk(usbmem_ext2* v, uint64_t dir, int depth, int& budget) {
  if (depth > 12 || budget <= 0) return;
  std::vector<Entry> ents;
  if (list_dir(v, dir, &ents)) return;
  for (auto& e : ents) {
    if (--budget <= 0) return;
    if (e.name == "." || e.name == "..") continue;
    usbmem_ext2_stat st;
    if (usbmem_ext2_stat_ino(v, e.ino, &st)) continue;
    if ((st.mode & 0170000) == 0040000) walk(v, e.ino, depth + 1, budget);
    else { std::vector<uint8_t> buf(65536); uint64_t sz = std::min<uint64_t>(st.size, 1u << 20); for (uint64_t o = 0; o < sz && budget > 0; o += buf.size(), budget--) if (usbmem_ext2_read(v, e.ino, o, buf.data(), buf.size()) <= 0) break; }
  }
}

// スーパーブロックは 1024 バイト目から。フィールド (ext2_super_block): 0x00 inodes_count, 0x04 blocks_count, 0x14 first_data_block, 0x18 log_block_size,
// 0x20 blocks_per_group, 0x28 inodes_per_group, 0x38 magic, 0x4C rev_level, 0x54 first_ino, 0x58 inode_size, 0x5C feature_compat, 0x60 incompat, 0x64 ro_compat
static void test_corrupt(const std::string& dir) {
  for (auto& name : list_images(dir)) {
    BlockDevicePtr base = open_img(dir + "/" + name + ".img");
    CHECK(base);
    CHECK(mount_dev(std::make_shared<SpyDevice>(base))->err == 0);   // 正常
    const uint64_t SB = 1024;
    struct Case { const char* what; std::map<uint64_t, uint8_t> mut; uint64_t size; bool must_fail; };
    std::vector<Case> cases = {
      {"magic", {{SB + 0x38, 0x00}, {SB + 0x39, 0x00}}, 0, true},
      {"log_block_size = 255", {{SB + 0x18, 0xFF}}, 0, true},
      {"unknown incompat feature (extents)", {{SB + 0x60, 0x40 | 0x02}}, 0, true},
      {"unknown incompat feature (64bit)", {{SB + 0x61, 0x80}}, 0, true},
      {"blocks_per_group = 0", {{SB + 0x20, 0}, {SB + 0x21, 0}, {SB + 0x22, 0}, {SB + 0x23, 0}}, 0, true},
      {"inodes_per_group = 0", {{SB + 0x28, 0}, {SB + 0x29, 0}, {SB + 0x2A, 0}, {SB + 0x2B, 0}}, 0, true},
      {"inode_size = 100 (not a power of 2)", {{SB + 0x58, 100}}, 0, true},
      {"inode_size = 8192", {{SB + 0x58, 0x00}, {SB + 0x59, 0x20}}, 0, true},
      {"blocks_count huge", {{SB + 0x07, 0x7F}}, 0, true},
      {"inodes_count = 0", {{SB + 0x00, 0}, {SB + 0x01, 0}, {SB + 0x02, 0}, {SB + 0x03, 0}}, 0, true},
      {"first_data_block wrong", {{SB + 0x14, 0x05}}, 0, false},   // 上流 ext2 は検査しない (マウントできる。クラッシュしないことだけ見る)
      {"rev_level = 7", {{SB + 0x4C, 7}}, 0, false},
      {"first_ino = 0", {{SB + 0x54, 0}}, 0, false},
      {"device truncated to 1 KiB", {}, 1024, true},
      {"device truncated to 64 KiB", {}, 65536, true},
      {"device truncated to 1 MiB", {}, 1u << 20, false},   // ブロック数との不整合は検出するが、実装によってはマウントできる
      {"zeroed superblock", {}, 0, true},
    };
    for (int i = 0; i < 1024; i++) cases.back().mut[SB + (uint64_t)i] = 0;
    for (auto& c : cases) {
      alarm(30);
      auto m = mount_dev(std::make_shared<SpyDevice>(base, -1, c.mut, c.size));
      if (m->v) { int b = 400; walk(m->v, USBMEM_EXT2_ROOT_INO, 0, b); }
      if (c.must_fail && m->err == 0) { std::fprintf(stderr, "%s: [%s] mount unexpectedly succeeded\n", name.c_str(), c.what); g_fail++; }
      std::fprintf(stderr, "   %-6s %-40s -> mount %s (%d)\n", name.c_str(), c.what, m->err ? "failed" : "ok", m->err);
      alarm(0);
    }
  }
  // ext4 の機能 (extents / 64bit / metadata_csum ...) を持つボリュームは、fs/ext2 が拒否する。ext3 (ジャーナル付き・クリーン) は incompat を持たないのでマウントできる
  BlockDevicePtr e4 = open_img(dir + "/ext4_empty.img");
  if (e4) {
    auto m = mount_dev(std::make_shared<SpyDevice>(e4));
    std::fprintf(stderr, "   ext4_empty (extent/64bit/metadata_csum) -> mount %s (%d)\n", m->err ? "failed" : "ok", m->err);
    CHECK(m->err != 0);
    CHECK(!m->v);
  }
  BlockDevicePtr e3 = open_img(dir + "/ext3_empty.img");
  if (e3) {
    auto m = mount_dev(std::make_shared<SpyDevice>(e3));
    std::fprintf(stderr, "   ext3_empty (clean journal)             -> mount %s (%d)\n", m->err ? "failed" : "ok", m->err);
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
      if (m->v) { ok++; int b = 400; walk(m->v, USBMEM_EXT2_ROOT_INO, 0, b); } else mount_fail++;
      alarm(0);
    }
    for (long n = 60; n < 400; n += 7) {
      alarm(30);
      auto m = mount_dev(std::make_shared<SpyDevice>(base, n));
      if (m->v) { int b = 400; walk(m->v, USBMEM_EXT2_ROOT_INO, 0, b); }
      alarm(0);
    }
    std::fprintf(stderr, "   %s: read-failure injection: mount failed %d, mounted-then-failed %d (no crash)\n", name.c_str(), mount_fail, ok);
    CHECK(mount_fail > 0);   // 最初の read から失敗するなら、マウントは失敗するはず
  }
}

static uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static int fuzz(const std::string& dir, int iters, unsigned seed) {
  auto images = list_images(dir);
  uint64_t x = seed * 2654435761u + 1;
  auto rnd = [&]() { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
  int mounted = 0, total = 0;
  for (auto& name : images) {
    BlockDevicePtr base = open_img(dir + "/" + name + ".img");
    if (!base) { g_fail++; continue; }
    uint8_t sb[1024] = {0};
    base->read_at(1024, sb, 1024);
    uint32_t bsz = 1024u << le32(sb + 0x18);
    uint64_t gdt = (le32(sb + 0x14) + 1ull) * bsz;                  // グループディスクリプタ (first_data_block の次のブロック)
    uint8_t gd[32] = {0}; base->read_at(gdt, gd, 32);
    uint64_t bbmp = (uint64_t)le32(gd + 0) * bsz, ibmp = (uint64_t)le32(gd + 4) * bsz, itab = (uint64_t)le32(gd + 8) * bsz;
    uint64_t isz = le32(sb + 0x58) ? (uint16_t)(sb[0x58] | (sb[0x59] << 8)) : 128;
    (void)isz;
    for (int it = 0; it < iters; it++) {
      std::map<uint64_t, uint8_t> mut;
      int nmut = 1 + (int)(rnd() % 24);
      unsigned region = (unsigned)(rnd() % 7);   // 1 反復では 1 つの領域だけを壊す (スーパーブロックを壊すと即失敗して奥のコードに届かないため、領域を分ける)
      for (int k = 0; k < nmut; k++) {
        uint64_t off;
        switch (region) {
          case 0: off = 1024 + rnd() % 1024; break;                                         // スーパーブロック
          case 1: off = gdt + rnd() % 32; break;                                            // グループディスクリプタ (先頭 1 個)
          case 2: off = itab + rnd() % (64u << 10); break;                                  // inode テーブルの先頭 (ルート・lost+found など)
          case 3: off = itab + rnd() % std::min<uint64_t>(base->size() - itab, 2u << 20); break;   // inode テーブル全域・その先 (ディレクトリ・間接ブロック)
          case 4: off = (rnd() % 2 ? bbmp : ibmp) + rnd() % bsz; break;                     // ビットマップ
          case 5: off = rnd() % std::min<uint64_t>(base->size(), 4u << 20); break;         // 先頭 4 MiB のどこか
          default: off = rnd() % base->size(); break;
        }
        mut[off] = (uint8_t)(rnd() % 3 == 0 ? 0xFF : rnd());
      }
      if (const char* lp = std::getenv("USBMEM_FUZZ_LAST")) {
        if (FILE* lf = std::fopen(lp, "w")) { std::fprintf(lf, "%s\n", name.c_str()); for (auto& kv : mut) std::fprintf(lf, "%llu %u\n", (unsigned long long)kv.first, kv.second); std::fclose(lf); }
      }
      alarm(30);   // ハング検出
      auto m = mount_dev(std::make_shared<SpyDevice>(base, -1, mut));
      total++;
      if (m->v) { mounted++; int budget = 1500; walk(m->v, USBMEM_EXT2_ROOT_INO, 0, budget); }
      CHECK(m->dev->writes == 0);
      alarm(0);
    }
  }
  std::fprintf(stderr, "fuzz: %d iterations (seed %u), %d mounted, no crash/hang\n", total, seed, mounted);
  return g_fail ? 1 : 0;
}

int main(int argc, char** argv) {
  std::signal(SIGALRM, [](int) { std::fprintf(stderr, "TIMEOUT (hang)\n"); std::abort(); });
  if (argc < 2) { std::fprintf(stderr, "usage: %s <fixture_dir> | --corrupt <dir> | --ioerr <dir> | --fuzz <dir> <iters> <seed>\n", argv[0]); return 2; }
  std::string mode = argv[1];
  if (mode == "--fuzz" && argc >= 5) return fuzz(argv[2], std::atoi(argv[3]), (unsigned)std::atoi(argv[4]));
  if (mode == "--corrupt" && argc >= 3) { test_corrupt(argv[2]); return g_fail ? 1 : 0; }
  if (mode == "--ioerr" && argc >= 3) { test_ioerr(argv[2]); return g_fail ? 1 : 0; }
  auto images = list_images(argv[1]);
  CHECK(!images.empty());
  for (auto& n : images) verify_image(argv[1], n);
  if (g_fail) { std::fprintf(stderr, "FAILED: %d checks\n", g_fail); return 1; }
  std::fprintf(stderr, "ext2: all checks passed (%zu images)\n", images.size());
  return 0;
}
