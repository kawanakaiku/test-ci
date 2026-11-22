// SPDX-License-Identifier: Apache-2.0
// FAT12 / FAT16 の mkfs (書き込む内容を組み立てるだけ)。Microsoft の FAT 仕様 (fatgen103) の公開仕様から自前で書く。dosfstools のコードは使わない。
// 配置: 予約領域 (ブートセクタ 1 + データ領域をクラスタ境界に揃えるための余白) → FAT x2 → ルートディレクトリ (固定の 512 エントリ。小さい容量は 224) → データ領域。
// クラスタサイズの既定: FAT16 は Microsoft の表 (16 MiB まで 1 KiB / 128 MiB まで 2 KiB / 256 MiB まで 4 KiB / 512 MiB まで 8 KiB / 1 GiB まで 16 KiB / 2 GiB まで 32 KiB / それ以上 64 KiB。
// ただしクラスタ数が FAT16 の下限 (4085) を割るときは小さくする)。FAT12 はクラスタ数が 4084 以下になる最小のサイズ。
#include <cstdio>

#include "mkfs_internal.h"
#include "usbmem/partition_edit.h"  // make_random_guid

namespace usbmem {
namespace mkfsimpl {

namespace {

constexpr uint32_t kFat12Max = 4084, kFat16Min = 4085, kFat16Max = 65524;

uint64_t fat16_table_bytes(uint64_t bytes) {
  constexpr uint64_t MiB = 1024 * 1024, GiB = 1024 * MiB;
  if (bytes <= 16 * MiB) return 1024;
  if (bytes <= 128 * MiB) return 2048;
  if (bytes <= 256 * MiB) return 4096;
  if (bytes <= 512 * MiB) return 8192;
  if (bytes <= 1 * GiB) return 16384;
  if (bytes <= 2 * GiB) return 32768;
  return 65536;
}

}  // namespace

void build_fat1216(const Params& p, Build* b, bool fat16) {
  const uint32_t S = p.sector_size;
  if (S < 512 || S > 4096 || !pow2(S)) { b->error("bad-sector-size", "sector size must be 512, 1024, 2048 or 4096"); return; }
  const uint64_t V = p.length / S;
  const char* name = fat16 ? "FAT16" : "FAT12";
  if (V > 0xFFFFFFFFull) { b->error("too-large", std::string("the volume is too large for ") + name); return; }
  uint8_t label[11];
  std::string why;
  if (!fat_label(p.opt.label, label, &why)) { b->error("bad-label", why); return; }
  const bool has_label = !p.opt.label.empty();

  const uint32_t nfats = 2;
  const uint32_t root_entries = static_cast<uint32_t>(align_up(p.length < (1u << 20) ? 224 : 512, S / 32));
  const uint64_t root_secs = ceil_div(static_cast<uint64_t>(root_entries) * 32, S);
  const uint64_t lo_clusters = fat16 ? kFat16Min : 1, hi_clusters = fat16 ? kFat16Max : kFat12Max;

  uint64_t reserved = 1, spc = 0, clusters = 0, fat_sz = 0;
  auto geometry = [&](uint64_t cbytes) {
    spc = cbytes / S;
    reserved = 1;
    fat_sz = 1;
    for (int it = 0; it < 64; it++) {
      uint64_t overhead = reserved + nfats * fat_sz + root_secs;
      clusters = V > overhead ? (V - overhead) / spc : 0;
      uint64_t need = fat16 ? ceil_div((clusters + 2) * 2, S) : ceil_div(((clusters + 2) * 3 + 1) / 2, S);
      if (need <= fat_sz) break;
      fat_sz = need;
    }
    // データ領域の先頭をクラスタ境界に揃える (予約領域を増やす)
    uint64_t pad = (reserved + nfats * fat_sz + root_secs) % spc;
    if (pad) {
      reserved += spc - pad;
      uint64_t overhead = reserved + nfats * fat_sz + root_secs;
      clusters = V > overhead ? (V - overhead) / spc : 0;
    }
  };
  uint64_t cbytes;
  if (p.opt.cluster_size) {
    cbytes = p.opt.cluster_size;
    if (!pow2(cbytes) || cbytes < S || cbytes > 65536 || cbytes / S > 128) { b->error("bad-cluster-size", "the cluster size must be a power of two, at least the sector size, at most 64 KiB (128 sectors)"); return; }
    geometry(cbytes);
  } else {
    // 候補を小さい方から。FAT16: 表の値以上で、クラスタ数が上限以下の最小。表の値でクラスタ数が下限を割るときは小さくして探す。FAT12: クラスタ数が上限以下の最小
    uint64_t want = fat16 ? std::max<uint64_t>(fat16_table_bytes(V * S), S) : S;
    bool found = false;
    cbytes = 0;
    for (uint64_t c = want; c <= 65536 && c / S <= 128; c *= 2) {
      geometry(c);
      if (clusters <= hi_clusters) { cbytes = c; found = true; break; }
    }
    if (found && clusters < lo_clusters) {
      for (uint64_t c = cbytes / 2; c >= S; c /= 2) {
        geometry(c);
        cbytes = c;
        if (clusters >= lo_clusters && clusters <= hi_clusters) break;
      }
    }
    if (!found) {
      b->error("too-large", std::string("the volume is too large for ") + name + " (at most " + std::to_string(hi_clusters) + " clusters of 64 KiB); use " + (fat16 ? "FAT32 or exFAT" : "FAT16 or FAT32"));
      return;
    }
  }
  if (clusters > hi_clusters) {
    b->error("too-large", std::string("the volume has too many clusters for ") + name + " with this cluster size (" + std::to_string(clusters) + " > " + std::to_string(hi_clusters) + "); use a larger cluster size or " + (fat16 ? "FAT32" : "FAT16"));
    return;
  }
  if (clusters < lo_clusters || clusters < 8) {
    b->error("too-small", std::string("the volume is too small for ") + name + " with this cluster size (" + std::to_string(clusters) + " clusters" +
                              (fat16 ? "; FAT16 needs at least 4085, use FAT12 for small volumes" : "") + ")");
    return;
  }
  if (reserved > 0xFFFF || fat_sz > 0xFFFF) { b->error("too-large", "the file system structures are too large"); return; }
  const uint64_t data_start = reserved + nfats * fat_sz + root_secs;

  auto id = make_random_guid(p.opt.seed);
  if (!p.opt.uuid.empty() && !parse_uuid(p.opt.uuid, id.data())) { b->error("bad-uuid", "the volume id must look like xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx (only the first 4 bytes are used as the serial number)"); return; }
  const uint32_t serial = g32(id.data());
  const uint32_t hidden = p.opt.hidden_sectors != 0xFFFFFFFFu ? p.opt.hidden_sectors : static_cast<uint32_t>(p.opt.offset / S);

  std::vector<uint8_t> boot(S, 0);
  boot[0] = 0xEB; boot[1] = 0x3C; boot[2] = 0x90;
  std::memcpy(&boot[3], "USBMEM  ", 8);
  p16(&boot[11], S);
  boot[13] = static_cast<uint8_t>(spc);
  p16(&boot[14], static_cast<uint32_t>(reserved));
  boot[16] = static_cast<uint8_t>(nfats);
  p16(&boot[17], root_entries);
  if (V < 0x10000) p16(&boot[19], static_cast<uint32_t>(V));
  boot[21] = 0xF8;
  p16(&boot[22], static_cast<uint32_t>(fat_sz));
  p16(&boot[24], 63);
  p16(&boot[26], 255);
  p32(&boot[28], hidden);
  if (V >= 0x10000) p32(&boot[32], static_cast<uint32_t>(V));
  boot[36] = 0x80;
  boot[38] = 0x29;
  p32(&boot[39], serial);
  if (has_label) std::memcpy(&boot[43], label, 11); else std::memcpy(&boot[43], "NO NAME    ", 11);
  std::memcpy(&boot[54], fat16 ? "FAT16   " : "FAT12   ", 8);
  boot[62] = 0xFA; boot[63] = 0xF4; boot[64] = 0xEB; boot[65] = 0xFD;   // 起動しない (cli; hlt; jmp $-1)
  boot[510] = 0x55; boot[511] = 0xAA;
  if (S > 512) { boot[S - 2] = 0x55; boot[S - 1] = 0xAA; }

  const std::string st_tables = "fat-tables", st_root = "root-directory", st_boot = "boot-sector", st_res = "reserved";
  std::vector<uint8_t> fat0(S, 0);
  if (fat16) { p16(&fat0[0], 0xFFF8); p16(&fat0[2], 0xFFFF); }
  else { fat0[0] = 0xF8; fat0[1] = 0xFF; fat0[2] = 0xFF; }
  for (uint32_t f = 0; f < nfats; f++) {
    uint64_t off = (reserved + f * fat_sz) * S;
    b->data(st_tables, off, fat0, "fat-first-sector");
    b->zero(st_tables, off + S, (fat_sz - 1) * S, "fat-body");
  }
  std::vector<uint8_t> root(static_cast<size_t>(root_secs * S), 0);
  if (has_label) { std::memcpy(&root[0], label, 11); root[11] = 0x08; }
  b->data(st_root, (reserved + nfats * fat_sz) * S, root, "root-directory");
  if (reserved > 1) b->zero(st_res, 1ull * S, (reserved - 1) * S, "reserved-zero");
  b->data(st_boot, 0, boot, "boot-sector");

  auto n = [](uint64_t v) { return std::to_string(v); };
  b->layout = {{"sectorSize", n(S)}, {"clusterSize", n(cbytes)}, {"sectorsPerCluster", n(spc)}, {"reservedSectors", n(reserved)}, {"fatCount", n(nfats)},
               {"fatSectors", n(fat_sz)}, {"rootEntries", n(root_entries)}, {"clusterCount", n(clusters)}, {"dataStartSector", n(data_start)}, {"totalSectors", n(V)},
               {"volumeSerial", [&] { char t[16]; std::snprintf(t, sizeof t, "%04X-%04X", serial >> 16, serial & 0xFFFF); return std::string(t); }()}};
  if (V * S < p.length) b->warn("unused-tail", n(p.length - V * S) + " bytes at the end of the range do not fill a whole sector and are not used");
  b->changes.push_back(std::string("create a ") + name + " file system: " + n(clusters) + " clusters of " + n(cbytes) + " bytes, 2 FATs of " + n(fat_sz) + " sectors");
}

}  // namespace mkfsimpl
}  // namespace usbmem
