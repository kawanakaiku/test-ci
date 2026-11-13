// SPDX-License-Identifier: GPL-2.0-or-later
// FAT12/16/32 (vfat) 書き込みのテスト (USBMEM_NTFS_WRITE=ON ビルド用。docs/fat-write.md)。独立実装 (fsck.fat / mtools / 7z / カーネルの vfat) との突き合わせはシェル側
// (tools/run_fat_write_tests.sh, run_fat_crash_tests.sh, run_fat_kernel_tests.sh)。
//   --ops <img> <script>               書き込みマウントで操作列を実行 → sync → unmount。環境変数 USBMEM_CRASH_AT/MODE/SEED/TORN で電源断を模擬。
//   --model <img> <seed> <nops> [dump] [small]  乱数で操作列を作り、メモリ上のモデルと突き合わせる (途中・unmount 後の再マウント後にも全体比較)。dump に tools/ops_ref.py --tree と同じ書式でモデルを書く
//   --noop <img>                       rw マウントして何もせず unmount: イメージが 1 バイトも変わらないこと
//   --refuse <img> <variant>           boot-dirty / boot-media / fat-unclean のイメージ: マウントは成功し writable()==false、書き込みは -EROFS、イメージ不変
//   --enospc <img>                     満杯・ディレクトリの伸長不能 (FAT12/16 は固定長のルート) で安全に失敗し、削除で回復すること
//   --names <img>                      名前の規則 (大文字小文字を区別しない / 禁止文字 / 長さ / UTF-16 サロゲート / 8.3 名の衝突 / 大文字小文字だけの rename)
//   --limits <img>                     4GiB-1 を超えるサイズは -EFBIG で、何も変えない
//   --clean <img>                      ダーティ印の確認 (最初の変更で立つ: ブートセクタ + FAT[1]、FSInfo は未知。クリーンな unmount で全部戻り FSInfo が正しい)
//   --tree <img> <out>                 自前ドライバ (読み取りマウント) でイメージを読み、tools/ops_ref.py --tree と同じ書式で全エントリを書く (カーネルの vfat が書いたイメージを自前ドライバで読む向き: tools/run_fat_kernel_tests.sh)
//   --fuzz <img> <seed> <iters>        メタデータ領域を壊したイメージに対し、rw マウントして乱数操作 (クラッシュ/ハングしないこと。ASan/UBSan 下で使う)
// script の各行: mkdir P | create P | write P OFF LEN SEED | truncate P SIZE | unlink P | rmdir P | rename A B
//   write の内容: byte i = ((OFF+i)*7+SEED)&255。
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "crash_device.h"
#include "sha256.h"
#include "usbmem/devices.h"
#include "usbmem/fat_volume.h"

using namespace usbmem;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static std::string slurp_hash(const std::string& path) {
  std::ifstream f(path, std::ios::binary); Sha256 h; std::vector<char> b(1 << 16);
  while (f) { f.read(b.data(), b.size()); h.update(reinterpret_cast<uint8_t*>(b.data()), (size_t)f.gcount()); }
  return h.hex();
}

static BlockDevicePtr open_rw(const std::string& img) {
  BlockDevicePtr d;
  // 論理ブロックサイズ: 4Kn のフィクスチャでも 512 で開いてよい (FAT ドライバがブートセクタの bytes/sector を使う)
  if (!RawImageDevice::open_path(img, false, 512, &d).ok()) return nullptr;
  // USBMEM_IMG_OFFSET=<バイト>: イメージの途中からを FAT ボリュームとして開く (パーティションのオフセット。前後の領域は 1 バイトも変わらないこと = シェル側で検査)
  if (const char* o = std::getenv("USBMEM_IMG_OFFSET")) {
    uint64_t off = std::strtoull(o, nullptr, 10), tail = std::getenv("USBMEM_IMG_TAIL") ? std::strtoull(std::getenv("USBMEM_IMG_TAIL"), nullptr, 10) : 0;
    BlockDevicePtr sub;
    if (off + tail >= d->size() || !SubRangeDevice::create(d, off, d->size() - off - tail, false, &sub).ok()) return nullptr;
    return sub;
  }
  return d;
}
static std::vector<uint8_t> fill(uint64_t off, size_t len, unsigned seed) {
  std::vector<uint8_t> b(len);
  for (size_t i = 0; i < len; i++) b[i] = static_cast<uint8_t>((((off + i) * 7) + seed) & 255);
  return b;
}
static void split_path(const std::string& p, std::string* dir, std::string* name) {
  std::string q = p; while (!q.empty() && q[0] == '/') q.erase(0, 1);
  size_t k = q.rfind('/');
  if (k == std::string::npos) { *dir = ""; *name = q; } else { *dir = q.substr(0, k); *name = q.substr(k + 1); }
}

// ---- ブートセクタの解析 (テスト側は独立に読む)
struct Geo {
  bool ok = false; int type = 0;   // 12/16/32
  uint32_t bps = 0, spc = 0, rsvd = 0, nfats = 0, root_entries = 0, fatsz = 0, fsinfo = 0, clusters = 0;
  uint64_t total = 0;
  uint64_t fat_off(int i) const { return (uint64_t)(rsvd + (uint32_t)i * fatsz) * bps; }
  uint64_t state_off() const { return type == 32 ? 0x41 : 0x25; }
};
static unsigned rd16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static Geo geo_of(const std::string& img) {
  Geo g; std::ifstream f(img, std::ios::binary); uint8_t b[512];
  f.read((char*)b, 512); if (f.gcount() != 512) return g;
  g.bps = rd16(b + 11); g.spc = b[13]; g.rsvd = rd16(b + 14); g.nfats = b[16]; g.root_entries = rd16(b + 17);
  uint32_t ts = rd16(b + 19) ? rd16(b + 19) : rd32(b + 32);
  g.fatsz = rd16(b + 22) ? rd16(b + 22) : rd32(b + 36);
  g.fsinfo = rd16(b + 48);
  if (!g.bps || !g.spc || !g.nfats || !g.fatsz) return g;
  uint32_t rootsec = (g.root_entries * 32 + g.bps - 1) / g.bps;
  uint32_t data = ts - g.rsvd - g.nfats * g.fatsz - rootsec;
  g.clusters = data / g.spc; g.total = ts;
  g.type = rd16(b + 22) == 0 ? 32 : (g.clusters < 4085 ? 12 : 16);
  g.ok = true; return g;
}
static std::vector<uint8_t> rd_bytes(const std::string& img, uint64_t off, size_t n) {
  std::ifstream f(img, std::ios::binary); std::vector<uint8_t> b(n); f.seekg((std::streamoff)off); f.read((char*)b.data(), n); return b;
}
static void wr_bytes(const std::string& img, uint64_t off, const std::vector<uint8_t>& v) {
  std::fstream f(img, std::ios::in | std::ios::out | std::ios::binary); f.seekp((std::streamoff)off); f.write((const char*)v.data(), v.size());
}

