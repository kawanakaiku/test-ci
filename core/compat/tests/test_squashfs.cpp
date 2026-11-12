// SPDX-License-Identifier: GPL-2.0-or-later
// squashfs (Linux fs/squashfs を LinuxCompat 上で動かしたもの) の読み取りテスト。
// 使い方: usbmem_squashfs_tests <fixture_dir>            tools/gen_squashfs_fixture.py が作った *.img / *.manifest を全部検証
//         usbmem_squashfs_tests --corrupt <fixture_dir>  壊れたボリューム (スーパーブロック/サイズ/圧縮方式) が、クラッシュせず失敗すること
//         usbmem_squashfs_tests --ioerr <fixture_dir>    マウント・走査中のデバイス I/O エラーで、クラッシュせず失敗すること
//         usbmem_squashfs_tests --fuzz <fixture_dir> <iterations> <seed>
//         usbmem_squashfs_tests --coexist <squashfs_dir> <ext2_dir>   squashfs と ext2 のコアを同一プロセスで同時に使う
#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>

#include "usbmem/devices.h"
#include "usbmem/ext2_c.h"
#include "usbmem/ntfs_c.h"   // usbmem_bdev_wrap / free
#include "usbmem/squashfs_c.h"
#include "usbmem/squashfs_volume.h"

using namespace usbmem;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

// 読み取り専用デバイス。write_at が 1 度でも呼ばれたら数える。fail_after >= 0 ならその回数の read_at の後はすべて EIO。mut は読み出し時に差し替えるバイト。
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
  std::shared_ptr<SpyDevice> dev; usbmem_bdev* bd = nullptr; usbmem_squashfs* v = nullptr; int err = 0; uint64_t root = 0;
  ~Mounted() { if (v) usbmem_squashfs_unmount(v); if (bd) usbmem_bdev_free(bd); }
};
static std::unique_ptr<Mounted> mount_dev(std::shared_ptr<SpyDevice> dev) {
  auto m = std::make_unique<Mounted>();
  m->dev = dev;
  BlockDevicePtr p = dev;
  m->bd = usbmem_bdev_wrap(&p);
  m->err = usbmem_squashfs_mount(m->bd, 1, &m->v);
  if (m->err) m->v = nullptr; else m->root = usbmem_squashfs_root_ino(m->v);
  return m;
}
static BlockDevicePtr open_img(const std::string& path) {
  BlockDevicePtr d;
  if (!RawImageDevice::open_path(path, true, 512, &d).ok()) return nullptr;
  return d;
}

struct Entry { std::string name; uint64_t ino; unsigned type; };
static int list_dir(usbmem_squashfs* v, uint64_t dir, std::vector<Entry>* out) {
  out->clear();
  return usbmem_squashfs_readdir(v, dir, [](void* u, const char* n, int len, uint64_t ino, unsigned t) {
    static_cast<std::vector<Entry>*>(u)->push_back({std::string(n, (size_t)len), ino, t}); return 0; }, out);
}
static int resolve(usbmem_squashfs* v, uint64_t root, const std::string& path, uint64_t* ino) {
  uint64_t cur = root; size_t i = 0;
  while (i < path.size()) {
    size_t j = path.find('/', i); if (j == std::string::npos) j = path.size();
    if (j > i) { std::string part = path.substr(i, j - i); int rc = usbmem_squashfs_lookup(v, cur, part.data(), part.size(), &cur); if (rc) return rc; }
    i = j + 1;
  }
  *ino = cur; return 0;
}

