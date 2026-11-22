// SPDX-License-Identifier: Apache-2.0
// パーティション探索 / ブートセクタ検査のテスト用の部品: 各 FS の最小限のヘッダ (+ バックアップ) を持つ合成イメージ。
// 実際の mkfs が作ったイメージとの照合は tools/check_partscan_tools.py。ここは検出器の境界条件と破損入力のための手組み。
#pragma once
#include <random>

#include "partedit_test_util.h"
#include "usbmem/crc32.h"
#include "usbmem/sha256.h"

namespace ps_test {
using tb::Bytes;

inline uint32_t rd32(const Bytes& b, size_t o) { return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (uint32_t(b[o + 3]) << 24); }
inline void be16(Bytes& b, size_t o, uint16_t v) { b[o] = v >> 8; b[o + 1] = v & 0xFF; }
inline void be32(Bytes& b, size_t o, uint32_t v) { for (int i = 0; i < 4; i++) b[o + i] = (v >> (24 - 8 * i)) & 0xFF; }
inline void be64(Bytes& b, size_t o, uint64_t v) { for (int i = 0; i < 8; i++) b[o + i] = (v >> (56 - 8 * i)) & 0xFF; }
inline void put(Bytes& b, size_t o, const char* s, size_t n) { std::memcpy(&b[o], s, n); }

// ---- FAT32 (512 バイトセクタ、1 セクタ/クラスタ。FSInfo = 1、バックアップ = 6)
inline Bytes make_fat32(uint64_t bytes) {
  Bytes img(bytes, 0);
  uint32_t tot = static_cast<uint32_t>(bytes / 512), rsvd = 32, nfats = 2;
  uint32_t fatsz = 1;
  while (true) {
    uint32_t clusters = tot - rsvd - nfats * fatsz;
    uint32_t need = (clusters + 2) * 4 / 512 + 1;
    if (need <= fatsz) break;
    fatsz = need;
  }
  uint32_t clusters = tot - rsvd - nfats * fatsz;
  Bytes s(512, 0);
  s[0] = 0xEB; s[1] = 0x58; s[2] = 0x90;
  put(s, 3, "MSDOS5.0", 8);
  tb::le16(s, 11, 512); s[13] = 1; tb::le16(s, 14, rsvd); s[16] = nfats; s[21] = 0xF8;
  tb::le16(s, 24, 63); tb::le16(s, 26, 255);
  tb::le32(s, 32, tot); tb::le32(s, 36, fatsz); tb::le32(s, 44, 2); tb::le16(s, 48, 1); tb::le16(s, 50, 6);
  s[64] = 0x80; s[66] = 0x29; tb::le32(s, 67, 0x12345678);
  put(s, 71, "NO NAME    ", 11); put(s, 82, "FAT32   ", 8);
  tb::sign55aa(s);
  Bytes fi(512, 0);
  tb::le32(fi, 0, 0x41615252u); tb::le32(fi, 484, 0x61417272u); tb::le32(fi, 488, clusters - 1); tb::le32(fi, 492, 3); tb::le32(fi, 508, 0xAA550000u);
  Bytes s2(512, 0);
  tb::sign55aa(s2);
  for (int i = 0; i < 2; i++) {
    std::memcpy(&img[(i ? 6 : 0) * 512], s.data(), 512);
    std::memcpy(&img[(i ? 7 : 1) * 512], fi.data(), 512);
    std::memcpy(&img[(i ? 8 : 2) * 512], s2.data(), 512);
  }
  for (uint32_t f = 0; f < nfats; f++) {
    size_t o = (rsvd + static_cast<size_t>(f) * fatsz) * 512;
    tb::le32(img, o, 0x0FFFFFF8u); tb::le32(img, o + 4, 0x0FFFFFFFu); tb::le32(img, o + 8, 0x0FFFFFFFu);
  }
  return img;
}

// ---- FAT16 (512 バイトセクタ、ルート 512 エントリ、FAT 2 本)。BPB の再構築の試験用
inline Bytes make_fat16(uint64_t bytes) {
  Bytes img(bytes, 0);
  uint32_t tot = static_cast<uint32_t>(bytes / 512), rsvd = 1, nfats = 2, root = 512, spc = 4;
  uint32_t fatsz = 1;
  uint32_t rootsecs = root * 32 / 512;
  while (true) {
    uint32_t clusters = (tot - rsvd - nfats * fatsz - rootsecs) / spc;
    uint32_t need = ((clusters + 2) * 2 + 511) / 512;
    if (need <= fatsz) break;
    fatsz = need;
  }
  Bytes s(512, 0);
  s[0] = 0xEB; s[1] = 0x3C; s[2] = 0x90;
  put(s, 3, "mkfs.fat", 8);
  tb::le16(s, 11, 512); s[13] = spc; tb::le16(s, 14, rsvd); s[16] = nfats; tb::le16(s, 17, root);
  if (tot < 65536) tb::le16(s, 19, tot); else tb::le32(s, 32, tot);
  s[21] = 0xF8; tb::le16(s, 22, fatsz); tb::le16(s, 24, 32); tb::le16(s, 26, 64);
  s[36] = 0x80; s[38] = 0x29; tb::le32(s, 39, 0xCAFEBABE);
  put(s, 43, "NO NAME    ", 11); put(s, 54, "FAT16   ", 8);
  tb::sign55aa(s);
  std::memcpy(img.data(), s.data(), 512);
  for (uint32_t f = 0; f < nfats; f++) {
    size_t o = (rsvd + static_cast<size_t>(f) * fatsz) * 512;
    img[o] = 0xF8; img[o + 1] = img[o + 2] = img[o + 3] = 0xFF;
  }
  return img;
}

// ---- NTFS (512 バイトセクタ、8 セクタ/クラスタ。$MFT = クラスタ 4、$MFTMirr = クラスタ 2)
inline Bytes make_ntfs(uint64_t bytes) {
  Bytes img(bytes, 0);
  uint64_t total = bytes / 512 - 1;
  Bytes s(512, 0);
  s[0] = 0xEB; s[1] = 0x52; s[2] = 0x90;
  put(s, 3, "NTFS    ", 8);
  tb::le16(s, 11, 512); s[13] = 8; s[21] = 0xF8; tb::le16(s, 24, 63); tb::le16(s, 26, 255);
  tb::le64(s, 0x28, total); tb::le64(s, 0x30, 4); tb::le64(s, 0x38, 2);
  s[0x40] = 0xF6; s[0x44] = 1;
  tb::le64(s, 0x48, 0x1122334455667788ull);
  tb::sign55aa(s);
  std::memcpy(img.data(), s.data(), 512);
  std::memcpy(&img[total * 512], s.data(), 512);
  std::memcpy(&img[4 * 4096], "FILE", 4);
  std::memcpy(&img[2 * 4096], "FILE", 4);
  return img;
}

// ---- exFAT (512 バイトセクタ、8 セクタ/クラスタ)。ブート領域 12 セクタ + バックアップ
inline void exfat_checksum_sector(Bytes& img, size_t base) {
  uint32_t cs = 0;
  for (size_t i = 0; i < 11 * 512; i++) {
    if (i == 106 || i == 107 || i == 112) continue;
    cs = ((cs & 1) ? 0x80000000u : 0u) + (cs >> 1) + img[base + i];
  }
  for (size_t i = 0; i < 512; i += 4) tb::le32(img, base + 11 * 512 + i, cs);
}
inline Bytes make_exfat(uint64_t bytes) {
  Bytes img(bytes, 0);
  Bytes s(512, 0);
  s[0] = 0xEB; s[1] = 0x76; s[2] = 0x90;
  put(s, 3, "EXFAT   ", 8);
  uint64_t vol = bytes / 512;
  tb::le64(s, 64, 2048); tb::le64(s, 72, vol);
  uint32_t fat_off = 128, fat_len = 64, heap = 256;
  tb::le32(s, 80, fat_off); tb::le32(s, 84, fat_len); tb::le32(s, 88, heap);
  uint32_t clusters = static_cast<uint32_t>((vol - heap) / 8);
  tb::le32(s, 92, clusters); tb::le32(s, 96, 4); tb::le32(s, 100, 0xDEADBEEF);
  tb::le16(s, 104, 0x0100); tb::le16(s, 106, 0); s[108] = 9; s[109] = 3; s[110] = 1; s[111] = 0x80; s[112] = 5;
  tb::sign55aa(s);
  for (int sec = 1; sec <= 8; sec++) { img[sec * 512 + 510] = 0x55; img[sec * 512 + 511] = 0xAA; }
  std::memcpy(img.data(), s.data(), 512);
  exfat_checksum_sector(img, 0);
  std::memcpy(&img[12 * 512], img.data(), 12 * 512);
  return img;
}

// ---- ext2 / ext4 (1 KiB ブロック)。グループ 1 に予備のスーパーブロックと GDT。flags: 0 = ext2, 1 = ext4 (extents + flex_bg + metadata_csum)
inline void ext_fix_csum(Bytes& img, size_t sb_off) {
  uint32_t cs = usbmem::crc32c_raw(0xFFFFFFFFu, &img[sb_off], 1020);
  tb::le32(img, sb_off + 0x3FC, cs);
}
inline Bytes make_ext(uint64_t bytes, int flags) {
  Bytes img(bytes, 0);
  uint32_t bs = 1024, bpg = 8192, blocks = static_cast<uint32_t>(bytes / bs);
  uint32_t groups = (blocks - 1 + bpg - 1) / bpg, ipg = 2048;
  auto fill_sb = [&](size_t off, uint16_t group) {
    tb::le32(img, off + 0, ipg * groups); tb::le32(img, off + 4, blocks); tb::le32(img, off + 12, blocks - 400); tb::le32(img, off + 16, ipg * groups - 11);
    tb::le32(img, off + 20, 1); tb::le32(img, off + 24, 0); tb::le32(img, off + 28, 0); tb::le32(img, off + 32, bpg); tb::le32(img, off + 36, bpg); tb::le32(img, off + 40, ipg);
    tb::le16(img, off + 56, 0xEF53); tb::le16(img, off + 58, 1); tb::le16(img, off + 60, 1); tb::le32(img, off + 76, 1); tb::le32(img, off + 84, 11); tb::le16(img, off + 88, 256);
    tb::le16(img, off + 0x5A, group);
    uint32_t compat = 0x38, incompat = 0x2, ro = 0x1;
    if (flags & 1) { incompat |= 0x40 | 0x200; ro |= 0x400 | 0x2 | 0x8; compat |= 0x4; }
    tb::le32(img, off + 0x5C, compat); tb::le32(img, off + 0x60, incompat); tb::le32(img, off + 0x64, ro);
    for (int i = 0; i < 16; i++) img[off + 104 + i] = static_cast<uint8_t>(0xA0 + i);
    put(img, off + 120, "scantest", 8);
    if (flags & 1) ext_fix_csum(img, off);
  };
  auto fill_gdt = [&](size_t off) {
    for (uint32_t g = 0; g < groups; g++) {
      size_t d = off + g * 32;
      tb::le32(img, d + 0, 100 + g * 3); tb::le32(img, d + 4, 101 + g * 3); tb::le32(img, d + 8, 200 + g * 7);
      tb::le16(img, d + 12, 100); tb::le16(img, d + 14, 200);
    }
  };
  fill_sb(1024, 0);
  fill_gdt(2048);
  for (uint32_t g : {1u, 3u, 5u, 7u}) {
    if (g >= groups) break;
    size_t blk = static_cast<size_t>(g) * bpg + 1;
    fill_sb(blk * bs, static_cast<uint16_t>(g));
    fill_gdt((blk + 1) * bs);
  }
  return img;
}

// ---- XFS (4 KiB ブロック、2 AG)
inline Bytes make_xfs(uint64_t bytes) {
  Bytes img(bytes, 0);
  uint32_t bs = 4096;
  uint64_t dblocks = bytes / bs;
  uint32_t agblocks = static_cast<uint32_t>((dblocks + 1) / 2), agcount = 2;
  for (uint32_t ag = 0; ag < agcount; ag++) {
    size_t o = static_cast<size_t>(ag) * agblocks * bs;
    put(img, o, "XFSB", 4);
    be32(img, o + 4, bs); be64(img, o + 8, dblocks);
    for (int i = 0; i < 16; i++) img[o + 32 + i] = static_cast<uint8_t>(0x30 + i);
    be32(img, o + 84, agblocks); be32(img, o + 88, agcount);
    be16(img, o + 100, 0xB4A5); be16(img, o + 102, 512); be16(img, o + 104, 512);
    img[o + 120] = 12; img[o + 121] = 9; img[o + 122] = 9; img[o + 123] = 3; img[o + 124] = 15;
  }
  return img;
}

// ---- Btrfs (64 KiB の primary と 64 MiB のミラー)
inline void btrfs_sb(Bytes& img, size_t off, uint64_t bytenr, uint64_t total) {
  put(img, off + 0x40, "_BHRfS_M", 8);
  for (int i = 0; i < 16; i++) img[off + 32 + i] = static_cast<uint8_t>(0x50 + i);
  tb::le64(img, off + 0x30, bytenr); tb::le64(img, off + 0x70, total); tb::le64(img, off + 0x88, 1);
  tb::le32(img, off + 0x90, 4096); tb::le32(img, off + 0x94, 16384);
  uint32_t cs = ~usbmem::crc32c_raw(0xFFFFFFFFu, &img[off + 32], 4096 - 32);
  tb::le32(img, off, cs);
}
inline Bytes make_btrfs(uint64_t bytes) {
  Bytes img(bytes, 0);
  btrfs_sb(img, 0x10000, 0x10000, bytes);
  if (bytes > 0x4000000 + 4096) btrfs_sb(img, 0x4000000, 0x4000000, bytes);
  return img;
}

// ---- F2FS
inline Bytes make_f2fs(uint64_t bytes) {
  Bytes img(bytes, 0);
  for (size_t off : {size_t(1024), size_t(4096 + 1024)}) {
    tb::le32(img, off, 0xF2F52010u); tb::le16(img, off + 4, 1); tb::le16(img, off + 6, 14);
    tb::le32(img, off + 8, 9); tb::le32(img, off + 12, 3); tb::le32(img, off + 16, 12); tb::le32(img, off + 20, 9);
    tb::le32(img, off + 24, 1); tb::le32(img, off + 28, 1);
    tb::le64(img, off + 36, bytes / 4096 - 100);
  }
  return img;
}

// ---- squashfs / erofs / ISO 9660 / UDF
inline Bytes make_squashfs(uint64_t bytes, uint64_t used) {
  Bytes img(bytes, 0);
  put(img, 0, "hsqs", 4);
  tb::le32(img, 4, 10); tb::le32(img, 12, 131072); tb::le16(img, 20, 1); tb::le16(img, 22, 17); tb::le16(img, 28, 4); tb::le16(img, 30, 0);
  tb::le64(img, 40, used);
  return img;
}
inline Bytes make_erofs(uint64_t bytes) {
  Bytes img(bytes, 0);
  tb::le32(img, 1024, 0xE0F5E1E2u); img[1024 + 12] = 12; tb::le16(img, 1024 + 14, 36);
  tb::le32(img, 1024 + 36, static_cast<uint32_t>(bytes / 4096)); tb::le32(img, 1024 + 40, 1);
  return img;
}
inline Bytes make_iso(uint64_t bytes) {
  Bytes img(bytes, 0);
  size_t o = 0x8000;
  img[o] = 1; put(img, o + 1, "CD001", 5); img[o + 6] = 1;
  uint32_t vss = static_cast<uint32_t>(bytes / 2048);
  tb::le32(img, o + 80, vss); be32(img, o + 84, vss);
  tb::le16(img, o + 128, 2048); be16(img, o + 130, 2048);
  o += 2048;
  img[o] = 255; put(img, o + 1, "CD001", 5); img[o + 6] = 1;
  return img;
}

// ---- LUKS1 / LUKS2
inline Bytes make_luks1(uint64_t bytes) {
  Bytes img(bytes, 0);
  put(img, 0, "LUKS\xba\xbe", 6);
  be16(img, 6, 1);
  put(img, 8, "aes", 3); put(img, 40, "xts-plain64", 11); put(img, 72, "sha256", 6);
  be32(img, 104, 4096); be32(img, 108, 64);
  put(img, 168, "0aa5e6d6-1111-4222-8333-444455556666", 36);
  return img;
}
inline void luks2_header(Bytes& img, size_t off, uint64_t hdr_size, const char* magic, const std::string& json, bool secondary, uint64_t self_off) {
  put(img, off, magic, 6);
  be16(img, off + 6, 2); be64(img, off + 8, hdr_size); be64(img, off + 16, secondary ? 2 : 1);
  put(img, off + 72, "sha256", 6);
  put(img, off + 168, "0aa5e6d6-1111-4222-8333-444455556666", 36);
  be64(img, off + 256, self_off);
  std::memcpy(&img[off + 4096], json.data(), json.size());
  std::memset(&img[off + 448], 0, 64);
  auto h = usbmem::Sha256::hash(&img[off], hdr_size);
  std::memcpy(&img[off + 448], h.data(), 32);
}
inline Bytes make_luks2(uint64_t bytes) {
  Bytes img(bytes, 0);
  uint64_t seg_off = 16u << 20;
  std::string json = "{\"keyslots\":{},\"tokens\":{},\"segments\":{\"0\":{\"type\":\"crypt\",\"offset\":\"" + std::to_string(seg_off) + "\",\"size\":\"" +
                     std::to_string(bytes - seg_off) + "\"}},\"digests\":{},\"config\":{}}";
  luks2_header(img, 0, 16384, "LUKS\xba\xbe", json, false, 0);
  luks2_header(img, 16384, 16384, "SKUL\xba\xbe", json, true, 16384);
  return img;
}

// ---- BitLocker (Windows 7 以降の形式)
inline Bytes make_bitlocker(uint64_t bytes) {
  Bytes img(bytes, 0);
  Bytes s(512, 0);
  s[0] = 0xEB; s[1] = 0x58; s[2] = 0x90;
  put(s, 3, "-FVE-FS-", 8);
  tb::le16(s, 11, 512); s[13] = 8; s[21] = 0xF8;
  tb::le64(s, 0x28, bytes / 512 - 1);
  static const uint8_t guid[16] = {0x3b, 0xd6, 0x67, 0x49, 0x29, 0x2e, 0xd8, 0x4a, 0x83, 0x99, 0xf6, 0xa3, 0x39, 0xe3, 0xd0, 0x01};
  std::memcpy(&s[0xA0], guid, 16);
  tb::le64(s, 0xB0, 8 * 512 * 100); tb::le64(s, 0xB8, 8 * 512 * 200); tb::le64(s, 0xC0, 8 * 512 * 300);
  tb::sign55aa(s);
  std::memcpy(img.data(), s.data(), 512);
  return img;
}

// 配置情報つきの FS の名前と作り方
struct FsMaker {
  const char* name;
  usbmem::FsType fs;
  Bytes (*make)(uint64_t);
  uint64_t size;
  bool exact;  // ヘッダの大きさ == イメージの大きさ
};
inline Bytes mk_ext2(uint64_t b) { return make_ext(b, 0); }
inline Bytes mk_ext4(uint64_t b) { return make_ext(b, 1); }
inline Bytes mk_squash(uint64_t b) { return make_squashfs(b, b - 8192); }
inline Bytes mk_btrfs_big(uint64_t b) { return make_btrfs(b); }
inline std::vector<FsMaker> all_makers() {
  using usbmem::FsType;
  return {
      {"fat32", FsType::Fat32, make_fat32, 40u << 20, true},       {"fat16", FsType::Fat16, make_fat16, 20u << 20, true},
      {"ntfs", FsType::Ntfs, make_ntfs, 16u << 20, true},          {"exfat", FsType::Exfat, make_exfat, 16u << 20, true},
      {"ext2", FsType::Ext2, mk_ext2, 24u << 20, true},            {"ext4", FsType::Ext4, mk_ext4, 24u << 20, true},
      {"xfs", FsType::Xfs, make_xfs, 16u << 20, true},             {"btrfs", FsType::Btrfs, mk_btrfs_big, 100u << 20, true},
      {"f2fs", FsType::F2fs, make_f2fs, 16u << 20, false},         {"squashfs", FsType::Squashfs, mk_squash, 4u << 20, false},
      {"erofs", FsType::Erofs, make_erofs, 4u << 20, true},        {"iso9660", FsType::Iso9660, make_iso, 8u << 20, true},
      {"luks1", FsType::Luks, make_luks1, 8u << 20, false},        {"luks2", FsType::Luks, make_luks2, 32u << 20, false},
      {"bitlocker", FsType::BitLocker, make_bitlocker, 8u << 20, false},
  };
}

inline void place(Bytes& disk, uint64_t off, const Bytes& fs) {
  std::memcpy(&disk[off], fs.data(), fs.size());
}

inline Bytes random_bytes(size_t n, uint32_t seed) {
  std::mt19937 g(seed);
  Bytes b(n);
  for (auto& x : b) x = static_cast<uint8_t>(g());
  return b;
}

}  // namespace ps_test