// ---- --ops
static int ops_run(const std::string& img, const std::string& script) {
  BlockDevicePtr dev = open_rw(img);
  if (!dev) return 2;
  std::shared_ptr<CrashDevice> crash;
  if (const char* cl = std::getenv("USBMEM_CRASH_AT")) {
    const char* md = std::getenv("USBMEM_CRASH_MODE"); const char* sd = std::getenv("USBMEM_CRASH_SEED"); const char* tn = std::getenv("USBMEM_CRASH_TORN");
    crash = std::make_shared<CrashDevice>(dev, std::atol(cl), md && std::string(md) == "cache", tn && *tn == '1', sd ? (unsigned)std::atoi(sd) : 1);
    dev = crash;
  }
  int err = 0; auto vol = FatVolume::mount_rw(dev, &err);
  if (!vol) { std::fprintf(stderr, "mount_rw failed: errno %d\n", err); return 1; }
  if (!vol->writable()) { std::fprintf(stderr, "not writable\n"); return 3; }
  auto dir_ino = [&](const std::string& d, uint64_t* ino) { if (d.empty()) { *ino = vol->root(); return 0; } return vol->resolve(d, ino); };
  std::ifstream in(script); std::string line; int n = 0, bad = 0;
  const bool sync_each = std::getenv("USBMEM_SYNC_EVERY") != nullptr;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream is(line); std::string op, a, b;
    is >> op >> a;
    int rc = 0; std::string d, nm; uint64_t di = 0, ino = 0;
    if (op == "mkdir" || op == "create" || op == "unlink" || op == "rmdir") {
      split_path(a, &d, &nm); rc = dir_ino(d, &di);
      if (!rc) rc = op == "mkdir" ? vol->mkdir(di, nm, 0755, &ino) : op == "create" ? vol->create(di, nm, 0644, &ino) : op == "unlink" ? vol->unlink(di, nm) : vol->rmdir(di, nm);
    } else if (op == "truncate") {
      uint64_t sz; is >> sz; rc = vol->resolve(a, &ino); if (!rc) rc = vol->truncate(ino, sz);
    } else if (op == "write") {
      uint64_t off, len; unsigned seed; is >> off >> len >> seed;
      rc = vol->resolve(a, &ino);
      if (!rc) { auto buf = fill(off, len, seed); int64_t w = vol->write(ino, off, buf.data(), buf.size()); rc = w == (int64_t)len ? 0 : (w < 0 ? (int)w : -EIO); }
    } else if (op == "rename") {
      is >> b; std::string d2, n2; uint64_t di2 = 0; split_path(a, &d, &nm); split_path(b, &d2, &n2);
      rc = dir_ino(d, &di); if (!rc) rc = dir_ino(d2, &di2); if (!rc) rc = vol->rename(di, nm, di2, n2);
    } else { std::fprintf(stderr, "bad op: %s\n", line.c_str()); return 2; }
    n++; if (rc) { bad++; std::fprintf(stderr, "op failed (%d): %s\n", rc, line.c_str()); }
    if (sync_each) vol->sync();   // Android (SAF) は 1 操作ごとに同期する使い方に近い
  }
  int rc = vol->sync(); if (rc && !crash) { std::fprintf(stderr, "sync -> %d\n", rc); return 1; }
  vol.reset();
  std::fprintf(stderr, "ops: %d run, %d failed\n", n, bad);
  if (crash) std::fprintf(stderr, "writes: %ld\n", crash->count());
  return (bad && !crash) ? 1 : 0;
}

// ---- --model: メモリ上のモデルとの突き合わせ。FAT は名前の大文字小文字を区別しないので、キーは ASCII の小文字化
static std::string fold(const std::string& s) { std::string r = s; for (auto& c : r) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); return r; }
struct MNode {
  enum Kind { File, Dir } kind = File;
  std::vector<uint8_t> data;                                           // File
  std::map<std::string, std::pair<std::string, std::shared_ptr<MNode>>> kids;  // Dir: fold(name) -> (実際の名前, ノード)
};
using MP = std::shared_ptr<MNode>;
struct Model {
  MP root = std::make_shared<MNode>();
  Model() { root->kind = MNode::Dir; }
  static std::vector<std::string> parts(const std::string& p) {
    std::vector<std::string> v; std::stringstream ss(p); std::string t;
    while (std::getline(ss, t, '/')) if (!t.empty()) v.push_back(t);
    return v;
  }
  MP find(const std::string& p) {
    MP cur = root;
    for (auto& s : parts(p)) { if (cur->kind != MNode::Dir) return nullptr; auto it = cur->kids.find(fold(s)); if (it == cur->kids.end()) return nullptr; cur = it->second.second; }
    return cur;
  }
  MP parent(const std::string& p, std::string* name) {
    auto v = parts(p); if (v.empty()) return nullptr;
    *name = v.back(); v.pop_back();
    MP cur = root;
    for (auto& s : v) { if (cur->kind != MNode::Dir) return nullptr; auto it = cur->kids.find(fold(s)); if (it == cur->kids.end()) return nullptr; cur = it->second.second; }
    return cur->kind == MNode::Dir ? cur : nullptr;
  }
  void walk(const MP& n, const std::string& path, std::vector<std::pair<std::string, MP>>* out) {
    for (auto& kv : n->kids) { std::string p = path.empty() ? kv.second.first : path + "/" + kv.second.first; out->push_back({p, kv.second.second}); if (kv.second.second->kind == MNode::Dir) walk(kv.second.second, p, out); }
  }
  std::vector<std::pair<std::string, MP>> all() { std::vector<std::pair<std::string, MP>> v; walk(root, "", &v); return v; }
};

static void fill_pattern(std::vector<uint8_t>* d, uint64_t off, size_t len, unsigned seed) {
  if (d->size() < off + len) d->resize(off + len, 0);
  for (size_t i = 0; i < len; i++) (*d)[off + i] = static_cast<uint8_t>((((off + i) * 7) + seed) & 255);
}

static bool in_subtree(const MP& root, const MP& target) {   // target が root 自身か下にあるか
  if (root == target) return true;
  if (root->kind != MNode::Dir) return false;
  for (auto& kv : root->kids) if (in_subtree(kv.second.second, target)) return true;
  return false;
}

// FS 全体とモデルを比較。differences があれば報告して数を返す。
static int compare_fs(FsVolume& vol, Model& m, const char* when) {
  int diffs = 0;
  auto bad = [&](const std::string& what) { if (diffs < 15) std::fprintf(stderr, "MISMATCH(%s): %s\n", when, what.c_str()); diffs++; };
  std::vector<std::pair<std::string, uint64_t>> stack{{"", vol.root()}};
  std::map<std::string, MP> dirs; dirs[""] = m.root;
  while (!stack.empty()) {
    auto [path, ino] = stack.back(); stack.pop_back();
    MP mn = dirs[path];
    std::vector<FsDirEntry> ents;
    int rc = vol.readdir(ino, &ents);
    if (rc) { bad("readdir " + path + " -> " + std::to_string(rc)); continue; }
    std::set<std::string> names;
    for (auto& e : ents) {
      if (e.name == "." || e.name == "..") continue;
      names.insert(fold(e.name));
      std::string p = path.empty() ? e.name : path + "/" + e.name;
      auto it = mn->kids.find(fold(e.name));
      if (it == mn->kids.end()) { bad("extra entry " + p); continue; }
      if (it->second.first != e.name) bad("name case " + p + " model=" + it->second.first);
      MP k = it->second.second; FsStat st;
      if (vol.stat(e.ino, &st)) { bad("stat " + p); continue; }
      uint32_t type = st.mode & 0170000;
      if (k->kind == MNode::Dir) {
        if (type != 0040000) { bad("not dir " + p); continue; }
        dirs[p] = k; stack.push_back({p, e.ino});
      } else {
        if (type != 0100000) { bad("not file " + p); continue; }
        if (st.size != k->data.size()) { bad("size " + p + " fs=" + std::to_string(st.size) + " model=" + std::to_string(k->data.size())); continue; }
        std::vector<uint8_t> got(k->data.size() + 1);
        size_t off = 0;
        while (off < k->data.size()) { int64_t r = vol.read(e.ino, off, got.data() + off, std::min<size_t>(k->data.size() - off, 1 << 18)); if (r <= 0) break; off += (size_t)r; }
        if (off != k->data.size() || (!k->data.empty() && memcmp(got.data(), k->data.data(), k->data.size()) != 0)) bad("content " + p);
      }
    }
    for (auto& kv : mn->kids) if (!names.count(kv.first)) bad("missing entry " + (path.empty() ? kv.second.first : path + "/" + kv.second.first));
  }
  return diffs;
}

