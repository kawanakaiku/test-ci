// SPDX-License-Identifier: GPL-2.0-or-later
// 暗号ボリューム (LUKS / BitLocker) の書き込み可の復号デバイスの上で、書き込み可の FS (ext4 / ext2 / NTFS / FAT) を動かす検証 (USBMEM_NTFS_WRITE=ON + USBMEM_CRYPT_WRITE=ON のビルド用)。
// 独立実装での突き合わせはシェル側 (tools/run_crypt_fs_tests.sh): 書いた後の暗号イメージを cryptsetup + Python (または実カーネルの dm-crypt) で復号し、e2fsck / ntfsfix / fsck.vfat で整合を見て、
// このツールの dump (平文イメージを自前ドライバで読む) とモデルのダンプを比べる。
//   run <img> <fs> <cred> <seed> <nops> <model_dump> [--full]
//       暗号イメージを writable で開き (USBMEM_TRANSFORM の writable)、FS を mount_rw、乱数の操作列を実行してメモリ上のモデルと突き合わせる。
//       unmount → 復号デバイスを閉じる → 読み取り専用で開き直してモデルと全体比較。model_dump に tools/ops_ref.py --tree と同じ書式でモデルを書く。
//       既定は「新規作成のみ」(mkdir / create+write / 追記)、--full で上書き・truncate・unlink・rmdir も行う。
//   dump <plain_img> <fs> <out>     平文のイメージ (独立実装で復号したもの) を自前ドライバで読み取り専用で読み、ツリーを出力する
//   decrypt <img> <cred> <out> <bytes>  暗号イメージを自前の層で復号して先頭 bytes バイトを書く (BitLocker: libbde が NTFS 以外を開けないので、bitlocker_synth.py での暗号化し直しとの照合に使う)
//   ro <img> <fs> <cred>            writable を要求せずに開く: mount_rw は書き込み不可 (writable()==false) で、書き込みは -EROFS。イメージは変わらない (シェルが前後を比べる)
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
#include <string>
#include <vector>

#include "sha256.h"
#include "usbmem/devices.h"
#include "usbmem/fs_volume.h"
#include "usbmem/partition.h"
#include "usbmem/transform.h"
#ifdef USBMEM_CFW_EXT4
#include "usbmem/ext4_volume.h"
#endif
#ifdef USBMEM_CFW_EXT2
#include "usbmem/ext2_volume.h"
#endif
#ifdef USBMEM_CFW_FAT
#include "usbmem/fat_volume.h"
#endif
#ifdef USBMEM_CFW_NTFS
#include "usbmem/ntfs_volume.h"
#endif

using namespace usbmem;

static std::unique_ptr<FsVolume> mount_fs(const std::string& fs, const BlockDevicePtr& dev, bool rw, int* err) {
#ifdef USBMEM_CFW_EXT4
  if (fs == "ext4") return rw ? std::unique_ptr<FsVolume>(Ext4Volume::mount_rw(dev, err)) : std::unique_ptr<FsVolume>(Ext4Volume::mount(dev, err));
#endif
#ifdef USBMEM_CFW_EXT2
  if (fs == "ext2") return rw ? std::unique_ptr<FsVolume>(Ext2Volume::mount_rw(dev, err)) : std::unique_ptr<FsVolume>(Ext2Volume::mount(dev, err));
#endif
#ifdef USBMEM_CFW_FAT
  if (fs == "fat") return rw ? std::unique_ptr<FsVolume>(FatVolume::mount_rw(dev, err)) : std::unique_ptr<FsVolume>(FatVolume::mount(dev, err));
#endif
#ifdef USBMEM_CFW_NTFS
  if (fs == "ntfs") return rw ? std::unique_ptr<FsVolume>(NtfsVolume::mount_rw(dev, err)) : std::unique_ptr<FsVolume>(NtfsVolume::mount(dev, err));
#endif
  (void)rw;
  if (err) *err = 38;
  return nullptr;
}