// manifest (tools/gen_squashfs_fixture.py): D path mtime / F path size mtime nlink spec / L path target / S path kind / R path size mtime reffile
struct Region { char kind; uint64_t off, len; uint32_t seed; };   // 'd' = pattern(seed) を (ファイルオフセット) から、'z' = ゼロ
struct MFile { std::string path; uint64_t size; int64_t mtime; uint32_t nlink; std::vector<Region> regions; std::string reffile; };
struct MLink { std::string path, target; };
struct MSpecial { std::string path; char kind; };
struct Manifest {
  std::vector<std::pair<std::string, int64_t>> dirs; std::vector<MFile> files; std::vector<MLink> links; std::vector<MSpecial> specials;
};
static bool load_manifest(const std::string& path, Manifest* m) {
  std::ifstream f(path);
  if (!f) return false;
  std::string line;
  while (std::getline(f, line)) {
    std::vector<std::string> c; std::stringstream ss(line); std::string t;
    while (std::getline(ss, t, '\t')) c.push_back(t);
    if (c.empty()) continue;
    if (c.size() == 3 && c[0] == "D") m->dirs.push_back({c[1], std::stoll(c[2])});
    else if (c.size() == 3 && c[0] == "L") m->links.push_back({c[1], c[2]});
    else if (c.size() == 3 && c[0] == "S") m->specials.push_back({c[1], c[2][0]});
    else if (c[0] == "R" && c.size() == 5) { MFile mf{c[1], std::stoull(c[2]), std::stoll(c[3]), 1, {}, c[4]}; m->files.push_back(mf); }
    else if (c[0] == "F" && c.size() >= 5) {
      MFile mf{c[1], std::stoull(c[2]), std::stoll(c[3]), (uint32_t)std::stoul(c[4]), {}, ""};
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
static std::vector<uint8_t> slurp(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static void verify_file(usbmem_squashfs* v, const std::string& dir, const MFile& f, uint64_t ino) {
  usbmem_squashfs_stat st;
  CHECK(usbmem_squashfs_stat_ino(v, ino, &st) == 0);
  CHECK(st.size == f.size);
  CHECK((st.mode & 0170000) == 0100000);
  CHECK(st.mtime_sec == f.mtime);
  CHECK(st.nlink == f.nlink);
  std::vector<uint8_t> tmp(16, 0xAB);
  CHECK(usbmem_squashfs_read(v, ino, f.size, tmp.data(), 10) == 0);          // ちょうど EOF
  CHECK(usbmem_squashfs_read(v, ino, f.size + 4096, tmp.data(), 10) == 0);   // EOF より先
  std::vector<uint8_t> ref;
  if (!f.reffile.empty()) ref = slurp(dir + "/" + f.reffile);
  if (f.size <= (16u << 20)) {
    std::vector<uint8_t> want(f.size, 0);
    if (!f.reffile.empty()) { CHECK(ref.size() == f.size); want = ref; want.resize(f.size); }
    for (auto& r : f.regions) if (r.kind == 'd') { auto p = pattern_at(r.seed, r.off, (size_t)r.len); std::memcpy(want.data() + r.off, p.data(), p.size()); }
    std::vector<uint8_t> got(f.size + 16, 0xAB);
    int64_t n = usbmem_squashfs_read(v, ino, 0, got.data(), got.size());     // 要求が EOF を超えても、残りだけ返す
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
        CHECK(usbmem_squashfs_read(v, ino, off, part.data(), len) == (int64_t)len);
        CHECK(std::memcmp(part.data(), want.data() + off, len) == 0);
      }
    }
    return;
  }
  // 巨大な疎ファイル: データ領域は全部、ゼロ領域は先頭・末尾・途中
  auto check_range = [&](uint64_t off, uint64_t len, uint32_t seed, bool zero) {
    std::vector<uint8_t> got((size_t)len);
    int64_t n = usbmem_squashfs_read(v, ino, off, got.data(), got.size());
    CHECK(n == (int64_t)len);
    if (n != (int64_t)len) return;
    if (zero) { for (size_t i = 0; i < got.size(); i++) if (got[i] != 0) { std::fprintf(stderr, "%s: nonzero at %llu\n", f.path.c_str(), (unsigned long long)(off + i)); g_fail++; break; } }
    else { auto w = pattern_at(seed, off, (size_t)len); CHECK(std::memcmp(got.data(), w.data(), w.size()) == 0); }
  };
  for (auto& r : f.regions) {
    if (r.kind == 'd') check_range(r.off, r.len, r.seed, false);
    else {
      uint64_t head = std::min<uint64_t>(r.len, 8192);
      check_range(r.off, head, 0, true);
      if (r.len > head) check_range(r.off + r.len - head, head, 0, true);
      if (r.len > 3 * head) check_range(r.off + r.len / 2, head, 0, true);
    }
  }
}

static unsigned mode_to_dtype(uint32_t mode) {
  switch (mode & 0170000) {
    case 0040000: return 4; case 0120000: return 10; case 0100000: return 8;
    case 0020000: return 2; case 0060000: return 6; case 0010000: return 1; case 0140000: return 12;
  }
  return 0;
}

static std::string base_name_of_image(const std::string& dir, const std::string& name) { return dir + "/" + name; }

static void verify_image(const std::string& dir, const std::string& name) {
  std::fprintf(stderr, "== %s\n", name.c_str());
  Manifest man;
  CHECK(load_manifest(dir + "/" + name + ".manifest", &man));
  BlockDevicePtr base = open_img(base_name_of_image(dir, name) + ".img");
  CHECK(base != nullptr);
  if (!base) return;
  auto spy = std::make_shared<SpyDevice>(base);
  auto m = mount_dev(spy);
  CHECK(m->err == 0 && m->v);
  if (!m->v) { std::fprintf(stderr, "mount failed: %d\n", m->err); return; }
  usbmem_squashfs* v = m->v;
  const uint64_t root = m->root;

  // 期待するディレクトリの内容 (パス → 名前の集合)
  std::map<std::string, std::set<std::string>> expect;
  expect[""];
  for (auto& d : man.dirs) { expect[parent_of(d.first)].insert(base_of(d.first)); expect[d.first]; }
  for (auto& f : man.files) expect[parent_of(f.path)].insert(base_of(f.path));
  for (auto& l : man.links) expect[parent_of(l.path)].insert(base_of(l.path));
  for (auto& s : man.specials) expect[parent_of(s.path)].insert(base_of(s.path));
  std::map<std::string, char> special_kind;
  for (auto& s : man.specials) special_kind[s.path] = s.kind;
  for (auto& kv : expect) {
    uint64_t dino = 0;
    CHECK(resolve(v, root, kv.first, &dino) == 0);
    std::vector<Entry> ents;
    CHECK(list_dir(v, dino, &ents) == 0);
    std::set<std::string> got;
    for (auto& e : ents) got.insert(e.name);
    CHECK(got.size() == ents.size());      // 重複なし ("." / ".." は返さない)
    if (got != kv.second) {
      std::fprintf(stderr, "dir '%s': expected %zu entries, got %zu\n", kv.first.c_str(), kv.second.size(), got.size());
      for (auto& n : kv.second) if (!got.count(n)) std::fprintf(stderr, "  missing: %s\n", n.c_str());
      for (auto& n : got) if (!kv.second.count(n)) std::fprintf(stderr, "  extra: %s\n", n.c_str());
      g_fail++;
    }
    for (auto& e : ents) {   // readdir が返した ino でそのまま stat できる。dirent の種別と inode の種別が一致する
      usbmem_squashfs_stat st;
      CHECK(usbmem_squashfs_stat_ino(v, e.ino, &st) == 0);
      std::string full = kv.first.empty() ? e.name : kv.first + "/" + e.name;
      bool is_dir = expect.count(full) > 0;
      bool is_link = false; for (auto& l : man.links) if (l.path == full) is_link = true;
      CHECK(is_dir == (e.type == 4 /*DT_DIR*/));
      CHECK(is_link == (e.type == 10 /*DT_LNK*/));
      CHECK(((st.mode & 0170000) == 0040000) == is_dir);
      CHECK(((st.mode & 0170000) == 0120000) == is_link);
      CHECK(mode_to_dtype(st.mode) == e.type);
      if (is_dir) CHECK(st.nlink >= 2);
      uint64_t again = 0;
      CHECK(usbmem_squashfs_lookup(v, dino, e.name.data(), e.name.size(), &again) == 0 && again == e.ino);   // lookup と readdir の ino が一致
    }
  }
  for (auto& d : man.dirs) {   // ディレクトリの mtime
    uint64_t ino = 0; usbmem_squashfs_stat st;
    CHECK(resolve(v, root, d.first, &ino) == 0 && usbmem_squashfs_stat_ino(v, ino, &st) == 0);
    if (st.mtime_sec != d.second) std::fprintf(stderr, "dir mtime %s: got %lld want %lld\n", d.first.c_str(), (long long)st.mtime_sec, (long long)d.second);
    CHECK(st.mtime_sec == d.second);
  }
  for (auto& f : man.files) {   // 全ファイル: サイズ・内容・mtime・nlink
    uint64_t ino = 0;
    int rc = resolve(v, root, f.path, &ino);
    CHECK(rc == 0);
    if (rc) { std::fprintf(stderr, "resolve %s: %d\n", f.path.c_str(), rc); continue; }
    verify_file(v, dir, f, ino);
  }
  // シンボリックリンク: stat の mode は S_IFLNK、read でリンク先の文字列、readlink (C++ ラッパ) も同じ
  for (auto& l : man.links) {
    uint64_t ino = 0; usbmem_squashfs_stat st;
    CHECK(resolve(v, root, l.path, &ino) == 0);       // lookup はリンクを辿らない (リンク自体の inode)
    CHECK(usbmem_squashfs_stat_ino(v, ino, &st) == 0);
    CHECK((st.mode & 0170000) == 0120000);
    CHECK(st.size == l.target.size());
    std::vector<char> b(4096, 'x');
    int64_t n = usbmem_squashfs_read(v, ino, 0, b.data(), b.size());
    CHECK(n == (int64_t)l.target.size());
    if (n == (int64_t)l.target.size()) CHECK(std::memcmp(b.data(), l.target.data(), l.target.size()) == 0);
  }
  CHECK(!man.links.empty());
  // 特殊ファイル (キャラクタ / ブロック / FIFO / ソケット): 種別が正しく、read は -EINVAL
  for (auto& s : man.specials) {
    uint64_t ino = 0; usbmem_squashfs_stat st;
    CHECK(resolve(v, root, s.path, &ino) == 0);
    CHECK(usbmem_squashfs_stat_ino(v, ino, &st) == 0);
    uint32_t want = s.kind == 'c' ? 0020000 : s.kind == 'b' ? 0060000 : s.kind == 'p' ? 0010000 : 0140000;
    CHECK((st.mode & 0170000) == want);
    char b[8];
    CHECK(usbmem_squashfs_read(v, ino, 0, b, 8) == -EINVAL);
  }
  CHECK(man.specials.size() == 4);
  // 名前の扱い
  uint64_t a = 0, b = 0, c = 0;
  CHECK(resolve(v, root, "small.txt", &a) == 0);
  CHECK(resolve(v, root, "hardlink_to_small.txt", &b) == 0);
  CHECK(a == b);                                // ハードリンクは同じ inode
  CHECK(resolve(v, root, "SMALL.TXT", &c) == -ENOENT);
  CHECK(resolve(v, root, "日本語ファイル名.txt", &c) == 0);
  CHECK(resolve(v, root, std::string(255, 'n'), &c) == 0);
  CHECK(resolve(v, root, std::string(256, 'n'), &c) != 0);
  CHECK(resolve(v, root, "nonexistent", &c) == -ENOENT);
  CHECK(resolve(v, root, "d1/nonexistent", &c) == -ENOENT);
  CHECK(resolve(v, root, "small.txt/x", &c) == -ENOTDIR);
  uint64_t d1 = 0; CHECK(resolve(v, root, "d1", &d1) == 0);
  std::vector<uint8_t> tmp(16);
  CHECK(usbmem_squashfs_read(v, d1, 0, tmp.data(), 16) == -EISDIR);
  // 不正な ino (ピンしていない番号は ENOENT)
  CHECK(usbmem_squashfs_stat_ino(v, 0, nullptr) < 0);
  CHECK(usbmem_squashfs_stat_ino(v, 999999999, nullptr) < 0);
  CHECK(usbmem_squashfs_stat_ino(v, 1ull << 40, nullptr) < 0);
  CHECK(usbmem_squashfs_lookup(v, 999999999, "x", 1, &c) < 0);
  // 書き込みマウントは常に拒否 (squashfs は読み取り専用の形式)
  BlockDevicePtr p2 = spy; usbmem_bdev* bd2 = usbmem_bdev_wrap(&p2); usbmem_squashfs* v2 = nullptr;
  CHECK(usbmem_squashfs_mount(bd2, 0, &v2) == -EROFS);
  usbmem_bdev_free(bd2);
  CHECK(spy->writes == 0);
  CHECK(spy->flushes == 0);
  // 2 回目のマウント (別インスタンス・同時) も同じ結果
  auto m2 = mount_dev(std::make_shared<SpyDevice>(base));
  CHECK(m2->err == 0);
  if (m2->v) { uint64_t x = 0; CHECK(resolve(m2->v, m2->root, "d1/d2/d3/d4/d5/d6/d7/d8/leaf.txt", &x) == 0); }
  std::fprintf(stderr, "   %zu files, %zu dirs, %zu links, %zu specials, device reads %ld\n", man.files.size(), man.dirs.size(), man.links.size(), man.specials.size(), (long)spy->reads);

  // C++ ラッパ (FsVolume) 経由: fs_name / readlink / readdir
  {
    auto dev2 = std::make_shared<SpyDevice>(base);
    BlockDevicePtr dp = dev2;
    int err = 0;
    auto vol = SquashfsVolume::mount(dp, &err);
    CHECK(vol != nullptr && err == 0);
    if (vol) {
      CHECK(std::string(vol->fs_name()) == "squashfs");
      uint64_t li = 0; CHECK(vol->lookup(vol->root(), "link_slow", &li) == 0);
      std::string tgt; CHECK(vol->readlink(li, &tgt) == 0);
      for (auto& l : man.links) if (l.path == "link_slow") CHECK(tgt == l.target);
      uint64_t fi = 0; CHECK(vol->lookup(vol->root(), "small.txt", &fi) == 0);
      CHECK(vol->readlink(fi, &tgt) == -EINVAL);
      std::vector<FsDirEntry> ents; CHECK(vol->readdir(vol->root(), &ents) == 0);
      CHECK(!ents.empty());
    }
  }
}

static std::vector<std::string> list_images(const std::string& dir) {
  std::vector<std::string> out;
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) {
      std::string n = e->d_name;
      if (n.size() > 4 && n.substr(n.size() - 4) == ".img" && access((dir + "/" + n.substr(0, n.size() - 4) + ".manifest").c_str(), R_OK) == 0) out.push_back(n.substr(0, n.size() - 4));
    }
    closedir(d);
  }
  std::sort(out.begin(), out.end());
  return out;
}