static void dump_model(Model& m, const std::string& out) {
  std::ofstream f(out);
  std::vector<std::string> lines;
  for (auto& kv : m.all()) {
    const MP& n = kv.second; char buf[64];
    if (n->kind == MNode::Dir) lines.push_back("d 0 - " + kv.first);
    else { Sha256 h; h.update(n->data.data(), n->data.size()); std::snprintf(buf, sizeof buf, "f %zu ", n->data.size()); lines.push_back(std::string(buf) + h.hex() + " " + kv.first); }
  }
  std::sort(lines.begin(), lines.end());
  for (auto& l : lines) f << l << "\n";
}

static int model_run(const std::string& img, unsigned seed, int nops, const std::string& dump, bool small) {
  Geo geo = geo_of(img);
  const bool fixed_root = geo.ok && geo.type != 32;
  BlockDevicePtr dev = open_rw(img);
  if (!dev) return 2;
  int err = 0; auto vol = FatVolume::mount_rw(dev, &err);
  if (!vol || !vol->writable()) { std::fprintf(stderr, "mount_rw failed/not writable (%d)\n", err); return 1; }
  Model m; std::mt19937 rng(seed);
  auto R = [&](uint32_t n) { return n ? (uint32_t)(rng() % n) : 0u; };
  const char* names[] = {"a", "b", "c", "dir", "file.txt", "日本語.txt", "テスト", "x y", "ドライブ", ".hidden", "long", "😀emoji", "MiXeD", "Long File Name", "longname_alpha", "longname_beta", "A.B.C", "~tilde", "semi;colon", "UPPER.TXT", "lower.txt"};
  const uint32_t nnames = sizeof names / sizeof *names;
  auto rname = [&]() {
    uint32_t k = R(nnames + 3);
    if (k < nnames) return std::string(names[k]) + (R(3) ? std::to_string(R(40)) : "");
    if (k == nnames) return small ? std::string(100, 'n') : std::string(255, 'n');
    if (k == nnames + 1) return std::string(small ? 100 : 250, 'm') + std::to_string(R(9999));
    return std::string("n") + std::to_string(R(1000));
  };
  auto paths_of = [&](MNode::Kind k) { std::vector<std::string> r; for (auto& kv : m.all()) if (kv.second->kind == k) r.push_back(kv.first); return r; };
  auto rdir = [&]() { auto d = paths_of(MNode::Dir); d.push_back(""); return d[R((uint32_t)d.size())]; };
  auto join = [](const std::string& d, const std::string& n) { return d.empty() ? n : d + "/" + n; };
  auto ino_of = [&](const std::string& p, uint64_t* ino) { return p.empty() ? (*ino = vol->root(), 0) : vol->resolve(p, ino); };
  auto setkid = [&](const MP& par, const std::string& name, const MP& n) { par->kids[fold(name)] = {name, n}; };
  int ops = 0, mism = 0, badrc = 0, nospc_root = 0;
  // FAT12/16 のルートは固定長: そこへ増やす操作は -ENOSPC で安全に失敗してよい (何も変わらないこと)
  auto root_full = [&](int rc, const std::string& dir) { if (rc == -ENOSPC && fixed_root && dir.empty()) { nospc_root++; return true; } return false; };
  auto expect = [&](int rc, int want, const std::string& what) {
    // want == 0: 成功を期待。want < 0: その errno で失敗を期待。
    if (rc != want) { if (badrc < 20) std::fprintf(stderr, "UNEXPECTED rc %d (want %d): %s\n", rc, want, what.c_str()); badrc++; }
  };
  const size_t sizes_big[] = {0, 1, 11, 511, 512, 513, 1023, 1024, 1025, 2047, 4095, 4096, 4097, 8192, 12 * 1024, 12 * 1024 + 1, 32768, 32769, 65536, 270 * 1024, 300 * 1024};
  const size_t sizes_small[] = {0, 1, 11, 511, 512, 513, 1023, 1024, 1025, 2047, 4095, 4096, 4097, 8192, 12 * 1024 + 1, 20000};
  const size_t* sizes = small ? sizes_small : sizes_big; const size_t nsizes = small ? sizeof sizes_small / sizeof *sizes_small : sizeof sizes_big / sizeof *sizes_big;
  for (int it = 0; it < nops; it++) {
    uint32_t k = R(100);
    // モデルが満杯に近づいたら消す側に倒す: 本当にディスクが満杯になると途中までの書き込み (ENOSPC) の内容をモデルが知らないため。満杯の挙動は --enospc が別に検査する。
    // 使用量の見積もり: ファイルはクラスタ単位に切り上げ、ディレクトリは 1 クラスタ + 名前ごとのエントリ (長い名前のスロットを含む) 分。半分を超えたら消す。
    if (geo.ok) {
      uint64_t cl = (uint64_t)geo.spc * geo.bps, cap = (uint64_t)geo.clusters * cl, used = 0;
      bool anyfile = false;
      for (auto& kv : m.all()) {
        const std::string& pth = kv.first; size_t leaf = pth.size() - (pth.rfind('/') == std::string::npos ? 0 : pth.rfind('/') + 1);
        used += (uint64_t)(leaf / 13 + 3) * 32;
        if (kv.second->kind == MNode::Dir) used += cl; else { anyfile = true; used += (kv.second->data.size() + cl - 1) / cl * cl; }
      }
      if (used * 2 > cap) k = anyfile ? 70 : 80;
    }
    if (k < 12) {   // mkdir
      std::string d = rdir(), n = rname(), p = join(d, n);
      std::string nn; MP par = m.parent(p, &nn);
      uint64_t di; int rc = ino_of(d, &di); if (!rc) { uint64_t ino; rc = vol->mkdir(di, n, 0755, &ino); }
      if (par->kids.count(fold(nn))) expect(rc, -EEXIST, "mkdir exists " + p);
      else if (root_full(rc, d)) {}
      else { expect(rc, 0, "mkdir " + p); if (!rc) { auto x = std::make_shared<MNode>(); x->kind = MNode::Dir; setkid(par, nn, x); } }
    } else if (k < 40) {   // create + write
      std::string d = rdir(), n = rname(), p = join(d, n);
      std::string nn; MP par = m.parent(p, &nn);
      uint64_t di, ino = 0; int rc = ino_of(d, &di); if (!rc) rc = vol->create(di, n, 0644, &ino);
      if (par->kids.count(fold(nn))) { expect(rc, -EEXIST, "create exists " + p); }
      else if (root_full(rc, d)) {}
      else {
        expect(rc, 0, "create " + p);
        if (!rc) {
          auto x = std::make_shared<MNode>(); setkid(par, nn, x);
          size_t sz = sizes[R((uint32_t)nsizes)]; unsigned sd = R(256);
          if (sz) { auto b = fill(0, sz, sd); int64_t w = vol->write(ino, 0, b.data(), b.size()); expect(w == (int64_t)sz ? 0 : (int)w, 0, "write " + p); if (w == (int64_t)sz) fill_pattern(&x->data, 0, sz, sd); else { x->data.assign(b.begin(), b.begin() + std::max<int64_t>(w, 0)); } }
        }
      }
    } else if (k < 58) {   // 既存ファイルへ書く (上書き / 拡張 / 隙間)
      auto fs = paths_of(MNode::File); if (fs.empty()) continue;
      std::string p = fs[R((uint32_t)fs.size())]; MP x = m.find(p);
      uint64_t off = R(4) == 0 ? x->data.size() + R(20000) : (x->data.empty() ? 0 : R((uint32_t)x->data.size()));
      size_t len = 1 + R(R(3) == 0 ? (small ? 9000 : 70000) : 3000); unsigned sd = R(256);
      uint64_t ino; int rc = vol->resolve(p, &ino); if (!rc) { auto b = fill(off, len, sd); int64_t w = vol->write(ino, off, b.data(), b.size()); rc = w == (int64_t)len ? 0 : (w < 0 ? (int)w : -EIO); }
      expect(rc, 0, "write2 " + p); if (!rc) fill_pattern(&x->data, off, len, sd);
    } else if (k < 68) {   // truncate
      auto fs = paths_of(MNode::File); if (fs.empty()) continue;
      std::string p = fs[R((uint32_t)fs.size())]; MP x = m.find(p);
      uint64_t sz = R(3) == 0 ? 0 : (R(2) ? R((uint32_t)x->data.size() + 1) : x->data.size() + R(30000));
      uint64_t ino; int rc = vol->resolve(p, &ino); if (!rc) rc = vol->truncate(ino, sz);
      expect(rc, 0, "truncate " + p); if (!rc) x->data.resize(sz, 0);
    } else if (k < 78) {   // unlink
      auto fs = paths_of(MNode::File);
      if (fs.empty()) continue;
      std::string p = fs[R((uint32_t)fs.size())]; std::string d, n; split_path(p, &d, &n);
      uint64_t di; int rc = ino_of(d, &di); if (!rc) rc = vol->unlink(di, n);
      expect(rc, 0, "unlink " + p); if (!rc) { std::string nn; MP par = m.parent(p, &nn); par->kids.erase(fold(nn)); }
    } else if (k < 83) {   // rmdir (空でなければ ENOTEMPTY を期待)
      auto ds = paths_of(MNode::Dir); if (ds.empty()) continue;
      std::string p = ds[R((uint32_t)ds.size())];
      MP x = m.find(p); std::string d, n; split_path(p, &d, &n);
      uint64_t di; int rc = ino_of(d, &di); if (!rc) rc = vol->rmdir(di, n);
      if (!x->kids.empty()) expect(rc, -ENOTEMPTY, "rmdir nonempty " + p);
      else { expect(rc, 0, "rmdir " + p); if (!rc) { std::string nn; MP par = m.parent(p, &nn); par->kids.erase(fold(nn)); } }
    } else {   // rename
      auto all = m.all(); if (all.empty()) continue;
      auto src = all[R((uint32_t)all.size())];
      std::string dd = rdir(), nn2 = R(3) == 0 ? rname() : std::string();
      std::string dstp;
      if (nn2.empty()) { auto cand = all[R((uint32_t)all.size())]; dstp = cand.first; }   // 既存の名前へ (上書き)
      else if (R(8) == 0) { std::string d0, n0; split_path(src.first, &d0, &n0); std::string up = n0; for (auto& c : up) if (c >= 'a' && c <= 'z') c = (char)(c - 32); dstp = join(d0, up); }   // 大文字小文字だけの変更
      else dstp = join(dd, nn2);
      std::string sn, dn; MP sp = m.parent(src.first, &sn); MP dp = m.parent(dstp, &dn);
      if (!dp) continue;   // 親が無い (別ファイルの下など)
      MP dst = dp->kids.count(fold(dn)) ? dp->kids[fold(dn)].second : nullptr;
      int want = 0;
      if (dst && dst == src.second && sn == dn && sp == dp) want = 0;   // 同じ名前: 何もしない
      else if (src.second->kind == MNode::Dir && in_subtree(src.second, dp)) want = -EINVAL;
      else if (dst && dst != src.second && (dst->kind == MNode::Dir) != (src.second->kind == MNode::Dir)) want = src.second->kind == MNode::Dir ? -ENOTDIR : -EISDIR;
      else if (dst && dst != src.second && dst->kind == MNode::Dir && !dst->kids.empty()) want = -ENOTEMPTY;
      std::string sd_, sn_, dd_, dn_; split_path(src.first, &sd_, &sn_); split_path(dstp, &dd_, &dn_);
      uint64_t a, b2; int rc = ino_of(sd_, &a); if (!rc) rc = ino_of(dd_, &b2); if (!rc) rc = vol->rename(a, sn_, b2, dn_);
      if (want == 0 && root_full(rc, dd_)) { ops++; continue; }
      expect(rc, want, "rename " + src.first + " -> " + dstp);
      if (rc == 0 && want == 0 && !(dst == src.second && sn == dn && sp == dp)) { sp->kids.erase(fold(sn)); setkid(dp, dn, src.second); }
    }
    ops++;
    if (it % 60 == 59) { int rc = vol->sync(); CHECK(rc == 0); mism += compare_fs(*vol, m, "checkpoint"); }
  }
  CHECK(vol->sync() == 0);
  mism += compare_fs(*vol, m, "final");
  vol.reset();
  // 再マウント (読み取り専用・新しいページキャッシュ) して、ディスクに書かれた内容が一致すること
  dev = open_rw(img);
  auto ro = FatVolume::mount(dev, &err);
  if (!ro) { std::fprintf(stderr, "remount failed %d\n", err); return 1; }
  mism += compare_fs(*ro, m, "remount");
  ro.reset();
  if (!dump.empty()) dump_model(m, dump);
  std::fprintf(stderr, "model: %d ops, %d mismatches, %d unexpected results, %d root-full refusals (seed %u, FAT%d)\n", ops, mism, badrc, nospc_root, seed, geo.type);
  return (mism || badrc || g_fail) ? 1 : 0;
}

