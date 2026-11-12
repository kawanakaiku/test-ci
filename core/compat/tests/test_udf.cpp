// SPDX-License-Identifier: GPL-2.0-or-later
// UDF (Linux fs/udf を LinuxCompat 上で動かしたもの) の読み取りテスト。
// 使い方: usbmem_udf_tests <fixture_dir>            tools/gen_udf_fixture.py が作った *.img / *.manifest を全部検証
//         usbmem_udf_tests --corrupt <fixture_dir>  壊れたボリューム (VRS / アンカー / ボリューム記述子列) のマウントが、クラッシュせず失敗するか、開けても安全に走査できること
//         usbmem_udf_tests --ioerr <fixture_dir>    マウント・走査中のデバイス I/O エラーで、クラッシュせず失敗すること
//         usbmem_udf_tests --fuzz <fixture_dir> <iterations> <seed>
//         usbmem_udf_tests --coexist <udf_fixture_dir> <ext2_fixture_dir> <iso_fixture_dir>   UDF と ext2 と ISO 9660 を同じプロセスで同時にマウントできる
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
#include "usbmem/isofs_c.h"
#include "usbmem/udf_c.h"
#include "usbmem/ntfs_c.h"   // usbmem_bdev_wrap / free

using namespace usbmem;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

// 読み取り専用デバイス。write_at が 1 度でも呼ばれたら数える。fail_after >= 0 なら、その回数の read_at の後はすべて EIO。mut は読み出し時に差し替えるバイト。
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
  std::shared_ptr<SpyDevice> dev; usbmem_bdev* bd = nullptr; usbmem_udf* v = nullptr; int err = 0;
  ~Mounted() { if (v) usbmem_udf_unmount(v); if (bd) usbmem_bdev_free(bd); }
};
static std::unique_ptr<Mounted> mount_dev(std::shared_ptr<SpyDevice> dev) {
  auto m = std::make_unique<Mounted>();
  m->dev = dev;
  BlockDevicePtr p = dev;
  m->bd = usbmem_bdev_wrap(&p);
  m->err = usbmem_udf_mount(m->bd, 1, &m->v);
  if (m->err) m->v = nullptr;
  return m;
}
static BlockDevicePtr open_img(const std::string& path) {
  BlockDevicePtr d;
  if (!RawImageDevice::open_path(path, true, 512, &d).ok()) return nullptr;
  return d;
}

struct Entry { std::string name; uint64_t ino; unsigned type; };
static int list_dir(usbmem_udf* v, uint64_t dir, std::vector<Entry>* out) {
  out->clear();
  return usbmem_udf_readdir(v, dir, [](void* u, const char* n, int len, uint64_t ino, unsigned t) {
    static_cast<std::vector<Entry>*>(u)->push_back({std::string(n, (size_t)len), ino, t}); return 0; }, out);
}
static int resolve(usbmem_udf* v, const std::string& path, uint64_t* ino) {
  uint64_t cur = usbmem_udf_root_ino(v); size_t i = 0;
  while (i < path.size()) {
    size_t j = path.find('/', i); if (j == std::string::npos) j = path.size();
    if (j > i) { std::string part = path.substr(i, j - i); int rc = usbmem_udf_lookup(v, cur, part.data(), part.size(), &cur); if (rc) return rc; }
    i = j + 1;
  }
  *ino = cur; return 0;
}

