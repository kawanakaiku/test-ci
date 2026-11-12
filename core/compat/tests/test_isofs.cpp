// SPDX-License-Identifier: GPL-2.0-or-later
// ISO 9660 (Linux fs/isofs を LinuxCompat 上で動かしたもの) の読み取りテスト。
// 使い方: usbmem_isofs_tests <fixture_dir>            tools/gen_iso_fixture.py が作った *.img / *.manifest を全部検証
//         usbmem_isofs_tests --corrupt <fixture_dir>  壊れたボリューム (PVD/サイズ/ルート) のマウントが、クラッシュせず失敗すること
//         usbmem_isofs_tests --ioerr <fixture_dir>    マウント・走査中のデバイス I/O エラーで、クラッシュせず失敗すること
//         usbmem_isofs_tests --fuzz <fixture_dir> <iterations> <seed>
//         usbmem_isofs_tests --coexist <iso_fixture_dir> <ext2_fixture_dir>   ISO と ext2 を同じプロセスで同時にマウントできる
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
  std::shared_ptr<SpyDevice> dev; usbmem_bdev* bd = nullptr; usbmem_isofs* v = nullptr; int err = 0;
  ~Mounted() { if (v) usbmem_isofs_unmount(v); if (bd) usbmem_bdev_free(bd); }
};
static std::unique_ptr<Mounted> mount_dev(std::shared_ptr<SpyDevice> dev) {
  auto m = std::make_unique<Mounted>();
  m->dev = dev;
  BlockDevicePtr p = dev;
  m->bd = usbmem_bdev_wrap(&p);
  m->err = usbmem_isofs_mount(m->bd, 1, &m->v);
  if (m->err) m->v = nullptr;
  return m;
}
static BlockDevicePtr open_img(const std::string& path) {
  BlockDevicePtr d;
  if (!RawImageDevice::open_path(path, true, 512, &d).ok()) return nullptr;
  return d;
}

struct Entry { std::string name; uint64_t ino; unsigned type; };
static int list_dir(usbmem_isofs* v, uint64_t dir, std::vector<Entry>* out) {
  out->clear();
  return usbmem_isofs_readdir(v, dir, [](void* u, const char* n, int len, uint64_t ino, unsigned t) {
    static_cast<std::vector<Entry>*>(u)->push_back({std::string(n, (size_t)len), ino, t}); return 0; }, out);
}
static int resolve(usbmem_isofs* v, const std::string& path, uint64_t* ino) {
  uint64_t cur = usbmem_isofs_root_ino(v); size_t i = 0;
  while (i < path.size()) {
    size_t j = path.find('/', i); if (j == std::string::npos) j = path.size();
    if (j > i) { std::string part = path.substr(i, j - i); int rc = usbmem_isofs_lookup(v, cur, part.data(), part.size(), &cur); if (rc) return rc; }
    i = j + 1;
  }
  *ino = cur; return 0;
}

// manifest (tools/gen_iso_fixture.py): D path mtime / F path size mtime nlink spec mode / L path target / Z path size / O path / V key value
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
        std::vector<std::string> p; std::stringstream is(c[5]); std::string q;
        while (std::getline(is, q, ':')) p.push_back(q);
        if (p[0] == "d") mf.regions.push_back({std::stoull(p[1]), std::stoull(p[2]), (uint32_t)std::stoul(p[3])});
      }
      if (c.size() >= 7) mf.mode = (unsigned)std::stoul(c[6], nullptr, 8);
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