// ---- --noop
static int noop_run(const std::string& img) {
  std::string before = slurp_hash(img);
  { BlockDevicePtr dev = open_rw(img); int err = 0; auto vol = FatVolume::mount_rw(dev, &err);
    if (!vol) return 1;
    CHECK(vol->writable());
    std::vector<FsDirEntry> e; CHECK(vol->readdir(vol->root(), &e) == 0); }
  std::string after = slurp_hash(img);
  CHECK(before == after);
  std::fprintf(stderr, "noop: image %s\n", before == after ? "unchanged" : "CHANGED");
  return g_fail ? 1 : 0;
}

// ---- --refuse
static int refuse_run(const std::string& img, const std::string& variant) {
  Geo g = geo_of(img);
  if (!g.ok) return 2;
  if (variant == "boot-dirty" || variant == "boot-media") {
    auto b = rd_bytes(img, g.state_off(), 1); b[0] |= variant == "boot-dirty" ? 1 : 2; wr_bytes(img, g.state_off(), b);
  } else if (variant == "fat-unclean") {
    if (g.type == 12) { std::fprintf(stderr, "refuse fat-unclean: FAT12 has no clean bit (n/a)\n"); return 0; }
    for (uint32_t i = 0; i < g.nfats; i++) {   // どの FAT の FAT[1] でも (FAT 1 面目だけ) クリーン bit を落とす
      if (i > 0) break;
      uint64_t o = g.fat_off((int)i) + (g.type == 16 ? 2 : 4);
      auto b = rd_bytes(img, o, 4);
      if (g.type == 16) b[1] &= 0x7f; else b[3] &= 0xf7;
      wr_bytes(img, o, b);
    }
  } else if (variant == "fat-unclean-2") {   // 2 面目の FAT[1] だけ (1 面目はクリーン)。FAT が 2 面あるイメージ向け
    if (g.type == 12 || g.nfats < 2) { std::fprintf(stderr, "refuse fat-unclean-2: n/a\n"); return 0; }
    uint64_t o = g.fat_off(1) + (g.type == 16 ? 2 : 4); auto b = rd_bytes(img, o, 4);
    if (g.type == 16) b[1] &= 0x7f; else b[3] &= 0xf7;
    wr_bytes(img, o, b);
  } else return 2;
  std::string before = slurp_hash(img);
  {
    BlockDevicePtr dev = open_rw(img); int err = 0; auto vol = FatVolume::mount_rw(dev, &err);
    if (!vol) { std::fprintf(stderr, "mount failed %d\n", err); return 1; }
    CHECK(!vol->writable());
    uint64_t ino = 0;
    CHECK(vol->create(vol->root(), "x", 0644, &ino) == -EROFS);
    CHECK(vol->mkdir(vol->root(), "d", 0755, &ino) == -EROFS);
    CHECK(vol->unlink(vol->root(), "nothing") == -EROFS);
    CHECK(vol->sync() == 0);
    std::vector<FsDirEntry> e; CHECK(vol->readdir(vol->root(), &e) == 0);   // 読み取りはできる
  }
  CHECK(before == slurp_hash(img));
  std::fprintf(stderr, "refuse %s: %s\n", variant.c_str(), g_fail ? "FAIL" : "ok (読み取り専用・イメージ不変)");
  return g_fail ? 1 : 0;
}

