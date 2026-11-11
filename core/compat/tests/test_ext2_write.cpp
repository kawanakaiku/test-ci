// SPDX-License-Identifier: GPL-2.0-or-later
// ext2 書き込みのテスト (USBMEM_NTFS_WRITE=ON ビルド用。docs/ext2-write.md)。独立実装 (e2fsck / fuse2fs / debugfs) との突き合わせはシェル側 (tools/run_ext2_write_tests.sh)。
//   --ops <img> <script>              書き込みマウントで操作列を実行 → sync → unmount。環境変数 USBMEM_CRASH_AT/MODE/SEED/TORN で電源断を模擬。
//   --model <img> <seed> <nops> [dump] 乱数で操作列を作り、メモリ上のモデルと突き合わせる (途中・unmount 後の再マウント後にも全体比較)。dump に tools/ops_ref.py --tree と同じ書式でモデルを書く
//   --noop <img>                      rw マウントして何もせず unmount: イメージが 1 バイトも変わらないこと
//   --refuse <img>                    rw を要求したが書き込み不可のイメージ (汚れている/エラー状態/未知の機能): マウントは成功し writable()==false、書き込みは -EROFS、イメージ不変
//   --enospc <img>                    満杯・inode 枯渇で安全に失敗し、削除で回復すること
// script の各行: mkdir P | create P | write P OFF LEN SEED | truncate P SIZE | unlink P | rmdir P | rename A B | symlink P TARGET | link NEW EXISTING
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
#include <vector>