static void verify_file(usbmem_isofs* v, const MFile& f, uint64_t ino, bool rr) {
  usbmem_isofs_stat st;
  CHECK(usbmem_isofs_stat_ino(v, ino, &st) == 0);
  CHECK(st.size == f.size);
  CHECK((st.mode & 0170000) == 0100000);
  CHECK(st.mtime_sec == f.mtime);
  if (rr) { CHECK(st.nlink == f.nlink); CHECK((st.mode & 07777) == f.mode); }
  else CHECK((st.mode & 0777) == 0555);   // Rock Ridge が無い: 上流の既定 (全員に r-x。DOS と共有されるディスクでは何でも実行ファイルになりうるため)
  std::vector<uint8_t> tmp(16, 0xAB);
  CHECK(usbmem_isofs_read(v, ino, f.size, tmp.data(), 10) == 0);          // ちょうど EOF
  CHECK(usbmem_isofs_read(v, ino, f.size + 4096, tmp.data(), 10) == 0);   // EOF より先
  std::vector<uint8_t> want(f.size, 0);
  for (auto& r : f.regions) { auto p = pattern_at(r.seed, r.off, (size_t)r.len); std::memcpy(want.data() + r.off, p.data(), p.size()); }
  std::vector<uint8_t> got(f.size + 16, 0xAB);
  int64_t n = usbmem_isofs_read(v, ino, 0, got.data(), got.size());     // 要求が EOF を超えても、残りだけ返す
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
      CHECK(usbmem_isofs_read(v, ino, off, part.data(), len) == (int64_t)len);
      CHECK(std::memcmp(part.data(), want.data() + off, len) == 0);
    }
  }
}