// ---- --enospc
static int enospc_run(const std::string& img) {
  Geo geo = geo_of(img);
  BlockDevicePtr dev = open_rw(img); int err = 0; auto vol = FatVolume::mount_rw(dev, &err);
  if (!vol || !vol->writable()) { std::fprintf(stderr, "mount_rw failed\n"); return 1; }
  std::vector<uint8_t> chunk(1 << 16, 0xAB);
  // 1. 大きなファイルでディスクを満たす: ENOSPC で失敗し、書けた分は読める
  uint64_t big; CHECK(vol->create(vol->root(), "big", 0644, &big) == 0);
  uint64_t total = 0; int64_t w;
  while ((w = vol->write(big, total, chunk.data(), chunk.size())) > 0) { total += (uint64_t)w; if (w < (int64_t)chunk.size()) break; }
  std::fprintf(stderr, "enospc: wrote %llu bytes, last write -> %lld\n", (unsigned long long)total, (long long)w);
  CHECK(w == -ENOSPC || w > 0);
  FsStat st; CHECK(vol->stat(big, &st) == 0); CHECK(st.size == total);
  std::vector<uint8_t> back(chunk.size()); CHECK(vol->read(big, total - chunk.size() / 2, back.data(), chunk.size() / 2) == (int64_t)(chunk.size() / 2));
  CHECK(back[0] == 0xAB);
  // 2. 満杯でも作成は失敗するだけで壊れない
  uint64_t x; int rc = vol->mkdir(vol->root(), "d", 0755, &x); CHECK(rc == 0 || rc == -ENOSPC);
  rc = vol->create(vol->root(), "f", 0644, &x);
  if (rc == 0) { int64_t ww = vol->write(x, 0, chunk.data(), 100); CHECK(ww == -ENOSPC || ww == 100); }
  // 満杯のときの巨大な truncate (拡張) は -ENOSPC で、サイズは 4GiB 未満の妥当な値のまま
  { uint64_t t2; int r2 = vol->create(vol->root(), "g", 0644, &t2); if (r2 == 0) { r2 = vol->truncate(t2, 1ull << 30); CHECK(r2 == -ENOSPC || r2 == 0); FsStat s2; CHECK(vol->stat(t2, &s2) == 0); CHECK(s2.size <= (1ull << 30)); CHECK(vol->truncate(t2, 0) == 0); CHECK(vol->unlink(vol->root(), "g") == 0); } }
  CHECK(vol->sync() == 0);
  // 3. 削除で回復
  CHECK(vol->unlink(vol->root(), "big") == 0);
  CHECK(vol->sync() == 0);
  uint64_t y; CHECK(vol->create(vol->root(), "after", 0644, &y) == 0);
  w = vol->write(y, 0, chunk.data(), chunk.size()); CHECK(w == (int64_t)chunk.size());
  CHECK(vol->unlink(vol->root(), "after") == 0);
  if (vol->lookup(vol->root(), "f", &x) == 0) CHECK(vol->unlink(vol->root(), "f") == 0);
  if (vol->lookup(vol->root(), "d", &x) == 0) CHECK(vol->rmdir(vol->root(), "d") == 0);
  CHECK(vol->sync() == 0);
  // 4. FAT12/16 の固定長ルートの枯渇: 8.3 名のファイルを作り続けると、ルートのエントリ数で -ENOSPC。削除で回復する
  if (geo.ok && geo.type != 32) {
    int made = 0, lastrc = 0;
    for (int i = 0; i < 5000; i++) {
      char nm[16]; std::snprintf(nm, sizeof nm, "R%05d.DAT", i);
      uint64_t t; lastrc = vol->create(vol->root(), nm, 0644, &t); if (lastrc) break; made++;
    }
    std::fprintf(stderr, "enospc: root (%u entries) took %d short-named files, last -> %d\n", geo.root_entries, made, lastrc);
    CHECK(lastrc == -ENOSPC);
    CHECK((uint32_t)made <= geo.root_entries && (uint32_t)made + 4 >= geo.root_entries);
    // 長い名前 (複数エントリが要る) も、足りなければ何も作らずに失敗する
    { uint64_t t; CHECK(vol->create(vol->root(), std::string(200, 'q'), 0644, &t) == -ENOSPC); CHECK(vol->mkdir(vol->root(), "newdir", 0755, &t) == -ENOSPC); }
    CHECK(vol->sync() == 0);
    for (int i = 0; i < made; i++) { char nm[16]; std::snprintf(nm, sizeof nm, "R%05d.DAT", i); CHECK(vol->unlink(vol->root(), nm) == 0); }
    CHECK(vol->sync() == 0);
    uint64_t z; CHECK(vol->create(vol->root(), "again0", 0644, &z) == 0); CHECK(vol->unlink(vol->root(), "again0") == 0);
  }
  // 5. サブディレクトリの伸長不能 (1 ディレクトリは最大 2MiB = 65536 エントリ。ディスクが小さければ領域不足): 長い名前 (255 文字 = 21 エントリ) のファイルを作り続ける
  uint64_t sub; CHECK(vol->mkdir(vol->root(), "sub", 0755, &sub) == 0);
  int made = 0; int lastrc = 0;
  for (int i = 0; i < 4000; i++) {
    char pre[16]; std::snprintf(pre, sizeof pre, "%06d", i);   // 先頭 6 文字を毎回変える (同じ先頭だと 8.3 名の衝突の探索が 2 乗に遅くなる: Linux の vfat も同じ)
    std::string nm = std::string(pre) + std::string(249, 'z');
    uint64_t t; lastrc = vol->create(sub, nm, 0644, &t); if (lastrc) break; made++;
  }
  std::fprintf(stderr, "enospc: created %d long-named files in a subdirectory, last -> %d\n", made, lastrc);
  CHECK(lastrc == -ENOSPC);
  CHECK(vol->sync() == 0);
  for (int i = 0; i < made; i++) { char pre[16]; std::snprintf(pre, sizeof pre, "%06d", i); CHECK(vol->unlink(sub, std::string(pre) + std::string(249, 'z')) == 0); }
  CHECK(vol->sync() == 0);
  uint64_t z; CHECK(vol->create(sub, "again", 0644, &z) == 0);
  CHECK(vol->unlink(sub, "again") == 0);
  CHECK(vol->rmdir(vol->root(), "sub") == 0);
  vol.reset();
  return g_fail ? 1 : 0;
}

