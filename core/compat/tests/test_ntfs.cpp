// SPDX-License-Identifier: GPL-2.0-or-later
// 実 NTFS イメージ(tools/gen_ntfs_fixture.sh が mkntfs + ntfs-3g で作成)を fs/ntfs 経由で読み、
// マニフェスト(サイズ・sha256)と照合する。使い方: usbmem_ntfs_tests <fixture_dir>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

#include "usbmem/devices.h"
#include "usbmem/ntfs_volume.h"

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

int main(int argc, char** argv) {
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
  CHECK(files >= 800 + 5);
  // many/ には 800 件が重複・欠落なく並ぶ (index allocation 経由の列挙)
  uint64_t many = 0; CHECK(vol->resolve("many", &many) == 0);
  std::vector<usbmem::NtfsDirEntry> me; CHECK(vol->readdir(many, &me) == 0);
  std::set<std::string> mn; for (auto& e : me) if (e.name != "." && e.name != "..") mn.insert(e.name);  // dots は VFS 同様に含まれる
  CHECK(mn.size() == 800 && me.size() == 802);
  for (int k = 1; k <= 800; k++) CHECK(mn.count("file_" + std::to_string(k) + ".txt") == 1);
  uint64_t x; CHECK(vol->resolve("/nonexistent", &x) != 0);
  std::vector<usbmem::NtfsDirEntry> sub; CHECK(vol->readdir(vol->root(), &sub) == 0);
  vol.reset();
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