// manifest (tools/gen_udf_fixture.py): D path mtime / F path size mtime nlink spec mode / L path target / Z path size / O path / V key value
struct Region { uint64_t off, len; uint32_t seed; };
struct MFile { std::string path; uint64_t size; int64_t mtime; uint32_t nlink; std::vector<Region> regions; unsigned mode; };
struct MLink { std::string path, target; };
struct Manifest {
  std::vector<std::pair<std::string, int64_t>> dirs; std::vector<MFile> files; std::vector<MLink> links;
  std::vector<std::pair<std::string, uint64_t>> zfiles; std::set<std::string> optional; std::map<std::string, int> v;
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
    else if (c.size() == 3 && c[0] == "Z") m->zfiles.push_back({c[1], std::stoull(c[2])});
    else if (c.size() == 2 && c[0] == "O") m->optional.insert(c[1]);
    else if (c.size() == 3 && c[0] == "V") m->v[c[1]] = std::stoi(c[2]);
    else if (c[0] == "F" && c.size() >= 5) {
      MFile mf{c[1], std::stoull(c[2]), std::stoll(c[3]), (uint32_t)std::stoul(c[4]), {}, 0};
      if (c.size() >= 6 && !c[5].empty()) {
        std::stringstream rs(c[5]); std::string part;
        while (std::getline(rs, part, ',')) {   // 疎ファイル: "d:0:100:3,d:3145628:100:3"
          std::vector<std::string> p; std::stringstream is(part); std::string q;
          while (std::getline(is, q, ':')) p.push_back(q);
          if (p[0] == "d") mf.regions.push_back({std::stoull(p[1]), std::stoull(p[2]), (uint32_t)std::stoul(p[3])});
        }
      }
      if (c.size() >= 7) mf.mode = (unsigned)std::stoul(c[6], nullptr, 8);
      m->files.push_back(mf);
    }
  }
  return true;
}
static std::string parent_of(const std::string& p) { size_t k = p.rfind('/'); return k == std::string::npos ? "" : p.substr(0, k); }
static std::string base_of(const std::string& p) { size_t k = p.rfind('/'); return k == std::string::npos ? p : p.substr(k + 1); }
static std::vector<uint8_t> pattern_at(uint32_t seed, uint64_t off, size_t n) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; i++) { uint64_t o = off + i; v[i] = static_cast<uint8_t>((o * 31 + seed * 7 + (o >> 8)) & 0xFF); }
  return v;
}

static void verify_file(usbmem_udf* v, const MFile& f, uint64_t ino, bool exact_mode) {
  usbmem_udf_stat st;
  CHECK(usbmem_udf_stat_ino(v, ino, &st) == 0);
  CHECK(st.size == f.size);
  CHECK((st.mode & 0170000) == 0100000);
  CHECK(st.mtime_sec == f.mtime);
  if (exact_mode) { CHECK(st.nlink == f.nlink); CHECK((st.mode & 07777) == f.mode); }
  std::vector<uint8_t> tmp(16, 0xAB);
  CHECK(usbmem_udf_read(v, ino, f.size, tmp.data(), 10) == 0);          // ちょうど EOF
  CHECK(usbmem_udf_read(v, ino, f.size + 4096, tmp.data(), 10) == 0);   // EOF より先
  std::vector<uint8_t> want(f.size, 0);
  for (auto& r : f.regions) { auto p = pattern_at(r.seed, r.off, (size_t)r.len); std::memcpy(want.data() + r.off, p.data(), p.size()); }
  std::vector<uint8_t> got(f.size + 16, 0xAB);
  int64_t n = usbmem_udf_read(v, ino, 0, got.data(), got.size());     // 要求が EOF を超えても、残りだけ返す
  CHECK(n == (int64_t)f.size);
  if (n == (int64_t)f.size && f.size && std::memcmp(got.data(), want.data(), (size_t)f.size) != 0) {
    size_t k = 0; while (k < f.size && got[k] == want[k]) k++;
    std::fprintf(stderr, "content mismatch %s: size %llu first diff at %zu (got %u want %u)\n", f.path.c_str(), (unsigned long long)f.size, k, got[k], want[k]);
    g_fail++;
  }
  if (f.size > 3) {   // 不揃いな位置・長さ (2048 の境界をまたぐものを含む)
    for (uint64_t off : {uint64_t(1), uint64_t(f.size / 2), uint64_t(f.size - 1), uint64_t(2047), uint64_t(2048), uint64_t(4095)}) {
      if (off >= f.size) continue;
      size_t len = (size_t)std::min<uint64_t>(f.size - off, 4099);
      std::vector<uint8_t> part(len);
      CHECK(usbmem_udf_read(v, ino, off, part.data(), len) == (int64_t)len);
      CHECK(std::memcmp(part.data(), want.data() + off, len) == 0);
    }
  }
}