static bool parse_cred(const std::string& spec, Credentials* c) {
  auto set = [](crypto::Secret& s, const std::string& v) { s.assign(reinterpret_cast<const uint8_t*>(v.data()), v.size()); };
  size_t colon = spec.find(':');
  if (colon == std::string::npos) return spec == "none";
  std::string k = spec.substr(0, colon), v = spec.substr(colon + 1);
  if (k == "pw") { set(c->password, v); return true; }
  if (k == "pwfile") {
    std::ifstream f(v, std::ios::binary);
    std::string b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    while (!b.empty() && (b.back() == '\n' || b.back() == '\r')) b.pop_back();
    set(c->password, b);
    return true;
  }
  return false;
}

static BlockDevicePtr open_crypt(const std::string& img, const std::string& cred, bool writable, VolumeInfo* info) {
  BlockDevicePtr raw;
  if (!RawImageDevice::open_path(img, false, 512, &raw).ok()) { std::fprintf(stderr, "cannot open %s\n", img.c_str()); return nullptr; }
  Credentials c;
  if (!parse_cred(cred, &c)) { std::fprintf(stderr, "bad credential spec\n"); return nullptr; }
  OpenOptions o;
  o.writable = writable;
  BlockDevicePtr dec;
  Status s = open_crypto_volume(raw, CryptoKind::Auto, c, o, &dec, info);
  if (!s) { std::fprintf(stderr, "open_crypto_volume: %s\n", errc_name(s.code)); return nullptr; }
  return dec;
}

// ---- ツリーのダンプ (tools/ops_ref.py --tree と同じ書式)
static int dump_tree(FsVolume& vol, std::vector<std::string>* lines) {
  std::vector<std::pair<std::string, uint64_t>> stack{{"", vol.root()}};
  while (!stack.empty()) {
    auto [path, ino] = stack.back();
    stack.pop_back();
    std::vector<FsDirEntry> ents;
    int rc = vol.readdir(ino, &ents);
    if (rc) return rc;
    for (auto& e : ents) {
      if (e.name == "." || e.name == "..") continue;
      if (path.empty() && (e.name == "lost+found" || e.name == "System Volume Information" || e.name[0] == '$')) continue;
      std::string p = path.empty() ? e.name : path + "/" + e.name;
      FsStat st;
      rc = vol.stat(e.ino, &st);
      if (rc) return rc;
      if (st.is_dir()) { lines->push_back("d 0 - " + p); stack.push_back({p, e.ino}); continue; }
      std::vector<uint8_t> d(st.size);
      size_t off = 0;
      while (off < st.size) {
        int64_t r = vol.read(e.ino, off, d.data() + off, st.size - off);
        if (r <= 0) return r < 0 ? int(r) : -EIO;
        off += size_t(r);
      }
      Sha256 h;
      h.update(d.data(), d.size());
      char b[64];
      std::snprintf(b, sizeof b, "f %llu ", (unsigned long long)st.size);
      lines->push_back(std::string(b) + h.hex() + " " + p);
    }
  }
  std::sort(lines->begin(), lines->end());
  return 0;
}

// ---- モデル
struct Node {
  bool dir = false;
  std::vector<uint8_t> data;
};
using Model = std::map<std::string, Node>;  // パス ("a/b") → ノード

static std::vector<std::string> model_lines(const Model& m) {
  std::vector<std::string> l;
  for (auto& kv : m) {
    if (kv.second.dir) { l.push_back("d 0 - " + kv.first); continue; }
    Sha256 h;
    h.update(kv.second.data.data(), kv.second.data.size());
    char b[64];
    std::snprintf(b, sizeof b, "f %zu ", kv.second.data.size());
    l.push_back(std::string(b) + h.hex() + " " + kv.first);
  }
  std::sort(l.begin(), l.end());
  return l;
}

