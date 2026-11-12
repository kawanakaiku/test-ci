// SPDX-License-Identifier: GPL-2.0-or-later
// Btrfs 読み取りリーダー (core/compat/src/btrfs_volume.cpp) のテスト。
// 使い方: usbmem_btrfs_tests <fixture_dir>            tools/gen_btrfs_fixture.py が作った *.img / *.manifest を全部検証 + サブボリューム / nodatasum / 複数デバイス / ログ / データ破損
//         usbmem_btrfs_tests --corrupt <fixture_dir>  壊れたボリューム (スーパーブロックの全フィールド・メタデータ) が、クラッシュせず失敗する/読めること
//         usbmem_btrfs_tests --ioerr <fixture_dir>    マウント・走査中のデバイス I/O エラーで、クラッシュせず失敗すること
//         usbmem_btrfs_tests --fuzz <fixture_dir> <iterations> <seed>   メタデータのノードを壊し、チェックサムを直して (内容の検査に届かせて) 走査する
//         usbmem_btrfs_tests tree <img> [subvol_id]   全ツリーを "path<TAB>type<TAB>size<TAB>mtime<TAB>sha256" で出力 (独立した検証用)
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "usbmem/btrfs_volume.h"
#include "usbmem/devices.h"
#include "usbmem/sha256.h"

using namespace usbmem;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t le64(const uint8_t* p) { return le32(p) | ((uint64_t)le32(p + 4) << 32); }
static void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t crc32c(const uint8_t* p, size_t n) {   // 標準の CRC-32C (btrfs の既定 csum)
  static uint32_t t[256]; static bool init = false;
  if (!init) { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1; t[i] = c; } init = true; }
  uint32_t c = 0xFFFFFFFFu;
  while (n--) c = t[(c ^ *p++) & 0xFF] ^ (c >> 8);
  return ~c;
}

// 読み取り専用デバイス。write_at / flush が呼ばれたら数える。fail_after >= 0 ならその回数の read_at の後はすべて EIO。mut は読み出し時に差し替えるバイト。
class SpyDevice final : public BlockDevice {
 public:
  SpyDevice(BlockDevicePtr base, long fail_after = -1, std::map<uint64_t, uint8_t> mut = {}) : base_(std::move(base)), fail_after_(fail_after), mut_(std::move(mut)) {}
  Status read_at(uint64_t off, void* buf, size_t len, const IoOptions& o = {}) override {
    reads++;
    if (fail_after_ >= 0 && reads > fail_after_) return Status::Make(Errc::Io, EIO);
    if (!range_ok(base_->size(), off, len)) return Status::Make(Errc::OutOfRange);
    Status st = base_->read_at(off, buf, len, o);
    if (!st.ok()) return st;
    for (auto it = mut_.lower_bound(off); it != mut_.end() && it->first < off + len; ++it) static_cast<uint8_t*>(buf)[it->first - off] = it->second;
    return st;
  }
  Status write_at(uint64_t, const void*, size_t, const IoOptions& = {}) override { writes++; return Status::Make(Errc::ReadOnly); }
  Status flush() override { flushes++; return Status::Ok(); }
  Status close() override { return Status::Ok(); }
  uint64_t size() const override { return base_->size(); }
  uint32_t logical_block_size() const override { return 512; }
  bool is_read_only() const override { return true; }
  std::string describe() const override { return "spy"; }
  std::atomic<long> reads{0}, writes{0}, flushes{0};
  std::map<uint64_t, uint8_t>& muts() { return mut_; }
 private:
  BlockDevicePtr base_; long fail_after_; std::map<uint64_t, uint8_t> mut_;
};