static void verify_image(const std::string& dir, const std::string& name) {
  std::fprintf(stderr, "== %s\n", name.c_str());
  Manifest man;
  CHECK(load_manifest(dir + "/" + name + ".manifest", &man));
  bool exact = man.v["bridge"] == 0;   // ブリッジ (genisoimage -udf) は mode / nlink を木と同じには書かない
  BlockDevicePtr base = open_img(dir + "/" + name + ".img");
  CHECK(base != nullptr);
  if (!base) return;
  auto spy = std::make_shared<SpyDevice>(base);
  auto m = mount_dev(spy);
  CHECK(m->err == 0 && m->v);
  if (!m->v) { std::fprintf(stderr, "mount failed: %d\n", m->err); return; }
  usbmem_udf* v = m->v;
  const uint64_t root = usbmem_udf_root_ino(v);

  // 期待するディレクトリの内容 (パス → 名前の集合)。O (relocation の rr_moved など) はあってもなくてもよい
  std::map<std::string, std::set<std::string>> expect;
  expect[""];
  for (auto& d : man.dirs) { expect[parent_of(d.first)].insert(base_of(d.first)); expect[d.first]; }
  for (auto& f : man.files) expect[parent_of(f.path)].insert(base_of(f.path));
  for (auto& l : man.links) expect[parent_of(l.path)].insert(base_of(l.path));
  // 深さ 8 を超える階層は relocation されるが、カーネルは元の場所に透過的に見せる (d8 の下に d9 が現れる)。ディレクトリの宣言は manifest に無いので補う
  for (auto& f : man.files) { std::string p = parent_of(f.path); while (!p.empty()) { expect[parent_of(p)].insert(base_of(p)); expect[p]; p = parent_of(p); } }
  for (auto& kv : expect) {
    uint64_t dino = 0;
    CHECK(resolve(v, kv.first, &dino) == 0);
    std::vector<Entry> ents;
    CHECK(list_dir(v, dino, &ents) == 0);
    std::set<std::string> got; int dots = 0, dotdots = 0; size_t nonspecial = 0;
    for (auto& e : ents) {
      if (e.name == ".") { dots++; CHECK(e.ino == dino); continue; }
      if (e.name == "..") { dotdots++; if (kv.first.empty()) CHECK(e.ino == root); continue; }
      got.insert(e.name); nonspecial++;
    }
    CHECK(dots == 1 && dotdots == 1);
    CHECK(got.size() == nonspecial);      // 重複なし
    std::set<std::string> want = kv.second;
    if (got != want) {
      std::fprintf(stderr, "dir '%s': expected %zu entries, got %zu\n", kv.first.c_str(), want.size(), got.size());
      for (auto& n : want) if (!got.count(n)) std::fprintf(stderr, "  missing: %s\n", n.c_str());
      for (auto& n : got) if (!want.count(n)) std::fprintf(stderr, "  extra: %s\n", n.c_str());
      g_fail++;
    }
    for (auto& e : ents) {   // readdir が返した ino で、そのまま stat できる。種別が一致する (udf の readdir の d_type は DT_UNKNOWN)
      if (e.name == "." || e.name == "..") continue;
      usbmem_udf_stat st;
      int src = usbmem_udf_stat_ino(v, e.ino, &st);
      if (src) std::fprintf(stderr, "stat '%s/%s' ino %llu: %d\n", kv.first.c_str(), e.name.c_str(), (unsigned long long)e.ino, src);
      CHECK(src == 0);
      std::string full = kv.first.empty() ? e.name : kv.first + "/" + e.name;
      bool is_dir = expect.count(full) > 0;
      bool is_link = false; for (auto& l : man.links) if (l.path == full) is_link = true;
      CHECK(((st.mode & 0170000) == 0040000) == is_dir);
      CHECK(((st.mode & 0170000) == 0120000) == is_link);
      CHECK(st.ino == e.ino);
    }
  }
  for (auto& d : man.dirs) {   // ディレクトリの mtime
    uint64_t ino = 0; usbmem_udf_stat st;
    CHECK(resolve(v, d.first, &ino) == 0 && usbmem_udf_stat_ino(v, ino, &st) == 0);
    CHECK(st.mtime_sec == d.second);
  }
  // 全ファイル: サイズ・内容・mtime・nlink・権限
  for (auto& f : man.files) {
    uint64_t ino = 0;
    int rc = resolve(v, f.path, &ino);
    CHECK(rc == 0);
    if (rc) { std::fprintf(stderr, "resolve %s: %d\n", f.path.c_str(), rc); continue; }
    verify_file(v, f, ino, exact);
  }
  // シンボリックリンク (Rock Ridge SL): stat の mode は S_IFLNK、read でリンク先の文字列が読める
  for (auto& l : man.links) {
    uint64_t ino = 0; usbmem_udf_stat st;
    CHECK(resolve(v, l.path, &ino) == 0);       // lookup はリンクを辿らない
    CHECK(usbmem_udf_stat_ino(v, ino, &st) == 0);
    CHECK((st.mode & 0170000) == 0120000);
    std::vector<char> b(4096, 'x');
    int64_t n = usbmem_udf_read(v, ino, 0, b.data(), b.size());
    CHECK(n == (int64_t)l.target.size());
    if (n == (int64_t)l.target.size()) CHECK(std::memcmp(b.data(), l.target.data(), l.target.size()) == 0);
  }
  if (exact && !man.files.empty()) CHECK(!man.links.empty());
  // 名前の扱い
  uint64_t c = 0;
  CHECK(resolve(v, "nonexistent", &c) == -ENOENT);
  CHECK(resolve(v, "x/nonexistent", &c) == -ENOENT);
  if (!man.files.empty()) { std::string f0 = man.files[0].path; CHECK(resolve(v, f0 + "/x", &c) == -ENOTDIR); }
  std::vector<uint8_t> tmp(16);
  CHECK(usbmem_udf_read(v, root, 0, tmp.data(), 16) == -EISDIR);
  // 不正な ino
  CHECK(usbmem_udf_stat_ino(v, 0, nullptr) < 0);
  CHECK(usbmem_udf_stat_ino(v, 1, nullptr) < 0);
  CHECK(usbmem_udf_stat_ino(v, 999999999, nullptr) < 0);
  CHECK(usbmem_udf_stat_ino(v, 1ull << 40, nullptr) < 0);
  CHECK(usbmem_udf_lookup(v, 999999999, "x", 1, &c) < 0);
  // 読み取り専用: 書き込みマウント要求は拒否、デバイスには 1 度も書かない
  BlockDevicePtr p2 = spy; usbmem_bdev* bd2 = usbmem_bdev_wrap(&p2); usbmem_udf* v2 = nullptr;
  CHECK(usbmem_udf_mount(bd2, 0, &v2) == -EROFS);
  usbmem_bdev_free(bd2);
  CHECK(spy->writes == 0);
  CHECK(spy->flushes == 0);
  // 2 回目のマウント (別インスタンス) も同じ結果
  auto m2 = mount_dev(std::make_shared<SpyDevice>(base));
  CHECK(m2->err == 0);
  if (m2->v && !man.files.empty()) { uint64_t x = 0; CHECK(resolve(m2->v, man.files[0].path, &x) == 0); }
  std::fprintf(stderr, "   %zu files, %zu dirs, %zu links, device reads %ld\n", man.files.size(), man.dirs.size(), man.links.size(), (long)spy->reads);
}

