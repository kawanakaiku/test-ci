// SPDX-License-Identifier: Apache-2.0
// FAT32 の mkfs (書き込む内容を組み立てるだけ。書き込みは mkfs.cpp)。
// 配置: 予約領域 32 セクタ (0 = ブート, 1 = FSInfo, 2 = 拡張ブート, 6〜8 = それらの控え) → FAT x2 → データ領域 (クラスタ 2 = ルート)。
#include <cstdio>

#include "mkfs_internal.h"
#include "usbmem/partition_edit.h"  // make_random_guid

namespace usbmem {
namespace mkfsimpl {

namespace {

constexpr uint32_t kMaxClusters = 0x0FFFFFF4;
constexpr uint32_t kMinClusters = 65525;   // これ未満は FAT16 と判別される (仕様)

uint32_t default_cluster_bytes(uint64_t vol_bytes) {
  constexpr uint64_t MiB = 1024 * 1024, GiB = 1024 * MiB;
  if (vol_bytes <= 260 * MiB) return 512;
  if (vol_bytes <= 8 * GiB) return 4096;
  if (vol_bytes <= 16 * GiB) return 8192;
  if (vol_bytes <= 32 * GiB) return 16384;
  return 32768;
}

// FAT32 のラベル: ASCII の大文字・数字・安全な記号。11 文字までを空白で埋める。
bool fat_label(const std::string& in, uint8_t out[11], std::string* why) {
  std::memset(out, ' ', 11);
  std::string s = in;
  while (!s.empty() && s.back() == ' ') s.pop_back();
  if (s.size() > 11) { *why = "the label is longer than 11 bytes"; return false; }
  if (!s.empty() && s[0] == ' ') { *why = "the label starts with a space"; return false; }
  for (size_t i = 0; i < s.size(); i++) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x20 || c >= 0x7F) { *why = "the label has a non-ASCII or control character (FAT32 labels are limited to ASCII here)"; return false; }
    if (std::strchr("*?.,;:/\\|+=<>[]\"", c)) { *why = std::string("the label has the character '") + static_cast<char>(c) + "' which FAT does not allow"; return false; }
    if (c >= 'a' && c <= 'z') c = static_cast<unsigned char>(c - 32);
    out[i] = c;
  }
  return true;
}

}  // namespace

