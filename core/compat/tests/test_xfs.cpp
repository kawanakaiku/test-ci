// SPDX-License-Identifier: GPL-2.0-or-later
// XFS (core/compat/src/xfs_volume.cpp) の読み取りテスト。
// 使い方: usbmem_xfs_tests <fixture_dir>            tools/gen_xfs_fixture.py が作った *.img / *.manifest を全部検証
//         usbmem_xfs_tests --features <fixture_dir> スーパーブロックの機能ビット (未知の incompat / needsrepair / 外部ログなど) と、ログの未再生 (noload) の扱い
//         usbmem_xfs_tests --corrupt <fixture_dir>  壊れたボリュームが、クラッシュせず失敗する/読める範囲だけ読めること
//         usbmem_xfs_tests --ioerr <fixture_dir>    マウント・走査中のデバイス I/O エラーで、クラッシュせず失敗すること
//         usbmem_xfs_tests --fuzz <fixture_dir> <iterations> <seed>
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
#include "usbmem/xfs_volume.h"

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
  std::shared_ptr<SpyDevice> dev; std::unique_ptr<XfsVolume> vol; int err = 0;
};
static std::unique_ptr<Mounted> mount_dev(std::shared_ptr<SpyDevice> dev, bool noload = false) {
  auto m = std::make_unique<Mounted>();
  m->dev = dev;
  BlockDevicePtr p = dev;
  XfsMountOptions o; o.allow_noload = noload;
  m->vol = XfsVolume::mount(p, &m->err, o);
  if (!m->vol && m->err == 0) m->err = -1;
  return m;
}
static BlockDevicePtr open_img(const std::string& path) {
  BlockDevicePtr d;
  if (!RawImageDevice::open_path(path, true, 512, &d).ok()) return nullptr;
  return d;
}

static int resolve(FsVolume& v, const std::string& path, uint64_t* ino) { return v.resolve(path, ino); }

// manifest (tools/gen_xfs_fixture.py): D path mtime / F path size mtime nlink spec / L path target / S path kind / R path size mtime reffile
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