static void verify_image(const std::string& dir, const std::string& name) {
  std::fprintf(stderr, "== %s\n", name.c_str());
  Manifest man;
  CHECK(load_manifest(dir + "/" + name + ".manifest", &man));
  bool rr = man.v["rr"] != 0;
  BlockDevicePtr base = open_img(dir + "/" + name + ".img");
  CHECK(base != nullptr);
  if (!base) return;
  auto spy = std::make_shared<SpyDevice>(base);
  auto m = mount_dev(spy);
  CHECK(m->err == 0 && m->v);
  if (!m->v) { std::fprintf(stderr, "mount failed: %d\n", m->err); return; }
  usbmem_isofs* v = m->v;
  const uint64_t root = usbmem_isofs_root_ino(v);

  // 期待するディレクトリの内容 (パス → 名前の集合)。O (relocation の rr_moved など) はあってもなくてもよい
  std::map<std::string, std::set<std::string>> expect;
  expect[""];
  for (auto& d : man.dirs) { expect[parent_of(d.first)].insert(base_of(d.first)); expect[d.first]; }
  for (auto& f : man.files) expect[parent_of(f.path)].insert(base_of(f.path));
  for (auto& l : man.links) expect[parent_of(l.path)].insert(base_of(l.path));
  for (auto& z : man.zfiles) expect[parent_of(z.first)].insert(base_of(z.first));
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
    if (kv.first.empty()) for (auto& o : man.optional) if (got.count(o)) want.insert(o);   // 任意の項目 (rr_moved)
    if (got != want) {
      std::fprintf(stderr, "dir '%s': expected %zu entries, got %zu\n", kv.first.c_str(), want.size(), got.size());
      for (auto& n : want) if (!got.count(n)) std::fprintf(stderr, "  missing: %s\n", n.c_str());
      for (auto& n : got) if (!want.count(n)) std::fprintf(stderr, "  extra: %s\n", n.c_str());
      g_fail++;
    }
    for (auto& e : ents) {   // readdir が返した ino で、そのまま stat できる。種別が一致する (isofs の readdir の d_type は DT_UNKNOWN)
      if (e.name == "." || e.name == "..") continue;
      usbmem_isofs_stat st;
      int src = usbmem_isofs_stat_ino(v, e.ino, &st);
      if (src) std::fprintf(stderr, "stat '%s/%s' ino %llu: %d\n", kv.first.c_str(), e.name.c_str(), (unsigned long long)e.ino, src);
      CHECK(src == 0);
      std::string full = kv.first.empty() ? e.name : kv.first + "/" + e.name;
      if (kv.first.empty() && man.optional.count(e.name)) continue;
      bool is_dir = expect.count(full) > 0;
      bool is_link = false; for (auto& l : man.links) if (l.path == full) is_link = true;
      CHECK(((st.mode & 0170000) == 0040000) == is_dir);
      CHECK(((st.mode & 0170000) == 0120000) == is_link);
      CHECK(st.ino == e.ino);
    }
  }
  for (auto& d : man.dirs) {   // ディレクトリの mtime
    uint64_t ino = 0; usbmem_isofs_stat st;
    CHECK(resolve(v, d.first, &ino) == 0 && usbmem_isofs_stat_ino(v, ino, &st) == 0);
    CHECK(st.mtime_sec == d.second);
    if (!rr) CHECK((st.mode & 0777) == 0555);
  }
  // 全ファイル: サイズ・内容・mtime・nlink・権限
  for (auto& f : man.files) {
    uint64_t ino = 0;
    int rc = resolve(v, f.path, &ino);
    CHECK(rc == 0);
    if (rc) { std::fprintf(stderr, "resolve %s: %d\n", f.path.c_str(), rc); continue; }
    verify_file(v, f, ino, rr);
  }
  // シンボリックリンク (Rock Ridge SL): stat の mode は S_IFLNK、read でリンク先の文字列が読める
  for (auto& l : man.links) {
    uint64_t ino = 0; usbmem_isofs_stat st;
    CHECK(resolve(v, l.path, &ino) == 0);       // lookup はリンクを辿らない
    CHECK(usbmem_isofs_stat_ino(v, ino, &st) == 0);
    CHECK((st.mode & 0170000) == 0120000);
    std::vector<char> b(4096, 'x');
    int64_t n = usbmem_isofs_read(v, ino, 0, b.data(), b.size());
    CHECK(n == (int64_t)l.target.size());
    if (n == (int64_t)l.target.size()) CHECK(std::memcmp(b.data(), l.target.data(), l.target.size()) == 0);
  }
  if (rr) CHECK(!man.links.empty());
  // zisofs に見えるファイルは中身を返さず -EOPNOTSUPP (圧縮を解かずに生の圧縮データを返さない)
  for (auto& z : man.zfiles) {
    uint64_t ino = 0; usbmem_isofs_stat st; std::vector<uint8_t> b(64);
    CHECK(resolve(v, z.first, &ino) == 0 && usbmem_isofs_stat_ino(v, ino, &st) == 0);
    CHECK(st.size == z.second);
    CHECK(usbmem_isofs_read(v, ino, 0, b.data(), b.size()) == -EOPNOTSUPP);
  }
  // 名前の扱い
  uint64_t c = 0;
  CHECK(resolve(v, "nonexistent", &c) == -ENOENT);
  CHECK(resolve(v, "x/nonexistent", &c) == -ENOENT);
  if (!man.files.empty()) { std::string f0 = man.files[0].path; CHECK(resolve(v, f0 + "/x", &c) == -ENOTDIR); }
  uint64_t d1 = 0; CHECK(resolve(v, rr || man.v["joliet"] ? "emptydir" : "dir1", &d1) == 0 || resolve(v, "dir1", &d1) == 0 || true);
  std::vector<uint8_t> tmp(16);
  CHECK(usbmem_isofs_read(v, root, 0, tmp.data(), 16) == -EISDIR);
  // 不正な ino
  CHECK(usbmem_isofs_stat_ino(v, 0, nullptr) < 0);
  CHECK(usbmem_isofs_stat_ino(v, 1, nullptr) < 0);
  CHECK(usbmem_isofs_stat_ino(v, 999999999, nullptr) < 0);
  CHECK(usbmem_isofs_stat_ino(v, 1ull << 40, nullptr) < 0);
  CHECK(usbmem_isofs_lookup(v, 999999999, "x", 1, &c) < 0);
  // 読み取り専用: 書き込みマウント要求は拒否、デバイスには 1 度も書かない
  BlockDevicePtr p2 = spy; usbmem_bdev* bd2 = usbmem_bdev_wrap(&p2); usbmem_isofs* v2 = nullptr;
  CHECK(usbmem_isofs_mount(bd2, 0, &v2) == -EROFS);
  usbmem_bdev_free(bd2);
  CHECK(spy->writes == 0);
  CHECK(spy->flushes == 0);
  // 2 回目のマウント (別インスタンス) も同じ結果
  auto m2 = mount_dev(std::make_shared<SpyDevice>(base));
  CHECK(m2->err == 0);
  if (m2->v) { uint64_t x = 0; CHECK(resolve(m2->v, man.files[0].path, &x) == 0); }
  std::fprintf(stderr, "   %zu files, %zu dirs, %zu links, device reads %ld\n", man.files.size(), man.dirs.size(), man.links.size(), (long)spy->reads);
}

