// SPDX-License-Identifier: GPL-2.0-or-later
// 実 NTFS イメージ(tools/gen_ntfs_fixture.sh が mkntfs + ntfs-3g で作成)を fs/ntfs 経由で読み、
// マニフェスト(サイズ・sha256)と照合する。使い方: usbmem_ntfs_tests <fixture_dir>
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

#include "../../tests/fake_bot.h"
#include "usbmem/devices.h"
#include "usbmem/ntfs_volume.h"
#include "usbmem/partition.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

// ---- 最小 SHA-256
struct Sha256 {
  uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
  uint8_t buf[64]; size_t n = 0; uint64_t total = 0;
  static uint32_t rotr(uint32_t x, int k) { return (x >> k) | (x << (32 - k)); }
  void block(const uint8_t* p) {
    static const uint32_t K[64] = {0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4*i] << 24 | p[4*i+1] << 16 | p[4*i+2] << 8 | p[4*i+3];
    for (int i = 16; i < 64; i++) {
      uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3), s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2] >> 10);
      w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
    for (int i = 0; i < 64; i++) {
      uint32_t t1 = hh + (rotr(e,6)^rotr(e,11)^rotr(e,25)) + ((e&f)^(~e&g)) + K[i] + w[i];
      uint32_t t2 = (rotr(a,2)^rotr(a,13)^rotr(a,22)) + ((a&b)^(a&c)^(b&c));
      hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
  }
  void update(const uint8_t* p, size_t len) {
    total += len;
    while (len) { size_t c = std::min(len, 64 - n); std::memcpy(buf + n, p, c); n += c; p += c; len -= c; if (n == 64) { block(buf); n = 0; } }
  }
  std::string hex() {
    uint64_t bits = total * 8; uint8_t pad = 0x80; update(&pad, 1); uint8_t z = 0;
    while (n != 56) update(&z, 1);
    uint8_t l[8]; for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i)); update(l, 8);
    char o[65]; for (int i = 0; i < 8; i++) std::snprintf(o + 8 * i, 9, "%08x", h[i]); return o;
  }
};

#include <csignal>
#include <map>
#include <unistd.h>

// 元イメージを書き換えずに、一部バイトだけ差し替えて見せる読み取りデバイス (破損イメージ fuzz 用)。
class MutatedDevice final : public usbmem::BlockDevice {
 public:
  MutatedDevice(usbmem::BlockDevicePtr base, std::map<uint64_t, uint8_t> m) : base_(std::move(base)), m_(std::move(m)) {}
  usbmem::Status read_at(uint64_t off, void* buf, size_t len, const usbmem::IoOptions& o = {}) override {
    usbmem::Status st = base_->read_at(off, buf, len, o);
    if (!st.ok()) return st;
    for (auto it = m_.lower_bound(off); it != m_.end() && it->first < off + len; ++it) static_cast<uint8_t*>(buf)[it->first - off] = it->second;
    return st;
  }
  usbmem::Status write_at(uint64_t, const void*, size_t, const usbmem::IoOptions& = {}) override { return usbmem::Status::Make(usbmem::Errc::ReadOnly); }
  usbmem::Status flush() override { return usbmem::Status::Ok(); }
  usbmem::Status close() override { return usbmem::Status::Ok(); }
  uint64_t size() const override { return base_->size(); }
  uint32_t logical_block_size() const override { return base_->logical_block_size(); }
  bool is_read_only() const override { return true; }
  std::string describe() const override { return "mutated"; }
 private:
  usbmem::BlockDevicePtr base_;
  std::map<uint64_t, uint8_t> m_;
};

static void walk(usbmem::NtfsVolume& v, uint64_t dir, int depth, int& budget) {
  if (depth > 8 || budget <= 0) return;
  std::vector<usbmem::NtfsDirEntry> es;
  if (v.readdir(dir, &es) != 0) return;
  std::vector<uint8_t> buf(8192);
  for (auto& e : es) {
    if (e.name == "." || e.name == ".." || budget-- <= 0) continue;
    usbmem::NtfsStat st;
    if (v.stat(e.ino, &st) != 0) continue;
    if (st.is_dir()) walk(v, e.ino, depth + 1, budget);
    else { uint64_t off = 0; for (int k = 0; k < 4; k++) { int64_t n = v.read(e.ino, off, buf.data(), buf.size()); if (n <= 0) break; off += (uint64_t)n; } }
  }
}