// 全体を歩く (fuzz / ioerr 用)。クラッシュ・ハング・過大なメモリ確保をしないことだけを見る。budget で打ち切り。
static void walk(usbmem_squashfs* v, uint64_t dir, int depth, int& budget) {
  if (depth > 12 || budget <= 0) return;
  std::vector<Entry> ents;
  if (list_dir(v, dir, &ents)) return;
  for (auto& e : ents) {
    if (--budget <= 0) return;
    usbmem_squashfs_stat st;
    if (usbmem_squashfs_stat_ino(v, e.ino, &st)) continue;
    uint32_t t = st.mode & 0170000;
    if (t == 0040000) { if (e.ino != dir) walk(v, e.ino, depth + 1, budget); }
    else if (t == 0100000 || t == 0120000) {
      std::vector<uint8_t> buf(65536);
      uint64_t sz = std::min<uint64_t>(st.size, 1u << 20);
      for (uint64_t o = 0; o < sz && budget > 0; o += buf.size(), budget--) if (usbmem_squashfs_read(v, e.ino, o, buf.data(), buf.size()) <= 0) break;
    }
  }
}

static uint64_t le64(const uint8_t* p) { uint64_t r = 0; for (int i = 7; i >= 0; i--) r = (r << 8) | p[i]; return r; }
static uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