static void verify_file(FsVolume& v, const std::string& dir, const MFile& f, uint64_t ino) {
  FsStat st;
  CHECK(v.stat(ino, &st) == 0);
  CHECK(st.size == f.size);
  CHECK((st.mode & 0170000) == 0100000);
  CHECK(f.mtime < 0 || st.mtime == f.mtime);   // mtime -1 = 未検査 (mkfs.xfs の protofile は作成時刻を入れる)
  CHECK(st.nlink == f.nlink);
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
  auto m = mount_dev(spy);
  CHECK(m->err == 0 && m->vol);
  if (!m->vol) { std::fprintf(stderr, "mount failed: %d\n", m->err); return; }
  FsVolume& v = *m->vol;
  std::fprintf(stderr, "   %s\n", m->vol->describe_features().c_str());
  CHECK(std::string(v.fs_name()) == "xfs");
  CHECK(!v.writable());
  CHECK(!m->vol->journal_ignored());
  const bool ci = name.find("_ci") != std::string::npos;

  // 期待するディレクトリの内容 (パス → 名前の集合)
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
    CHECK(v.readdir(dino, &ents) == 0);
    std::set<std::string> got;
    for (auto& e : ents) got.insert(e.name);
    CHECK(got.size() == ents.size());      // 重複なし ("." / ".." は返さない)
    if (got != kv.second) {
      std::fprintf(stderr, "dir '%s': expected %zu entries, got %zu\n", kv.first.c_str(), kv.second.size(), got.size());
      int shown = 0;
      for (auto& n : kv.second) if (!got.count(n) && shown++ < 5) std::fprintf(stderr, "  missing: %s\n", n.c_str());
      for (auto& n : got) if (!kv.second.count(n) && shown++ < 10) std::fprintf(stderr, "  extra: %s\n", n.c_str());
      g_fail++;
    }
    for (auto& e : ents) {   // readdir が返した ino でそのまま stat できる。dirent の種別と inode の種別が一致する
      FsStat st;
      CHECK(v.stat(e.ino, &st) == 0);
      std::string full = kv.first.empty() ? e.name : kv.first + "/" + e.name;
      bool is_dir = expect.count(full) > 0;
      bool is_link = false; for (auto& l : man.links) if (l.path == full) is_link = true;
      CHECK(is_dir == (e.dtype == 4 /*DT_DIR*/));
      CHECK(is_link == (e.dtype == 10 /*DT_LNK*/));
      CHECK(((st.mode & 0170000) == 0040000) == is_dir);
      CHECK(((st.mode & 0170000) == 0120000) == is_link);
      CHECK(mode_to_dtype(st.mode) == e.dtype);
      if (is_dir) CHECK(st.nlink >= 2);
      uint64_t again = 0;
      CHECK(v.lookup(dino, e.name, &again) == 0 && again == e.ino);   // lookup と readdir の ino が一致
    }
  }
  for (auto& d : man.dirs) {   // ディレクトリの mtime
    uint64_t ino = 0; FsStat st;
    CHECK(resolve(v, d.first, &ino) == 0 && v.stat(ino, &st) == 0);
    if (d.second >= 0 && st.mtime != d.second) std::fprintf(stderr, "dir mtime %s: got %lld want %lld\n", d.first.c_str(), (long long)st.mtime, (long long)d.second);
    CHECK(d.second < 0 || st.mtime == d.second);
  }
  for (auto& f : man.files) {   // 全ファイル: サイズ・内容・mtime・nlink
    uint64_t ino = 0;
    int rc = resolve(v, f.path, &ino);
    CHECK(rc == 0);
    if (rc) { std::fprintf(stderr, "resolve %s: %d\n", f.path.c_str(), rc); continue; }
    verify_file(v, dir, f, ino);
  }
  // シンボリックリンク: stat の mode は S_IFLNK、read でも readlink でもリンク先の文字列
  for (auto& l : man.links) {
    uint64_t ino = 0; FsStat st;
    CHECK(resolve(v, l.path, &ino) == 0);              // lookup はリンクを辿らない (リンク自体の inode)
    CHECK(v.stat(ino, &st) == 0);
    CHECK((st.mode & 0170000) == 0120000);
    CHECK(st.size == l.target.size());
    std::vector<char> b(4096, 'x');
    int64_t n = v.read(ino, 0, b.data(), b.size());
    CHECK(n == (int64_t)l.target.size());
    if (n == (int64_t)l.target.size()) CHECK(std::memcmp(b.data(), l.target.data(), l.target.size()) == 0);
    std::string tgt; CHECK(v.readlink(ino, &tgt) == 0); CHECK(tgt == l.target);
  }
  CHECK(!man.links.empty());
  // 特殊ファイル: 種別が正しく、read は -EINVAL
  for (auto& s : man.specials) {
    uint64_t ino = 0; FsStat st;
    CHECK(resolve(v, s.path, &ino) == 0);
    CHECK(v.stat(ino, &st) == 0);
    uint32_t want = s.kind == 'c' ? 0020000 : s.kind == 'b' ? 0060000 : s.kind == 'p' ? 0010000 : 0140000;
    CHECK((st.mode & 0170000) == want);
    char b[8];
    CHECK(v.read(ino, 0, b, 8) == -EINVAL);
  }
  if (name == "special_proto") return;   // 小さなイメージ: 以降はフルツリー用の検査
  // 名前の扱い
  uint64_t a = 0, b = 0, c = 0;
  CHECK(resolve(v, "small.txt", &a) == 0);
  CHECK(resolve(v, "hardlink_to_small.txt", &b) == 0);
  CHECK(a == b);                                // ハードリンクは同じ inode
  if (!ci) CHECK(resolve(v, "SMALL.TXT", &c) == -ENOENT);
  CHECK(resolve(v, "日本語ファイル名.txt", &c) == 0);
  CHECK(resolve(v, std::string(255, 'n'), &c) == 0);
  CHECK(resolve(v, std::string(256, 'n'), &c) != 0);
  CHECK(resolve(v, "nonexistent", &c) == -ENOENT);
  CHECK(resolve(v, "d1/nonexistent", &c) == -ENOENT);
  CHECK(resolve(v, "small.txt/x", &c) == -ENOTDIR);
  uint64_t d1 = 0; CHECK(resolve(v, "d1", &d1) == 0);
  std::vector<uint8_t> tmp(16);
  CHECK(v.read(d1, 0, tmp.data(), 16) == -EISDIR);
  // "." と ".." の lookup
  uint64_t dot = 0, dotdot = 0;
  CHECK(v.lookup(d1, ".", &dot) == 0 && dot == d1);
  CHECK(v.lookup(d1, "..", &dotdot) == 0 && dotdot == v.root());
  // 不正な ino
  FsStat st0;
  CHECK(v.stat(0, &st0) < 0);
  CHECK(v.stat(999999999999ull, &st0) < 0);
  CHECK(v.stat(1ull << 60, &st0) < 0);
  CHECK(v.lookup(999999999999ull, "x", &c) < 0);
  // 書き込み系は常に EROFS
  uint64_t nin = 0;
  CHECK(v.create(v.root(), "x", 0644, &nin) == -EROFS);
  CHECK(v.mkdir(v.root(), "x", 0755, &nin) == -EROFS);
  CHECK(v.unlink(v.root(), "small.txt") == -EROFS);
  CHECK(v.write(a, 0, "x", 1) == -EROFS);
  CHECK(v.truncate(a, 0) == -EROFS);
  CHECK(spy->writes == 0);
  CHECK(spy->flushes == 0);
  // 2 回目のマウント (別インスタンス・同時) も同じ結果
  auto m2 = mount_dev(std::make_shared<SpyDevice>(base));
  CHECK(m2->err == 0);
  if (m2->vol) { uint64_t x = 0; CHECK(resolve(*m2->vol, "d1/d2/d3/d4/d5/d6/d7/d8/leaf.txt", &x) == 0); }
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
static void walk(FsVolume& v, uint64_t dir, int depth, int& budget) {
  if (depth > 12 || budget <= 0) return;
  std::vector<FsDirEntry> ents;
  if (v.readdir(dir, &ents)) return;
  for (auto& e : ents) {
    if (--budget <= 0) return;
    FsStat st;
    if (v.stat(e.ino, &st)) continue;
    uint32_t t = st.mode & 0170000;
    if (t == 0040000) { if (e.ino != dir) walk(v, e.ino, depth + 1, budget); }
    else if (t == 0100000 || t == 0120000) {
      std::vector<uint8_t> buf(65536);
      uint64_t sz = std::min<uint64_t>(st.size, 1u << 20);
      for (uint64_t o = 0; o < sz && budget > 0; o += buf.size(), budget--) if (v.read(e.ino, o, buf.data(), buf.size()) <= 0) break;
      if (t == 0120000) { std::string tg; v.readlink(e.ino, &tg); }
    }
  }
}