static int compare_lines(const std::vector<std::string>& got, const std::vector<std::string>& want, const char* when) {
  if (got == want) return 0;
  std::set<std::string> g(got.begin(), got.end()), w(want.begin(), want.end());
  int shown = 0;
  for (auto& x : want) if (!g.count(x) && shown++ < 8) std::fprintf(stderr, "[%s] missing/different: %s\n", when, x.c_str());
  for (auto& x : got) if (!w.count(x) && shown++ < 16) std::fprintf(stderr, "[%s] unexpected: %s\n", when, x.c_str());
  std::fprintf(stderr, "[%s] tree mismatch (got %zu entries, want %zu)\n", when, got.size(), want.size());
  return 1;
}

static std::vector<uint8_t> fill(uint64_t off, size_t len, unsigned seed) {
  std::vector<uint8_t> b(len);
  for (size_t i = 0; i < len; i++) b[i] = uint8_t((((off + i) * 7) + seed) & 255);
  return b;
}

static void put(std::vector<uint8_t>* d, uint64_t off, const std::vector<uint8_t>& b) {
  if (d->size() < off + b.size()) d->resize(off + b.size(), 0);
  std::memcpy(d->data() + off, b.data(), b.size());
}

static int cmd_run(int argc, char** argv) {
  if (argc < 8) return 2;
  const std::string img = argv[2], fs = argv[3], cred = argv[4], dumpf = argv[7];
  const unsigned seed = unsigned(std::atoi(argv[5]));
  const int nops = std::atoi(argv[6]);
  const bool full = argc > 8 && std::string(argv[8]) == "--full";
  VolumeInfo info;
  BlockDevicePtr dev = open_crypt(img, cred, true, &info);
  if (!dev) return 2;
  if (dev->is_read_only() || !info.writable) { std::fprintf(stderr, "crypt layer is not writable: %s\n", info.write_note.c_str()); return 3; }
  Model m;
  std::mt19937 rng(seed);
  auto R = [&](uint32_t n) { return n ? uint32_t(rng() % n) : 0u; };
  int bad = 0, ops = 0;
  {
    int err = 0;
    auto vol = mount_fs(fs, dev, true, &err);
    if (!vol || !vol->writable()) { std::fprintf(stderr, "mount_rw failed or not writable (err %d)\n", err); return 1; }
    auto dirs = [&]() { std::vector<std::string> d{""}; for (auto& kv : m) if (kv.second.dir) d.push_back(kv.first); return d; };
    auto files = [&]() { std::vector<std::string> d; for (auto& kv : m) if (!kv.second.dir) d.push_back(kv.first); return d; };
    auto ino_of = [&](const std::string& p, uint64_t* ino) { if (p.empty()) { *ino = vol->root(); return 0; } return vol->resolve(p, ino); };
    auto split = [](const std::string& p, std::string* d, std::string* n) { size_t k = p.rfind('/'); if (k == std::string::npos) { *d = ""; *n = p; } else { *d = p.substr(0, k); *n = p.substr(k + 1); } };
    auto want = [&](int rc, const std::string& what) { if (rc != 0) { if (bad < 12) std::fprintf(stderr, "UNEXPECTED rc %d: %s\n", rc, what.c_str()); bad++; } };
    const size_t sizes[] = {0, 1, 11, 511, 512, 513, 1023, 1025, 4095, 4096, 4097, 8192, 12 * 1024 + 1, 65536, 100 * 1024};
    int serial = 0;
    for (int it = 0; it < nops; it++) {
      uint32_t k = R(100);
      if (!full && k >= 70) k = 40 + (k % 30);  // 新規作成のみ: 70 未満に寄せる
      auto ds = dirs();
      if (k < 15) {  // mkdir
        std::string d = ds[R(uint32_t(ds.size()))], n = "d" + std::to_string(serial++), p = d.empty() ? n : d + "/" + n;
        if (std::count(p.begin(), p.end(), '/') > 4) continue;
        uint64_t di, ino;
        int rc = ino_of(d, &di);
        if (!rc) rc = vol->mkdir(di, n, 0755, &ino);
        want(rc, "mkdir " + p);
        if (!rc) m[p].dir = true;
      } else if (k < 50) {  // create + write
        std::string d = ds[R(uint32_t(ds.size()))], n = "f" + std::to_string(serial++) + (R(5) == 0 ? ".txt" : ""), p = d.empty() ? n : d + "/" + n;
        uint64_t di, ino = 0;
        int rc = ino_of(d, &di);
        if (!rc) rc = vol->create(di, n, 0644, &ino);
        want(rc, "create " + p);
        if (rc) continue;
        Node& x = m[p];
        size_t sz = sizes[R(sizeof sizes / sizeof *sizes)];
        unsigned sd = R(256);
        if (sz) {
          auto b = fill(0, sz, sd);
          int64_t w = vol->write(ino, 0, b.data(), b.size());
          want(w == int64_t(sz) ? 0 : (w < 0 ? int(w) : -EIO), "write " + p);
          put(&x.data, 0, b);
        }
      } else if (k < 70) {  // 追記
        auto fs2 = files();
        if (fs2.empty()) continue;
        std::string p = fs2[R(uint32_t(fs2.size()))];
        Node& x = m[p];
        uint64_t off = x.data.size();
        size_t len = 1 + R(R(3) == 0 ? 50000 : 3000);
        unsigned sd = R(256);
        uint64_t ino;
        int rc = vol->resolve(p, &ino);
        if (!rc) { auto b = fill(off, len, sd); int64_t w = vol->write(ino, off, b.data(), b.size()); rc = w == int64_t(len) ? 0 : (w < 0 ? int(w) : -EIO); if (!rc) put(&x.data, off, b); }
        want(rc, "append " + p);
      } else if (k < 82) {  // 上書き (Full)
        auto fs2 = files();
        if (fs2.empty()) continue;
        std::string p = fs2[R(uint32_t(fs2.size()))];
        Node& x = m[p];
        if (x.data.empty()) continue;
        uint64_t off = R(uint32_t(x.data.size()));
        size_t len = 1 + R(3000);
        unsigned sd = R(256);
        uint64_t ino;
        int rc = vol->resolve(p, &ino);
        if (!rc) { auto b = fill(off, len, sd); int64_t w = vol->write(ino, off, b.data(), b.size()); rc = w == int64_t(len) ? 0 : (w < 0 ? int(w) : -EIO); if (!rc) put(&x.data, off, b); }
        want(rc, "overwrite " + p);
      } else if (k < 90) {  // truncate (Full)
        auto fs2 = files();
        if (fs2.empty()) continue;
        std::string p = fs2[R(uint32_t(fs2.size()))];
        Node& x = m[p];
        uint64_t sz = R(3) == 0 ? 0 : R(uint32_t(x.data.size() + 1));
        uint64_t ino;
        int rc = vol->resolve(p, &ino);
        if (!rc) rc = vol->truncate(ino, sz);
        want(rc, "truncate " + p);
        if (!rc) x.data.resize(sz, 0);
      } else {  // unlink (Full)
        auto fs2 = files();
        if (fs2.empty()) continue;
        std::string p = fs2[R(uint32_t(fs2.size()))], d, n;
        split(p, &d, &n);
        uint64_t di;
        int rc = ino_of(d, &di);
        if (!rc) rc = vol->unlink(di, n);
        want(rc, "unlink " + p);
        if (!rc) m.erase(p);
      }
      ops++;
      if (it % 40 == 39) {  // 途中でも整合: 実体を読み直して全体比較 (書き込みキャッシュの整合も見る)
        std::vector<std::string> got;
        int rc = dump_tree(*vol, &got);
        if (rc || compare_lines(got, model_lines(m), "mid")) { std::fprintf(stderr, "mid-run mismatch at op %d (rc %d)\n", it, rc); return 1; }
      }
    }
    int rc = vol->sync();
    if (rc) { std::fprintf(stderr, "sync rc %d\n", rc); bad++; }
    std::vector<std::string> got;
    rc = dump_tree(*vol, &got);
    if (rc || compare_lines(got, model_lines(m), "before-unmount")) return 1;
  }  // vol を破棄 = unmount
  Status fl = dev->flush();
  if (!fl) { std::fprintf(stderr, "flush failed\n"); return 1; }
  if (dev->is_read_only()) { std::fprintf(stderr, "crypt layer fell back to read-only during the run\n"); return 1; }
  dev->close();
  dev.reset();
  // 読み取り専用で開き直す (暗号層を通して全体を比較)
  BlockDevicePtr ro = open_crypt(img, cred, false, nullptr);
  if (!ro) return 1;
  int err = 0;
  auto v2 = mount_fs(fs, ro, false, &err);
  if (!v2) { std::fprintf(stderr, "remount failed (%d)\n", err); return 1; }
  std::vector<std::string> got;
  int rc = dump_tree(*v2, &got);
  auto want_l = model_lines(m);
  if (rc || compare_lines(got, want_l, "remount")) return 1;
  std::ofstream f(dumpf);
  for (auto& l : want_l) f << l << "\n";
  std::printf("ok: fs=%s ops=%d bad=%d entries=%zu\n", fs.c_str(), ops, bad, want_l.size());
  return bad ? 1 : 0;
}

