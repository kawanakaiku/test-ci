// SPDX-License-Identifier: Apache-2.0
// F2FS 読み取り専用リーダー (F2fsVolume) のテスト。
// 使い方: usbmem_f2fs_tests <fixture_dir>            tools/gen_f2fs_fixture.py (mkfs.f2fs + sload.f2fs) と tools/f2fs_vm (本物のカーネルが書いたもの) の *.img / *.manifest を全部検証
//         usbmem_f2fs_tests --corrupt <fixture_dir>  壊れたボリューム (スーパーブロック / チェックポイント / ノード) が、クラッシュせず失敗する (または別のコピーで開く) こと
//         usbmem_f2fs_tests --ioerr <fixture_dir>    マウント・走査中のデバイス I/O エラーで、クラッシュせず失敗すること
//         usbmem_f2fs_tests --fuzz <fixture_dir> <iterations> <seed>
//         usbmem_f2fs_tests --noload <fixture_dir>   回復 (roll-forward) が必要なイメージ (k_unclean*): 既定では EUCLEAN、allow_noload なら読み取り専用で開く
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
#include "usbmem/f2fs_volume.h"

using namespace usbmem;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

// 読み取り専用デバイス。write_at が 1 度でも呼ばれたら数える。fail_after >= 0 ならその回数の read_at の後はすべて EIO。mut は読み出し時に差し替えるバイト。touched は読まれたブロック (4 KiB)。
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
    if (track) for (uint64_t b = off / 4096; b <= (off + len - 1) / 4096; b++) touched.insert(b);
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
  bool track = false; std::set<uint64_t> touched;
 private:
  BlockDevicePtr base_; long fail_after_; std::map<uint64_t, uint8_t> mut_; uint64_t size_;
};

static BlockDevicePtr open_img(const std::string& path) {
  BlockDevicePtr d;
  if (!RawImageDevice::open_path(path, true, 512, &d).ok()) return nullptr;
  return d;
}
static std::unique_ptr<F2fsVolume> mount_dev(const std::shared_ptr<SpyDevice>& dev, int* err, bool noload = false) {
  BlockDevicePtr p = dev;
  F2fsMountOptions o; o.allow_noload = noload;
  *err = 0;
  return F2fsVolume::mount(p, err, o);
}

// manifest (tools/gen_f2fs_fixture.py): D path mtime / F path size mtime nlink spec / L path target / S path kind / R path size mtime reffile
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