static uint32_t crc32c_raw(uint32_t c, const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0x82F63B78u & (0u - (c & 1))); }
  return c;
}
static uint32_t rd32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
static uint64_t rd64(const uint8_t* p) { return (uint64_t(rd32(p)) << 32) | rd32(p + 4); }
struct SbInfo { uint32_t bs, agblocks, agcount, sectsize, inodesize, logblocks; uint8_t agblklog, inopblog, dirblklog; uint64_t logstart, rootino, dblocks; bool v5; };
static bool read_sb(BlockDevicePtr base, SbInfo* s, std::vector<uint8_t>* sector) {
  std::vector<uint8_t> b(512);
  if (!base->read_at(0, b.data(), 512).ok()) return false;
  s->bs = rd32(&b[4]); s->dblocks = rd64(&b[8]); s->logstart = rd64(&b[48]); s->rootino = rd64(&b[56]);
  s->agblocks = rd32(&b[84]); s->agcount = rd32(&b[88]); s->logblocks = rd32(&b[96]);
  s->v5 = (b[100] * 256 + b[101]) % 16 == 5;
  s->sectsize = (b[102] << 8) | b[103]; s->inodesize = (b[104] << 8) | b[105];
  s->inopblog = b[123]; s->agblklog = b[124]; s->dirblklog = b[192];
  if (sector) { sector->assign(s->sectsize, 0); base->read_at(0, sector->data(), s->sectsize); }
  return true;
}
// スーパーブロックのバイトを書き換え、v5 なら CRC を計算し直した mutation を返す
static std::map<uint64_t, uint8_t> sb_edit(BlockDevicePtr base, const std::map<uint64_t, uint8_t>& edits, bool fix_crc = true) {
  SbInfo s; std::vector<uint8_t> sec;
  read_sb(base, &s, &sec);
  for (auto& kv : edits) sec[kv.first] = kv.second;
  if (s.v5 && fix_crc) {
    uint8_t zero[4] = {0, 0, 0, 0};
    uint32_t c = 0xFFFFFFFFu;
    c = crc32c_raw(c, sec.data(), 224); c = crc32c_raw(c, zero, 4); c = crc32c_raw(c, sec.data() + 228, sec.size() - 228);
    c = ~c;
    for (int i = 0; i < 4; i++) sec[224 + i] = (uint8_t)(c >> (8 * i));
  }
  std::map<uint64_t, uint8_t> m;
  for (size_t i = 0; i < sec.size(); i++) m[i] = sec[i];
  return m;
}
static std::map<uint64_t, uint8_t> Q(uint64_t off, uint64_t val, int n) { std::map<uint64_t, uint8_t> m; for (int i = 0; i < n; i++) m[off + (uint64_t)i] = (uint8_t)(val >> (8 * (n - 1 - i))); return m; }   // ビッグエンディアン
static std::map<uint64_t, uint8_t> merge(std::map<uint64_t, uint8_t> a, const std::map<uint64_t, uint8_t>& b) { a.insert(b.begin(), b.end()); return a; }