static const char* const kImages[] = {"hd512", "hd2048", "hd4096", "hd102", "spar150", "cdrw150", "bridge", "vat250_empty"};
static std::vector<std::string> list_images(const std::string& dir) {
  std::vector<std::string> out;
  for (const char* n : kImages) if (access((dir + "/" + n + ".img").c_str(), R_OK) == 0) out.push_back(n);
  return out;
}

// 全体を歩く (fuzz / ioerr 用)。クラッシュ・ハングしないことだけを見る。budget で打ち切り。
static void walk(usbmem_udf* v, uint64_t dir, int depth, int& budget) {
  if (depth > 20 || budget <= 0) return;
  std::vector<Entry> ents;
  if (list_dir(v, dir, &ents)) return;
  for (auto& e : ents) {
    if (--budget <= 0) return;
    if (e.name == "." || e.name == "..") continue;
    usbmem_udf_stat st;
    if (usbmem_udf_stat_ino(v, e.ino, &st)) continue;
    if ((st.mode & 0170000) == 0040000) walk(v, e.ino, depth + 1, budget);
    else {
      std::vector<uint8_t> buf(65536); uint64_t sz = std::min<uint64_t>(st.size, 1u << 20);
      for (uint64_t o = 0; o < sz && budget > 0; o += buf.size(), budget--) if (usbmem_udf_read(v, e.ino, o, buf.data(), buf.size()) <= 0) break;
    }
  }
}

static uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

