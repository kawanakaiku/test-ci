// SPDX-License-Identifier: Apache-2.0
#include "usbmem/fs_probe.h"

#include <cstring>
#include <vector>

namespace usbmem {
namespace {
uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
bool is_pow2(uint32_t v) { return v && !(v & (v - 1)); }

// FAT: Microsoft のクラスタ数基準で 12/16/32 を決める。BPB が不正なら Unknown。
FsType probe_fat(const uint8_t* s, size_t sz, uint64_t total_bytes) {
  if (sz < 512 || s[510] != 0x55 || s[511] != 0xAA) return FsType::Unknown;
  if (!(s[0] == 0xEB && s[2] == 0x90) && s[0] != 0xE9) return FsType::Unknown;
  uint32_t bps = le16(s + 11), spc = s[13], rsvd = le16(s + 14), nfats = s[16];
  uint32_t root = le16(s + 17);
  uint32_t tot = le16(s + 19) ? le16(s + 19) : le32(s + 32);
  uint32_t fatsz = le16(s + 22) ? le16(s + 22) : le32(s + 36);
  if (bps < 512 || bps > 4096 || !is_pow2(bps) || !is_pow2(spc) || spc > 128) return FsType::Unknown;
  if (rsvd == 0 || nfats == 0 || nfats > 2 || fatsz == 0 || tot == 0) return FsType::Unknown;
  uint64_t root_secs = (static_cast<uint64_t>(root) * 32 + bps - 1) / bps;
  uint64_t meta = rsvd + static_cast<uint64_t>(nfats) * fatsz + root_secs;
  if (meta >= tot) return FsType::Unknown;
  // 申告容量が実際のデバイス/領域を超える BPB は採用しない (MBR のブートコードの誤認防止)
  if (total_bytes && static_cast<uint64_t>(tot) * bps > total_bytes) return FsType::Unknown;
  // FAT32 は BPB の構造(root entries == 0 && 16bit FAT サイズ == 0)で判定する。
  // mkfs.fat -F 32 は 65525 クラスタ未満でも FAT32 を作り、Linux も受け入れるため。
  if (root == 0 && le16(s + 22) == 0) return FsType::Fat32;
  uint64_t clusters = (tot - meta) / spc;
  if (clusters < 4085) return FsType::Fat12;
  if (clusters < 65525) return FsType::Fat16;
  return FsType::Fat32;
}

FsType probe_ext(const uint8_t* sb /* superblock 先頭 */) {
  if (le16(sb + 56) != 0xEF53) return FsType::Unknown;
  uint32_t compat = le32(sb + 0x5C), incompat = le32(sb + 0x60);
  // EXTENTS(0x40) / 64BIT(0x80) / FLEX_BG(0x200) 等は ext4 専用
  uint32_t rocompat = le32(sb + 0x64);
  if (incompat & (0x40 | 0x80 | 0x200 | 0x400 | 0x1000)) return FsType::Ext4;
  // ext2 (fs/ext2) が正しく扱える機能の外 (metadata_csum / huge_file / dir_nlink / extra_isize / orphan_file 等) があれば ext4 側のドライバで読む
  //   compat: 0x3F (dir_prealloc imagic has_journal ext_attr resize_inode dir_index) + incompat: filetype(0x2) + ro_compat: sparse_super large_file btree_dir (0x7)
  if ((compat & ~0x3Fu) || (incompat & ~0x2u & ~0x4u) || (rocompat & ~0x7u)) return FsType::Ext4;
  if (compat & 0x4) return FsType::Ext3;  // has_journal
  return FsType::Ext2;
}
}  // namespace

const char* fs_type_name(FsType t) {
  switch (t) {
    case FsType::Unknown: return "unknown";
    case FsType::Ntfs: return "ntfs";
    case FsType::Exfat: return "exfat";
    case FsType::Fat12: return "fat12";
    case FsType::Fat16: return "fat16";
    case FsType::Fat32: return "fat32";
    case FsType::Ext2: return "ext2";
    case FsType::Ext3: return "ext3";
    case FsType::Ext4: return "ext4";
    case FsType::Iso9660: return "iso9660";
    case FsType::Udf: return "udf";
    case FsType::BitLocker: return "bitlocker";
    case FsType::Luks: return "luks";
    case FsType::Btrfs: return "btrfs";
    case FsType::Xfs: return "xfs";
    case FsType::F2fs: return "f2fs";
  }
  return "unknown";
}

FsType probe_boot_sector(const uint8_t* s, size_t sz, uint64_t total_bytes) {
  if (sz < 512) return FsType::Unknown;
  if (std::memcmp(s + 3, "NTFS    ", 8) == 0 && s[510] == 0x55 && s[511] == 0xAA) return FsType::Ntfs;
  if (std::memcmp(s + 3, "EXFAT   ", 8) == 0) {
    for (int i = 11; i < 64; i++)
      if (s[i]) return FsType::Unknown;  // exFAT は BPB 領域がゼロ
    return FsType::Exfat;
  }
  if (std::memcmp(s + 3, "-FVE-FS-", 8) == 0) return FsType::BitLocker;
  return probe_fat(s, sz, total_bytes);
}

FsType probe_filesystem(BlockDevice& dev, uint64_t offset, uint64_t length, Status* io) {
  auto rd = [&](uint64_t rel, uint8_t* buf, size_t n) -> bool {
    if (!range_ok(length, rel, n) || !range_ok(dev.size(), offset, length)) return false;
    Status st = dev.read_at(offset + rel, buf, n);
    if (!st.ok() && io && io->ok()) *io = st;
    return st.ok();
  };
  if (io) *io = Status::Ok();
  std::vector<uint8_t> head(4096);
  size_t hn = static_cast<size_t>(length < head.size() ? length : head.size());
  if (hn < 512 || !rd(0, head.data(), hn)) return FsType::Unknown;

  if (FsType t = probe_boot_sector(head.data(), hn, length); t != FsType::Unknown) return t;
  if (std::memcmp(head.data(), "LUKS\xba\xbe", 6) == 0) return FsType::Luks;
  if (std::memcmp(head.data(), "XFSB", 4) == 0) return FsType::Xfs;

  uint8_t sb[1024];
  if (rd(1024, sb, sizeof sb)) {
    if (FsType t = probe_ext(sb); t != FsType::Unknown) return t;
    if (le32(sb) == 0xF2F52010u) return FsType::F2fs;
  }
  uint8_t bt[8];
  if (rd(0x10040, bt, 8) && std::memcmp(bt, "_BHRfS_M", 8) == 0) return FsType::Btrfs;

  // ISO9660 / UDF: 0x8000 から 2048 バイトの記述子列を走査
  bool iso = false, udf = false;
  for (uint32_t i = 0; i < 16; i++) {
    uint8_t d[8];
    if (!rd(0x8000 + static_cast<uint64_t>(i) * 2048, d, 8)) break;
    if (std::memcmp(d + 1, "CD001", 5) == 0) iso = true;
    else if (std::memcmp(d + 1, "NSR02", 5) == 0 || std::memcmp(d + 1, "NSR03", 5) == 0) udf = true;
    else if (std::memcmp(d + 1, "BEA01", 5) != 0 && std::memcmp(d + 1, "TEA01", 5) != 0) break;
  }
  if (udf) return FsType::Udf;
  if (iso) return FsType::Iso9660;
  return FsType::Unknown;
}

}  // namespace usbmem