// ---- --names
static int names_run(const std::string& img) {
  BlockDevicePtr dev = open_rw(img); int err = 0; auto vol = FatVolume::mount_rw(dev, &err);
  if (!vol || !vol->writable()) { std::fprintf(stderr, "mount_rw failed\n"); return 1; }
  uint64_t r = vol->root(), a, b;
  CHECK(vol->create(r, "Hello.TXT", 0644, &a) == 0);
  CHECK(vol->create(r, "hello.txt", 0644, &b) == -EEXIST);                  // 大文字小文字を区別しない
  CHECK(vol->lookup(r, "HELLO.txt", &b) == 0 && a == b);
  CHECK(vol->mkdir(r, "HELLO.TXT", 0755, &b) == -EEXIST);
  { std::vector<FsDirEntry> e; vol->readdir(r, &e); bool found = false; for (auto& x : e) if (x.name == "Hello.TXT") found = true; CHECK(found); }   // 名前は作ったとおりの大文字小文字で残る
  // 大文字小文字だけの名前変更は、同じファイルのまま名前の綴りだけ変わる
  CHECK(vol->rename(r, "hello.txt", r, "HeLLo.Txt") == 0);
  { std::vector<FsDirEntry> e; vol->readdir(r, &e); int n = 0; bool found = false; for (auto& x : e) { if (x.name == "HeLLo.Txt") found = true; if (fold(x.name) == "hello.txt") n++; } CHECK(found); CHECK(n == 1); }
  std::vector<uint8_t> d1 = fill(0, 5000, 3); { uint64_t i; CHECK(vol->lookup(r, "hello.TXT", &i) == 0); CHECK(vol->write(i, 0, d1.data(), d1.size()) == 5000); }
  // 小文字だけ / 大文字だけの名前: 8.3 名 + 大小文字の保持 (LFN エントリで)
  for (const char* nm : {"lower.txt", "UPPER.TXT", "Mixed.Txt", "noext", "NOEXT2", "a.b", "x"}) {
    uint64_t t; CHECK(vol->create(r, nm, 0644, &t) == 0);
    std::vector<FsDirEntry> e; vol->readdir(r, &e); bool f2 = false; for (auto& x : e) if (x.name == nm) f2 = true; if (!f2) { std::fprintf(stderr, "name not preserved: %s\n", nm); g_fail++; }
  }
  // 禁止文字・空・"."・".."・長すぎる名前・'/'・末尾の空白
  const char* bad[] = {"a:b", "a*b", "a?b", "a<b", "a>b", "a|b", "a\"b", "a\\b", "ctl\x01x", "tab\tx", "trail "};
  for (const char* nm : bad) { uint64_t t; int rc = vol->create(r, nm, 0644, &t); if (rc != -EINVAL) { std::fprintf(stderr, "bad name accepted: rc=%d (%s)\n", rc, nm); g_fail++; } }
  { uint64_t t; CHECK(vol->create(r, "", 0644, &t) == -EINVAL); CHECK(vol->create(r, ".", 0644, &t) == -EINVAL); CHECK(vol->create(r, "..", 0644, &t) == -EINVAL); CHECK(vol->create(r, "a/b", 0644, &t) == -EINVAL); }
  { uint64_t t; CHECK(vol->create(r, std::string(255, 'x'), 0644, &t) == 0); int rc = vol->create(r, std::string(256, 'y'), 0644, &t); CHECK(rc == -ENAMETOOLONG); }
  // UTF-16: 日本語 255 文字 (= 255 UTF-16 単位) は作れる、256 文字は長すぎる。絵文字 (サロゲートペア) 127 個 = 254 単位は作れる、128 個 = 256 単位は長すぎる
  { std::string jp; for (int i = 0; i < 255; i++) jp += "あ"; uint64_t t; CHECK(vol->create(r, jp, 0644, &t) == 0); CHECK(vol->lookup(r, jp, &b) == 0);
    CHECK(vol->create(r, jp + "あ", 0644, &t) == -ENAMETOOLONG);
    std::string em; for (int i = 0; i < 127; i++) em += "😀"; CHECK(vol->create(r, em, 0644, &t) == 0); CHECK(vol->lookup(r, em, &b) == 0);
    std::string em2; for (int i = 0; i < 128; i++) em2 += "😀"; CHECK(vol->create(r, em2, 0644, &t) == -ENAMETOOLONG); }
  { uint64_t t; CHECK(vol->create(r, "bad\xff" "utf8", 0644, &t) == -EINVAL); }   // 不正な UTF-8
  // 8.3 名の衝突: 同じ先頭 6 文字の長い名前を 25 個 (LONGNA~1 ... ~25)・同じ拡張子の短縮
  { std::set<uint64_t> inos; uint64_t t;
    for (int i = 0; i < 25; i++) { char nm[64]; std::snprintf(nm, sizeof nm, "Long File Name %02d.text", i); CHECK(vol->create(r, nm, 0644, &t) == 0); inos.insert(t); }
    for (int i = 0; i < 25; i++) { char nm[64]; std::snprintf(nm, sizeof nm, "Long File Name %02d.text", i); uint64_t u; CHECK(vol->lookup(r, nm, &u) == 0 && inos.count(u)); }
    CHECK(inos.size() == 25);
    // 既存の 8.3 名と短縮名が衝突する長い名前 (README と readme_long → READMЕ~1)
    CHECK(vol->create(r, "README", 0644, &t) == 0); CHECK(vol->create(r, "readme_long_name.md", 0644, &t) == 0); CHECK(vol->create(r, "readme_long_name2.md", 0644, &t) == 0);
    // 日本語: OEM コードページに変換できない文字は短縮名で '_' になる。衝突しても全部作れる
    for (int i = 0; i < 12; i++) { std::string nm = std::string("日本語の名前") + std::to_string(i) + ".txt"; CHECK(vol->create(r, nm, 0644, &t) == 0); }
    for (int i = 0; i < 12; i++) { std::string nm = std::string("日本語の名前") + std::to_string(i) + ".txt"; uint64_t u; CHECK(vol->lookup(r, nm, &u) == 0); }
  }
  // ディレクトリ: 中に作る・同名の大小違いで衝突・空でない rmdir・ファイルとして rmdir
  { uint64_t d, f; CHECK(vol->mkdir(r, "Sub", 0755, &d) == 0); CHECK(vol->create(d, "inner", 0644, &f) == 0);
    CHECK(vol->rmdir(r, "sub") == -ENOTEMPTY); CHECK(vol->unlink(r, "SUB") == -EISDIR); CHECK(vol->rmdir(r, "Hello.TXT") == -ENOTDIR);
    CHECK(vol->unlink(d, "INNER") == 0); CHECK(vol->rmdir(r, "sUb") == 0); uint64_t t; CHECK(vol->lookup(r, "Sub", &t) == -ENOENT); }
  // ディレクトリの大文字小文字だけの rename (中身が残ること)
  { uint64_t d, f; CHECK(vol->mkdir(r, "CaseDir", 0755, &d) == 0); CHECK(vol->create(d, "keep.txt", 0644, &f) == 0);
    CHECK(vol->rename(r, "CaseDir", r, "casedir") == 0); uint64_t d2, f2; CHECK(vol->lookup(r, "CASEDIR", &d2) == 0 && d2 == d); CHECK(vol->lookup(d2, "keep.txt", &f2) == 0); }
  CHECK(vol->sync() == 0);
  vol.reset();
  std::fprintf(stderr, "names: %s\n", g_fail ? "FAIL" : "ok");
  return g_fail ? 1 : 0;
}