// スーパーブロック (struct squashfs_super_block): 0 magic(4) 4 inodes 8 mkfs_time 12 block_size 16 fragments 20 compression(2) 22 block_log(2) 24 flags(2) 26 no_ids(2)
// 28 s_major(2) 30 s_minor(2) 32 root_inode(8) 40 bytes_used(8) 48 id_table_start 56 xattr_id_table_start 64 inode_table_start 72 directory_table_start 80 fragment_table_start 88 lookup_table_start
static void test_corrupt(const std::string& dir) {
  int n_cases = 0;
  for (auto& name : list_images(dir)) {
    if (name != "gzip_128k" && name != "xz_128k" && name != "zstd_8k" && name != "lz4_hc" && name != "nocomp") continue;
    BlockDevicePtr base = open_img(dir + "/" + name + ".img");
    CHECK(base);
    CHECK(mount_dev(std::make_shared<SpyDevice>(base))->err == 0);   // 正常
    auto Q = [](uint64_t off, uint64_t val, int n) { std::map<uint64_t, uint8_t> m; for (int i = 0; i < n; i++) m[off + (uint64_t)i] = (uint8_t)(val >> (8 * i)); return m; };
    struct Case { const char* what; std::map<uint64_t, uint8_t> mut; uint64_t size; bool must_fail; };
    std::vector<Case> cases = {
      {"magic", Q(0, 0x12345678, 4), 0, true},
      {"block_size = 0", Q(12, 0, 4), 0, true},
      {"block_size not power of 2 (3000)", Q(12, 3000, 4), 0, true},
      {"block_size = 2 MiB (over limit)", Q(12, 2u << 20, 4), 0, true},
      {"block_size = 512 (below limit)", Q(12, 512, 4), 0, true},
      {"block_log mismatch", Q(22, 3, 2), 0, true},
      {"block_log = 40", Q(22, 40, 2), 0, true},
      {"compression = 99", Q(20, 99, 2), 0, true},
      {"compression = 0", Q(20, 0, 2), 0, true},
      {"compression = lzma1 (2: unsupported)", Q(20, 2, 2), 0, true},
      {"major = 3", Q(28, 3, 2), 0, true},
      {"major = 4, minor = 99", Q(30, 99, 2), 0, true},
      {"bytes_used = huge", Q(40, 0x7FFFFFFFFFFFFFFFull, 8), 0, true},
      {"bytes_used = 1", Q(40, 1, 8), 0, false},
      {"root_inode = garbage", Q(32, 0xFFFFFFFFFFFFFFFFull, 8), 0, false},
      {"root_inode = 0", Q(32, 0, 8), 0, false},
      {"inode_table_start = huge", Q(64, 0x7FFFFFFFFFFFFFFFull, 8), 0, false},
      {"directory_table_start = huge", Q(72, 0x7FFFFFFFFFFFFFFFull, 8), 0, false},
      {"directory_table_start = 0", Q(72, 0, 8), 0, false},
      {"fragment_table_start = huge", Q(80, 0x7FFFFFFFFFFFFFFFull, 8), 0, false},
      {"id_table_start = huge", Q(48, 0x7FFFFFFFFFFFFFFFull, 8), 0, true},
      {"lookup_table_start = huge", Q(88, 0x7FFFFFFFFFFFFFFFull, 8), 0, false},
      {"xattr_id_table_start = huge (xattr 無視)", Q(56, 0x7FFFFFFFFFFFFFFFull, 8), 0, false},
      {"no_ids = 0", Q(26, 0, 2), 0, true},
      {"no_ids = 65535", Q(26, 0xFFFF, 2), 0, false},
      {"fragments = 0xFFFFFFFF", Q(16, 0xFFFFFFFFull, 4), 0, false},
      {"inodes = 0xFFFFFFFF", Q(4, 0xFFFFFFFFull, 4), 0, false},
      {"flags = all set", Q(24, 0xFFFF, 2), 0, false},
      {"device truncated to 0.5 KiB", {}, 512, true},
      {"device truncated to 64 B", {}, 64, true},
      {"device truncated to 1/2", {}, base->size() / 2, true},
      {"device truncated to size - 4096", {}, base->size() - 4096, false},
      {"zeroed superblock", {}, 0, true},
    };
    for (int i = 0; i < 96; i++) cases.back().mut[(uint64_t)i] = 0;
    for (auto& c : cases) {
      alarm(30);
      auto m = mount_dev(std::make_shared<SpyDevice>(base, -1, c.mut, c.size));
      if (m->v) { int b = 400; walk(m->v, m->root, 0, b); }
      if (c.must_fail && m->err == 0) { std::fprintf(stderr, "%s: [%s] mount unexpectedly succeeded\n", name.c_str(), c.what); g_fail++; }
      std::fprintf(stderr, "   %-10s %-42s -> mount %s (%d)\n", name.c_str(), c.what, m->err ? "failed" : "ok", m->err);
      alarm(0);
      n_cases++;
    }
  }
  // 非対応の圧縮方式 (旧 LZMA1): マウントは EOPNOTSUPP で拒否される
  BlockDevicePtr lz = open_img(dir + "/lzma_refused.img");
  if (lz) {
    auto m = mount_dev(std::make_shared<SpyDevice>(lz));
    std::fprintf(stderr, "   lzma_refused (LZMA1 compression)           -> mount %s (%d)\n", m->err ? "failed" : "ok", m->err);
    CHECK(m->err == -EOPNOTSUPP);   // 非対応の圧縮方式は EOPNOTSUPP で拒否 (上流は EINVAL。ホスト入口で区別)
    CHECK(!m->v);
  } else std::fprintf(stderr, "   lzma_refused.img absent (mksquashfs cannot produce lzma): skipped\n");
  // 展開が壊れたブロック: データ領域の先頭を全部 0xFF にして読み、クラッシュしない (エラーが返る)
  for (auto& name : list_images(dir)) {
    if (name != "gzip_128k" && name != "xz_128k" && name != "zstd_128k" && name != "lz4_128k" && name != "lzo_128k") continue;
    BlockDevicePtr base = open_img(dir + "/" + name + ".img");
    std::map<uint64_t, uint8_t> mut;
    for (uint64_t o = 96; o < 96 + 4096; o++) mut[o] = (uint8_t)(0xFF - (o & 3));   // 先頭のデータブロックの圧縮ストリームを壊す
    alarm(60);
    auto m = mount_dev(std::make_shared<SpyDevice>(base, -1, mut));
    if (m->v) { int b = 3000; walk(m->v, m->root, 0, b); }
    std::fprintf(stderr, "   %-10s damaged first data block -> mount %s (%d), no crash\n", name.c_str(), m->err ? "failed" : "ok", m->err);
    alarm(0);
    n_cases++;
  }
  std::fprintf(stderr, "corrupt: %d cases, no crash/hang\n", n_cases);
}