// イメージのブロックサイズ (manifest の V bs)。無ければ 2048
static uint32_t image_bs(const std::string& dir, const std::string& name) {
  Manifest m;
  if (!load_manifest(dir + "/" + name + ".manifest", &m)) return 2048;
  return m.v.count("bs") ? (uint32_t)m.v["bs"] : 2048;
}
// UDF の場所: VRS は 32768 から。アンカー (AVDP) はブロック 256 と、最終ブロック / 最終 - 256 など。AVDP は +16 に主 VDS の extent (長さ, 位置)、+24 に予備 VDS の extent。
struct Layout { uint32_t bs; uint64_t size; std::vector<uint64_t> anchor_off; int n_anchor; uint64_t main_off, main_len, resv_off, resv_len; };
static Layout find_layout(BlockDevicePtr base, uint32_t bs) {
  Layout L{}; L.bs = bs; L.size = base->size();
  uint64_t nblocks = L.size / bs;
  std::vector<uint64_t> cand = {256ull * bs, 512ull * bs};
  for (uint64_t b = nblocks > 400 ? nblocks - 400 : 0; b < nblocks; b++) cand.push_back(b * bs);   // 末尾付近 (genisoimage は余白に何枚もアンカーを書く)
  for (uint64_t off : cand) {
    uint8_t t[32];
    if (off + 32 > L.size || !base->read_at(off, t, 32).ok()) continue;
    if (t[0] == 2 && t[1] == 0) {   // TAG_IDENT_AVDP = 2
      L.anchor_off.push_back(off); L.n_anchor++;
      if (!L.main_len) { L.main_len = le32(t + 16); L.main_off = (uint64_t)le32(t + 20) * bs; L.resv_len = le32(t + 24); L.resv_off = (uint64_t)le32(t + 28) * bs; }
    }
  }
  return L;
}

static void test_corrupt(const std::string& dir) {
  for (auto& name : list_images(dir)) {
    BlockDevicePtr base = open_img(dir + "/" + name + ".img");
    CHECK(base);
    CHECK(mount_dev(std::make_shared<SpyDevice>(base))->err == 0);   // 正常
    Layout L = find_layout(base, image_bs(dir, name));
    CHECK(L.n_anchor >= 1);
    struct Case { const char* what; std::map<uint64_t, uint8_t> mut; uint64_t size; bool must_fail; };
    std::vector<Case> cases;
    auto zero = [](std::map<uint64_t, uint8_t>& m, uint64_t off, uint64_t len) { for (uint64_t i = 0; i < len && i < (1u << 20); i++) m[off + i] = 0; };
    { Case c{"VRS area zeroed (32 KiB..48 KiB)", {}, 0, true}; zero(c.mut, 32768, 16384); cases.push_back(c); }
    { Case c{"BEA01 only broken", {}, 0, false}; c.mut[32768 + 1] = 'X'; cases.push_back(c); }
    { Case c{"first anchor tag broken", {}, 0, false}; if (L.n_anchor) { c.mut[L.anchor_off[0]] = 0xFF; c.mut[L.anchor_off[0] + 1] = 0xFF; } cases.push_back(c); }
    { Case c{"all anchors broken", {}, 0, true}; for (int i = 0; i < L.n_anchor; i++) { c.mut[L.anchor_off[i]] = 0xFF; c.mut[L.anchor_off[i] + 1] = 0xFF; } cases.push_back(c); }
    { Case c{"all anchors' VDS pointers = 0", {}, 0, false}; for (int i = 0; i < L.n_anchor; i++) for (int k = 16; k < 32; k++) c.mut[L.anchor_off[i] + k] = 0; cases.push_back(c); }
    { Case c{"main VDS zeroed", {}, 0, false}; zero(c.mut, L.main_off, L.main_len); cases.push_back(c); }
    { Case c{"main and reserve VDS zeroed", {}, 0, true}; zero(c.mut, L.main_off, L.main_len); zero(c.mut, L.resv_off, L.resv_len); cases.push_back(c); }
    { Case c{"main VDS tag CRC broken", {}, 0, false}; c.mut[L.main_off + 8] ^= 0xFF; cases.push_back(c); }
    cases.push_back({"device truncated to 36 KiB", {}, 36864, true});
    cases.push_back({"device truncated to 64 KiB", {}, 65536, true});
    cases.push_back({"device truncated to 1 MiB", {}, 1u << 20, false});
    { Case c{"device truncated by one block", {}, L.size - L.bs, false}; cases.push_back(c); }
    { Case c{"system area + VRS + anchor zeroed", {}, 0, true}; zero(c.mut, 0, 65536); for (int i = 0; i < L.n_anchor; i++) zero(c.mut, L.anchor_off[i], 64); cases.push_back(c); }
    for (auto& c : cases) {
      alarm(30);
      auto m = mount_dev(std::make_shared<SpyDevice>(base, -1, c.mut, c.size));
      if (m->v) { int b = 400; walk(m->v, usbmem_udf_root_ino(m->v), 0, b); }
      if (c.must_fail && m->err == 0) { std::fprintf(stderr, "%s: [%s] mount unexpectedly succeeded\n", name.c_str(), c.what); g_fail++; }
      std::fprintf(stderr, "   %-10s %-40s -> mount %s (%d)\n", name.c_str(), c.what, m->err ? "failed" : "ok", m->err);
      alarm(0);
    }
  }
}