void build_fat32(const Params& p, Build* b) {
  const uint32_t S = p.sector_size;
  if (S < 512 || S > 4096 || !pow2(S)) { b->error("bad-sector-size", "sector size must be 512, 1024, 2048 or 4096"); return; }
  const uint64_t V = p.length / S;   // 全セクタ数 (端数は使わない)
  if (V > 0xFFFFFFFFull) { b->error("too-large", "FAT32 cannot address more than 2^32 sectors; use exFAT for this size"); return; }
  uint8_t label[11];
  std::string why;
  if (!fat_label(p.opt.label, label, &why)) { b->error("bad-label", why); return; }
  bool has_label = !p.opt.label.empty();

  // クラスタサイズ
  uint64_t cbytes = p.opt.cluster_size ? p.opt.cluster_size : default_cluster_bytes(V * S);
  if (p.opt.cluster_size) {
    if (!pow2(cbytes) || cbytes < S || cbytes > 65536 || cbytes / S > 128) { b->error("bad-cluster-size", "the cluster size must be a power of two, at least the sector size, at most 64 KiB (128 sectors)"); return; }
  }
  if (cbytes < S) cbytes = S;
  uint32_t reserved = 32;
  uint32_t nfats = 2;
  uint64_t spc = 0, clusters = 0, fat_sz = 0;
  for (;;) {
    spc = cbytes / S;
    if (spc > 128) { b->error("too-large", "the volume is too large for FAT32 with 64 KiB clusters"); return; }
    fat_sz = 1;
    for (int it = 0; it < 64; it++) {
      uint64_t overhead = reserved + nfats * fat_sz;
      clusters = V > overhead ? (V - overhead) / spc : 0;
      uint64_t need = ceil_div((clusters + 2) * 4, S);
      if (need <= fat_sz) break;
      fat_sz = need;
    }
    if (clusters > kMaxClusters && !p.opt.cluster_size && cbytes < 65536) { cbytes *= 2; continue; }
    if (clusters < kMinClusters && !p.opt.cluster_size && cbytes > S) { cbytes /= 2; continue; }
    break;
  }
  if (clusters > kMaxClusters) { b->error("too-large", "the volume has too many clusters for FAT32 with this cluster size; use a larger cluster size or exFAT"); return; }
  if (clusters < kMinClusters) {
    b->error("too-small", "the volume is too small for FAT32 (needs at least " + std::to_string(kMinClusters) + " clusters, about " +
                              std::to_string((static_cast<uint64_t>(kMinClusters) * cbytes + 32ull * S + 2 * ceil_div(kMinClusters * 4ull, S) * S) / (1024 * 1024) + 1) +
                              " MiB at this cluster size); use FAT16/exFAT or a larger volume");
    return;
  }
  // データ領域の先頭をクラスタ境界に揃える (予約領域を増やす)
  uint64_t pad = (reserved + nfats * fat_sz) % spc;
  if (pad) reserved += static_cast<uint32_t>(spc - pad);
  clusters = (V - reserved - nfats * fat_sz) / spc;
  if (clusters < kMinClusters || ceil_div((clusters + 2) * 4, S) > fat_sz) { b->error("too-small", "the volume is too small for FAT32 after alignment"); return; }
  const uint64_t data_start = reserved + nfats * fat_sz;
  const uint32_t total_sectors = static_cast<uint32_t>(V);

  auto id = make_random_guid(p.opt.seed);
  uint32_t serial = g32(id.data());
  uint32_t hidden = p.opt.hidden_sectors != 0xFFFFFFFFu ? p.opt.hidden_sectors : static_cast<uint32_t>(p.opt.offset / S);

  // ブートセクタ
  std::vector<uint8_t> boot(S, 0);
  boot[0] = 0xEB; boot[1] = 0x58; boot[2] = 0x90;
  std::memcpy(&boot[3], "USBMEM  ", 8);
  p16(&boot[11], S);
  boot[13] = static_cast<uint8_t>(spc);
  p16(&boot[14], reserved);
  boot[16] = static_cast<uint8_t>(nfats);
  p16(&boot[17], 0);            // ルートエントリ数 (FAT32 は 0)
  p16(&boot[19], 0);            // 16 ビットの総セクタ数
  boot[21] = 0xF8;              // メディア = 固定ディスク
  p16(&boot[22], 0);            // FAT16 の FAT サイズ
  p16(&boot[24], 63);           // セクタ/トラック (互換用)
  p16(&boot[26], 255);          // ヘッド数 (互換用)
  p32(&boot[28], hidden);
  p32(&boot[32], total_sectors);
  p32(&boot[36], static_cast<uint32_t>(fat_sz));
  p16(&boot[40], 0);            // フラグ: 両方の FAT が有効・ミラー
  p16(&boot[42], 0);            // バージョン 0.0
  p32(&boot[44], 2);            // ルートディレクトリのクラスタ
  p16(&boot[48], 1);            // FSInfo
  p16(&boot[50], 6);            // ブートセクタの控え
  boot[64] = 0x80;              // ドライブ番号
  boot[66] = 0x29;              // 拡張ブート署名
  p32(&boot[67], serial);
  std::memcpy(&boot[71], label, 11);
  if (!has_label) std::memcpy(&boot[71], "NO NAME    ", 11);
  std::memcpy(&boot[82], "FAT32   ", 8);
  // ブートコード: 起動しないディスクなので停止するだけ (cli; hlt; jmp $-1)
  boot[90] = 0xFA; boot[91] = 0xF4; boot[92] = 0xEB; boot[93] = 0xFD;
  boot[510] = 0x55; boot[511] = 0xAA;
  if (S > 512) { boot[S - 2] = 0x55; boot[S - 1] = 0xAA; }

  std::vector<uint8_t> fsinfo(S, 0);
  p32(&fsinfo[0], 0x41615252);
  p32(&fsinfo[484], 0x61417272);
  p32(&fsinfo[488], static_cast<uint32_t>(clusters - 1));   // ルートが 1 クラスタ使う
  p32(&fsinfo[492], 3);
  p32(&fsinfo[508], 0xAA550000);

  std::vector<uint8_t> ext(S, 0);
  p32(&ext[508], 0xAA550000);

  // ステージ: 控えと FAT とルート → 最後にブートセクタ (これが「できあがり」の印)
  const std::string st_tables = "fat-tables", st_root = "root-directory", st_copy = "boot-copies", st_boot = "boot-sector";
  std::vector<uint8_t> fat0(S, 0);
  p32(&fat0[0], 0x0FFFFFF8);
  p32(&fat0[4], 0x0FFFFFFF);
  p32(&fat0[8], 0x0FFFFFFF);   // クラスタ 2 = ルート (終端)
  for (uint32_t f = 0; f < nfats; f++) {
    uint64_t off = (static_cast<uint64_t>(reserved) + f * fat_sz) * S;
    b->data(st_tables, off, fat0, "fat-first-sector");
    b->zero(st_tables, off + S, (fat_sz - 1) * S, "fat-body");
  }
  std::vector<uint8_t> root(spc * S, 0);
  if (has_label) {
    std::memcpy(&root[0], label, 11);
    root[11] = 0x08;   // ATTR_VOLUME_ID
  }
  b->data(st_root, data_start * S, root, "root-directory-cluster");
  b->data(st_copy, 1ull * S, fsinfo, "fsinfo");
  b->data(st_copy, 2ull * S, ext, "boot-extension");
  b->data(st_copy, 6ull * S, boot, "backup-boot-sector");
  b->data(st_copy, 7ull * S, fsinfo, "backup-fsinfo");
  b->data(st_copy, 8ull * S, ext, "backup-boot-extension");
  // 予約領域の残りはゼロにする (古いデータを残さない)
  b->zero(st_copy, 3ull * S, 3ull * S, "reserved-zero");
  b->zero(st_copy, 9ull * S, (reserved - 9ull) * S, "reserved-zero");
  b->data(st_boot, 0, boot, "boot-sector");

  auto n = [](uint64_t v) { return std::to_string(v); };
  b->layout = {{"sectorSize", n(S)}, {"clusterSize", n(cbytes)}, {"sectorsPerCluster", n(spc)}, {"reservedSectors", n(reserved)}, {"fatCount", n(nfats)},
               {"fatSectors", n(fat_sz)}, {"clusterCount", n(clusters)}, {"dataStartSector", n(data_start)}, {"totalSectors", n(V)},
               {"volumeSerial", [&] { char t[16]; std::snprintf(t, sizeof t, "%04X-%04X", serial >> 16, serial & 0xFFFF); return std::string(t); }()}};
  if (V * S < p.length) b->warn("unused-tail", n(p.length - V * S) + " bytes at the end of the range do not fill a whole sector and are not used");
  b->changes.push_back("create a FAT32 file system: " + n(clusters) + " clusters of " + n(cbytes) + " bytes, 2 FATs of " + n(fat_sz) + " sectors");
}

}  // namespace mkfsimpl
}  // namespace usbmem