// ---- --limits: FAT のファイルサイズは 32 bit (最大 4GiB-1)
static int limits_run(const std::string& img) {
  BlockDevicePtr dev = open_rw(img); int err = 0; auto vol = FatVolume::mount_rw(dev, &err);
  if (!vol || !vol->writable()) { std::fprintf(stderr, "mount_rw failed\n"); return 1; }
  uint64_t f; CHECK(vol->create(vol->root(), "lim.bin", 0644, &f) == 0);
  uint8_t one = 1; std::vector<uint8_t> blk(4096, 7);
  CHECK(vol->write(f, 0xFFFFFFFFull, &one, 1) == -EFBIG);                    // 末尾が 4GiB に届く
  CHECK(vol->write(f, 0x100000000ull, &one, 1) == -EFBIG);
  CHECK(vol->write(f, 0x100000000ull + 4096, blk.data(), blk.size()) == -EFBIG);
  CHECK(vol->write(f, 0x7fffffffffffffffull, &one, 1) <= 0);
  CHECK(vol->truncate(f, 0x100000000ull) == -EFBIG);
  CHECK(vol->truncate(f, 0xFFFFFFFFFFFFFFFFull) == -EFBIG);
  FsStat st; CHECK(vol->stat(f, &st) == 0); CHECK(st.size == 0);
  CHECK(vol->write(f, 0, blk.data(), blk.size()) == (int64_t)blk.size());   // 失敗のあとも普通に使える
  CHECK(vol->stat(f, &st) == 0); CHECK(st.size == 4096);
  CHECK(vol->sync() == 0);
  vol.reset();
  std::fprintf(stderr, "limits: %s\n", g_fail ? "FAIL" : "ok");
  return g_fail ? 1 : 0;
}

// ---- --clean: ダーティ印 (ブートセクタの bit と FAT[1] の clean bit) と FSInfo
static int clean_run(const std::string& img) {
  Geo g = geo_of(img);
  if (!g.ok) return 2;
  auto boot_dirty = [&]() { return (int)(rd_bytes(img, g.state_off(), 1)[0] & 1); };
  auto fat1 = [&](int i) {   // 1 = クリーン bit が立っている、0 = 落ちている、-1 = 無い (FAT12)
    if (g.type == 12) return -1;
    if (g.type == 16) return (rd_bytes(img, g.fat_off(i) + 2, 2)[1] & 0x80) ? 1 : 0;
    return (rd_bytes(img, g.fat_off(i) + 4, 4)[3] & 0x08) ? 1 : 0;
  };
  auto fsinfo_free = [&]() { if (g.type != 32) return 0u; return rd32(rd_bytes(img, (uint64_t)g.fsinfo * g.bps + 488, 4).data()); };
  CHECK(boot_dirty() == 0);
  for (uint32_t i = 0; i < g.nfats; i++) CHECK(fat1((int)i) != 0);
  uint32_t free0 = fsinfo_free();
  BlockDevicePtr dev = open_rw(img); int err = 0; auto vol = FatVolume::mount_rw(dev, &err);
  if (!vol || !vol->writable()) return 1;
  // マウントしただけでは何も変わらない
  CHECK(boot_dirty() == 0);
  uint64_t ino; CHECK(vol->create(vol->root(), "c.bin", 0644, &ino) == 0);
  CHECK(vol->sync() == 0);
  CHECK(boot_dirty() == 1);   // 最初の変更の直後 (まだ unmount していない) にはダーティ印がディスクに立っている
  for (uint32_t i = 0; i < g.nfats; i++) CHECK(fat1((int)i) != 1);
  CHECK(fsinfo_free() == 0xFFFFFFFFu || g.type != 32);   // FSInfo の空きクラスタ数は「不明」(ダーティ中は信用させない)
  auto b = fill(0, 100000, 1); CHECK(vol->write(ino, 0, b.data(), b.size()) == 100000);
  CHECK(vol->sync() == 0);
  CHECK(boot_dirty() == 1);   // sync しても、unmount までは立ったまま
  for (uint32_t i = 0; i < g.nfats; i++) CHECK(fat1((int)i) != 1);
  CHECK(fsinfo_free() == 0xFFFFFFFFu || g.type != 32);
  vol.reset();
  CHECK(boot_dirty() == 0);
  for (uint32_t i = 0; i < g.nfats; i++) CHECK(fat1((int)i) != 0);
  if (g.type == 32) { uint32_t f1 = fsinfo_free(); CHECK(f1 != 0xFFFFFFFFu); CHECK(f1 <= g.clusters); CHECK(f1 < free0 || free0 == 0xFFFFFFFFu); }
  // FAT が 2 面あるなら、同期している
  if (g.nfats == 2) { auto a = rd_bytes(img, g.fat_off(0), (size_t)g.fatsz * g.bps), c = rd_bytes(img, g.fat_off(1), (size_t)g.fatsz * g.bps); CHECK(a == c); }
  std::fprintf(stderr, "clean: FAT%d %s\n", g.type, g_fail ? "FAIL" : "ok");
  return g_fail ? 1 : 0;
}