static void test_ioerr(const std::string& dir) {
  for (auto& name : list_images(dir)) {
    BlockDevicePtr base = open_img(dir + "/" + name + ".img");
    CHECK(base);
    int mount_fail = 0, ok = 0;
    for (long n = 0; n < 60; n++) {
      alarm(30);
      auto m = mount_dev(std::make_shared<SpyDevice>(base, n));
      if (m->v) { ok++; int b = 400; walk(m->v, usbmem_udf_root_ino(m->v), 0, b); } else mount_fail++;
      alarm(0);
    }
    for (long n = 60; n < 400; n += 7) {
      alarm(30);
      auto m = mount_dev(std::make_shared<SpyDevice>(base, n));
      if (m->v) { int b = 400; walk(m->v, usbmem_udf_root_ino(m->v), 0, b); }
      alarm(0);
    }
    std::fprintf(stderr, "   %s: read-failure injection: mount failed %d, mounted-then-failed %d (no crash)\n", name.c_str(), mount_fail, ok);
    CHECK(mount_fail > 0);
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
    Layout L = find_layout(base, image_bs(dir, name));
    uint64_t size = base->size();
    for (int it = 0; it < iters; it++) {
      std::map<uint64_t, uint8_t> mut;
      int nmut = 1 + (int)(rnd() % 24);
      unsigned region = (unsigned)(rnd() % 9);   // 1 反復では 1 つの領域だけを壊す
      for (int k = 0; k < nmut; k++) {
        uint64_t off;
        switch (region) {
          case 0: off = 32768 + rnd() % 8192; break;                                                      // VRS
          case 1: off = L.n_anchor ? L.anchor_off[rnd() % L.n_anchor] + rnd() % 64 : 256ull * L.bs; break;  // アンカー (タグと VDS の extent)
          case 2: off = L.main_off + rnd() % std::max<uint64_t>(std::min<uint64_t>(L.main_len, 16384), 1); break;   // 主 VDS
          case 3: off = L.resv_off + rnd() % std::max<uint64_t>(std::min<uint64_t>(L.resv_len, 16384), 1); break;   // 予備 VDS
          case 4: off = (L.main_off + L.main_len) + rnd() % (256u << 10); break;                          // VDS の後 (ファイルセット記述子・ルート ICB・ルートディレクトリがある辺り)
          case 5: off = rnd() % std::min<uint64_t>(size, 1u << 20); break;
          case 6: off = rnd() % std::min<uint64_t>(size, 4u << 20); break;
          case 7: off = size - 1 - rnd() % std::min<uint64_t>(size, 1u << 18); break;                     // 末尾 (アンカー・VAT 付近)
          default: off = rnd() % size; break;
        }
        if (off >= size) off = size - 1;
        mut[off] = (uint8_t)(rnd() % 3 == 0 ? 0xFF : rnd());
      }
      if (const char* lp = std::getenv("USBMEM_FUZZ_LAST")) {
        if (FILE* lf = std::fopen(lp, "w")) { std::fprintf(lf, "%s\n", name.c_str()); for (auto& kv : mut) std::fprintf(lf, "%llu %u\n", (unsigned long long)kv.first, kv.second); std::fclose(lf); }
      }
      alarm(30);   // ハング検出
      auto m = mount_dev(std::make_shared<SpyDevice>(base, -1, mut));
      total++;
      if (m->v) { mounted++; int budget = 1500; walk(m->v, usbmem_udf_root_ino(m->v), 0, budget); }
      CHECK(m->dev->writes == 0);
      alarm(0);
    }
  }
  std::fprintf(stderr, "fuzz: %d iterations (seed %u), %d mounted, no crash/hang\n", total, seed, mounted);
  return g_fail ? 1 : 0;
}