// スーパーブロックの機能ビットとログ
static void test_features(const std::string& dir) {
  int n = 0;
  for (auto& name : list_images(dir)) {
    if (name != "v5_default" && name != "v4_ftype1") continue;
    BlockDevicePtr base = open_img(dir + "/" + name + ".img");
    CHECK(base);
    SbInfo s; read_sb(base, &s, nullptr);
    struct Case { const char* what; std::map<uint64_t, uint8_t> edits; bool v5_only; int expect_err; };   // expect_err: 0 = マウントできる / それ以外 = そのエラー (正の errno)
    std::vector<Case> cases = {
      {"unknown incompat bit 31", Q(216, 0x80000000ull | 0xeb, 4), true, EOPNOTSUPP},
      {"NEEDSREPAIR", Q(216, 0xeb | 0x10, 4), true, EUCLEAN},
      {"ZONED", Q(216, 0xeb | 0x200, 4), true, EOPNOTSUPP},
      {"unknown ro_compat bit (read-only mount is fine)", Q(212, 0xf | 0x100, 4), true, 0},
      {"unknown compat bit", Q(208, 0x1, 4), true, 0},
      {"unknown log_incompat bit", Q(220, 0x80, 4), true, 0},
      {"external log (logstart = 0)", Q(48, 0, 8), false, EOPNOTSUPP},
      {"realtime blocks != 0", Q(16, 1000, 8), false, EOPNOTSUPP},
      {"realtime extents != 0", Q(24, 1000, 8), false, EOPNOTSUPP},
      {"inprogress = 1", Q(126, 1, 1), false, EUCLEAN},
      {"blocksize 3000", Q(4, 3000, 4), false, EINVAL},
      {"blocksize 0", Q(4, 0, 4), false, EINVAL},
      {"agcount 0", Q(88, 0, 4), false, EINVAL},
      {"agblocks 0", Q(84, 0, 4), false, EINVAL},
      {"dblocks beyond device", Q(8, 0x7fffffffull, 8), false, EINVAL},
      {"inodesize 100", Q(104, 100, 2), false, EINVAL},
      {"sectsize 64", Q(102, 64, 2), false, EINVAL},
      {"dirblklog 15", Q(192, 15, 1), false, EINVAL},
      {"versionnum = 6", Q(100, 6, 2), false, EOPNOTSUPP},
      {"rootino -> free/nonexistent inode", Q(56, 0x7fffff, 8), false, EUCLEAN},
      {"logblocks 0", Q(96, 0, 4), false, EUCLEAN},
    };
    for (auto& c : cases) {
      if (c.v5_only && !s.v5) continue;
      if (!c.v5_only && s.v5 && std::string(c.what).find("versionnum") != std::string::npos) {}
      auto mut = sb_edit(base, c.edits);
      auto m = mount_dev(std::make_shared<SpyDevice>(base, -1, mut));
      bool ok = c.expect_err == 0 ? (m->err == 0) : (m->err == c.expect_err);
      if (m->vol) { int b = 300; walk(*m->vol, m->vol->root(), 0, b); }
      std::fprintf(stderr, "   %-10s %-48s -> mount %s (%d)%s\n", name.c_str(), c.what, m->err ? "failed" : "ok", m->err, ok ? "" : "   <== UNEXPECTED");
      if (!ok) g_fail++;
      n++;
    }
    // v5: メタデータが 1 バイト壊れていて CRC が合わない → スーパーブロック CRC で拒否
    if (s.v5) {
      // uuid の 1 バイトを必ず元と違う値にする (固定値 0xAB だと、フィクスチャの uuid がたまたま 0xAB のとき何も変わらず 1/256 で落ちていた)
      SbInfo s2; std::vector<uint8_t> sec2; read_sb(base, &s2, &sec2);
      auto mut = sb_edit(base, Q(32, (uint8_t)(sec2[32] ^ 0xFF), 1), false);   // CRC は計算し直さない
      auto m = mount_dev(std::make_shared<SpyDevice>(base, -1, mut));
      CHECK(m->err == EUCLEAN);
      std::fprintf(stderr, "   %-10s %-48s -> mount %s (%d)\n", name.c_str(), "bad superblock crc", m->err ? "failed" : "ok", m->err);
      n++;
    }
    // ルート inode が壊れている (v5: CRC で EBADMSG)
    {
      uint64_t shift = s.agblklog + s.inopblog;
      uint64_t agno = s.rootino >> shift, agbno = (s.rootino >> s.inopblog) & ((1ull << s.agblklog) - 1), slot = s.rootino & ((1ull << s.inopblog) - 1);
      uint64_t off = (agno * s.agblocks + agbno) * s.bs + slot * s.inodesize;
      std::map<uint64_t, uint8_t> mut; mut[off + 60] = 0x55;   // di_size
      auto m = mount_dev(std::make_shared<SpyDevice>(base, -1, mut));
      std::fprintf(stderr, "   %-10s %-48s -> mount %s (%d)\n", name.c_str(), "root inode damaged", m->err ? "failed" : "ok", m->err);
      if (s.v5) CHECK(m->err == EBADMSG); else CHECK(m->err == 0 || m->err == EUCLEAN);
      n++;
    }
    // ログ: クリーン → 通常どおりマウントできる (journal_ignored は false)。未再生に見える状態 → EUCLEAN。noload なら読み取り専用で開ける
    {
      uint64_t agno = s.logstart >> s.agblklog, agbno = s.logstart & ((1ull << s.agblklog) - 1);
      uint64_t logoff = (agno * s.agblocks + agbno) * s.bs;
      auto clean = mount_dev(std::make_shared<SpyDevice>(base));
      CHECK(clean->err == 0 && clean->vol && !clean->vol->journal_ignored());
      // mkfs 直後のログ: ブロック 0 = レコードヘッダ、ブロック 1 = アンマウントレコード (oh_flags = 0x20)。フラグを消すと「未コミットの更新がある」状態に見える
      std::map<uint64_t, uint8_t> dirty; dirty[logoff + 512 + 9] = 0x00;
      auto d1 = mount_dev(std::make_shared<SpyDevice>(base, -1, dirty));
      std::fprintf(stderr, "   %-10s %-48s -> mount %s (%d)\n", name.c_str(), "log not cleanly unmounted", d1->err ? "failed" : "ok", d1->err);
      CHECK(d1->err == EUCLEAN);
      auto d2 = mount_dev(std::make_shared<SpyDevice>(base, -1, dirty), true);
      CHECK(d2->err == 0 && d2->vol && d2->vol->journal_ignored());
      if (d2->vol) {
        FsStat st; uint64_t ino = 0;
        CHECK(d2->vol->resolve("small.txt", &ino) == 0 && d2->vol->stat(ino, &st) == 0 && st.size == 11);
        CHECK(!d2->vol->writable());
        CHECK(d2->vol->create(d2->vol->root(), "x", 0644, &ino) == -EROFS);
      }
      CHECK(d2->dev->writes == 0);
      // 違うサイクルのブロックが混ざった (書き込み中に切れた) ログも、クリーンとは判定されない
      std::map<uint64_t, uint8_t> torn = dirty;
      torn[logoff + 512 * 5 + 3] = 0x07;   // 周期 0 のはずのブロックを非 0 に
      auto d3 = mount_dev(std::make_shared<SpyDevice>(base, -1, torn));
      CHECK(d3->err == EUCLEAN);
      // レコードヘッダの破壊 → ヘッダが見つからない → 判定不能は未再生扱い
      std::map<uint64_t, uint8_t> nohdr; nohdr[logoff] = 0x11;   // magic の先頭を壊す (周期として 0 にならない値に)
      auto d4 = mount_dev(std::make_shared<SpyDevice>(base, -1, nohdr));
      CHECK(d4->err == EUCLEAN);
      n += 4;
    }
  }
  std::fprintf(stderr, "features: %d cases\n", n);
}