// ---- --fuzz: メタデータ領域を壊したイメージ。クラッシュ・ハングしないこと (ASan 下で使う)
static int fuzz_run(const std::string& img, unsigned seed, int iters) {
  Geo g = geo_of(img);
  if (!g.ok) return 2;
  std::mt19937 rng(seed);
  std::string tmp = img + ".fz" + std::to_string((long)getpid());
  uint64_t meta_end = (uint64_t)(g.rsvd + g.nfats * g.fatsz + (g.root_entries * 32 + g.bps - 1) / g.bps) * g.bps;
  uint64_t lim = std::min<uint64_t>(meta_end + 64 * 1024, (uint64_t)256 << 20);
  int mounted = 0, ran = 0;
  for (int it = 0; it < iters; it++) {
    { std::ifstream s(img, std::ios::binary); std::ofstream d(tmp, std::ios::binary | std::ios::trunc); d << s.rdbuf(); }
    int nmut = 1 + (int)(rng() % 12);
    for (int k = 0; k < nmut; k++) {
      uint64_t off;
      switch (rng() % 4) {
        case 0: off = rng() % 96; break;                                 // BPB
        case 1: off = (uint64_t)g.rsvd * g.bps + rng() % std::min<uint64_t>(4096, (uint64_t)g.fatsz * g.bps); break;   // FAT の先頭
        case 2: off = (uint64_t)(g.rsvd + g.nfats * g.fatsz) * g.bps + rng() % 8192; break;    // ルート/データの先頭
        default: off = rng() % lim; break;
      }
      std::vector<uint8_t> v{(uint8_t)(rng() & 255)};
      if (rng() % 3 == 0) v[0] = (uint8_t)(rng() % 2 ? 0xFF : 0);
      wr_bytes(tmp, off, v);
    }
    BlockDevicePtr dev = open_rw(tmp); int err = 0;
    auto vol = dev ? FatVolume::mount_rw(dev, &err) : nullptr;
    if (!vol) continue;
    mounted++;
    std::vector<std::pair<std::string, uint64_t>> dirs{{"", vol->root()}}; std::vector<std::pair<std::string, uint64_t>> files;
    for (size_t di = 0; di < dirs.size() && di < 40; di++) {
      std::vector<FsDirEntry> ents; if (vol->readdir(dirs[di].second, &ents)) continue;
      for (auto& e : ents) { if (e.name == "." || e.name == "..") continue; FsStat st; if (vol->stat(e.ino, &st)) continue; std::string p = dirs[di].first.empty() ? e.name : dirs[di].first + "/" + e.name; if ((st.mode & 0170000) == 0040000) dirs.push_back({p, e.ino}); else files.push_back({p, e.ino}); }
    }
    for (int k = 0; k < 24; k++) {
      uint32_t op = rng() % 8; auto& dd = dirs[rng() % dirs.size()]; uint64_t t; std::string nm = "fz" + std::to_string(rng() % 50);
      std::vector<uint8_t> buf(1 + rng() % 9000, (uint8_t)k);
      switch (op) {
        case 0: vol->create(dd.second, nm, 0644, &t); break;
        case 1: vol->mkdir(dd.second, nm, 0755, &t); break;
        case 2: if (!files.empty()) { auto& f = files[rng() % files.size()]; vol->write(f.second, rng() % 20000, buf.data(), buf.size()); } break;
        case 3: if (!files.empty()) { auto& f = files[rng() % files.size()]; vol->truncate(f.second, rng() % 40000); } break;
        case 4: { std::vector<FsDirEntry> ents; if (!vol->readdir(dd.second, &ents) && !ents.empty()) { auto& e = ents[rng() % ents.size()]; if (e.name != "." && e.name != "..") { if (rng() % 2) vol->unlink(dd.second, e.name); else vol->rmdir(dd.second, e.name); } } break; }
        case 5: { std::vector<FsDirEntry> ents; if (!vol->readdir(dd.second, &ents) && !ents.empty()) { auto& e = ents[rng() % ents.size()]; if (e.name != "." && e.name != "..") vol->rename(dd.second, e.name, dirs[rng() % dirs.size()].second, nm + "r"); } break; }
        case 6: if (!files.empty()) { auto& f = files[rng() % files.size()]; std::vector<uint8_t> rb(8192); vol->read(f.second, rng() % 30000, rb.data(), rb.size()); } break;
        default: vol->sync(); break;
      }
      ran++;
    }
    vol->sync();
    vol.reset();
  }
  unlink(tmp.c_str());
  std::fprintf(stderr, "fuzz: %d iterations, %d mounted rw, %d ops run (seed %u)\n", iters, mounted, ran, seed);
  return 0;
}

// 自前ドライバ (読み取りマウント) で全体を読んで、ops_ref.py --tree と同じ書式 ("種別 サイズ sha256 パス") で書く
static int tree_run(const std::string& img, const std::string& out) {
  BlockDevicePtr dev = open_rw(img);
  if (!dev) { std::fprintf(stderr, "open failed\n"); return 1; }
  int err = 0;
  auto vol = FatVolume::mount(dev, &err);
  if (!vol) { std::fprintf(stderr, "mount failed: errno %d\n", err); return 1; }
  std::vector<std::string> lines;
  std::vector<std::pair<std::string, uint64_t>> stack{{"", vol->root()}};
  while (!stack.empty()) {
    auto [path, ino] = stack.back(); stack.pop_back();
    std::vector<FsDirEntry> ents;
    if (vol->readdir(ino, &ents)) { std::fprintf(stderr, "readdir %s failed\n", path.c_str()); return 1; }
    for (auto& e : ents) {
      if (e.name == "." || e.name == "..") continue;
      std::string p = path.empty() ? e.name : path + "/" + e.name;
      FsStat st;
      if (vol->stat(e.ino, &st)) { std::fprintf(stderr, "stat %s failed\n", p.c_str()); return 1; }
      if ((st.mode & 0170000) == 0040000) { lines.push_back("d 0 - " + p); stack.push_back({p, e.ino}); continue; }
      Sha256 h; std::vector<uint8_t> buf(1 << 18); uint64_t off = 0;
      while (off < st.size) { int64_t r = vol->read(e.ino, off, buf.data(), std::min<uint64_t>(buf.size(), st.size - off)); if (r <= 0) { std::fprintf(stderr, "read %s failed\n", p.c_str()); return 1; } h.update(buf.data(), (size_t)r); off += (uint64_t)r; }
      char b[48]; std::snprintf(b, sizeof b, "f %llu ", (unsigned long long)st.size);
      lines.push_back(std::string(b) + h.hex() + " " + p);
    }
  }
  std::sort(lines.begin(), lines.end());
  std::ofstream f(out);
  for (auto& l : lines) f << l << "\n";
  return 0;
}

int main(int argc, char** argv) {
  if (argc >= 4 && std::string(argv[1]) == "--ops") return ops_run(argv[2], argv[3]);
  if (argc >= 5 && std::string(argv[1]) == "--model") return model_run(argv[2], (unsigned)std::atoi(argv[3]), std::atoi(argv[4]), argc >= 6 ? argv[5] : "", argc >= 7 && std::string(argv[6]) == "small");
  if (argc >= 4 && std::string(argv[1]) == "--tree") return tree_run(argv[2], argv[3]);
  if (argc >= 3 && std::string(argv[1]) == "--noop") return noop_run(argv[2]);
  if (argc >= 4 && std::string(argv[1]) == "--refuse") return refuse_run(argv[2], argv[3]);
  if (argc >= 3 && std::string(argv[1]) == "--enospc") return enospc_run(argv[2]);
  if (argc >= 3 && std::string(argv[1]) == "--names") return names_run(argv[2]);
  if (argc >= 3 && std::string(argv[1]) == "--limits") return limits_run(argv[2]);
  if (argc >= 3 && std::string(argv[1]) == "--clean") return clean_run(argv[2]);
  if (argc >= 5 && std::string(argv[1]) == "--fuzz") return fuzz_run(argv[2], (unsigned)std::atoi(argv[3]), std::atoi(argv[4]));
  std::fprintf(stderr, "usage: see header\n");
  return 2;
}