static usbmem::BlockDevicePtr open_ntfs_partition(const std::string& img, int nth) {
  usbmem::BlockDevicePtr base;
  // 4Kn ディスクのイメージは USBMEM_IMG_BS=4096 を指定 (デバイスの論理ブロックサイズとして与える。実機では MscDevice が申告する)
  const char* bse = std::getenv("USBMEM_IMG_BS");
  uint32_t bsz = bse ? (uint32_t)std::atoi(bse) : 512;
  if (!usbmem::RawImageDevice::open_path(img, true, bsz, &base).ok()) return nullptr;
  usbmem::PartitionTable pt;
  if (!usbmem::read_partition_table(*base, &pt).ok()) return nullptr;
  if (pt.whole_device_volume) return base;
  int k = 0;
  for (auto& p : pt.partitions) {
    if (p.fs != usbmem::FsType::Ntfs) continue;
    if (k++ != nth) continue;
    usbmem::BlockDevicePtr sub;
    if (!usbmem::SubRangeDevice::create(base, p.start_bytes, p.size_bytes, true, &sub).ok()) return nullptr;
    return sub;
  }
  return nullptr;
}

// 使い方: usbmem_ntfs_tests --fuzz <fixture_dir> <iterations> <seed>
static int fuzz(const std::string& dir, int iters, unsigned seed, const std::string& image = "ntfs_basic.img") {
  // image にパーティションテーブルがあれば最初の NTFS パーティション内だけを対象にする
  usbmem::BlockDevicePtr base = open_ntfs_partition(dir + "/" + image, 0);
  if (!base) return 2;
  uint64_t x = seed * 2654435761u + 1;
  auto rnd = [&]() { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
  int mounted = 0;
  // MFT 位置 (ブートセクタ 0x30 = $MFT の LCN, 0x0D = クラスタあたりセクタ数) 周辺を重点的に壊す
  uint8_t bs[512] = {0};
  uint64_t mft = 0;
  if (base->read_at(0, bs, 512).ok()) {
    uint64_t lcn = 0; std::memcpy(&lcn, bs + 0x30, 8);
    mft = lcn * bs[0x0D] * 512ull;
    if (mft + (512u << 10) > base->size()) mft = 0;
  }
  for (int it = 0; it < iters; it++) {
    std::map<uint64_t, uint8_t> m;
    int nmut = 1 + (int)(rnd() % 24);
    for (int k = 0; k < nmut; k++) {
      // 境界: 先頭セクタ / MFT 周辺 (先頭 6MiB) を重点的に。残りは全域。
      uint64_t off = (mft && rnd() % 2 == 0) ? mft + rnd() % (512u << 10) : (rnd() % 4 == 0) ? rnd() % 512 : (rnd() % 8 == 0) ? rnd() % base->size() : rnd() % (6u << 20);
      m[off] = (uint8_t)(rnd() % 3 == 0 ? 0xFF : rnd());
    }
    if (const char* lp = std::getenv("USBMEM_FUZZ_LAST")) {  // 直前の変異を保存 (クラッシュ再現用: --replay)
      if (FILE* lf = std::fopen(lp, "w")) { for (auto& kv : m) std::fprintf(lf, "%llu %u\n", (unsigned long long)kv.first, kv.second); std::fclose(lf); }
    }
    alarm(30);  // ハング検出: 30 秒で SIGALRM → 異常終了
    int err = 0;
    auto dev = std::make_shared<MutatedDevice>(base, m);
    auto v = usbmem::NtfsVolume::mount(dev, &err);
    if (v) { mounted++; int budget = 1200; walk(*v, v->root(), 0, budget); }
    alarm(0);
  }
  std::fprintf(stderr, "fuzz: %d iterations (seed %u), %d mounted, no crash/hang\n", iters, seed, mounted);
  return 0;
}

// パーティションテーブル付きイメージ (Windows が作ったディスク等) から n 番目の NTFS パーティションを BlockDevice として得る。
// 使い方: --manifest <image> <manifest> [n番目のNTFS]  manifest の全エントリを sha256 で検証する (追加の仮定なし)。
static int verify_manifest(const std::string& img, const std::string& manifest, int nth) {
  auto dev = open_ntfs_partition(img, nth);
  if (!dev) { std::fprintf(stderr, "no NTFS partition #%d in %s\n", nth, img.c_str()); return 2; }
  int err = 0;
  auto vol = usbmem::NtfsVolume::mount(dev, &err);
  if (!vol) { std::fprintf(stderr, "mount failed: errno %d\n", err); return 1; }
  std::ifstream mf(manifest);
  std::string line; int files = 0, fails = 0;
  while (std::getline(mf, line)) {
    std::istringstream is(line);
    std::string kind, size, sha; is >> kind >> size >> sha; std::string path; std::getline(is >> std::ws, path);
    uint64_t ino = 0; int rc = vol->resolve(path, &ino);
    if (rc) { std::fprintf(stderr, "FAIL resolve %s -> %d\n", path.c_str(), rc); fails++; continue; }
    usbmem::NtfsStat st; if (vol->stat(ino, &st)) { std::fprintf(stderr, "FAIL stat %s\n", path.c_str()); fails++; continue; }
    if (kind == "d") { if (!st.is_dir()) { std::fprintf(stderr, "FAIL not dir %s\n", path.c_str()); fails++; } continue; }
    Sha256 h; std::vector<uint8_t> buf(100000); uint64_t off = 0; bool bad = false;
    for (;;) { int64_t n = vol->read(ino, off, buf.data(), buf.size()); if (n < 0) { std::fprintf(stderr, "FAIL read %s -> %lld\n", path.c_str(), (long long)n); bad = true; break; } if (!n) break; h.update(buf.data(), (size_t)n); off += (uint64_t)n; }
    if (!bad && (off != std::stoull(size) || st.size != std::stoull(size) || h.hex() != sha)) { std::fprintf(stderr, "FAIL content %s (size %llu vs %s)\n", path.c_str(), (unsigned long long)off, size.c_str()); bad = true; }
    if (bad) fails++; else files++;
  }
  std::fprintf(stderr, "manifest: %d files ok, %d failures\n", files, fails);
  return fails ? 1 : 0;
}

// 使い方: --cat <image> <path> [n番目のNTFS]  ファイルの内容を stdout へ (診断用)
static int cat_file(const std::string& img, const std::string& path, int nth) {
  auto dev = open_ntfs_partition(img, nth);
  if (!dev) return 2;
  int err = 0; auto vol = usbmem::NtfsVolume::mount(dev, &err);
  if (!vol) return 1;
  uint64_t ino = 0; if (vol->resolve(path, &ino)) return 1;
  std::vector<uint8_t> b(65536); uint64_t off = 0;
  for (;;) { int64_t n = vol->read(ino, off, b.data(), b.size()); if (n <= 0) break; std::fwrite(b.data(), 1, (size_t)n, stdout); off += (uint64_t)n; }
  return 0;
}

static int replay(const std::string& dir, const char* file) {
  usbmem::BlockDevicePtr base;
  if (!usbmem::RawImageDevice::open_path(dir + "/ntfs_basic.img", true, 512, &base).ok()) return 2;
  std::map<uint64_t, uint8_t> m; unsigned long long o; unsigned v;
  FILE* f = std::fopen(file, "r"); if (!f) return 2;
  while (std::fscanf(f, "%llu %u", &o, &v) == 2) m[o] = (uint8_t)v;
  std::fclose(f);
  int err = 0; auto v2 = usbmem::NtfsVolume::mount(std::make_shared<MutatedDevice>(base, m), &err);
  std::fprintf(stderr, "replay: mount %s (err %d)\n", v2 ? "ok" : "failed", err);
  if (v2) { int budget = 1200; walk(*v2, v2->root(), 0, budget); }
  return 0;
}

int main(int argc, char** argv) {
  if (argc >= 4 && std::string(argv[1]) == "--cat") return cat_file(argv[2], argv[3], argc >= 5 ? std::atoi(argv[4]) : 0);
  if (argc >= 4 && std::string(argv[1]) == "--manifest") return verify_manifest(argv[2], argv[3], argc >= 5 ? std::atoi(argv[4]) : 0);
  if (argc >= 4 && std::string(argv[1]) == "--replay") return replay(argv[2], argv[3]);
  if (argc >= 5 && std::string(argv[1]) == "--fuzz") return fuzz(argv[2], std::atoi(argv[3]), (unsigned)std::atoi(argv[4]), argc >= 6 ? argv[5] : "ntfs_basic.img");
  if (argc < 2) { std::fprintf(stderr, "usage: %s <fixture_dir>\n", argv[0]); return 2; }
  std::string dir = argv[1];
  usbmem::BlockDevicePtr dev;
  if (!usbmem::RawImageDevice::open_path(dir + "/ntfs_basic.img", true, 512, &dev).ok()) { std::fprintf(stderr, "cannot open image\n"); return 2; }
  int err = 0;
  auto vol = usbmem::NtfsVolume::mount(dev, &err);
  if (!vol) { std::fprintf(stderr, "mount failed: errno %d\n", err); return 1; }
  std::fprintf(stderr, "mounted\n");

  // ルート列挙: マニフェストの最上位エントリと一致する
  std::vector<usbmem::NtfsDirEntry> ents;
  CHECK(vol->readdir(vol->root(), &ents) == 0);
  std::set<std::string> names;
  for (auto& e : ents) names.insert(e.name);

  std::ifstream mf(dir + "/ntfs_basic.manifest");
  std::string line; int files = 0;
  while (std::getline(mf, line)) {
    std::istringstream is(line);
    std::string kind, size, sha; is >> kind >> size >> sha; std::string path; std::getline(is >> std::ws, path);
    uint64_t ino = 0;
    int rc = vol->resolve(path, &ino);
    if (rc) { std::fprintf(stderr, "resolve %s -> %d\n", path.c_str(), rc); g_fail++; continue; }
    usbmem::NtfsStat st; CHECK(vol->stat(ino, &st) == 0);
    if (kind == "d") { CHECK(st.is_dir()); continue; }
    CHECK(!st.is_dir());
    CHECK(st.size == std::stoull(size));
    Sha256 h; std::vector<uint8_t> buf(100000); uint64_t off = 0;
    for (;;) { int64_t n = vol->read(ino, off, buf.data(), buf.size()); if (n < 0) { std::fprintf(stderr, "read %s -> %lld\n", path.c_str(), (long long)n); g_fail++; break; } if (!n) break; h.update(buf.data(), (size_t)n); off += (uint64_t)n; }
    CHECK(off == st.size);
    if (sha != "-") { bool ok = h.hex() == sha; if (!ok) std::fprintf(stderr, "sha mismatch %s\n", path.c_str()); CHECK(ok); }
    files++;
    if (path.find('/') == std::string::npos) CHECK(names.count(path) == 1);
  }
  CHECK(files >= 800 + 8);
  // many/ には 800 件が重複・欠落なく並ぶ (index allocation 経由の列挙)
  uint64_t many = 0; CHECK(vol->resolve("many", &many) == 0);
  std::vector<usbmem::NtfsDirEntry> me; CHECK(vol->readdir(many, &me) == 0);
  std::set<std::string> mn; for (auto& e : me) if (e.name != "." && e.name != "..") mn.insert(e.name);  // dots は VFS 同様に含まれる
  CHECK(mn.size() == 800 && me.size() == 802);
  for (int k = 1; k <= 800; k++) CHECK(mn.count("file_" + std::to_string(k) + ".txt") == 1);
  uint64_t x; CHECK(vol->resolve("/nonexistent", &x) != 0);
  std::vector<usbmem::NtfsDirEntry> sub; CHECK(vol->readdir(vol->root(), &sub) == 0);
  vol.reset();
  // 同じイメージを「USB Mass Storage デバイス」(模擬 BOT) 経由で読む: BlockDevice → MSC → fs/ntfs の全経路
  {
    auto bot = std::make_unique<fake::BotDevice>(dev->size() / 512, 512);
    bot->backing = [dev](uint64_t lba, uint8_t* out) { if (!dev->read_at(lba * 512, out, 512).ok()) std::memset(out, 0, 512); };
    usbmem::BlockDevicePtr usb;
    usbmem::MscOptions mo; mo.retry_delay_ms = 1;
    CHECK(usbmem::MscDevice::open(std::move(bot), mo, &usb).ok());
    auto uv = usb ? usbmem::NtfsVolume::mount(usb, &err) : nullptr;
    CHECK(uv != nullptr);
    if (uv) {
      for (const char* f : {"big.bin", "cdir/text.txt", "cdir/mixed.bin"}) {
        uint64_t ino = 0; CHECK(uv->resolve(f, &ino) == 0);
        Sha256 h; std::vector<uint8_t> b(70000); uint64_t off = 0;
        for (;;) { int64_t n = uv->read(ino, off, b.data(), b.size()); if (n <= 0) break; h.update(b.data(), (size_t)n); off += (uint64_t)n; }
        std::string want;
        { std::ifstream m2(dir + "/ntfs_basic.manifest"); std::string l; while (std::getline(m2, l)) { std::istringstream is2(l); std::string k, sz, sh, pth; is2 >> k >> sz >> sh; std::getline(is2 >> std::ws, pth); if (pth == f) want = sh; } }
        bool ok = !want.empty() && h.hex() == want; if (!ok) std::fprintf(stderr, "usb path sha mismatch %s\n", f);
        CHECK(ok);
      }
    }
  }
  // マウント/読み取り/アンマウントを繰り返してもリーク・UAF しない (ASan で検証)
  for (int k = 0; k < 20; k++) {
    auto v2 = usbmem::NtfsVolume::mount(dev, &err);
    CHECK(v2 != nullptr);
    if (!v2) break;
    uint64_t ino = 0; CHECK(v2->resolve("many/file_7.txt", &ino) == 0);
    char b[64]; CHECK(v2->read(ino, 0, b, sizeof b) > 0);
  }
  std::fprintf(stderr, "%d files verified, %d failures\n", files, g_fail);
  return g_fail ? 1 : 0;
}