static BlockDevicePtr open_img(const std::string& path) {
  BlockDevicePtr d;
  if (!RawImageDevice::open_path(path, true, 512, &d).ok()) return nullptr;
  return d;
}
static std::vector<uint8_t> slurp(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static unsigned mode_to_dtype(uint32_t mode) {
  switch (mode & 0170000) {
    case 0040000: return 4; case 0120000: return 10; case 0100000: return 8;
    case 0020000: return 2; case 0060000: return 6; case 0010000: return 1; case 0140000: return 12;
  }
  return 0;
}

static int resolve(FsVolume* v, const std::string& path, uint64_t* ino) { return v->resolve(path, ino); }

// manifest (tools/gen_btrfs_fixture.py → gen_squashfs_fixture.py): D path mtime / F path size mtime nlink spec / L path target / S path kind / R path size mtime reffile
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

static void verify_file(FsVolume* v, const std::string& dir, const MFile& f, uint64_t ino) {
  FsStat st;
  CHECK(v->stat(ino, &st) == 0);
  CHECK(st.size == f.size);
  CHECK((st.mode & 0170000) == 0100000);
  if (st.mtime != f.mtime) std::fprintf(stderr, "mtime %s: got %lld want %lld\n", f.path.c_str(), (long long)st.mtime, (long long)f.mtime);
  CHECK(st.mtime == f.mtime);
  CHECK(st.nlink == f.nlink);
  std::vector<uint8_t> tmp(16, 0xAB);
  CHECK(v->read(ino, f.size, tmp.data(), 10) == 0);          // ちょうど EOF
  CHECK(v->read(ino, f.size + 4096, tmp.data(), 10) == 0);   // EOF より先
  std::vector<uint8_t> ref;
  if (!f.reffile.empty()) ref = slurp(dir + "/" + f.reffile);
  if (f.size <= (16u << 20)) {
    std::vector<uint8_t> want(f.size, 0);
    if (!f.reffile.empty()) { CHECK(ref.size() == f.size); want = ref; want.resize(f.size); }
    for (auto& r : f.regions) if (r.kind == 'd') { auto p = pattern_at(r.seed, r.off, (size_t)r.len); std::memcpy(want.data() + r.off, p.data(), p.size()); }
    std::vector<uint8_t> got(f.size + 16, 0xAB);
    int64_t n = v->read(ino, 0, got.data(), got.size());     // 要求が EOF を超えても、残りだけ返す
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
        CHECK(v->read(ino, off, part.data(), len) == (int64_t)len);
        CHECK(std::memcmp(part.data(), want.data() + off, len) == 0);
      }
    }
    return;
  }
  // 巨大な疎ファイル: データ領域は全部、ゼロ領域は先頭・末尾・途中
  auto check_range = [&](uint64_t off, uint64_t len, uint32_t seed, bool zero) {
    std::vector<uint8_t> got((size_t)len);
    int64_t n = v->read(ino, off, got.data(), got.size());
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

static std::unique_ptr<BtrfsVolume> mount_img(const std::shared_ptr<SpyDevice>& spy, int* err, const BtrfsMountOptions& o = {}) {
  BlockDevicePtr p = spy;
  *err = 0;
  return BtrfsVolume::mount(p, err, o);
}

static void verify_image(const std::string& dir, const std::string& name) {
  std::fprintf(stderr, "== %s\n", name.c_str());
  Manifest man;
  CHECK(load_manifest(dir + "/" + name + ".manifest", &man));
  BlockDevicePtr base = open_img(dir + "/" + name + ".img");
  CHECK(base != nullptr);
  if (!base) return;
  auto spy = std::make_shared<SpyDevice>(base);
  int err = 0;
  auto vol = mount_img(spy, &err);
  CHECK(vol != nullptr);
  if (!vol) { std::fprintf(stderr, "mount failed: %d\n", err); return; }
  FsVolume* v = vol.get();
  CHECK(std::string(v->fs_name()) == "btrfs");
  CHECK(!v->writable());
  const uint64_t root = v->root();

  std::map<std::string, std::set<std::string>> expect;
  expect[""];
  for (auto& d : man.dirs) { expect[parent_of(d.first)].insert(base_of(d.first)); expect[d.first]; }
  for (auto& f : man.files) expect[parent_of(f.path)].insert(base_of(f.path));
  for (auto& l : man.links) expect[parent_of(l.path)].insert(base_of(l.path));
  for (auto& s : man.specials) expect[parent_of(s.path)].insert(base_of(s.path));
  for (auto& kv : expect) {
    uint64_t dino = 0;
    CHECK(resolve(v, kv.first, &dino) == 0);
    std::vector<FsDirEntry> ents;
    CHECK(v->readdir(dino, &ents) == 0);
    std::set<std::string> got;
    for (auto& e : ents) got.insert(e.name);
    CHECK(got.size() == ents.size());      // 重複なし ("." / ".." は返さない)
    if (got != kv.second) {
      std::fprintf(stderr, "dir '%s': expected %zu entries, got %zu\n", kv.first.c_str(), kv.second.size(), got.size());
      for (auto& n : kv.second) if (!got.count(n)) std::fprintf(stderr, "  missing: %s\n", n.c_str());
      for (auto& n : got) if (!kv.second.count(n)) std::fprintf(stderr, "  extra: %s\n", n.c_str());
      g_fail++;
    }
    for (auto& e : ents) {   // readdir が返した ino でそのまま stat できる。dirent の種別と inode の種別が一致する。lookup と readdir の ino が一致
      FsStat st;
      CHECK(v->stat(e.ino, &st) == 0);
      std::string full = kv.first.empty() ? e.name : kv.first + "/" + e.name;
      bool is_dir = expect.count(full) > 0;
      bool is_link = false; for (auto& l : man.links) if (l.path == full) is_link = true;
      CHECK(is_dir == (e.dtype == 4));
      CHECK(is_link == (e.dtype == 10));
      CHECK(((st.mode & 0170000) == 0040000) == is_dir);
      CHECK(((st.mode & 0170000) == 0120000) == is_link);
      CHECK(mode_to_dtype(st.mode) == e.dtype);
      uint64_t again = 0;
      CHECK(v->lookup(dino, e.name, &again) == 0 && again == e.ino);
    }
  }
  for (auto& d : man.dirs) {   // ディレクトリの mtime
    uint64_t ino = 0; FsStat st;
    CHECK(resolve(v, d.first, &ino) == 0 && v->stat(ino, &st) == 0);
    if (st.mtime != d.second) std::fprintf(stderr, "dir mtime %s: got %lld want %lld\n", d.first.c_str(), (long long)st.mtime, (long long)d.second);
    CHECK(st.mtime == d.second);
  }
  for (auto& f : man.files) {   // 全ファイル: サイズ・内容・mtime・nlink
    uint64_t ino = 0;
    int rc = resolve(v, f.path, &ino);
    CHECK(rc == 0);
    if (rc) { std::fprintf(stderr, "resolve %s: %d\n", f.path.c_str(), rc); continue; }
    verify_file(v, dir, f, ino);
  }
  for (auto& l : man.links) {   // シンボリックリンク: stat の mode は S_IFLNK、readlink でリンク先、read でも同じ文字列
    uint64_t ino = 0; FsStat st;
    CHECK(resolve(v, l.path, &ino) == 0);
    CHECK(v->stat(ino, &st) == 0);
    CHECK((st.mode & 0170000) == 0120000);
    CHECK(st.size == l.target.size());
    std::string tgt;
    CHECK(v->readlink(ino, &tgt) == 0);
    CHECK(tgt == l.target);
    std::vector<char> b(4096, 'x');
    int64_t n = v->read(ino, 0, b.data(), b.size());
    CHECK(n == (int64_t)l.target.size());
  }
  CHECK(!man.links.empty());
  for (auto& s : man.specials) {   // 特殊ファイル: 種別が正しく、read は -EINVAL
    uint64_t ino = 0; FsStat st;
    int rc = resolve(v, s.path, &ino);
    CHECK(rc == 0);
    if (rc) continue;
    CHECK(v->stat(ino, &st) == 0);
    uint32_t want = s.kind == 'c' ? 0020000 : s.kind == 'b' ? 0060000 : s.kind == 'p' ? 0010000 : 0140000;
    CHECK((st.mode & 0170000) == want);
    char b[8];
    CHECK(v->read(ino, 0, b, 8) == -EINVAL);
  }
  // 名前の扱い
  uint64_t a = 0, b = 0, c = 0;
  CHECK(resolve(v, "small.txt", &a) == 0);
  CHECK(resolve(v, "hardlink_to_small.txt", &b) == 0);
  CHECK(a == b);                                // ハードリンクは同じ inode
  CHECK(resolve(v, "SMALL.TXT", &c) == -ENOENT);
  CHECK(resolve(v, "日本語ファイル名.txt", &c) == 0);
  CHECK(resolve(v, std::string(255, 'n'), &c) == 0);
  CHECK(resolve(v, std::string(256, 'n'), &c) != 0);
  CHECK(resolve(v, "nonexistent", &c) == -ENOENT);
  CHECK(resolve(v, "d1/nonexistent", &c) == -ENOENT);
  CHECK(resolve(v, "small.txt/x", &c) == -ENOENT);
  uint64_t d1 = 0; CHECK(resolve(v, "d1", &d1) == 0);
  std::vector<uint8_t> tmp(16);
  CHECK(v->read(d1, 0, tmp.data(), 16) == -EISDIR);
  uint64_t up = 0;                              // ".." と "."
  CHECK(v->lookup(d1, "..", &up) == 0 && up == root);
  CHECK(v->lookup(d1, ".", &up) == 0 && up == d1);
  // 不正な ino
  FsStat bad;
  CHECK(v->stat(0, &bad) < 0);
  CHECK(v->stat(1ull << 50, &bad) < 0);
  CHECK(v->stat(999999999, &bad) < 0);
  CHECK(v->lookup(1ull << 50, "x", &c) < 0);
  std::vector<FsDirEntry> junk;
  CHECK(v->readdir(999999999, &junk) < 0);
  // 書き込みは常に拒否
  CHECK(v->create(root, "x", 0644, &c) == -30);
  CHECK(v->write(a, 0, "x", 1) == -30);
  CHECK(v->unlink(root, "small.txt") == -30);
  CHECK(spy->writes == 0);
  CHECK(spy->flushes == 0);
  // 2 回目のマウント (別インスタンス) も同じ結果
  {
    int e2 = 0;
    auto v2 = mount_img(std::make_shared<SpyDevice>(base), &e2);
    CHECK(v2 != nullptr);
    uint64_t x = 0;
    if (v2) CHECK(resolve(v2.get(), "d1/d2/d3/d4/d5/d6/d7/d8/leaf.txt", &x) == 0);
  }
  std::vector<BtrfsSubvolume> subs;
  CHECK(vol->list_subvolumes(&subs) == 0 && !subs.empty() && subs[0].id == 5);
  CHECK(vol->mounted_subvolume() == 5);
  CHECK(!vol->log_ignored());
  std::fprintf(stderr, "   %zu files, %zu dirs, %zu links, %zu specials, device reads %ld\n", man.files.size(), man.dirs.size(), man.links.size(), man.specials.size(), (long)spy->reads);
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
static void walk(FsVolume* v, uint64_t dir, int depth, int& budget) {
  if (depth > 12 || budget <= 0) return;
  std::vector<FsDirEntry> ents;
  if (v->readdir(dir, &ents)) return;
  for (auto& e : ents) {
    if (--budget <= 0) return;
    FsStat st;
    if (v->stat(e.ino, &st)) continue;
    uint32_t t = st.mode & 0170000;
    if (t == 0040000) { if (e.ino != dir) walk(v, e.ino, depth + 1, budget); }
    else if (t == 0100000 || t == 0120000) {
      std::vector<uint8_t> buf(65536);
      uint64_t sz = std::min<uint64_t>(st.size, 1u << 20);
      for (uint64_t o = 0; o < sz && budget > 0; o += buf.size(), budget--) if (v->read(e.ino, o, buf.data(), buf.size()) <= 0) break;
      std::string tgt; if (t == 0120000) v->readlink(e.ino, &tgt);
    }
  }
}

// ---- サブボリューム
static std::set<std::string> names_in(FsVolume* v, uint64_t dir) {
  std::vector<FsDirEntry> ents; std::set<std::string> s;
  if (v->readdir(dir, &ents) == 0) for (auto& e : ents) s.insert(e.name);
  return s;
}
static std::string read_all(FsVolume* v, uint64_t ino) {
  FsStat st; if (v->stat(ino, &st)) return "<stat failed>";
  std::string s((size_t)st.size, '\0');
  int64_t n = st.size ? v->read(ino, 0, &s[0], s.size()) : 0;
  if (n != (int64_t)st.size) return "<read failed>";
  return s;
}
static void verify_subvols(const std::string& dir) {
  for (const char* name : {"subvols", "subvols_default"}) {
    std::string img = dir + "/" + name + ".img";
    BlockDevicePtr base = open_img(img);
    if (!base) { std::fprintf(stderr, "== %s: not present (skipped)\n", name); continue; }
    std::fprintf(stderr, "== %s\n", name);
    bool is_def = std::string(name) == "subvols_default";
    auto spy = std::make_shared<SpyDevice>(base);
    int err = 0;
    auto vol = mount_img(spy, &err);
    CHECK(vol != nullptr);
    if (!vol) continue;
    FsVolume* v = vol.get();
    std::vector<BtrfsSubvolume> subs;
    CHECK(vol->list_subvolumes(&subs) == 0);
    std::map<std::string, BtrfsSubvolume> by;
    for (auto& s : subs) by[s.name] = s;
    for (auto& s : subs) std::fprintf(stderr, "   subvol id=%llu parent=%llu name=%s ro=%d\n", (unsigned long long)s.id, (unsigned long long)s.parent_id, s.name.c_str(), (int)s.read_only);
    if (is_def) {
      // 既定サブボリューム (svdef) がルート
      CHECK(vol->mounted_subvolume() != 5);
      CHECK(names_in(v, v->root()) == (std::set<std::string>{"d.txt", "sub"}));
      uint64_t d = 0; CHECK(resolve(v, "d.txt", &d) == 0);
      CHECK(read_all(v, d) == "in the default subvolume\n");
      uint64_t e = 0; CHECK(resolve(v, "sub/e.bin", &e) == 0);
      std::string eb = read_all(v, e); CHECK(eb.size() == 12800 && (uint8_t)eb[255] == 255 && (uint8_t)eb[256] == 0);
      // トップレベル (ID 5) を開く: svA / svdef がサブボリュームのディレクトリとして見える
      BtrfsMountOptions o; o.subvol_id = 5;
      int e2 = 0;
      auto top = mount_img(std::make_shared<SpyDevice>(base), &e2, o);
      CHECK(top != nullptr);
      if (top) {
        CHECK(names_in(top.get(), top->root()) == (std::set<std::string>{"top.txt", "svA", "svB", "svdef"}));
        uint64_t sa = 0; CHECK(resolve(top.get(), "svA/a.txt", &sa) == 0);
        std::string want_a; for (int i = 0; i < 100; i++) want_a += "in svA\n";
        CHECK(read_all(top.get(), sa) == want_a);
        uint64_t sd = 0; CHECK(resolve(top.get(), "svdef/sub/e.bin", &sd) == 0);
        FsStat st; CHECK(top->stat(sd, &st) == 0 && st.size == 12800);
      }
    } else {
      CHECK(names_in(v, v->root()) == (std::set<std::string>{"top.txt", "svA", "svB", "svdef"}));
      CHECK(by.count("svA") && by.count("svB") && by.count("inner"));
      if (by.count("svB")) CHECK(by["svB"].read_only);
      if (by.count("svA")) CHECK(!by["svA"].read_only);
      uint64_t t = 0; CHECK(resolve(v, "top.txt", &t) == 0 && read_all(v, t) == "top level file\n");
      // サブボリューム境界をまたぐ lookup / readdir / read / stat / ".." (サブボリュームのルートの ".." はそれ自身)
      uint64_t sa = 0; CHECK(resolve(v, "svA", &sa) == 0);
      FsStat st; CHECK(v->stat(sa, &st) == 0 && (st.mode & 0170000) == 0040000);
      std::vector<FsDirEntry> ents; CHECK(v->readdir(v->root(), &ents) == 0);
      for (auto& e : ents) if (e.name == "svA") CHECK(e.dtype == 4 && e.ino == sa);
      CHECK(names_in(v, sa) == (std::set<std::string>{"a.txt", "inner"}));
      uint64_t a = 0; CHECK(resolve(v, "svA/a.txt", &a) == 0);
      std::string want; for (int i = 0; i < 100; i++) want += "in svA\n";
      CHECK(read_all(v, a) == want);
      uint64_t i = 0; CHECK(resolve(v, "svA/inner/i.txt", &i) == 0 && read_all(v, i) == "nested subvolume\n");
      uint64_t b = 0; CHECK(resolve(v, "svB/b.txt", &b) == 0 && read_all(v, b) == "in svB (read-only)\n");
      uint64_t up = 0; CHECK(v->lookup(sa, "..", &up) == 0);
      CHECK(a != b && a != i);
      // 子サブボリュームだけを開く
      BtrfsMountOptions o; o.subvol_id = by.count("svA") ? by["svA"].id : 0;
      int e2 = 0;
      auto sv = mount_img(std::make_shared<SpyDevice>(base), &e2, o);
      CHECK(sv != nullptr);
      if (sv) {
        CHECK(sv->mounted_subvolume() == o.subvol_id);
        CHECK(names_in(sv.get(), sv->root()) == (std::set<std::string>{"a.txt", "inner"}));
      }
      // 存在しないサブボリューム
      BtrfsMountOptions bad; bad.subvol_id = 123456;
      auto nope = mount_img(std::make_shared<SpyDevice>(base), &e2, bad);
      CHECK(nope == nullptr);
    }
    CHECK(spy->writes == 0);
  }
}

// ---- NODATASUM / データの破損
static void verify_nodatasum(const std::string& dir) {
  BlockDevicePtr base = open_img(dir + "/nodatasum.img");
  if (!base) { std::fprintf(stderr, "== nodatasum: not present (skipped)\n"); return; }
  std::fprintf(stderr, "== nodatasum / data corruption\n");
  std::vector<uint8_t> n1 = slurp(dir + "/nodatasum.src/nodatasum.bin"), n2 = slurp(dir + "/nodatasum.src/nodatacow.bin"), n3 = slurp(dir + "/nodatasum.src/normal.bin");
  CHECK(n1.size() == 300000 && n2.size() == 300000 && n3.size() == 299999);
  auto find_in_image = [&](const std::vector<uint8_t>& needle, size_t at) -> int64_t {   // イメージ内でデータの位置を探す (4096 バイト境界)
    std::vector<uint8_t> blk(65536);
    for (uint64_t off = 0; off + needle.size() < base->size() && off < (200u << 20); off += 4096) {
      if (base->read_at(off, blk.data(), 4096).ok() && std::memcmp(blk.data(), needle.data() + at, 4096) == 0) return (int64_t)off - (int64_t)at;
    }
    return -1;
  };
  int64_t p1 = find_in_image(n1, 4096), p2 = find_in_image(n2, 4096), p3 = find_in_image(n3, 4096);
  std::fprintf(stderr, "   data at image offsets: nodatasum=%lld nodatacow=%lld normal=%lld\n", (long long)p1, (long long)p2, (long long)p3);
  CHECK(p1 >= 0 && p2 >= 0 && p3 >= 0);
  if (p1 < 0 || p2 < 0 || p3 < 0) return;
  auto check = [&](const char* nm, const std::vector<uint8_t>& want, bool expect_ok) {
    auto spy = std::make_shared<SpyDevice>(base);
    // 3 ファイルのデータの途中の 1 バイトずつを壊す
    for (int64_t p : {p1, p2, p3}) spy->muts()[(uint64_t)p + 10000] = 0x5A ^ (uint8_t)(p & 0xFF);
    int err = 0;
    auto vol = mount_img(spy, &err);
    CHECK(vol != nullptr);
    if (!vol) return;
    uint64_t ino = 0; CHECK(resolve(vol.get(), nm, &ino) == 0);
    std::vector<uint8_t> got(want.size() + 8);
    int64_t n = vol->read(ino, 0, got.data(), got.size());
    if (expect_ok) { CHECK(n == (int64_t)want.size()); CHECK(std::memcmp(got.data(), want.data(), want.size()) != 0); }   // csum なし: 壊れたまま返る (一致しない)
    else CHECK(n == -EIO);                                                                                                   // csum あり: 不一致は EIO
    // 壊した範囲に触れない読み取りは成功する
    std::vector<uint8_t> head(4096);
    CHECK(vol->read(ino, 0, head.data(), head.size()) == 4096);
    CHECK(std::memcmp(head.data(), want.data(), 4096) == 0);
  };
  check("nodatasum.bin", n1, true);
  check("nodatacow.bin", n2, true);
  check("normal.bin", n3, false);
  // 無傷なら 3 つとも一致
  {
    auto spy = std::make_shared<SpyDevice>(base);
    int err = 0;
    auto vol = mount_img(spy, &err);
    CHECK(vol != nullptr);
    if (vol) {
      const std::vector<uint8_t>* w[3] = {&n1, &n2, &n3};
      const char* nm[3] = {"nodatasum.bin", "nodatacow.bin", "normal.bin"};
      for (int k = 0; k < 3; k++) {
        uint64_t ino = 0; CHECK(resolve(vol.get(), nm[k], &ino) == 0);
        std::vector<uint8_t> got(w[k]->size());
        CHECK(vol->read(ino, 0, got.data(), got.size()) == (int64_t)got.size());
        CHECK(got == *w[k]);
      }
    }
  }
  // verify_data = false なら csum ありのファイルも (壊れたまま) 読める
  {
    auto spy = std::make_shared<SpyDevice>(base);
    spy->muts()[(uint64_t)p3 + 10000] = 0x77;
    BtrfsMountOptions o; o.verify_data = false;
    int err = 0;
    auto vol = mount_img(spy, &err, o);
    CHECK(vol != nullptr);
    if (vol) { uint64_t ino = 0; CHECK(resolve(vol.get(), "normal.bin", &ino) == 0); std::vector<uint8_t> got(n3.size()); CHECK(vol->read(ino, 0, got.data(), got.size()) == (int64_t)got.size()); CHECK(got != n3); }
  }
}

// ---- 複数デバイス / ログ / 未対応機能
static void patch_sb(std::shared_ptr<SpyDevice> spy, const std::vector<uint8_t>& sb, std::map<size_t, uint8_t> edits) {
  std::vector<uint8_t> s = sb;
  for (auto& e : edits) s[e.first] = e.second;
  put32(s.data(), crc32c(s.data() + 32, 4096 - 32));     // crc32c 以外の csum 種別のイメージには使わない
  for (size_t i = 0; i < s.size(); i++) spy->muts()[0x10000 + i] = s[i];
}
static void verify_refusals(const std::string& dir) {
  std::fprintf(stderr, "== refusals\n");
  if (BlockDevicePtr m = open_img(dir + "/multi_a.img")) {
    int err = 0;
    auto vol = mount_img(std::make_shared<SpyDevice>(m), &err);
    CHECK(vol == nullptr && err == EOPNOTSUPP);
  } else std::fprintf(stderr, "   multi_a.img not present (skipped)\n");
  BlockDevicePtr base = open_img(dir + "/crc32c.img");
  CHECK(base != nullptr);
  if (!base) return;
  std::vector<uint8_t> sb(4096);
  CHECK(base->read_at(0x10000, sb.data(), 4096).ok());
  // tree-log が未再生 (log_root != 0): 既定は EUCLEAN、allow_noload なら開ける (log_ignored)
  {
    auto spy = std::make_shared<SpyDevice>(base);
    patch_sb(spy, sb, {{0x60, 0x00}, {0x61, 0x00}, {0x62, 0x10}});   // log_root = 0x100000
    int err = 0;
    auto vol = mount_img(spy, &err);
    CHECK(vol == nullptr && err == EUCLEAN);
    BtrfsMountOptions o; o.allow_noload = true;
    auto vol2 = mount_img(spy, &err, o);
    CHECK(vol2 != nullptr);
    if (vol2) {
      CHECK(vol2->log_ignored());
      CHECK(!vol2->writable());
      uint64_t x = 0; CHECK(resolve(vol2.get(), "small.txt", &x) == 0);
    }
    CHECK(spy->writes == 0);
  }
  // 未知の incompat 機能 / RAID56 → EOPNOTSUPP
  for (uint8_t bit : {uint8_t(0x80) /* bit 7 RAID56 */}) {
    auto spy = std::make_shared<SpyDevice>(base);
    uint8_t cur = sb[0xbc];
    patch_sb(spy, sb, {{0xbc, (uint8_t)(cur | bit)}});
    int err = 0;
    CHECK(mount_img(spy, &err) == nullptr && err == EOPNOTSUPP);
  }
  {
    auto spy = std::make_shared<SpyDevice>(base);
    patch_sb(spy, sb, {{0xbe, (uint8_t)(sb[0xbe] | 0x40)}});   // incompat bit 22 (未知)
    int err = 0;
    CHECK(mount_img(spy, &err) == nullptr && err == EOPNOTSUPP);
  }
  // スーパーブロックのチェックサム不一致: 主コピーだけなら 64 MiB のミラーで開ける (世代が同じ)。全コピーが無効なら EINVAL
  {
    auto spy = std::make_shared<SpyDevice>(base);
    spy->muts()[0x10000 + 0x100] = sb[0x100] ^ 0xFF;
    int err = 0;
    auto vol = mount_img(spy, &err);
    CHECK(vol != nullptr);
    if (vol) { uint64_t x = 0; CHECK(resolve(vol.get(), "small.txt", &x) == 0); }
    spy->muts()[0x4000000 + 0x100] ^= 0xFF;
    uint8_t m = 0; base->read_at(0x4000000 + 0x100, &m, 1);
    spy->muts()[0x4000000 + 0x100] = m ^ 0xFF;
    CHECK(mount_img(spy, &err) == nullptr && err == EINVAL);
  }
  // 空のスーパーブロック (全コピー)
  {
    auto z = std::make_shared<SpyDevice>(base);
    for (uint64_t i = 0; i < 0x1000; i++) { z->muts()[0x10000 + i] = 0; z->muts()[0x4000000 + i] = 0; }
    int err = 0;
    CHECK(mount_img(z, &err) == nullptr && err == EINVAL);
  }
  // 書き込みは一度も呼ばれない
  auto spy = std::make_shared<SpyDevice>(base);
  int err = 0;
  auto vol = mount_img(spy, &err);
  CHECK(vol != nullptr);
  if (vol) { int budget = 100000; walk(vol.get(), vol->root(), 0, budget); }
  CHECK(spy->writes == 0 && spy->flushes == 0);
}

// ---- 壊れたスーパーブロック (全フィールド × 数種の値、csum を直して検査に届かせる)
static void corrupt_images(const std::string& dir) {
  BlockDevicePtr base = open_img(dir + "/crc32c.img");
  CHECK(base != nullptr);
  if (!base) return;
  std::vector<uint8_t> sb(4096);
  CHECK(base->read_at(0x10000, sb.data(), 4096).ok());
  int mounted = 0, refused = 0;
  const uint8_t vals[] = {0x00, 0x01, 0x7F, 0x80, 0xFF};
  for (size_t off = 0; off < 0x32b + 64; off++) {
    if (off < 32) continue;   // csum 自体
    for (uint8_t v : vals) {
      if (sb[off] == v) continue;
      auto spy = std::make_shared<SpyDevice>(base);
      patch_sb(spy, sb, {{off, v}});
      int err = 0;
      auto vol = mount_img(spy, &err);
      if (!vol) { refused++; continue; }
      mounted++;
      int budget = 5000;
      walk(vol.get(), vol->root(), 0, budget);
    }
  }
  std::fprintf(stderr, "== corrupt superblock: %d refused, %d mounted (walked), no crash\n", refused, mounted);
  CHECK(refused > 100);
  // sys_chunk_array を壊す (長さ・先頭のキー・stripe 数)
  for (int k = 0; k < 400; k++) {
    auto spy = std::make_shared<SpyDevice>(base);
    std::map<size_t, uint8_t> ed;
    std::mt19937 rng(1000 + k);
    for (int j = 0; j < 3; j++) ed[811 + rng() % 120] = (uint8_t)rng();
    patch_sb(spy, sb, ed);
    int err = 0;
    auto vol = mount_img(spy, &err);
    if (vol) { int budget = 2000; walk(vol.get(), vol->root(), 0, budget); }
  }
}

// ---- メタデータのノードを壊す fuzz。ノードを見つけて数バイト変え、crc32c を直す (検査の中身に届かせる)
struct NodeLoc { uint64_t off; uint32_t size; };
static std::vector<NodeLoc> find_nodes(BlockDevicePtr base) {
  std::vector<uint8_t> sb(4096);
  base->read_at(0x10000, sb.data(), 4096);
  uint32_t nodesize = le32(&sb[0x94]);
  std::vector<NodeLoc> out;
  std::vector<uint8_t> blk(nodesize);
  for (uint64_t off = 0; off + nodesize <= base->size() && off < (400u << 20); off += nodesize) {
    if (off == 0x10000 / nodesize * nodesize && 0x10000 % nodesize == 0) continue;
    if (!base->read_at(off, blk.data(), nodesize).ok()) continue;
    if (std::memcmp(&blk[32], &sb[0x20], 16) != 0) continue;
    if (crc32c(&blk[32], nodesize - 32) != le32(&blk[0])) continue;
    out.push_back({off, nodesize});
  }
  return out;
}
static void fuzz_metadata(const std::string& dir, int iterations, unsigned seed) {
  static const char* names[] = {"crc32c", "zlib", "lzo", "zstd", "node4k", "node64k", "dup_dup", "noholes", "mixed", "sector16k_lzo", "bgtree", "reflink"};
  std::mt19937 rng(seed);
  for (const char* nm : names) {
    BlockDevicePtr base = open_img(dir + "/" + nm + ".img");
    if (!base) continue;
    auto nodes = find_nodes(base);
    std::fprintf(stderr, "== fuzz %s: %zu metadata nodes\n", nm, nodes.size());
    CHECK(nodes.size() > 4);
    if (nodes.empty()) continue;
    int ok_mount = 0;
    for (int it = 0; it < iterations; it++) {
      auto spy = std::make_shared<SpyDevice>(base);
      int nmut = 1 + (int)(rng() % 3);
      std::map<uint64_t, std::vector<uint8_t>> edited;
      for (int j = 0; j < nmut; j++) {
        const NodeLoc& n = nodes[(rng() % 3 == 0) ? (rng() % std::min<size_t>(nodes.size(), 8)) : (rng() % nodes.size())];
        auto& buf = edited[n.off];
        if (buf.empty()) { buf.resize(n.size); base->read_at(n.off, buf.data(), n.size); }
        size_t pos = 32 + rng() % (n.size - 32);
        if (rng() % 2) pos = 32 + rng() % 200;   // ヘッダと先頭の項目に偏らせる
        buf[pos] = (uint8_t)rng();
      }
      for (auto& kv : edited) {
        put32(kv.second.data(), crc32c(kv.second.data() + 32, kv.second.size() - 32));
        for (size_t i = 0; i < kv.second.size(); i++) spy->muts()[kv.first + i] = kv.second[i];
      }
      int err = 0;
      auto vol = mount_img(spy, &err);
      if (!vol) continue;
      ok_mount++;
      int budget = 3000;
      walk(vol.get(), vol->root(), 0, budget);
      std::vector<BtrfsSubvolume> subs; vol->list_subvolumes(&subs);
      CHECK(spy->writes == 0);
    }
    std::fprintf(stderr, "   %d/%d mutated images still mounted (no crash)\n", ok_mount, iterations);
  }
}

// ---- I/O エラー: N 回目の read から失敗させる。マウントと走査が、クラッシュ・ハングせず結果が一貫する (エラーが成功にすり替わらない)
static void ioerr_images(const std::string& dir) {
  for (const char* nm : {"crc32c", "zlib", "dup_dup"}) {
    BlockDevicePtr base = open_img(dir + "/" + nm + ".img");
    if (!base) continue;
    // 失敗なしの基準: 読み取り回数
    auto spy0 = std::make_shared<SpyDevice>(base);
    int err = 0;
    auto v0 = mount_img(spy0, &err);
    CHECK(v0 != nullptr);
    if (!v0) continue;
    long mount_reads = spy0->reads;
    int b0 = 4000; walk(v0.get(), v0->root(), 0, b0);
    long total = spy0->reads;
    std::fprintf(stderr, "== ioerr %s: mount %ld reads, walk total %ld\n", nm, mount_reads, total);
    long step = std::max<long>(1, total / 300);
    int failed_mounts = 0;
    for (long n = 0; n < total + step; n += step) {
      auto spy = std::make_shared<SpyDevice>(base, n);
      auto vol = mount_img(spy, &err);
      if (!vol) { failed_mounts++; CHECK(err == EIO || err == EINVAL); continue; }
      int budget = 4000;
      walk(vol.get(), vol->root(), 0, budget);
      CHECK(spy->writes == 0);
    }
    CHECK(failed_mounts > 0);
  }
}

// ---- tree: 全ツリーを出力 (独立した検証用: Python が元ツリー / btrfs restore の結果と比較する)
static void dump(FsVolume* v, uint64_t dir, const std::string& prefix, int depth) {
  std::vector<FsDirEntry> ents;
  int rc = v->readdir(dir, &ents);
  if (rc) { std::fprintf(stderr, "readdir %s: %d\n", prefix.c_str(), rc); g_fail++; return; }
  std::sort(ents.begin(), ents.end(), [](const FsDirEntry& a, const FsDirEntry& b) { return a.name < b.name; });
  for (auto& e : ents) {
    FsStat st;
    std::string path = prefix.empty() ? e.name : prefix + "/" + e.name;
    rc = v->stat(e.ino, &st);
    if (rc) { std::fprintf(stderr, "stat %s: %d\n", path.c_str(), rc); g_fail++; continue; }
    uint32_t t = st.mode & 0170000;
    if (t == 0040000) {
      std::printf("%s\td\t0\t%lld\t-\n", path.c_str(), (long long)st.mtime);
      if (depth < 40) dump(v, e.ino, path, depth + 1);
    } else if (t == 0100000) {
      Sha256 h; std::vector<uint8_t> buf(1 << 20); uint64_t off = 0;
      for (;;) { int64_t n = v->read(e.ino, off, buf.data(), buf.size()); if (n < 0) { std::fprintf(stderr, "read %s: %lld\n", path.c_str(), (long long)n); g_fail++; break; } if (n == 0) break; h.update(buf.data(), (size_t)n); off += (uint64_t)n; }
      if (off != st.size) { std::fprintf(stderr, "size mismatch %s: read %llu stat %llu\n", path.c_str(), (unsigned long long)off, (unsigned long long)st.size); g_fail++; }
      std::printf("%s\tf\t%llu\t%lld\t%s\n", path.c_str(), (unsigned long long)st.size, (long long)st.mtime, hex_encode(h.finish()).c_str());
    } else if (t == 0120000) {
      std::string tgt; rc = v->readlink(e.ino, &tgt);
      if (rc) { std::fprintf(stderr, "readlink %s: %d\n", path.c_str(), rc); g_fail++; }
      std::printf("%s\tl\t%zu\t%lld\t%s\n", path.c_str(), tgt.size(), (long long)st.mtime, tgt.c_str());
    } else {
      std::printf("%s\ts\t0\t%lld\t%o\n", path.c_str(), (long long)st.mtime, t);
    }
  }
}

int main(int argc, char** argv) {
  if (argc >= 3 && std::string(argv[1]) == "tree") {
    BlockDevicePtr d = open_img(argv[2]);
    if (!d) { std::fprintf(stderr, "open failed\n"); return 2; }
    BtrfsMountOptions o;
    if (argc >= 4) o.subvol_id = std::strtoull(argv[3], nullptr, 0);
    int err = 0;
    auto v = BtrfsVolume::mount(d, &err, o);
    if (!v) { std::fprintf(stderr, "mount failed: %d\n", err); return 3; }
    dump(v.get(), v->root(), "", 0);
    return g_fail ? 1 : 0;
  }
  if (argc >= 3 && std::string(argv[1]) == "--corrupt") corrupt_images(argv[2]);
  else if (argc >= 3 && std::string(argv[1]) == "--ioerr") ioerr_images(argv[2]);
  else if (argc >= 5 && std::string(argv[1]) == "--fuzz") fuzz_metadata(argv[2], std::atoi(argv[3]), (unsigned)std::atoi(argv[4]));
  else if (argc >= 2 && argv[1][0] != '-') {
    std::string dir = argv[1];
    auto imgs = list_images(dir);
    CHECK(!imgs.empty());
    for (auto& n : imgs) verify_image(dir, n);
    verify_subvols(dir);
    verify_nodatasum(dir);
    verify_refusals(dir);
  } else {
    std::fprintf(stderr, "usage: %s <fixture_dir> | --corrupt <dir> | --ioerr <dir> | --fuzz <dir> <iterations> <seed> | tree <img> [subvol]\n", argv[0]);
    return 2;
  }
  if (g_fail) { std::fprintf(stderr, "FAILED: %d checks\n", g_fail); return 1; }
  std::fprintf(stderr, "OK\n");
  return 0;
}
