// SPDX-License-Identifier: Apache-2.0
// exFAT の mkfs (書き込む内容を組み立てるだけ)。
// 配置: ブート領域 12 セクタ + 控え 12 セクタ → FAT (1 本) → クラスタヒープ (ビットマップ → アップケーステーブル → ルート)。
#include <cstdio>

#include "mkfs_internal.h"
#include "usbmem/partition_edit.h"  // make_random_guid

namespace usbmem {
namespace mkfsimpl {

namespace {

#include "mkfs_upcase.inc"

constexpr uint32_t kMaxClusters = 0xFFFFFFF5;
constexpr uint32_t kMinClusters = 16;

uint32_t default_cluster_bytes(uint64_t vol_bytes) {
  constexpr uint64_t MiB = 1024 * 1024, GiB = 1024 * MiB;
  if (vol_bytes < 256 * MiB) return 4096;
  if (vol_bytes <= 32 * GiB) return 32768;
  return 131072;
}

// UTF-8 → UTF-16。不正な並びは false。
bool to_utf16(const std::string& s, std::vector<uint16_t>* out) {
  size_t i = 0;
  while (i < s.size()) {
    uint32_t c = static_cast<unsigned char>(s[i]);
    int n = 0;
    if (c < 0x80) n = 0;
    else if ((c & 0xE0) == 0xC0) { n = 1; c &= 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 2; c &= 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 3; c &= 0x07; }
    else return false;
    for (int k = 1; k <= n; k++) {
      if (i + static_cast<size_t>(k) >= s.size() || (static_cast<unsigned char>(s[i + static_cast<size_t>(k)]) & 0xC0) != 0x80) return false;
      c = (c << 6) | (static_cast<unsigned char>(s[i + static_cast<size_t>(k)]) & 0x3F);
    }
    static const uint32_t minv[4] = {0, 0x80, 0x800, 0x10000};
    if (c < minv[n] || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return false;
    i += static_cast<size_t>(n) + 1;
    if (c >= 0x10000) {
      c -= 0x10000;
      out->push_back(static_cast<uint16_t>(0xD800 + (c >> 10)));
      out->push_back(static_cast<uint16_t>(0xDC00 + (c & 0x3FF)));
    } else out->push_back(static_cast<uint16_t>(c));
  }
  return true;
}

uint32_t upcase_checksum(const std::vector<uint8_t>& t) {
  uint32_t c = 0;
  for (uint8_t b : t) c = ((c << 31) | (c >> 1)) + b;
  return c;
}

uint32_t boot_checksum(const std::vector<uint8_t>& region, uint32_t sector_size) {
  uint32_t c = 0;
  size_t n = static_cast<size_t>(sector_size) * 11;
  for (size_t i = 0; i < n; i++) {
    if (i == 106 || i == 107 || i == 112) continue;
    c = ((c << 31) | (c >> 1)) + region[i];
  }
  return c;
}

}  // namespace

void build_exfat(const Params& p, Build* b) {
  const uint32_t S = p.sector_size;
  if (S < 512 || S > 4096 || !pow2(S)) { b->error("bad-sector-size", "sector size must be 512, 1024, 2048 or 4096"); return; }
  const uint64_t V = p.length / S;
  // ラベル
  std::vector<uint16_t> lab;
  if (!to_utf16(p.opt.label, &lab)) { b->error("bad-label", "the label is not valid UTF-8"); return; }
  if (lab.size() > 11) { b->error("bad-label", "the label is longer than 11 UTF-16 units"); return; }
  for (uint16_t c : lab) {
    if (c < 0x20 || std::strchr("\"*/:<>?\\|", c < 0x80 ? static_cast<int>(c) : 'a') != nullptr) { b->error("bad-label", "the label has a control character or one of \" * / : < > ? \\ |"); return; }
  }

  uint64_t cbytes = p.opt.cluster_size ? p.opt.cluster_size : default_cluster_bytes(V * S);
  if (p.opt.cluster_size && (!pow2(cbytes) || cbytes < S || cbytes > (32u << 20))) { b->error("bad-cluster-size", "the cluster size must be a power of two, at least the sector size, at most 32 MiB"); return; }
  if (cbytes < S) cbytes = S;
  uint64_t spc = cbytes / S;
  const uint64_t fat_off = std::max<uint64_t>(24, 65536 / S);
  uint64_t fat_len = 1, heap_off = 0, cc = 0;
  for (int it = 0; it < 64; it++) {
    heap_off = align_up(fat_off + fat_len, spc);
    cc = V > heap_off ? (V - heap_off) / spc : 0;
    uint64_t need = ceil_div((cc + 2) * 4, S);
    if (need <= fat_len) break;
    fat_len = need;
  }
  if (cc > kMaxClusters) { b->error("too-large", "the volume has too many clusters for exFAT with this cluster size; use a larger cluster size"); return; }
  // ビットマップ・アップケース・ルート
  std::vector<uint8_t> up;
  {
    up.reserve(sizeof(kUpcaseCompressed));
    for (uint16_t w : kUpcaseCompressed) { up.push_back(static_cast<uint8_t>(w)); up.push_back(static_cast<uint8_t>(w >> 8)); }
  }
  uint64_t bm_bytes = ceil_div(cc, 8);
  uint64_t bm_clusters = std::max<uint64_t>(1, ceil_div(bm_bytes, cbytes));
  uint64_t up_clusters = ceil_div(up.size(), cbytes);
  uint64_t used = bm_clusters + up_clusters + 1;
  if (cc < kMinClusters || cc < used + 2) {
    b->error("too-small", "the volume is too small for exFAT with this cluster size (" + std::to_string(cc) + " clusters; the bitmap, upcase table and root directory need " + std::to_string(used) + ")");
    return;
  }
  const uint32_t bm_first = 2, up_first = static_cast<uint32_t>(2 + bm_clusters), root_first = static_cast<uint32_t>(2 + bm_clusters + up_clusters);

  auto id = make_random_guid(p.opt.seed);
  uint32_t serial = g32(id.data());
  const uint32_t percent = static_cast<uint32_t>(used * 100 / cc);

  // ブート領域 (セクタ 0〜11)
  std::vector<uint8_t> region(static_cast<size_t>(S) * 12, 0);
  uint8_t* s0 = region.data();
  s0[0] = 0xEB; s0[1] = 0x76; s0[2] = 0x90;
  std::memcpy(s0 + 3, "EXFAT   ", 8);
  p64(s0 + 64, p.opt.offset / S);
  p64(s0 + 72, V);
  p32(s0 + 80, static_cast<uint32_t>(fat_off));
  p32(s0 + 84, static_cast<uint32_t>(fat_len));
  p32(s0 + 88, static_cast<uint32_t>(heap_off));
  p32(s0 + 92, static_cast<uint32_t>(cc));
  p32(s0 + 96, root_first);
  p32(s0 + 100, serial);
  p16(s0 + 104, 0x0100);
  p16(s0 + 106, 0);
  s0[108] = static_cast<uint8_t>(ilog2(S));
  s0[109] = static_cast<uint8_t>(ilog2(spc));
  s0[110] = 1;
  s0[111] = 0x80;
  s0[112] = static_cast<uint8_t>(percent);
  std::memset(s0 + 120, 0xF4, 390);   // ブートコード領域: HLT で埋める (起動しないディスク)
  s0[510] = 0x55; s0[511] = 0xAA;
  if (S > 512) { s0[S - 2] = 0x55; s0[S - 1] = 0xAA; }
  for (int i = 1; i <= 8; i++) {   // 拡張ブートセクタ: 末尾の署名だけ
    uint8_t* e = region.data() + static_cast<size_t>(S) * static_cast<size_t>(i);
    p32(e + S - 4, 0xAA550000);
  }
  uint32_t csum = boot_checksum(region, S);
  uint8_t* cs = region.data() + static_cast<size_t>(S) * 11;
  for (uint32_t i = 0; i < S; i += 4) p32(cs + i, csum);

  // FAT
  const std::string st_fat = "fat", st_heap = "cluster-heap", st_backup = "boot-backup", st_boot = "boot-region";
  {
    // FAT の先頭セクタに収まらない鎖がありうるので、鎖はすべて 1 つのバッファで作る
    uint64_t entries_needed = static_cast<uint64_t>(root_first) + 1;
    std::vector<uint8_t> head(static_cast<size_t>(ceil_div(entries_needed * 4, S) * S), 0);
    p32(&head[0], 0xFFFFFFF8);
    p32(&head[4], 0xFFFFFFFF);
    auto chain = [&](uint32_t first, uint64_t n) {
      for (uint64_t k = 0; k < n; k++) p32(&head[(first + k) * 4], k + 1 == n ? 0xFFFFFFFFu : static_cast<uint32_t>(first + k + 1));
    };
    chain(bm_first, bm_clusters);
    chain(up_first, up_clusters);
    chain(root_first, 1);
    b->data(st_fat, fat_off * S, head, "fat-head");
    uint64_t done = head.size() / S;
    if (fat_len > done) b->zero(st_fat, (fat_off + done) * S, (fat_len - done) * S, "fat-body");
  }
  // ビットマップ (使用中 = ビットマップ + アップケース + ルート)
  std::vector<uint8_t> bm(static_cast<size_t>(bm_clusters * cbytes), 0);
  for (uint64_t k = 0; k < used; k++) bm[static_cast<size_t>(k / 8)] |= static_cast<uint8_t>(1u << (k % 8));
  b->data(st_heap, heap_off * S, bm, "allocation-bitmap");
  std::vector<uint8_t> upv(static_cast<size_t>(up_clusters * cbytes), 0);
  std::memcpy(upv.data(), up.data(), up.size());
  b->data(st_heap, (heap_off + (up_first - 2) * spc) * S, upv, "upcase-table");
  // ルート: ラベル (任意) + ビットマップ + アップケース
  std::vector<uint8_t> rootv(static_cast<size_t>(cbytes), 0);
  size_t e = 0;
  if (!lab.empty()) {
    rootv[e] = 0x83;
    rootv[e + 1] = static_cast<uint8_t>(lab.size());
    for (size_t i = 0; i < lab.size(); i++) p16(&rootv[e + 2 + 2 * i], lab[i]);
    e += 32;
  }
  rootv[e] = 0x81;
  p32(&rootv[e + 20], bm_first);
  p64(&rootv[e + 24], bm_bytes);
  e += 32;
  rootv[e] = 0x82;
  p32(&rootv[e + 4], upcase_checksum(up));
  p32(&rootv[e + 20], up_first);
  p64(&rootv[e + 24], up.size());
  b->data(st_heap, (heap_off + (root_first - 2) * spc) * S, rootv, "root-directory");

  b->data(st_backup, 12ull * S, region, "boot-region-backup");
  b->data(st_boot, 0, region, "boot-region");

  auto n = [](uint64_t v) { return std::to_string(v); };
  char ser[16];
  std::snprintf(ser, sizeof ser, "%04X-%04X", serial >> 16, serial & 0xFFFF);
  b->layout = {{"sectorSize", n(S)}, {"clusterSize", n(cbytes)}, {"sectorsPerCluster", n(spc)}, {"fatOffset", n(fat_off)}, {"fatSectors", n(fat_len)},
               {"clusterHeapOffset", n(heap_off)}, {"clusterCount", n(cc)}, {"rootCluster", n(root_first)}, {"totalSectors", n(V)}, {"volumeSerial", ser}};
  if (V * S < p.length) b->warn("unused-tail", n(p.length - V * S) + " bytes at the end of the range do not fill a whole sector and are not used");
  b->changes.push_back("create an exFAT file system: " + n(cc) + " clusters of " + n(cbytes) + " bytes");
}

}  // namespace mkfsimpl
}  // namespace usbmem