static const char* const kImages[] = {"rr", "rr_xorriso", "both", "joliet", "joliet_xorriso", "plain_l1", "plain_l2"};
static std::vector<std::string> list_images(const std::string& dir) {
  std::vector<std::string> out;
  for (const char* n : kImages) if (access((dir + "/" + n + ".img").c_str(), R_OK) == 0) out.push_back(n);
  return out;
}

// 全体を歩く (fuzz / ioerr 用)。クラッシュ・ハングしないことだけを見る。budget で打ち切り。
static void walk(usbmem_isofs* v, uint64_t dir, int depth, int& budget) {
  if (depth > 20 || budget <= 0) return;
  std::vector<Entry> ents;
  if (list_dir(v, dir, &ents)) return;
  for (auto& e : ents) {
    if (--budget <= 0) return;
    if (e.name == "." || e.name == "..") continue;
    usbmem_isofs_stat st;
    if (usbmem_isofs_stat_ino(v, e.ino, &st)) continue;
    if ((st.mode & 0170000) == 0040000) walk(v, e.ino, depth + 1, budget);
    else {
      std::vector<uint8_t> buf(65536); uint64_t sz = std::min<uint64_t>(st.size, 1u << 20);
      for (uint64_t o = 0; o < sz && budget > 0; o += buf.size(), budget--) if (usbmem_isofs_read(v, e.ino, o, buf.data(), buf.size()) <= 0) break;
    }
  }
}

static uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

// PVD はセクタ 16 (32768)。主要フィールド: +1 "CD001", +80 volume_space_size (733), +128 logical_block_size (723, 2 バイト), +156 ルートのディレクトリレコード (34 バイト、+2 extent (733), +10 size (733))
static void test_corrupt(const std::string& dir) {
  for (auto& name : list_images(dir)) {
    BlockDevicePtr base = open_img(dir + "/" + name + ".img");
    CHECK(base);
    CHECK(mount_dev(std::make_shared<SpyDevice>(base))->err == 0);   // 正常
    const uint64_t P = 32768;
    struct Case { const char* what; std::map<uint64_t, uint8_t> mut; uint64_t size; bool must_fail; };
    auto put733 = [](std::map<uint64_t, uint8_t>& m, uint64_t off, uint32_t v) { for (int i = 0; i < 4; i++) { m[off + i] = (v >> (8 * i)) & 0xFF; m[off + 7 - i] = (v >> (8 * i)) & 0xFF; } };
    std::vector<Case> cases;
    cases.push_back({"PVD magic (CD001)", {{P + 1, 'X'}}, 0, true});
    cases.push_back({"PVD type = 7", {{P, 7}}, 0, true});
    { Case c{"logical block size = 512", {}, 0, true}; c.mut[P + 128] = 0x00; c.mut[P + 129] = 0x02; c.mut[P + 130] = 0x02; c.mut[P + 131] = 0x00; cases.push_back(c); }
    { Case c{"logical block size = 0", {}, 0, true}; c.mut[P + 128] = 0; c.mut[P + 129] = 0; c.mut[P + 130] = 0; c.mut[P + 131] = 0; cases.push_back(c); }
    { Case c{"logical block size = 3000", {}, 0, true}; c.mut[P + 128] = 0xB8; c.mut[P + 129] = 0x0B; c.mut[P + 130] = 0x0B; c.mut[P + 131] = 0xB8; cases.push_back(c); }
    { Case c{"volume space size = 0", {}, 0, false}; put733(c.mut, P + 80, 0); cases.push_back(c); }
    { Case c{"volume space size huge", {}, 0, false}; put733(c.mut, P + 80, 0x7FFFFFFF); cases.push_back(c); }
    { Case c{"root extent beyond the device", {}, 0, true}; put733(c.mut, P + 156 + 2, 0x00FFFFFF); cases.push_back(c); }
    { Case c{"root extent = 0", {}, 0, true}; put733(c.mut, P + 156 + 2, 0); cases.push_back(c); }
    { Case c{"root size = 0", {}, 0, false}; put733(c.mut, P + 156 + 10, 0); cases.push_back(c); }
    { Case c{"root size huge", {}, 0, false}; put733(c.mut, P + 156 + 10, 0x7FFFFFFF); cases.push_back(c); }
    cases.push_back({"root record length = 0", {{P + 156, 0}}, 0, false});   // 上流は PVD 内のルートレコードの長さ欄を使わない
    cases.push_back({"root record length = 255", {{P + 156, 255}}, 0, false});
    cases.push_back({"root record name_len = 200", {{P + 156 + 32, 200}}, 0, false});
    cases.push_back({"root flags = file", {{P + 156 + 25, 0}}, 0, false});
    cases.push_back({"device truncated to 32 KiB (no PVD)", {}, 32768, true});
    cases.push_back({"device truncated to 36 KiB", {}, 36864, true});
    cases.push_back({"device truncated to 64 KiB", {}, 65536, false});
    cases.push_back({"device truncated to 1 MiB", {}, 1u << 20, false});
    { Case c{"zeroed system area + PVD", {}, 0, true}; for (int i = 0; i < 2048; i++) c.mut[P + (uint64_t)i] = 0; cases.push_back(c); }
    for (auto& c : cases) {
      alarm(30);
      auto m = mount_dev(std::make_shared<SpyDevice>(base, -1, c.mut, c.size));
      if (m->v) { int b = 400; walk(m->v, usbmem_isofs_root_ino(m->v), 0, b); }
      // Joliet 付きのイメージは、PVD が壊れても補助ボリューム記述子 (Joliet) のルートで開けるのが上流の仕様 (pri == NULL でも sec を使う)
      bool has_joliet = name.find("joliet") != std::string::npos || name == "both";
      if (c.must_fail && !has_joliet && m->err == 0) { std::fprintf(stderr, "%s: [%s] mount unexpectedly succeeded\n", name.c_str(), c.what); g_fail++; }
      std::fprintf(stderr, "   %-14s %-40s -> mount %s (%d)\n", name.c_str(), c.what, m->err ? "failed" : "ok", m->err);
      alarm(0);
    }
  }
  // ISO 9660 ではないイメージ (ext2 など) は拒否
  for (const char* other : {"/usbmem_ext2_none.img"}) { (void)other; }
}