static void test_corrupt(const std::string& dir) {
  int n_cases = 0;
  for (auto& name : list_images(dir)) {
    if (name != "v5_default" && name != "v4_ftype1" && name != "v5_bs1k") continue;
    BlockDevicePtr base = open_img(dir + "/" + name + ".img");
    CHECK(base);
    SbInfo s; read_sb(base, &s, nullptr);
    CHECK(mount_dev(std::make_shared<SpyDevice>(base))->err == 0);   // 正常
    struct Case { const char* what; std::map<uint64_t, uint8_t> mut; uint64_t size; bool must_fail; };
    std::vector<Case> cases = {
      {"magic", Q(0, 0x12345678, 4), 0, true},
      {"zeroed superblock sector", [&]() { std::map<uint64_t, uint8_t> m; for (int i = 0; i < 512; i++) m[(uint64_t)i] = 0; return m; }(), 0, true},
      {"device truncated to 512 B", {}, 512, true},
      {"device truncated to 4 KiB", {}, 4096, true},
      {"device truncated to 1 MiB", {}, 1u << 20, true},
      {"device truncated to 1/2", {}, base->size() / 2, true},
    };
    for (auto& c : cases) {
      alarm(30);
      auto m = mount_dev(std::make_shared<SpyDevice>(base, -1, c.mut, c.size));
      if (m->vol) { int b = 400; walk(*m->vol, m->vol->root(), 0, b); }
      if (c.must_fail && m->err == 0) { std::fprintf(stderr, "%s: [%s] mount unexpectedly succeeded\n", name.c_str(), c.what); g_fail++; }
      std::fprintf(stderr, "   %-10s %-40s -> mount %s (%d)\n", name.c_str(), c.what, m->err ? "failed" : "ok", m->err);
      alarm(0);
      n_cases++;
    }
    // 各 AG の先頭 2 MiB (AG ヘッダ・inode チャンク・ディレクトリなど) を壊して全体を歩く: クラッシュ・ハングしない
    for (uint32_t ag = 0; ag < std::min<uint32_t>(s.agcount, 4); ag++) {
      std::map<uint64_t, uint8_t> mut;
      uint64_t o0 = (uint64_t)ag * s.agblocks * s.bs + (ag ? 0 : s.sectsize * 4);
      for (uint64_t o = o0; o < o0 + 65536; o++) mut[o] = (uint8_t)(0xFF - (o & 3));
      alarm(60);
      auto m = mount_dev(std::make_shared<SpyDevice>(base, -1, mut));
      if (m->vol) { int b = 3000; walk(*m->vol, m->vol->root(), 0, b); }
      std::fprintf(stderr, "   %-10s damaged AG%u first 64 KiB -> mount %s (%d), no crash\n", name.c_str(), ag, m->err ? "failed" : "ok", m->err);
      alarm(0);
      n_cases++;
    }
  }
  std::fprintf(stderr, "corrupt: %d cases, no crash/hang\n", n_cases);
}