static int cmd_dump(int argc, char** argv) {
  if (argc < 5) return 2;
  BlockDevicePtr raw;
  if (!RawImageDevice::open_path(argv[2], true, 512, &raw).ok()) return 2;
  int err = 0;
  auto v = mount_fs(argv[3], raw, false, &err);
  if (!v) { std::fprintf(stderr, "mount failed (%d)\n", err); return 1; }
  std::vector<std::string> l;
  int rc = dump_tree(*v, &l);
  if (rc) { std::fprintf(stderr, "dump failed (%d)\n", rc); return 1; }
  std::ofstream f(argv[4]);
  for (auto& x : l) f << x << "\n";
  return 0;
}

static int cmd_ro(int argc, char** argv) {
  if (argc < 5) return 2;
  VolumeInfo info;
  BlockDevicePtr dev = open_crypt(argv[2], argv[4], false, &info);
  if (!dev) return 2;
  if (!dev->is_read_only()) { std::fprintf(stderr, "crypt layer is writable although writable was not requested\n"); return 1; }
  int err = 0;
  auto v = mount_fs(argv[3], dev, true, &err);  // mount_rw を要求しても、下が読み取り専用なら書き込み不可になる (または失敗する)
  if (!v) { std::printf("ok: mount_rw refused (%d)\n", err); return 0; }
  if (v->writable()) { std::fprintf(stderr, "FS is writable on top of a read-only crypt layer\n"); return 1; }
  uint64_t ino = 0;
  int rc = v->create(v->root(), "must_not_exist", 0644, &ino);
  if (rc == 0) { std::fprintf(stderr, "create succeeded on read-only stack\n"); return 1; }
  std::printf("ok: fs read-only on top of read-only crypt layer (create rc %d)\n", rc);
  return 0;
}

static int cmd_decrypt(int argc, char** argv) {
  if (argc < 6) return 2;
  BlockDevicePtr dev = open_crypt(argv[2], argv[3], false, nullptr);
  if (!dev) return 2;
  uint64_t n = std::min<uint64_t>(std::strtoull(argv[5], nullptr, 0), dev->size());
  std::ofstream f(argv[4], std::ios::binary);
  std::vector<uint8_t> b(1 << 20);
  for (uint64_t o = 0; o < n; o += b.size()) {
    size_t k = size_t(std::min<uint64_t>(b.size(), n - o));
    if (!dev->read_at(o, b.data(), k).ok()) return 1;
    f.write(reinterpret_cast<char*>(b.data()), std::streamsize(k));
  }
  return 0;
}

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: see header of test_cryptfs_write.cpp\n"); return 2; }
  std::string c = argv[1];
  if (c == "run") return cmd_run(argc, argv);
  if (c == "dump") return cmd_dump(argc, argv);
  if (c == "ro") return cmd_ro(argc, argv);
  if (c == "decrypt") return cmd_decrypt(argc, argv);
  return 2;
}