static void test_ioerr(const std::string& dir) {
  for (auto& name : list_images(dir)) {
    if (name != "gzip_128k" && name != "xz_128k" && name != "zstd_128k" && name != "lz4_128k" && name != "nocomp" && name != "gzip_4k") continue;
    BlockDevicePtr base = open_img(dir + "/" + name + ".img");
    CHECK(base);
    int mount_fail = 0, ok = 0;
    for (long n = 0; n < 40; n++) {
      alarm(30);
      auto m = mount_dev(std::make_shared<SpyDevice>(base, n));
      if (m->v) { ok++; int b = 400; walk(m->v, m->root, 0, b); } else mount_fail++;
      alarm(0);
    }
    for (long n = 40; n < 600; n += 9) {
      alarm(30);
      auto m = mount_dev(std::make_shared<SpyDevice>(base, n));
      if (m->v) { int b = 600; walk(m->v, m->root, 0, b); }
      alarm(0);
    }
    std::fprintf(stderr, "   %s: read-failure injection: mount failed %d, mounted-then-failed %d (no crash)\n", name.c_str(), mount_fail, ok);
    CHECK(mount_fail > 0);
  }
}

static int fuzz(const std::string& dir, int iters, unsigned seed) {
  std::vector<std::string> images;
  for (auto& n : list_images(dir)) if (n != "gzip_128k" && n != "xz_128k" && n != "zstd_8k" && n != "lz4_hc" && n != "lzo_128k" && n != "gzip_4k" && n != "nocomp") continue; else images.push_back(n);
  uint64_t x = seed * 2654435761u + 1;
  auto rnd = [&]() { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
  int mounted = 0, total = 0;
  for (auto& name : images) {
    BlockDevicePtr base = open_img(dir + "/" + name + ".img");
    if (!base) { g_fail++; continue; }
    uint8_t sb[96] = {0};
    base->read_at(0, sb, 96);
    uint64_t itab = le64(sb + 64), dtab = le64(sb + 72), ftab = le64(sb + 80), idt = le64(sb + 48), ltab = le64(sb + 88), used = le64(sb + 40);
    if (itab >= base->size() || dtab >= base->size()) { g_fail++; continue; }
    (void)le32;
    for (int it = 0; it < iters; it++) {
      std::map<uint64_t, uint8_t> mut;
      int nmut = 1 + (int)(rnd() % 24);
      unsigned region = (unsigned)(rnd() % 9);   // 1 反復では 1 つの領域だけを壊す (スーパーブロックを壊すと即失敗して奥のコードに届かないため)
      for (int k = 0; k < nmut; k++) {
        uint64_t off;
        switch (region) {
          case 0: off = rnd() % 96; break;                                                  // スーパーブロック
          case 1: off = 96 + rnd() % (itab > 96 ? itab - 96 : 1); break;                    // データブロック (先頭側)
          case 2: off = itab + rnd() % (dtab > itab ? dtab - itab : 1); break;              // inode テーブル (メタデータブロック)
          case 3: off = dtab + rnd() % (ftab > dtab ? ftab - dtab : 1); break;              // ディレクトリテーブル
          case 4: off = ftab + rnd() % 64; break;                                           // フラグメントテーブル
          case 5: off = idt + rnd() % 32; break;                                            // id テーブル
          case 6: off = ltab + rnd() % 64; break;                                           // エクスポート (lookup) テーブル
          case 7: off = rnd() % std::min<uint64_t>(used, 1u << 20); break;                  // 先頭 1 MiB のどこか
          default: off = rnd() % base->size(); break;
        }
        mut[off] = (uint8_t)(rnd() % 3 == 0 ? 0xFF : rnd());
      }
      if (const char* lp = std::getenv("USBMEM_FUZZ_LAST")) {
        if (FILE* lf = std::fopen(lp, "w")) { std::fprintf(lf, "%s\n", name.c_str()); for (auto& kv : mut) std::fprintf(lf, "%llu %u\n", (unsigned long long)kv.first, kv.second); std::fclose(lf); }
      }
      alarm(60);   // ハング検出
      auto m = mount_dev(std::make_shared<SpyDevice>(base, -1, mut));
      total++;
      if (m->v) { mounted++; int budget = 1500; walk(m->v, m->root, 0, budget); }
      CHECK(m->dev->writes == 0);
      alarm(0);
    }
  }
  struct rusage ru; getrusage(RUSAGE_SELF, &ru);
  std::fprintf(stderr, "fuzz: %d iterations (seed %u), %d mounted, no crash/hang, max RSS %ld MiB\n", total, seed, mounted, (long)(ru.ru_maxrss / 1024));
  return g_fail ? 1 : 0;
}

// squashfs と ext2 のコアを同一プロセスで同時に使う (互換層のグローバル状態・モジュール init の共存)
static int coexist(const std::string& dsq, const std::string& d2) {
  BlockDevicePtr bs = open_img(dsq + "/gzip_128k.img"), bx = open_img(dsq + "/xz_128k.img"), b2 = open_img(d2 + "/b4k.img");
  CHECK(bs && bx && b2);
  if (!bs || !bx || !b2) return 1;
  auto ms = mount_dev(std::make_shared<SpyDevice>(bs));
  auto mx = mount_dev(std::make_shared<SpyDevice>(bx));
  BlockDevicePtr p2 = std::make_shared<SpyDevice>(b2);
  usbmem_bdev* bd2 = usbmem_bdev_wrap(&p2);
  usbmem_ext2* v2 = nullptr;
  CHECK(usbmem_ext2_mount(bd2, 1, &v2) == 0);
  CHECK(ms->v && mx->v && v2);
  if (ms->v && mx->v && v2) {
    uint64_t is = 0, ix = 0, i2 = 0; std::vector<uint8_t> a(11), b(11), c(11);
    CHECK(resolve(ms->v, ms->root, "small.txt", &is) == 0);
    CHECK(resolve(mx->v, mx->root, "small.txt", &ix) == 0);
    CHECK(usbmem_ext2_lookup(v2, USBMEM_EXT2_ROOT_INO, "small.txt", 9, &i2) == 0);
    CHECK(usbmem_squashfs_read(ms->v, is, 0, a.data(), 11) == 11);
    CHECK(usbmem_squashfs_read(mx->v, ix, 0, b.data(), 11) == 11);
    CHECK(usbmem_ext2_read(v2, i2, 0, c.data(), 11) == 11);
    CHECK(a == b && a == c);              // 同じ内容 (どれも gen_*_fixture の small.txt = pattern(seed 2))
    // 大きなファイルを交互に読む (展開器の共有状態が混ざらない)
    uint64_t ls = 0, lx = 0; CHECK(resolve(ms->v, ms->root, "many/file_0999.dat", &ls) == 0 && resolve(mx->v, mx->root, "many/file_0999.dat", &lx) == 0);
    std::vector<uint8_t> x1(300), x2(300);
    CHECK(usbmem_squashfs_read(ms->v, ls, 0, x1.data(), 300) > 0);
    usbmem_squashfs_unmount(ms->v); ms->v = nullptr;     // 片方を外しても残りは使える
    CHECK(usbmem_squashfs_read(mx->v, lx, 0, x2.data(), 300) > 0);
    CHECK(usbmem_ext2_read(v2, i2, 0, c.data(), 11) == 11);
    ms = mount_dev(std::make_shared<SpyDevice>(bs));    // 再マウント
    CHECK(ms->v && resolve(ms->v, ms->root, "d1/d2/d3/d4/d5/d6/d7/d8/leaf.txt", &is) == 0);
  }
  if (v2) usbmem_ext2_unmount(v2);
  usbmem_bdev_free(bd2);
  std::fprintf(stderr, g_fail ? "coexist: FAILED\n" : "coexist: squashfs x2 + ext2 in one process OK\n");
  return g_fail ? 1 : 0;
}

int main(int argc, char** argv) {
  std::signal(SIGALRM, [](int) { std::fprintf(stderr, "TIMEOUT (hang)\n"); std::abort(); });
  if (argc < 2) { std::fprintf(stderr, "usage: %s <fixture_dir> | --corrupt <dir> | --ioerr <dir> | --fuzz <dir> <iters> <seed> | --coexist <sq_dir> <ext2_dir>\n", argv[0]); return 2; }
  std::string mode = argv[1];
  if (mode == "--fuzz" && argc >= 5) return fuzz(argv[2], std::atoi(argv[3]), (unsigned)std::atoi(argv[4]));
  if (mode == "--corrupt" && argc >= 3) { test_corrupt(argv[2]); return g_fail ? 1 : 0; }
  if (mode == "--ioerr" && argc >= 3) { test_ioerr(argv[2]); return g_fail ? 1 : 0; }
  if (mode == "--coexist" && argc >= 4) return coexist(argv[2], argv[3]);
  auto images = list_images(argv[1]);
  CHECK(!images.empty());
  for (auto& n : images) verify_image(argv[1], n);
  if (g_fail) { std::fprintf(stderr, "FAILED: %d checks\n", g_fail); return 1; }
  std::fprintf(stderr, "squashfs: all checks passed (%zu images)\n", images.size());
  return 0;
}