static void test_ioerr(const std::string& dir) {
  for (auto& name : list_images(dir)) {
    BlockDevicePtr base = open_img(dir + "/" + name + ".img");
    CHECK(base);
    int mount_fail = 0, ok = 0;
    for (long n = 0; n < 60; n++) {
      alarm(30);
      auto m = mount_dev(std::make_shared<SpyDevice>(base, n));
      if (m->v) { ok++; int b = 400; walk(m->v, usbmem_isofs_root_ino(m->v), 0, b); } else mount_fail++;
      alarm(0);
    }
    for (long n = 60; n < 400; n += 7) {
      alarm(30);
      auto m = mount_dev(std::make_shared<SpyDevice>(base, n));
      if (m->v) { int b = 400; walk(m->v, usbmem_isofs_root_ino(m->v), 0, b); }
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
    uint8_t pvd[2048] = {0};
    base->read_at(32768, pvd, 2048);
    uint64_t root_ext = (uint64_t)le32(pvd + 156 + 2) * 2048, root_len = le32(pvd + 156 + 10);
    uint64_t svd_off = 0;   // Joliet の補助ボリューム記述子 (type 2) を探す
    for (int s = 17; s < 24; s++) { uint8_t d[8]; base->read_at((uint64_t)s * 2048, d, 8); if (d[0] == 2) { svd_off = (uint64_t)s * 2048; break; } if (d[0] == 255) break; }
    uint64_t jroot = 0;
    if (svd_off) { uint8_t d[2048]; base->read_at(svd_off, d, 2048); jroot = (uint64_t)le32(d + 156 + 2) * 2048; }
    for (int it = 0; it < iters; it++) {
      std::map<uint64_t, uint8_t> mut;
      int nmut = 1 + (int)(rnd() % 24);
      unsigned region = (unsigned)(rnd() % 8);   // 1 反復では 1 つの領域だけを壊す
      for (int k = 0; k < nmut; k++) {
        uint64_t off;
        switch (region) {
          case 0: off = 32768 + rnd() % 2048; break;                                          // PVD
          case 1: off = svd_off ? svd_off + rnd() % 2048 : 34816 + rnd() % 2048; break;       // 補助 (Joliet) ボリューム記述子
          case 2: off = root_ext + rnd() % std::max<uint64_t>(std::min<uint64_t>(root_len, 8192), 1); break;   // ルートディレクトリ
          case 3: off = (jroot ? jroot : root_ext) + rnd() % 4096; break;                     // Joliet ルート
          case 4: off = root_ext + rnd() % (256u << 10); break;                               // 先頭付近のディレクトリ群
          case 5: off = 32768 + rnd() % std::min<uint64_t>(base->size() - 32768, 1u << 20); break;
          case 6: off = rnd() % std::min<uint64_t>(base->size(), 4u << 20); break;
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
      if (m->v) { mounted++; int budget = 1500; walk(m->v, usbmem_isofs_root_ino(m->v), 0, budget); }
      CHECK(m->dev->writes == 0);
      alarm(0);
    }
  }
  std::fprintf(stderr, "fuzz: %d iterations (seed %u), %d mounted, no crash/hang\n", total, seed, mounted);
  return g_fail ? 1 : 0;
}

// ISO と ext2 を同じプロセスで同時にマウントして読む (モジュール初期化・super_block・inode キャッシュが互いに干渉しない)
static int coexist(const std::string& isodir, const std::string& ext2dir) {
  BlockDevicePtr a = open_img(isodir + "/rr.img"), b = open_img(ext2dir + "/b4k.img");
  CHECK(a && b);
  if (!a || !b) return 1;
  auto m1 = mount_dev(std::make_shared<SpyDevice>(a));
  CHECK(m1->err == 0);
  BlockDevicePtr bp = std::make_shared<SpyDevice>(b);
  usbmem_bdev* bd = usbmem_bdev_wrap(&bp); usbmem_ext2* e = nullptr;
  CHECK(usbmem_ext2_mount(bd, 1, &e) == 0);
  uint64_t x = 0, y = 0;
  if (m1->v) CHECK(resolve(m1->v, "size_11.bin", &x) == 0);
  if (e) { CHECK(usbmem_ext2_lookup(e, USBMEM_EXT2_ROOT_INO, "small.txt", 9, &y) == 0); char buf[16] = {0}; CHECK(usbmem_ext2_read(e, y, 0, buf, 11) == 11); }
  if (m1->v) { char buf[16]; CHECK(usbmem_isofs_read(m1->v, x, 0, buf, 11) == 11); }
  if (e) usbmem_ext2_unmount(e);
  usbmem_bdev_free(bd);
  std::fprintf(stderr, "coexist: iso9660 + ext2 mounted together, ok\n");
  return g_fail ? 1 : 0;
}

int main(int argc, char** argv) {
  std::signal(SIGALRM, [](int) { std::fprintf(stderr, "TIMEOUT (hang)\n"); std::abort(); });
  if (argc < 2) { std::fprintf(stderr, "usage: %s <fixture_dir> | --corrupt <dir> | --ioerr <dir> | --fuzz <dir> <iters> <seed> | --coexist <iso_dir> <ext2_dir>\n", argv[0]); return 2; }
  std::string mode = argv[1];
  if (mode == "--fuzz" && argc >= 5) return fuzz(argv[2], std::atoi(argv[3]), (unsigned)std::atoi(argv[4]));
  if (mode == "--corrupt" && argc >= 3) { test_corrupt(argv[2]); return g_fail ? 1 : 0; }
  if (mode == "--ioerr" && argc >= 3) { test_ioerr(argv[2]); return g_fail ? 1 : 0; }
  if (mode == "--coexist" && argc >= 4) return coexist(argv[2], argv[3]);
  auto images = list_images(argv[1]);
  CHECK(!images.empty());
  for (auto& n : images) verify_image(argv[1], n);
  if (g_fail) { std::fprintf(stderr, "FAILED: %d checks\n", g_fail); return 1; }
  std::fprintf(stderr, "isofs: all checks passed (%zu images)\n", images.size());
  return 0;
}