// UDF と ext2 と ISO 9660 を同じプロセスで同時にマウントして読む (モジュール初期化・super_block・inode キャッシュ・bh が互いに干渉しない)
static int coexist(const std::string& udfdir, const std::string& ext2dir, const std::string& isodir) {
  BlockDevicePtr a = open_img(udfdir + "/hd2048.img"), b = open_img(ext2dir + "/b4k.img"), c = open_img(isodir + "/rr.img");
  CHECK(a && b && c);
  if (!a || !b || !c) return 1;
  auto m1 = mount_dev(std::make_shared<SpyDevice>(a));
  CHECK(m1->err == 0);
  BlockDevicePtr bp = std::make_shared<SpyDevice>(b), cp = std::make_shared<SpyDevice>(c);
  usbmem_bdev* bd = usbmem_bdev_wrap(&bp); usbmem_ext2* e = nullptr;
  usbmem_bdev* cd = usbmem_bdev_wrap(&cp); usbmem_isofs* iso = nullptr;
  CHECK(usbmem_ext2_mount(bd, 1, &e) == 0);
  CHECK(usbmem_isofs_mount(cd, 1, &iso) == 0);
  uint64_t x = 0, y = 0, z = 0;
  if (m1->v) CHECK(resolve(m1->v, "size_11.bin", &x) == 0);
  if (e) { CHECK(usbmem_ext2_lookup(e, USBMEM_EXT2_ROOT_INO, "small.txt", 9, &y) == 0); }
  if (iso) { CHECK(usbmem_isofs_lookup(iso, usbmem_isofs_root_ino(iso), "size_11.bin", 11, &z) == 0); }
  char b1[16] = {0}, b2[16] = {0}, b3[16] = {0};
  if (m1->v) CHECK(usbmem_udf_read(m1->v, x, 0, b1, 11) == 11);
  if (e) CHECK(usbmem_ext2_read(e, y, 0, b2, 11) == 11);
  if (iso) CHECK(usbmem_isofs_read(iso, z, 0, b3, 11) == 11);
  if (e) usbmem_ext2_unmount(e);
  if (iso) usbmem_isofs_unmount(iso);
  usbmem_bdev_free(bd); usbmem_bdev_free(cd);
  std::fprintf(stderr, "coexist: udf + ext2 + iso9660 mounted together, ok\n");
  return g_fail ? 1 : 0;
}

int main(int argc, char** argv) {
  std::signal(SIGALRM, [](int) { std::fprintf(stderr, "TIMEOUT (hang)\n"); std::abort(); });
  if (argc < 2) { std::fprintf(stderr, "usage: %s <fixture_dir> | --corrupt <dir> | --ioerr <dir> | --fuzz <dir> <iters> <seed> | --coexist <udf_dir> <ext2_dir> <iso_dir>\n", argv[0]); return 2; }
  std::string mode = argv[1];
  if (mode == "--fuzz" && argc >= 5) return fuzz(argv[2], std::atoi(argv[3]), (unsigned)std::atoi(argv[4]));
  if (mode == "--corrupt" && argc >= 3) { test_corrupt(argv[2]); return g_fail ? 1 : 0; }
  if (mode == "--ioerr" && argc >= 3) { test_ioerr(argv[2]); return g_fail ? 1 : 0; }
  if (mode == "--coexist" && argc >= 5) return coexist(argv[2], argv[3], argv[4]);
  auto images = list_images(argv[1]);
  CHECK(!images.empty());
  for (auto& n : images) verify_image(argv[1], n);
  if (g_fail) { std::fprintf(stderr, "FAILED: %d checks\n", g_fail); return 1; }
  std::fprintf(stderr, "udf: all checks passed (%zu images)\n", images.size());
  return 0;
}