static void verify_file(FsVolume& v, const std::string& dir, const MFile& f, uint64_t ino, bool check_nlink) {
  FsStat st;
  CHECK(v.stat(ino, &st) == 0);
  CHECK(st.size == f.size);
  CHECK((st.mode & 0170000) == 0100000);
  CHECK(st.mtime == f.mtime);
  if (check_nlink) CHECK(st.nlink == f.nlink);
  std::vector<uint8_t> tmp(16, 0xAB);
  CHECK(v.read(ino, f.size, tmp.data(), 10) == 0);          // ちょうど EOF
  CHECK(v.read(ino, f.size + 4096, tmp.data(), 10) == 0);   // EOF より先
  std::vector<uint8_t> ref;
  if (!f.reffile.empty()) ref = slurp(dir + "/" + f.reffile);
  if (f.size <= (16u << 20)) {
    std::vector<uint8_t> want(f.size, 0);
    if (!f.reffile.empty()) { CHECK(ref.size() == f.size); want = ref; want.resize(f.size); }
    for (auto& r : f.regions) if (r.kind == 'd') { auto p = pattern_at(r.seed, r.off, (size_t)r.len); std::memcpy(want.data() + r.off, p.data(), p.size()); }
    std::vector<uint8_t> got(f.size + 16, 0xAB);
    int64_t n = v.read(ino, 0, got.data(), got.size());     // 要求が EOF を超えても、残りだけ返す
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
        CHECK(v.read(ino, off, part.data(), len) == (int64_t)len);
        CHECK(std::memcmp(part.data(), want.data() + off, len) == 0);
      }
    }
    return;
  }
  // 巨大な疎ファイル: データ領域は全部、ゼロ領域は先頭・末尾・途中
  auto check_range = [&](uint64_t off, uint64_t len, uint32_t seed, bool zero) {
    std::vector<uint8_t> got((size_t)len);
    int64_t n = v.read(ino, off, got.data(), got.size());
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

static void verify_image(const std::string& dir, const std::string& name) {
  std::fprintf(stderr, "== %s\n", name.c_str());
  Manifest man;
  CHECK(load_manifest(dir + "/" + name + ".manifest", &man));
  BlockDevicePtr base = open_img(dir + "/" + name + ".img");
  CHECK(base != nullptr);
  if (!base) return;
  auto spy = std::make_shared<SpyDevice>(base);
  int err = 0;
  auto vol = mount_dev(spy, &err);
  CHECK(vol != nullptr && err == 0);
  if (!vol) { std::fprintf(stderr, "mount failed: %d\n", err); return; }
  FsVolume& v = *vol;
  const uint64_t root = v.root();
  CHECK(std::string(v.fs_name()) == "f2fs");
  CHECK(!v.writable());
  const bool kernel_made = name.rfind("k_", 0) == 0;   // 本物のカーネルが書いたイメージ: ソケット / 特殊ファイルを持たない小さなツリーのことがある

  std::map<std::string, std::set<std::string>> expect;
  expect[""];
  for (auto& d : man.dirs) { expect[parent_of(d.first)].insert(base_of(d.first)); expect[d.first]; }
  for (auto& f : man.files) expect[parent_of(f.path)].insert(base_of(f.path));
  for (auto& l : man.links) expect[parent_of(l.path)].insert(base_of(l.path));
  for (auto& s : man.specials) expect[parent_of(s.path)].insert(base_of(s.path));
  for (auto& kv : expect) {
    uint64_t dino = 0;
    CHECK(v.resolve(kv.first, &dino) == 0);
    std::vector<FsDirEntry> ents;
    { int rrc = v.readdir(dino, &ents); if (rrc) std::fprintf(stderr, "readdir('%s') -> %d\n", kv.first.c_str(), rrc); CHECK(rrc == 0); }
    std::set<std::string> got;
    for (auto& e : ents) got.insert(e.name);
    CHECK(got.size() == ents.size());      // 重複なし ("." / ".." は返さない)
    if (kv.first.empty()) got.erase("lost+found");   // lost_found 機能のイメージは、ルートに lost+found を持つ
    if (got != kv.second) {
      std::fprintf(stderr, "dir '%s': expected %zu entries, got %zu\n", kv.first.c_str(), kv.second.size(), got.size());
      for (auto& n : kv.second) if (!got.count(n)) std::fprintf(stderr, "  missing: %s\n", n.c_str());
      for (auto& n : got) if (!kv.second.count(n)) std::fprintf(stderr, "  extra: %s\n", n.c_str());
      g_fail++;
    }
    for (auto& e : ents) {
      FsStat st;
      CHECK(v.stat(e.ino, &st) == 0);
      std::string full = kv.first.empty() ? e.name : kv.first + "/" + e.name;
      if (full == "lost+found") { CHECK(e.dtype == 4 && (st.mode & 0170000) == 0040000); continue; }
      bool is_dir = expect.count(full) > 0;
      bool is_link = false; for (auto& l : man.links) if (l.path == full) is_link = true;
      CHECK(is_dir == (e.dtype == 4));
      CHECK(is_link == (e.dtype == 10));
      CHECK(((st.mode & 0170000) == 0040000) == is_dir);
      CHECK(((st.mode & 0170000) == 0120000) == is_link);
      CHECK(mode_to_dtype(st.mode) == e.dtype);
      if (is_dir) CHECK(st.nlink >= 2);
      uint64_t again = 0;
      CHECK(v.lookup(dino, e.name, &again) == 0 && again == e.ino);
    }
  }
  for (auto& d : man.dirs) {   // ディレクトリの mtime
    uint64_t ino = 0; FsStat st;
    CHECK(v.resolve(d.first, &ino) == 0 && v.stat(ino, &st) == 0);
    if (st.mtime != d.second) std::fprintf(stderr, "dir mtime %s: got %lld want %lld\n", d.first.c_str(), (long long)st.mtime, (long long)d.second);
    CHECK(st.mtime == d.second);
  }
  for (auto& f : man.files) {
    uint64_t ino = 0;
    int rc = v.resolve(f.path, &ino);
    CHECK(rc == 0);
    if (rc) { std::fprintf(stderr, "resolve %s: %d\n", f.path.c_str(), rc); continue; }
    verify_file(v, dir, f, ino, !kernel_made);
  }
  for (auto& l : man.links) {
    uint64_t ino = 0; FsStat st;
    CHECK(v.resolve(l.path, &ino) == 0);       // lookup はリンクを辿らない
    CHECK(v.stat(ino, &st) == 0);
    CHECK((st.mode & 0170000) == 0120000);
    CHECK(st.size == l.target.size());
    std::vector<char> b(4096, 'x');
    int64_t n = v.read(ino, 0, b.data(), b.size());
    CHECK(n == (int64_t)l.target.size());
    if (n == (int64_t)l.target.size()) CHECK(std::memcmp(b.data(), l.target.data(), l.target.size()) == 0);
    std::string tgt; CHECK(v.readlink(ino, &tgt) == 0 && tgt == l.target);
  }
  CHECK(!man.links.empty());
  for (auto& s : man.specials) {
    uint64_t ino = 0; FsStat st;
    int rc = v.resolve(s.path, &ino);
    CHECK(rc == 0);
    if (rc) continue;
    CHECK(v.stat(ino, &st) == 0);
    uint32_t want = s.kind == 'c' ? 0020000 : s.kind == 'b' ? 0060000 : s.kind == 'p' ? 0010000 : 0140000;
    CHECK((st.mode & 0170000) == want);
    char b[8];
    CHECK(v.read(ino, 0, b, 8) == -EINVAL);
  }
  // 名前の扱い
  uint64_t a = 0, b = 0, c = 0;
  if (!kernel_made) {
    CHECK(v.resolve("small.txt", &a) == 0);
    CHECK(v.resolve("hardlink_to_small.txt", &b) == 0);
    CHECK(a == b);                                // ハードリンクは同じ inode
    CHECK(v.resolve("SMALL.TXT", &c) == -ENOENT);
    CHECK(v.resolve("日本語ファイル名.txt", &c) == 0);
    CHECK(v.resolve(std::string(255, 'n'), &c) == 0);
    CHECK(v.resolve(std::string(256, 'n'), &c) != 0);
    CHECK(v.resolve("nonexistent", &c) == -ENOENT);
    CHECK(v.resolve("d1/nonexistent", &c) == -ENOENT);
    CHECK(v.resolve("small.txt/x", &c) == -ENOTDIR);
    uint64_t d1 = 0; CHECK(v.resolve("d1", &d1) == 0);
    std::vector<uint8_t> tmp(16);
    CHECK(v.read(d1, 0, tmp.data(), 16) == -EISDIR);
  }
  // 不正な ino
  FsStat st0;
  CHECK(v.stat(0, &st0) < 0);
  CHECK(v.stat(1, &st0) < 0);
  CHECK(v.stat(999999999, &st0) < 0);
  CHECK(v.stat(1ull << 40, &st0) < 0);
  CHECK(v.lookup(999999999, "x", &c) < 0);
  std::vector<FsDirEntry> junk;
  CHECK(v.readdir(999999999, &junk) < 0);
  // 書き込み系は常に EROFS
  uint64_t nn = 0;
  CHECK(v.create(root, "x", 0644, &nn) == -30);
  CHECK(v.mkdir(root, "x", 0755, &nn) == -30);
  CHECK(v.unlink(root, "x") == -30);
  CHECK(v.write(root, 0, "x", 1) == -30);
  CHECK(v.truncate(root, 0) == -30);
  CHECK(spy->writes == 0);
  CHECK(spy->flushes == 0);
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
static bool needs_recovery_image(const std::string& n) { return n.rfind("k_unclean", 0) == 0; }

// 全体を歩く (fuzz / ioerr 用)。クラッシュ・ハング・過大なメモリ確保をしないことだけを見る。budget で打ち切り。
static void walk(FsVolume& v, uint64_t dir, int depth, int& budget) {
  if (depth > 12 || budget <= 0) return;
  std::vector<FsDirEntry> ents;
  if (v.readdir(dir, &ents)) return;
  for (auto& e : ents) {
    if (--budget <= 0) return;
    FsStat st;
    if (v.stat(e.ino, &st)) continue;
    uint32_t t = st.mode & 0170000;
    uint64_t again = 0; v.lookup(dir, e.name, &again);
    if (t == 0040000) { if (e.ino != dir) walk(v, e.ino, depth + 1, budget); }
    else if (t == 0100000 || t == 0120000) {
      std::vector<uint8_t> buf(65536);
      uint64_t sz = std::min<uint64_t>(st.size, 1u << 20);
      for (uint64_t o = 0; o < sz && budget > 0; o += buf.size(), budget--) if (v.read(e.ino, o, buf.data(), buf.size()) <= 0) break;
      std::string tg; if (t == 0120000) v.readlink(e.ino, &tg);
    }
  }
}

static uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
struct Geo { uint32_t cp, nat, main; };
static Geo geo_of(BlockDevicePtr base) {
  uint8_t sb[256] = {0};
  base->read_at(1024, sb, 256);
  return {le32(sb + 76), le32(sb + 84), le32(sb + 92)};
}

static void test_corrupt(const std::string& dir) {
  int n_cases = 0;
  const uint64_t SB0 = 1024, SB1 = 4096 + 1024;   // スーパーブロックは 2 コピー (ブロック 0 と 1)
  for (auto& name : list_images(dir)) {
    if (name != "plain" && name != "ext" && name.rfind("k_", 0) != 0) continue;
    BlockDevicePtr base = open_img(dir + "/" + name + ".img");
    CHECK(base);
    Geo g = geo_of(base);
    int err = 0;
    CHECK(mount_dev(std::make_shared<SpyDevice>(base), &err) != nullptr);   // 正常
    auto Q = [](uint64_t off, uint64_t val, int n) { std::map<uint64_t, uint8_t> m; for (int i = 0; i < n; i++) m[off + (uint64_t)i] = (uint8_t)(val >> (8 * i)); return m; };
    auto both = [&](uint64_t rel, uint64_t val, int n) { auto m = Q(SB0 + rel, val, n); auto m1 = Q(SB1 + rel, val, n); m.insert(m1.begin(), m1.end()); return m; };
    const uint64_t CP0 = (uint64_t)g.cp * 4096, CP1 = (uint64_t)(g.cp + 512) * 4096;
    struct Case { const char* what; std::map<uint64_t, uint8_t> mut; uint64_t size; int expect; };   // expect: 1 = 必ず失敗、0 = 結果は問わない (クラッシュしない)、-1 = 必ず成功
    std::vector<Case> cases = {
      {"sb0 magic (sb1 intact)", Q(SB0, 0x12345678, 4), 0, -1},
      {"sb0 zeroed (sb1 intact)", [&]() { std::map<uint64_t, uint8_t> m; for (int i = 0; i < 3072; i++) m[SB0 + i] = 0; return m; }(), 0, -1},
      {"both magic", both(0, 0x12345678, 4), 0, 1},
      {"both log_blocksize = 13", both(16, 13, 4), 0, 1},
      {"both log_blocksize = 0", both(16, 0, 4), 0, 1},
      {"both log_blocks_per_seg = 8", both(20, 8, 4), 0, 1},
      {"both log_sectorsize = 8", both(8, 8, 4), 0, 1},
      {"both block_count = huge", both(36, 0xFFFFFFFFFFull, 8), 0, 1},
      {"both block_count = 0", both(36, 0, 8), 0, 1},
      {"both segment_count = 0", both(48, 0, 4), 0, 1},
      {"both segment_count = huge", both(48, 0xFFFFFFFFull, 4), 0, 1},
      {"both cp_blkaddr + 1", both(76, (uint64_t)g.cp + 1, 4), 0, 1},
      {"both nat_blkaddr = 0", both(84, 0, 4), 0, 1},
      {"both main_blkaddr = huge", both(92, 0xFFFFFFF0ull, 4), 0, 1},
      {"both root_ino = 0", both(96, 0, 4), 0, 1},
      {"both root_ino = huge", both(96, 0xFFFFFFFFull, 4), 0, 1},
      {"both root_ino = 4 (empty nid)", both(96, 4, 4), 0, 0},
      {"both cp_payload = huge", both(1664, 0xFFFFFFFFull, 4), 0, 1},
      {"both unknown feature bit 31", both(2180, 0x80000000ull, 4), 0, 1},
      {"both feature = encrypt", both(2180, 0x1, 4), 0, 1},
      {"both feature = casefold", both(2180, 0x1000, 4), 0, 1},
      {"both feature = verity", both(2180, 0x400, 4), 0, 1},
      {"both feature = compression-less all", both(2180, 0x1FFFF, 4), 0, 1},
      {"both device path non-empty (multi-device)", both(2201, 0x41, 1), 0, 1},
      {"cp pack0 crc (pack1 intact)", Q(CP0 + 4092, 0xDEADBEEF, 4), 0, -1},
      {"cp pack1 crc (pack0 intact)", Q(CP1 + 4092, 0xDEADBEEF, 4), 0, -1},
      {"both cp crc", [&]() { auto m = Q(CP0 + 4092, 0xDEADBEEF, 4); auto m1 = Q(CP1 + 4092, 0xDEADBEEF, 4); m.insert(m1.begin(), m1.end()); return m; }(), 0, 1},
      {"cp pack0 total_blocks = 1", Q(CP0 + 136, 1, 4), 0, 0},
      {"cp pack0 ver", Q(CP0, 0xFFFFFFFFFFFFFFFFull, 8), 0, 0},
      {"cp pack0 nat_bytes = 0", Q(CP0 + 160, 0, 4), 0, 0},
      {"cp both flags = error", [&]() { auto m = Q(CP0 + 132, 0x8, 4); auto m1 = Q(CP1 + 132, 0x8, 4); m.insert(m1.begin(), m1.end()); return m; }(), 0, 0},
      {"device truncated to 1 KiB", {}, 1024, 1},
      {"device truncated to 8 KiB", {}, 8192, 1},
      {"device truncated to 1/2", {}, base->size() / 2, 1},
      {"zeroed superblocks", [&]() { std::map<uint64_t, uint8_t> m; for (int i = 0; i < 3072; i++) { m[SB0 + i] = 0; m[SB1 + i] = 0; } return m; }(), 0, 1},
    };
    for (auto& c : cases) {
      alarm(30);
      auto spy = std::make_shared<SpyDevice>(base, -1, c.mut, c.size);
      auto vol = mount_dev(spy, &err);
      if (vol) { int b = 400; walk(*vol, vol->root(), 0, b); }
      if (c.expect == 1 && vol) { std::fprintf(stderr, "%s: [%s] mount unexpectedly succeeded\n", name.c_str(), c.what); g_fail++; }
      if (c.expect == -1 && !vol) { std::fprintf(stderr, "%s: [%s] mount unexpectedly failed (%d)\n", name.c_str(), c.what, err); g_fail++; }
      CHECK(spy->writes == 0);
      std::fprintf(stderr, "   %-10s %-42s -> mount %s (%d)\n", name.c_str(), c.what, vol ? "ok" : "failed", err);
      alarm(0);
      n_cases++;
    }
    // メタデータ領域 (ノード / NAT) を壊して歩く
    for (int k = 0; k < 4; k++) {
      std::map<uint64_t, uint8_t> mut;
      uint64_t from = (k == 0 ? (uint64_t)g.nat : k == 1 ? (uint64_t)g.main : k == 2 ? (uint64_t)g.main + 512 * 8 : (uint64_t)g.main + 512 * 10) * 4096;
      for (uint64_t o = from; o < from + 16384; o++) mut[o] = (uint8_t)(0xFF - (o & 3));
      alarm(60);
      auto vol = mount_dev(std::make_shared<SpyDevice>(base, -1, mut), &err);
      if (vol) { int b = 3000; walk(*vol, vol->root(), 0, b); }
      std::fprintf(stderr, "   %-10s damaged metadata area #%d -> mount %s (%d), no crash\n", name.c_str(), k, vol ? "ok" : "failed", err);
      alarm(0);
      n_cases++;
    }
  }
  std::fprintf(stderr, "corrupt: %d cases, no crash/hang\n", n_cases);
}

static void test_ioerr(const std::string& dir) {
  for (auto& name : list_images(dir)) {
    if (name != "plain" && name != "ext" && name.rfind("k_", 0) != 0) continue;
    BlockDevicePtr base = open_img(dir + "/" + name + ".img");
    CHECK(base);
    int mount_fail = 0, ok = 0;
    int err;
    for (long n = 0; n < 40; n++) {
      alarm(30);
      auto vol = mount_dev(std::make_shared<SpyDevice>(base, n), &err);
      if (vol) { ok++; int b = 400; walk(*vol, vol->root(), 0, b); } else mount_fail++;
      alarm(0);
    }
    for (long n = 40; n < 800; n += 7) {
      alarm(30);
      auto vol = mount_dev(std::make_shared<SpyDevice>(base, n), &err);
      if (vol) { int b = 600; walk(*vol, vol->root(), 0, b); }
      alarm(0);
    }
    std::fprintf(stderr, "   %s: read-failure injection: mount failed %d, mounted-then-failed %d (no crash)\n", name.c_str(), mount_fail, ok);
    CHECK(mount_fail > 0);
  }
}

static int fuzz(const std::string& dir, int iters, unsigned seed) {
  std::vector<std::string> images;
  for (auto& n : list_images(dir)) {
    if (needs_recovery_image(n) || n == "large") continue;
    images.push_back(n);
  }
  uint64_t x = seed * 2654435761u + 1;
  auto rnd = [&]() { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
  int mounted = 0, total = 0;
  for (auto& name : images) {
    BlockDevicePtr base = open_img(dir + "/" + name + ".img");
    if (!base) { g_fail++; continue; }
    // きれいな状態で歩いて、読まれたブロックの一覧を得る (ここだけを壊す: ほかの場所を壊しても読まれないため)
    std::vector<uint64_t> blocks;
    {
      auto spy = std::make_shared<SpyDevice>(base);
      spy->track = true;
      int err; auto vol = mount_dev(spy, &err);
      CHECK(vol != nullptr);
      if (vol) { int b = 1500; walk(*vol, vol->root(), 0, b); }
      blocks.assign(spy->touched.begin(), spy->touched.end());
    }
    if (blocks.empty()) { g_fail++; continue; }
    for (int it = 0; it < iters; it++) {
      std::map<uint64_t, uint8_t> mut;
      int nmut = 1 + (int)(rnd() % 24);
      uint64_t blk = blocks[rnd() % blocks.size()];
      unsigned mode = (unsigned)(rnd() % 3);
      for (int k = 0; k < nmut; k++) {
        uint64_t within = mode == 0 ? rnd() % 64 : mode == 1 ? 4096 - 1 - rnd() % 64 : rnd() % 4096;   // 先頭 (ヘッダ) / 末尾 (footer) / 全体
        mut[blk * 4096 + within] = (uint8_t)(rnd() % 3 == 0 ? 0xFF : rnd());
      }
      if (rnd() % 5 == 0) { uint64_t b2 = blocks[rnd() % blocks.size()]; mut[b2 * 4096 + rnd() % 4096] = (uint8_t)rnd(); }
      if (const char* lp = std::getenv("USBMEM_FUZZ_LAST")) {
        if (FILE* lf = std::fopen(lp, "w")) { std::fprintf(lf, "%s\n", name.c_str()); for (auto& kv : mut) std::fprintf(lf, "%llu %u\n", (unsigned long long)kv.first, kv.second); std::fclose(lf); }
      }
      alarm(60);   // ハング検出
      int err;
      auto spy = std::make_shared<SpyDevice>(base, -1, mut);
      auto vol = mount_dev(spy, &err);
      total++;
      if (vol) { mounted++; int budget = 1500; walk(*vol, vol->root(), 0, budget); }
      CHECK(spy->writes == 0);
      alarm(0);
    }
  }
  struct rusage ru; getrusage(RUSAGE_SELF, &ru);
  std::fprintf(stderr, "fuzz: %d iterations (seed %u), %d mounted, no crash/hang, max RSS %ld MiB\n", total, seed, mounted, (long)(ru.ru_maxrss / 1024));
  return g_fail ? 1 : 0;
}

// 回復 (roll-forward) が必要なイメージ: 既定では EUCLEAN (117)。allow_noload なら読み取り専用で開き、チェックポイント時点の内容が読める
static int test_noload(const std::string& dir) {
  int n = 0;
  for (auto& name : list_images(dir)) {
    if (!needs_recovery_image(name)) continue;
    n++;
    BlockDevicePtr base = open_img(dir + "/" + name + ".img");
    CHECK(base);
    int err = 0;
    auto spy = std::make_shared<SpyDevice>(base);
    auto v0 = mount_dev(spy, &err);
    std::fprintf(stderr, "   %s: default mount -> %s (%d)\n", name.c_str(), v0 ? "ok" : "failed", err);
    CHECK(!v0 && err == 117);
    auto v1 = mount_dev(spy, &err, true);
    CHECK(v1 != nullptr);
    if (v1) {
      CHECK(v1->recovery_ignored());
      CHECK(!v1->writable());
      int b = 2000; walk(*v1, v1->root(), 0, b);
      std::vector<FsDirEntry> ents; CHECK(v1->readdir(v1->root(), &ents) == 0);
      uint64_t c = 0; CHECK(v1->create(v1->root(), "x", 0644, &c) == -30);
      std::fprintf(stderr, "   %s: noload mount ok, %zu root entries, recovery_ignored\n", name.c_str(), ents.size());
    }
    CHECK(spy->writes == 0 && spy->flushes == 0);
  }
  if (n == 0) std::fprintf(stderr, "noload: no k_unclean* image (kernel-made fixtures not generated) — skipped\n");
  return g_fail ? 1 : 0;
}

int main(int argc, char** argv) {
  std::signal(SIGALRM, [](int) { std::fprintf(stderr, "TIMEOUT (hang)\n"); std::abort(); });
  if (argc < 2) { std::fprintf(stderr, "usage: %s <fixture_dir> | --corrupt <dir> | --ioerr <dir> | --fuzz <dir> <iters> <seed> | --noload <dir>\n", argv[0]); return 2; }
  std::string mode = argv[1];
  if (mode == "--fuzz" && argc >= 5) return fuzz(argv[2], std::atoi(argv[3]), (unsigned)std::atoi(argv[4]));
  if (mode == "--corrupt" && argc >= 3) { test_corrupt(argv[2]); return g_fail ? 1 : 0; }
  if (mode == "--ioerr" && argc >= 3) { test_ioerr(argv[2]); return g_fail ? 1 : 0; }
  if (mode == "--noload" && argc >= 3) return test_noload(argv[2]);
  auto images = list_images(argv[1]);
  CHECK(!images.empty());
  const char* only = std::getenv("F2FS_ONLY");
  for (auto& n : images) if (!needs_recovery_image(n) && (!only || n == only)) verify_image(argv[1], n);
  if (g_fail) { std::fprintf(stderr, "FAILED: %d checks\n", g_fail); return 1; }
  std::fprintf(stderr, "f2fs: all checks passed (%zu images)\n", images.size());
  return 0;
}