static void test_ioerr(const std::string& dir) {
  for (auto& name : list_images(dir)) {
    if (name != "v5_default" && name != "v4_ftype0") continue;
    BlockDevicePtr base = open_img(dir + "/" + name + ".img");
    CHECK(base);
    int mount_fail = 0, ok = 0;
    for (long n = 0; n < 40; n++) {
      alarm(30);
      auto m = mount_dev(std::make_shared<SpyDevice>(base, n));
      if (m->vol) { ok++; int b = 400; walk(*m->vol, m->vol->root(), 0, b); } else mount_fail++;
      alarm(0);
    }
    for (long n = 40; n < 600; n += 9) {
      alarm(30);
      auto m = mount_dev(std::make_shared<SpyDevice>(base, n));
      if (m->vol) { int b = 600; walk(*m->vol, m->vol->root(), 0, b); }
      alarm(0);
    }
    std::fprintf(stderr, "   %s: read-failure injection: mount failed %d, mounted-then-failed %d (no crash)\n", name.c_str(), mount_fail, ok);
    CHECK(mount_fail > 0);
  }
}

static int fuzz(const std::string& dir, int iters, unsigned seed) {
  std::vector<std::string> images;
  for (auto& n : list_images(dir)) if (n == "v5_default" || n == "v5_bs1k" || n == "v4_bs512" || n == "v5_bs64k" || n == "v5_dir8k" || n == "v4_ftype1" || n == "v4_ftype0" || n == "v5_old_features" || n == "v5_isize1k") images.push_back(n);
  uint64_t x = seed * 2654435761u + 1;
  auto rnd = [&]() { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
  int mounted = 0, total = 0;
  for (auto& name : images) {
    BlockDevicePtr base = open_img(dir + "/" + name + ".img");
    if (!base) { g_fail++; continue; }
    SbInfo s; read_sb(base, &s, nullptr);
    uint64_t imgsz = base->size();
    for (int it = 0; it < iters; it++) {
      std::map<uint64_t, uint8_t> mut;
      int nmut = 1 + (int)(rnd() % 24);
      unsigned region = (unsigned)(rnd() % 5);   // 1 反復では 1 つの領域だけを壊す
      uint32_t ag = (uint32_t)(rnd() % s.agcount);
      for (int k = 0; k < nmut; k++) {
        uint64_t off;
        switch (region) {
          case 0: off = 8 + rnd() % (s.sectsize - 8); break;                                        // スーパーブロック (magic 以外)
          case 1: off = (uint64_t)ag * s.agblocks * s.bs + rnd() % (64u << 10); break;              // AG の先頭 64 KiB (AG ヘッダ・最初の inode チャンクなど)
          case 2: off = (uint64_t)ag * s.agblocks * s.bs + rnd() % (1u << 20); break;               // AG の先頭 1 MiB
          case 3: off = rnd() % std::min<uint64_t>(imgsz, 4u << 20); break;                         // 先頭 4 MiB のどこか
          default: off = rnd() % imgsz; break;
        }
        mut[off] = (uint8_t)(rnd() % 3 == 0 ? 0xFF : rnd());
      }
      if (const char* lp = std::getenv("USBMEM_FUZZ_LAST")) {
        if (FILE* lf = std::fopen(lp, "w")) { std::fprintf(lf, "%s\n", name.c_str()); for (auto& kv : mut) std::fprintf(lf, "%llu %u\n", (unsigned long long)kv.first, kv.second); std::fclose(lf); }
      }
      alarm(60);   // ハング検出
      bool noload = (it & 1) != 0;   // 半分は noload (ログの判定を飛ばして奥のコードに届かせる)
      auto m = mount_dev(std::make_shared<SpyDevice>(base, -1, mut), noload);
      total++;
      if (m->vol) { mounted++; int budget = 1500; walk(*m->vol, m->vol->root(), 0, budget); }
      CHECK(m->dev->writes == 0);
      alarm(0);
    }
  }
  struct rusage ru; getrusage(RUSAGE_SELF, &ru);
  std::fprintf(stderr, "fuzz: %d iterations (seed %u), %d mounted, no crash/hang, max RSS %ld MiB\n", total, seed, mounted, (long)(ru.ru_maxrss / 1024));
  return g_fail ? 1 : 0;
}

int main(int argc, char** argv) {
  std::signal(SIGALRM, [](int) { std::fprintf(stderr, "TIMEOUT (hang)\n"); std::abort(); });
  if (argc < 2) { std::fprintf(stderr, "usage: %s <fixture_dir> | --features <dir> | --corrupt <dir> | --ioerr <dir> | --fuzz <dir> <iters> <seed>\n", argv[0]); return 2; }
  std::string mode = argv[1];
  if (mode == "--fuzz" && argc >= 5) return fuzz(argv[2], std::atoi(argv[3]), (unsigned)std::atoi(argv[4]));
  if (mode == "--features" && argc >= 3) { test_features(argv[2]); return g_fail ? 1 : 0; }
  if (mode == "--corrupt" && argc >= 3) { test_corrupt(argv[2]); return g_fail ? 1 : 0; }
  if (mode == "--ioerr" && argc >= 3) { test_ioerr(argv[2]); return g_fail ? 1 : 0; }
  auto images = list_images(argv[1]);
  CHECK(!images.empty());
  for (auto& n : images) verify_image(argv[1], n);
  if (g_fail) { std::fprintf(stderr, "FAILED: %d checks\n", g_fail); return 1; }
  std::fprintf(stderr, "xfs: all checks passed (%zu images)\n", images.size());
  return 0;
}