#include "crash_device.h"
#include "sha256.h"
#include "usbmem/devices.h"
#include "usbmem/ext2_volume.h"

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
  if (!RawImageDevice::open_path(img, false, 512, &d).ok()) return nullptr;
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
  int err = 0; auto vol = Ext2Volume::mount_rw(dev, &err);
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
    } else if (op == "symlink") {
      is >> b; split_path(a, &d, &nm); rc = dir_ino(d, &di); if (!rc) rc = vol->symlink(di, nm, b, &ino);
    } else if (op == "link") {
      is >> b; split_path(a, &d, &nm); rc = dir_ino(d, &di); if (!rc) rc = vol->resolve(b, &ino); if (!rc) rc = vol->link(di, nm, ino);
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

// ---- --model: メモリ上のモデルとの突き合わせ
struct MNode {
  enum Kind { File, Dir, Link } kind = File;
  std::vector<uint8_t> data;                           // File
  std::string target;                                  // Link
  std::map<std::string, std::shared_ptr<MNode>> kids;  // Dir
};
using MP = std::shared_ptr<MNode>;
struct Model {
  MP root = std::make_shared<MNode>();
  Model() { root->kind = MNode::Dir; auto lf = std::make_shared<MNode>(); lf->kind = MNode::Dir; root->kids["lost+found"] = lf; }
  static std::vector<std::string> parts(const std::string& p) {
    std::vector<std::string> v; std::stringstream ss(p); std::string t;
    while (std::getline(ss, t, '/')) if (!t.empty()) v.push_back(t);
    return v;
  }
  MP find(const std::string& p) {
    MP cur = root;
    for (auto& s : parts(p)) { if (cur->kind != MNode::Dir) return nullptr; auto it = cur->kids.find(s); if (it == cur->kids.end()) return nullptr; cur = it->second; }
    return cur;
  }
  MP parent(const std::string& p, std::string* name) {
    auto v = parts(p); if (v.empty()) return nullptr;
    *name = v.back(); v.pop_back();
    MP cur = root;
    for (auto& s : v) { if (cur->kind != MNode::Dir) return nullptr; auto it = cur->kids.find(s); if (it == cur->kids.end()) return nullptr; cur = it->second; }
    return cur->kind == MNode::Dir ? cur : nullptr;
  }
  void walk(const MP& n, const std::string& path, std::vector<std::pair<std::string, MP>>* out) {
    for (auto& kv : n->kids) { std::string p = path.empty() ? kv.first : path + "/" + kv.first; out->push_back({p, kv.second}); if (kv.second->kind == MNode::Dir) walk(kv.second, p, out); }
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
  for (auto& kv : root->kids) if (in_subtree(kv.second, target)) return true;
  return false;
}

// FS 全体とモデルを比較。differences があれば報告して数を返す。
static int compare_fs(FsVolume& vol, Model& m, const char* when) {
  int diffs = 0;
  auto bad = [&](const std::string& what) { if (diffs < 15) std::fprintf(stderr, "MISMATCH(%s): %s\n", when, what.c_str()); diffs++; };
  std::map<MNode*, uint32_t> expect_nlink;   // モデル上の名前の数 (ディレクトリは別計算)
  auto v = m.all();
  for (auto& kv : v) if (kv.second->kind != MNode::Dir) expect_nlink[kv.second.get()]++;
  // 再帰的にディレクトリを比べる
  std::vector<std::pair<std::string, uint64_t>> stack{{"", vol.root()}};
  std::map<std::string, MP> dirs; dirs[""] = m.root;
  size_t seen = 0;
  while (!stack.empty()) {
    auto [path, ino] = stack.back(); stack.pop_back();
    MP mn = dirs[path];
    std::vector<FsDirEntry> ents;
    int rc = vol.readdir(ino, &ents);
    if (rc) { bad("readdir " + path + " -> " + std::to_string(rc)); continue; }
    std::set<std::string> names;
    for (auto& e : ents) {
      if (e.name == "." || e.name == "..") continue;
      names.insert(e.name);
      std::string p = path.empty() ? e.name : path + "/" + e.name;
      auto it = mn->kids.find(e.name);
      if (it == mn->kids.end()) { bad("extra entry " + p); continue; }
      seen++;
      MP k = it->second; FsStat st;
      if (vol.stat(e.ino, &st)) { bad("stat " + p); continue; }
      uint32_t type = st.mode & 0170000;
      if (k->kind == MNode::Dir) {
        if (type != 0040000) { bad("not dir " + p); continue; }
        dirs[p] = k; stack.push_back({p, e.ino});
      } else if (k->kind == MNode::Link) {
        if (type != 0120000) { bad("not symlink " + p); continue; }
        std::string t; if (vol.readlink(e.ino, &t) || t != k->target) bad("symlink target " + p);
        if (st.size != k->target.size()) bad("symlink size " + p);
      } else {
        if (type != 0100000) { bad("not file " + p); continue; }
        if (st.size != k->data.size()) { bad("size " + p + " fs=" + std::to_string(st.size) + " model=" + std::to_string(k->data.size())); continue; }
        if (st.nlink != expect_nlink[k.get()]) bad("nlink " + p + " fs=" + std::to_string(st.nlink) + " model=" + std::to_string(expect_nlink[k.get()]));
        std::vector<uint8_t> got(k->data.size() + 1);
        size_t off = 0;
        while (off < k->data.size()) { int64_t r = vol.read(e.ino, off, got.data() + off, std::min<size_t>(k->data.size() - off, 1 << 18)); if (r <= 0) break; off += (size_t)r; }
        if (off != k->data.size() || (!k->data.empty() && memcmp(got.data(), k->data.data(), k->data.size()) != 0)) bad("content " + p);
      }
    }
    for (auto& kv : mn->kids) if (!names.count(kv.first)) bad("missing entry " + (path.empty() ? kv.first : path + "/" + kv.first));
  }
  return diffs;
}

static void dump_model(Model& m, const std::string& out) {
  std::ofstream f(out);
  std::vector<std::string> lines;
  for (auto& kv : m.all()) {
    const MP& n = kv.second; char buf[64];
    if (n->kind == MNode::Dir) lines.push_back("d 0 - " + kv.first);
    else if (n->kind == MNode::Link) lines.push_back("l 0 " + n->target + " " + kv.first);
    else { Sha256 h; h.update(n->data.data(), n->data.size()); std::snprintf(buf, sizeof buf, "f %zu ", n->data.size()); lines.push_back(std::string(buf) + h.hex() + " " + kv.first); }
  }
  std::sort(lines.begin(), lines.end());
  for (auto& l : lines) f << l << "\n";
}

static int model_run(const std::string& img, unsigned seed, int nops, const std::string& dump) {
  BlockDevicePtr dev = open_rw(img);
  if (!dev) return 2;
  int err = 0; auto vol = Ext2Volume::mount_rw(dev, &err);
  if (!vol || !vol->writable()) { std::fprintf(stderr, "mount_rw failed/not writable (%d)\n", err); return 1; }
  Model m; std::mt19937 rng(seed);
  auto R = [&](uint32_t n) { return n ? (uint32_t)(rng() % n) : 0u; };
  const char* names[] = {"a", "b", "c", "dir", "file.txt", "日本語.txt", "テスト", "x y", "ドライブ", ".hidden", "long"};
  auto rname = [&]() {
    uint32_t k = R(14);
    if (k < 11) return std::string(names[k]) + (R(3) ? std::to_string(R(40)) : "");
    if (k == 11) return std::string(255, 'n') + "";
    if (k == 12) return std::string(250, 'm') + std::to_string(R(9999));
    return std::string("n") + std::to_string(R(1000));
  };
  auto paths_of = [&](MNode::Kind k) { std::vector<std::string> r; for (auto& kv : m.all()) if (kv.second->kind == k) r.push_back(kv.first); return r; };
  auto rdir = [&]() { auto d = paths_of(MNode::Dir); d.push_back(""); return d[R((uint32_t)d.size())]; };
  auto join = [](const std::string& d, const std::string& n) { return d.empty() ? n : d + "/" + n; };
  auto ino_of = [&](const std::string& p, uint64_t* ino) { return p.empty() ? (*ino = vol->root(), 0) : vol->resolve(p, ino); };
  int ops = 0, mism = 0, badrc = 0;
  auto expect = [&](int rc, int want, const std::string& what) {
    // want == 0: 成功を期待。want < 0: その errno で失敗を期待。
    if (rc != want) { if (badrc < 20) std::fprintf(stderr, "UNEXPECTED rc %d (want %d): %s\n", rc, want, what.c_str()); badrc++; }
  };
  const size_t sizes[] = {0, 1, 11, 511, 512, 513, 1023, 1024, 1025, 2047, 4095, 4096, 4097, 8192, 12 * 1024, 12 * 1024 + 1, 65536, 270 * 1024, 300 * 1024};
  for (int it = 0; it < nops; it++) {
    uint32_t k = R(100);
    if (k < 12) {   // mkdir
      std::string d = rdir(), n = rname(), p = join(d, n);
      std::string nn; MP par = m.parent(p, &nn);
      uint64_t di; int rc = ino_of(d, &di); if (!rc) { uint64_t ino; rc = vol->mkdir(di, n, 0755, &ino); }
      if (par->kids.count(nn)) expect(rc, -EEXIST, "mkdir exists " + p);
      else { expect(rc, 0, "mkdir " + p); if (!rc) { auto x = std::make_shared<MNode>(); x->kind = MNode::Dir; par->kids[nn] = x; } }
    } else if (k < 40) {   // create + write
      std::string d = rdir(), n = rname(), p = join(d, n);
      std::string nn; MP par = m.parent(p, &nn);
      uint64_t di, ino = 0; int rc = ino_of(d, &di); if (!rc) rc = vol->create(di, n, 0644, &ino);
      if (par->kids.count(nn)) { expect(rc, -EEXIST, "create exists " + p); }
      else {
        expect(rc, 0, "create " + p);
        if (!rc) {
          auto x = std::make_shared<MNode>(); par->kids[nn] = x;
          size_t sz = sizes[R(sizeof sizes / sizeof *sizes)]; unsigned sd = R(256);
          if (sz) { auto b = fill(0, sz, sd); int64_t w = vol->write(ino, 0, b.data(), b.size()); expect(w == (int64_t)sz ? 0 : (int)w, 0, "write " + p); if (w == (int64_t)sz) fill_pattern(&x->data, 0, sz, sd); else { x->data.assign(b.begin(), b.begin() + std::max<int64_t>(w, 0)); } }
        }
      }
    } else if (k < 55) {   // 既存ファイルへ書く (上書き / 拡張 / 疎)
      auto fs = paths_of(MNode::File); if (fs.empty()) continue;
      std::string p = fs[R((uint32_t)fs.size())]; MP x = m.find(p);
      uint64_t off = R(4) == 0 ? x->data.size() + R(20000) : (x->data.empty() ? 0 : R((uint32_t)x->data.size()));
      size_t len = 1 + R(R(3) == 0 ? 70000 : 3000); unsigned sd = R(256);
      uint64_t ino; int rc = vol->resolve(p, &ino); if (!rc) { auto b = fill(off, len, sd); int64_t w = vol->write(ino, off, b.data(), b.size()); rc = w == (int64_t)len ? 0 : (w < 0 ? (int)w : -EIO); }
      expect(rc, 0, "write2 " + p); if (!rc) fill_pattern(&x->data, off, len, sd);
    } else if (k < 63) {   // truncate
      auto fs = paths_of(MNode::File); if (fs.empty()) continue;
      std::string p = fs[R((uint32_t)fs.size())]; MP x = m.find(p);
      uint64_t sz = R(3) == 0 ? 0 : (R(2) ? R((uint32_t)x->data.size() + 1) : x->data.size() + R(30000));
      uint64_t ino; int rc = vol->resolve(p, &ino); if (!rc) rc = vol->truncate(ino, sz);
      expect(rc, 0, "truncate " + p); if (!rc) x->data.resize(sz, 0);
    } else if (k < 73) {   // unlink (ファイル / リンク)
      auto fs = paths_of(MNode::File); auto ls = paths_of(MNode::Link); fs.insert(fs.end(), ls.begin(), ls.end());
      if (fs.empty()) continue;
      std::string p = fs[R((uint32_t)fs.size())]; std::string d, n; split_path(p, &d, &n);
      uint64_t di; int rc = ino_of(d, &di); if (!rc) rc = vol->unlink(di, n);
      expect(rc, 0, "unlink " + p); if (!rc) { std::string nn; MP par = m.parent(p, &nn); par->kids.erase(nn); }
    } else if (k < 78) {   // rmdir (空でなければ ENOTEMPTY を期待)
      auto ds = paths_of(MNode::Dir); if (ds.empty()) continue;
      std::string p = ds[R((uint32_t)ds.size())]; if (p == "lost+found") continue;
      MP x = m.find(p); std::string d, n; split_path(p, &d, &n);
      uint64_t di; int rc = ino_of(d, &di); if (!rc) rc = vol->rmdir(di, n);
      if (!x->kids.empty()) expect(rc, -ENOTEMPTY, "rmdir nonempty " + p);
      else { expect(rc, 0, "rmdir " + p); if (!rc) { std::string nn; MP par = m.parent(p, &nn); par->kids.erase(nn); } }
    } else if (k < 90) {   // rename
      auto all = m.all(); if (all.empty()) continue;
      auto src = all[R((uint32_t)all.size())];
      if (src.first == "lost+found") continue;
      std::string dd = rdir(), nn2 = R(3) == 0 ? rname() : std::string();
      std::string dstp;
      if (nn2.empty()) { auto cand = all[R((uint32_t)all.size())]; dstp = cand.first; if (cand.first == "lost+found") continue; }   // 既存の名前へ (上書き)
      else dstp = join(dd, nn2);
      std::string sn, dn; MP sp = m.parent(src.first, &sn); MP dp = m.parent(dstp, &dn);
      if (!dp) continue;   // 親が無い (別ファイルの下など)
      MP dst = dp->kids.count(dn) ? dp->kids[dn] : nullptr;
      int want = 0;
      if (dst && dst == src.second) want = 0;   // 同じ inode: 何もしない
      else if (src.second->kind == MNode::Dir && in_subtree(src.second, dp)) want = -EINVAL;
      else if (dst && (dst->kind == MNode::Dir) != (src.second->kind == MNode::Dir)) want = src.second->kind == MNode::Dir ? -ENOTDIR : -EISDIR;
      else if (dst && dst->kind == MNode::Dir && !dst->kids.empty()) want = -ENOTEMPTY;
      std::string sd_, sn_, dd_, dn_; split_path(src.first, &sd_, &sn_); split_path(dstp, &dd_, &dn_);
      uint64_t a, b2; int rc = ino_of(sd_, &a); if (!rc) rc = ino_of(dd_, &b2); if (!rc) rc = vol->rename(a, sn_, b2, dn_);
      expect(rc, want, "rename " + src.first + " -> " + dstp);
      if (rc == 0 && want == 0 && !(dst && dst == src.second)) { dp->kids[dn] = src.second; sp->kids.erase(sn); }
    } else if (k < 95) {   // symlink (高速: < 60 バイト / 低速)
      std::string d = rdir(), n = rname(), p = join(d, n); std::string nn; MP par = m.parent(p, &nn);
      std::string tgt = R(2) ? "/some/target/" + std::to_string(R(1000)) : std::string(60 + R(900), 't');
      uint64_t di, ino; int rc = ino_of(d, &di); if (!rc) rc = vol->symlink(di, n, tgt, &ino);
      if (par->kids.count(nn)) expect(rc, -EEXIST, "symlink exists");
      else { expect(rc, 0, "symlink " + p); if (!rc) { auto x = std::make_shared<MNode>(); x->kind = MNode::Link; x->target = tgt; par->kids[nn] = x; } }
    } else {   // hard link
      auto fs = paths_of(MNode::File); if (fs.empty()) continue;
      std::string src = fs[R((uint32_t)fs.size())]; std::string d = rdir(), n = rname(), p = join(d, n); std::string nn; MP par = m.parent(p, &nn);
      uint64_t di, ti; int rc = ino_of(d, &di); if (!rc) rc = vol->resolve(src, &ti); if (!rc) rc = vol->link(di, n, ti);
      if (par->kids.count(nn)) expect(rc, -EEXIST, "link exists");
      else { expect(rc, 0, "link " + p); if (!rc) par->kids[nn] = m.find(src); }
    }
    ops++;
    if (it % 60 == 59) { int rc = vol->sync(); CHECK(rc == 0); mism += compare_fs(*vol, m, "checkpoint"); }
  }
  CHECK(vol->sync() == 0);
  mism += compare_fs(*vol, m, "final");
  vol.reset();
  // 再マウント (読み取り専用・新しいページキャッシュ) して、ディスクに書かれた内容が一致すること
  dev = open_rw(img);
  auto ro = Ext2Volume::mount(dev, &err);
  if (!ro) { std::fprintf(stderr, "remount failed %d\n", err); return 1; }
  mism += compare_fs(*ro, m, "remount");
  ro.reset();
  if (!dump.empty()) dump_model(m, dump);
  std::fprintf(stderr, "model: %d ops, %d mismatches, %d unexpected results (seed %u)\n", ops, mism, badrc, seed);
  return (mism || badrc || g_fail) ? 1 : 0;
}

// ---- --noop
static int noop_run(const std::string& img) {
  std::string before = slurp_hash(img);
  { BlockDevicePtr dev = open_rw(img); int err = 0; auto vol = Ext2Volume::mount_rw(dev, &err);
    if (!vol) return 1;
    CHECK(vol->writable());
    std::vector<FsDirEntry> e; CHECK(vol->readdir(vol->root(), &e) == 0); CHECK(!e.empty()); }
  std::string after = slurp_hash(img);
  CHECK(before == after);
  std::fprintf(stderr, "noop: image %s\n", before == after ? "unchanged" : "CHANGED");
  return g_fail ? 1 : 0;
}

// ---- --refuse
static int refuse_run(const std::string& img) {
  std::string before = slurp_hash(img);
  {
    BlockDevicePtr dev = open_rw(img); int err = 0; auto vol = Ext2Volume::mount_rw(dev, &err);
    if (!vol) { std::fprintf(stderr, "mount failed %d\n", err); return 1; }
    CHECK(!vol->writable());
    uint64_t ino = 0;
    CHECK(vol->create(vol->root(), "x", 0644, &ino) == -EROFS);
    CHECK(vol->mkdir(vol->root(), "d", 0755, &ino) == -EROFS);
    CHECK(vol->unlink(vol->root(), "lost+found") == -EROFS);
    CHECK(vol->sync() == 0);
    std::vector<FsDirEntry> e; CHECK(vol->readdir(vol->root(), &e) == 0);   // 読み取りはできる
  }
  CHECK(before == slurp_hash(img));
  std::fprintf(stderr, "refuse: %s\n", g_fail ? "FAIL" : "ok (読み取り専用・イメージ不変)");
  return g_fail ? 1 : 0;
}

// ---- --enospc
static int enospc_run(const std::string& img) {
  BlockDevicePtr dev = open_rw(img); int err = 0; auto vol = Ext2Volume::mount_rw(dev, &err);
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
  // 2. 満杯でも作成は失敗するだけで壊れない (inode は空いていても、ディレクトリ/データのブロックが無い)
  uint64_t x; int rc = vol->mkdir(vol->root(), "d", 0755, &x); CHECK(rc == 0 || rc == -ENOSPC);
  rc = vol->create(vol->root(), "f", 0644, &x);
  if (rc == 0) { int64_t ww = vol->write(x, 0, chunk.data(), 100); CHECK(ww == -ENOSPC || ww == 100); }
  CHECK(vol->sync() == 0);
  // 3. 削除で回復
  CHECK(vol->unlink(vol->root(), "big") == 0);
  CHECK(vol->sync() == 0);
  uint64_t y; CHECK(vol->create(vol->root(), "after", 0644, &y) == 0);
  w = vol->write(y, 0, chunk.data(), chunk.size()); CHECK(w == (int64_t)chunk.size());
  // 4. inode 枯渇: 空ファイルを作り続ける
  int made = 0; int lastrc = 0;
  for (int i = 0; i < 100000; i++) { uint64_t t; lastrc = vol->create(vol->root(), "i" + std::to_string(i), 0644, &t); if (lastrc) break; made++; }
  std::fprintf(stderr, "enospc: created %d files, last -> %d\n", made, lastrc);
  CHECK(lastrc == -ENOSPC || lastrc == 0);
  CHECK(vol->sync() == 0);
  for (int i = 0; i < made; i++) CHECK(vol->unlink(vol->root(), "i" + std::to_string(i)) == 0);
  CHECK(vol->sync() == 0);
  uint64_t z; CHECK(vol->create(vol->root(), "again", 0644, &z) == 0);
  vol.reset();
  return g_fail ? 1 : 0;
}

int main(int argc, char** argv) {
  if (argc >= 4 && std::string(argv[1]) == "--ops") return ops_run(argv[2], argv[3]);
  if (argc >= 5 && std::string(argv[1]) == "--model") return model_run(argv[2], (unsigned)std::atoi(argv[3]), std::atoi(argv[4]), argc >= 6 ? argv[5] : "");
  if (argc >= 3 && std::string(argv[1]) == "--noop") return noop_run(argv[2]);
  if (argc >= 3 && std::string(argv[1]) == "--refuse") return refuse_run(argv[2]);
  if (argc >= 3 && std::string(argv[1]) == "--enospc") return enospc_run(argv[2]);
  std::fprintf(stderr, "usage: see header\n");
  return 2;
}
