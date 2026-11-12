// SPDX-License-Identifier: Apache-2.0
// ファイルシステムの割り当て情報から「使用中ブロックの地図」を作る (読み取りのみ)。
// ext2/3/4 (グループのブロックビットマップ)、exFAT (割り当てビットマップ)、FAT12/16/32 (FAT)、NTFS ($Bitmap)。
// ライブラリを使わず、オンディスク構造を直接読む。壊れた入力に対しては必ず Corrupt 系の失敗で返す (範囲外アクセスをしない)。
#include <algorithm>
#include <cstring>

#include "usbmem/backup.h"
#include "usbmem/fs_probe.h"

namespace usbmem {

void UsedMap::set_range(uint64_t first, uint64_t count) {
  if (first >= total_blocks || count == 0) return;
  uint64_t end = std::min<uint64_t>(total_blocks, first + count);
  if (end < first) end = total_blocks;
  uint64_t b = first;
  while (b < end && (b & 7)) { set(b); b++; }
  while (b + 8 <= end) { bits[b >> 3] = 0xFF; b += 8; }
  while (b < end) { set(b); b++; }
}

void UsedMap::count_used() {
  uint64_t n = 0;
  for (uint64_t i = 0; i < total_blocks / 8; i++) n += static_cast<uint64_t>(__builtin_popcount(bits[i]));
  for (uint64_t b = (total_blocks / 8) * 8; b < total_blocks; b++) n += test(b) ? 1 : 0;
  used_blocks = n;
}

namespace {
using Bytes = std::vector<uint8_t>;

uint16_t L16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t L32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint64_t L64(const uint8_t* p) { return static_cast<uint64_t>(L32(p)) | (static_cast<uint64_t>(L32(p + 4)) << 32); }

FsStatus fail(Errc e, const char* code, const std::string& msg) {
  FsStatus f;
  f.st = Status::Make(e);
  f.code = code;
  f.message = msg;
  return f;
}
FsStatus good() {
  FsStatus f;
  f.code = "ok";
  return f;
}
FsStatus io_fail(const Status& s, const char* what) {
  FsStatus f;
  f.st = s;
  f.code = "read-error";
  f.message = std::string("could not read ") + what;
  return f;
}

Status rd(BlockDevice& d, uint64_t off, size_t len, Bytes* out) {
  out->assign(len, 0);
  if (len == 0) return Status::Ok();
  return d.read_at(off, out->data(), len);
}

bool bitmap_too_big(uint64_t total, const ExtractOptions& opt) { return (total + 7) / 8 > opt.max_bitmap_bytes; }

void make_all_used(UsedMap* m) {
  std::fill(m->bits.begin(), m->bits.end(), 0);
  m->set_range(0, m->total_blocks);
  m->full_copy = true;
}

// ------------------------------------------------------------------------------------------------------------- ext
bool ext_bg_has_super(uint32_t g, uint32_t compat, uint32_t ro, const uint32_t* backup_bgs) {
  if (g == 0) return true;
  if (compat & 0x200) return g == backup_bgs[0] || g == backup_bgs[1];  // sparse_super2
  if (!(ro & 0x1)) return true;
  if (g == 1) return true;
  if (!(g & 1)) return false;
  for (uint32_t b : {3u, 5u, 7u}) {
    uint64_t p = b;
    while (p < g) p *= b;
    if (p == g) return true;
  }
  return false;
}

FsStatus build_ext(BlockDevice& dev, const ExtractOptions& opt, UsedMap* m) {
  Bytes sb;
  Status s = rd(dev, 1024, 1024, &sb);
  if (!s.ok()) return io_fail(s, "the ext superblock");
  if (L16(&sb[56]) != 0xEF53) return fail(Errc::Corrupt, "bad-superblock", "not an ext superblock");
  uint32_t compat = L32(&sb[92]), incompat = L32(&sb[96]), ro = L32(&sb[100]);
  bool is64 = (incompat & 0x80) != 0;
  uint64_t blocks = L32(&sb[4]) | (is64 ? static_cast<uint64_t>(L32(&sb[336])) << 32 : 0);
  uint32_t log_bs = L32(&sb[24]);
  if (log_bs > 6) return fail(Errc::Corrupt, "bad-superblock", "unsupported block size");
  uint64_t bs = 1024ull << log_bs;
  uint32_t first_data = L32(&sb[20]), bpg = L32(&sb[32]), ipg = L32(&sb[40]);
  bool bigalloc = (ro & 0x200) != 0;
  uint32_t ratio = 1;
  if (bigalloc) {
    uint32_t lc = L32(&sb[28]);
    if (lc < log_bs || lc - log_bs > 16) return fail(Errc::Corrupt, "bad-superblock", "bad cluster size");
    ratio = 1u << (lc - log_bs);
  }
  uint32_t cpg = bigalloc ? L32(&sb[36]) : bpg;
  uint32_t rev = L32(&sb[76]);
  uint32_t isize = rev >= 1 ? L16(&sb[88]) : 128;
  uint32_t desc_size = is64 ? L16(&sb[254]) : 32;
  uint32_t reserved_gdt = L16(&sb[206]);
  uint32_t first_meta_bg = L32(&sb[260]);
  bool meta_bg = (incompat & 0x10) != 0;
  uint32_t backup_bgs[2] = {L32(&sb[0x24c]), L32(&sb[0x250])};
  if (bpg == 0 || cpg == 0 || ipg == 0 || isize < 128 || isize > bs || blocks == 0 || first_data >= blocks || first_data > 1) return fail(Errc::Corrupt, "bad-superblock", "implausible superblock fields");
  if (desc_size < 32 || (is64 && desc_size < 64) || desc_size > bs || (desc_size & (desc_size - 1))) return fail(Errc::Corrupt, "bad-superblock", "bad descriptor size");
  if (bs * 8 < cpg) return fail(Errc::Corrupt, "bad-superblock", "clusters per group exceed the bitmap block");
  uint64_t groups64 = (blocks - first_data + bpg - 1) / bpg;
  if (groups64 > (1ull << 32) / 2) return fail(Errc::Corrupt, "bad-superblock", "too many groups");
  uint32_t groups = static_cast<uint32_t>(groups64);
  if (bitmap_too_big(blocks, opt)) return fail(Errc::NoMemory, "bitmap-too-large", "the bitmap would be too large");
  if (dev.size() / bs < blocks) return fail(Errc::Corrupt, "fs-larger-than-device", "the filesystem is larger than the device");

  m->fs = "EXTFS";
  m->block_size = static_cast<uint32_t>(bs);
  m->total_blocks = blocks;
  m->device_size = blocks * bs;
  m->bits.assign(static_cast<size_t>((blocks + 7) / 8), 0);

  bool needs_recovery = (incompat & 0x4) != 0;
  bool not_clean = !(L16(&sb[58]) & 1);
  if (needs_recovery || not_clean) {
    if (!opt.full_if_unclean) return fail(Errc::Corrupt, "fs-not-clean", needs_recovery ? "the journal has not been replayed (needs_recovery)" : "the filesystem was not cleanly unmounted");
    m->warnings.push_back(needs_recovery ? "journal not replayed: every block is copied" : "not cleanly unmounted: every block is copied");
    make_all_used(m);
    m->count_used();
    return good();
  }

  uint64_t desc_per_block = bs / desc_size;
  uint64_t itb = (static_cast<uint64_t>(ipg) * isize + bs - 1) / bs;
  uint64_t gdt_blocks = (groups + desc_per_block - 1) / desc_per_block;
  bool group_csum = (ro & (0x10 | 0x400)) != 0;
  m->set_range(0, first_data + 1);  // ブート領域と先頭のスーパーブロック

  // グループ記述子の読み込み (meta_bg では記述子ブロックの置き場所が違う)
  auto desc_block_loc = [&](uint64_t i) -> uint64_t {
    uint64_t adjust = (i == 0 && bs == 1024 && ratio > 1) ? 1 : 0;
    if (!meta_bg || i < first_meta_bg) return first_data + i + 1 + adjust;
    uint64_t bg = desc_per_block * i;
    uint64_t first = first_data + bg * bpg;
    bool has_super = ext_bg_has_super(static_cast<uint32_t>(std::min<uint64_t>(bg, 0xFFFFFFFFu)), compat, ro, backup_bgs);
    return first + (has_super ? 1 : 0) + adjust;
  };
  Bytes gd;
  uint64_t gd_block = UINT64_MAX;
  for (uint32_t g = 0; g < groups; g++) {
    uint64_t dblock = g / desc_per_block;
    if (dblock != gd_block) {
      uint64_t loc = desc_block_loc(dblock);
      if (loc >= blocks) return fail(Errc::Corrupt, "bad-descriptor", "group descriptor block is out of range");
      s = rd(dev, loc * bs, static_cast<size_t>(bs), &gd);
      if (!s.ok()) return io_fail(s, "a group descriptor block");
      gd_block = dblock;
    }
    const uint8_t* d = &gd[static_cast<size_t>((g % desc_per_block) * desc_size)];
    uint64_t bb = L32(d), ib = L32(d + 4), it = L32(d + 8);
    uint16_t flags = L16(d + 18);
    if (is64 && desc_size >= 64) {
      bb |= static_cast<uint64_t>(L32(d + 0x20)) << 32;
      ib |= static_cast<uint64_t>(L32(d + 0x24)) << 32;
      it |= static_cast<uint64_t>(L32(d + 0x28)) << 32;
    }
    uint64_t gb = first_data + static_cast<uint64_t>(g) * bpg;
    uint64_t gblocks = std::min<uint64_t>(bpg, blocks - gb);
    if (bb >= blocks || ib >= blocks || it >= blocks || itb > blocks - it) return fail(Errc::Corrupt, "bad-descriptor", "group metadata is out of range");
    m->set(bb);
    m->set(ib);
    m->set_range(it, itb);
    bool uninit = group_csum && (flags & 0x2);
    if (uninit) {
      uint64_t over = ext_bg_has_super(g, compat, ro, backup_bgs) ? 1 + gdt_blocks + reserved_gdt : 0;
      if (meta_bg) over += 2;
      m->set_range(gb, std::min<uint64_t>(over, gblocks));
      continue;
    }
    Bytes bm;
    s = rd(dev, bb * bs, static_cast<size_t>(bs), &bm);
    if (!s.ok()) return io_fail(s, "a block bitmap");
    uint64_t nclusters = (gblocks + ratio - 1) / ratio;
    for (uint64_t i = 0; i < nclusters; i++)
      if ((bm[static_cast<size_t>(i >> 3)] >> (i & 7)) & 1) m->set_range(gb + i * ratio, ratio);
  }
  m->count_used();
  return good();
}

// ------------------------------------------------------------------------------------------------------------- FAT
FsStatus build_fat(BlockDevice& dev, const ExtractOptions& opt, UsedMap* m) {
  Bytes bs0;
  Status s = rd(dev, 0, 512, &bs0);
  if (!s.ok()) return io_fail(s, "the boot sector");
  const uint8_t* b = bs0.data();
  if (b[510] != 0x55 || b[511] != 0xAA) return fail(Errc::Corrupt, "bad-boot-sector", "missing boot signature");
  uint32_t bps = L16(b + 11), spc = b[13], rsv = L16(b + 14), nfats = b[16], root_ent = L16(b + 17);
  uint64_t ts = L16(b + 19) ? L16(b + 19) : L32(b + 32);
  uint64_t fatsz = L16(b + 22) ? L16(b + 22) : L32(b + 36);
  if (bps < 512 || bps > 4096 || (bps & (bps - 1)) || spc == 0 || (spc & (spc - 1)) || rsv == 0 || nfats == 0 || nfats > 2 || ts == 0 || fatsz == 0)
    return fail(Errc::Corrupt, "bad-boot-sector", "implausible BPB");
  uint64_t root_secs = (static_cast<uint64_t>(root_ent) * 32 + bps - 1) / bps;
  uint64_t first_data = rsv + nfats * fatsz + root_secs;
  if (first_data >= ts) return fail(Errc::Corrupt, "bad-boot-sector", "the data area is outside the volume");
  uint64_t clusters = (ts - first_data) / spc;
  // FAT32 かどうかは BPB の配置で決める (16 ビットの FAT サイズ欄が 0)。クラスタ数が少ない FAT32 も、Linux や fsck.fat と同じく FAT32 として読む。
  bool fat32 = L16(b + 22) == 0;
  if (fat32 && root_ent != 0) return fail(Errc::Corrupt, "bad-boot-sector", "FAT32 with a fixed root directory");
  const char* fsname = fat32 ? "FAT32" : clusters < 4085 ? "FAT12" : "FAT16";
  if (bitmap_too_big(ts, opt)) return fail(Errc::NoMemory, "bitmap-too-large", "the bitmap would be too large");
  if (dev.size() / bps < ts) return fail(Errc::Corrupt, "fs-larger-than-device", "the filesystem is larger than the device");
  uint64_t entry_bits = fat32 ? 32 : clusters < 4085 ? 12 : 16;
  uint64_t fat_bytes = (entry_bits == 12 ? ((clusters + 2) * 3 + 1) / 2 : (clusters + 2) * entry_bits / 8);
  if (fat_bytes > fatsz * bps) return fail(Errc::Corrupt, "bad-boot-sector", "the FAT is too small for the cluster count");

  m->fs = fsname;
  m->block_size = bps;
  m->total_blocks = ts;
  m->device_size = ts * bps;
  m->bits.assign(static_cast<size_t>((ts + 7) / 8), 0);
  m->set_range(0, first_data);

  // 状態ビット (読み取りのみの警告)
  if (fat32 ? (b[0x41] & 1) : (b[0x25] & 1)) m->warnings.push_back("the dirty flag of the volume is set");

  // FAT を 1 MiB ずつ読む。FAT12 は 3 バイト = 2 エントリの境界で処理する。
  uint64_t fat_off = static_cast<uint64_t>(rsv) * bps;
  const uint64_t kPiece = 3 * 1024 * 512;  // 3 と 2/4 バイトの倍数
  Bytes buf;
  uint64_t next_cluster = 0;  // 次に調べるエントリ番号 (0 から)
  uint64_t total_entries = clusters + 2;
  for (uint64_t pos = 0; pos < fat_bytes; pos += kPiece) {
    size_t n = static_cast<size_t>(std::min<uint64_t>(kPiece, fat_bytes - pos));
    s = rd(dev, fat_off + pos, n, &buf);
    if (!s.ok()) return io_fail(s, "the FAT");
    uint64_t entries_here = entry_bits == 12 ? (static_cast<uint64_t>(n) * 2 / 3) : n * 8 / entry_bits;
    if (pos + n >= fat_bytes) entries_here = total_entries - next_cluster;  // 最後の端数
    for (uint64_t k = 0; k < entries_here && next_cluster < total_entries; k++, next_cluster++) {
      uint32_t v;
      if (entry_bits == 12) {
        uint64_t o = (k * 3) / 2;
        if (o + 1 >= n) break;
        uint16_t w = L16(&buf[static_cast<size_t>(o)]);
        v = (k & 1) ? (w >> 4) : (w & 0xFFF);
      } else if (entry_bits == 16) {
        v = L16(&buf[static_cast<size_t>(k * 2)]);
      } else {
        v = L32(&buf[static_cast<size_t>(k * 4)]) & 0x0FFFFFFF;
      }
      if (next_cluster >= 2 && v != 0) m->set_range(first_data + (next_cluster - 2) * spc, spc);
    }
  }
  // 最後のクラスタに満たない端数のセクタ (どのクラスタにも属さない) も、Partclone と同じく使用中として保存する
  uint64_t slack_start = first_data + clusters * spc;
  if (slack_start < ts) m->set_range(slack_start, ts - slack_start);
  m->count_used();
  return good();
}

// ----------------------------------------------------------------------------------------------------------- exFAT
FsStatus build_exfat(BlockDevice& dev, const ExtractOptions& opt, UsedMap* m) {
  Bytes bs0;
  Status s = rd(dev, 0, 512, &bs0);
  if (!s.ok()) return io_fail(s, "the boot sector");
  const uint8_t* b = bs0.data();
  if (std::memcmp(b + 3, "EXFAT   ", 8) != 0) return fail(Errc::Corrupt, "bad-boot-sector", "not an exFAT boot sector");
  uint32_t bps_shift = b[108], spc_shift = b[109];
  if (bps_shift < 9 || bps_shift > 12 || spc_shift > 25 - bps_shift) return fail(Errc::Corrupt, "bad-boot-sector", "bad sector or cluster size");
  uint64_t bps = 1ull << bps_shift, spc = 1ull << spc_shift;
  uint64_t vol_len = L64(b + 72), fat_off = L32(b + 80), fat_len = L32(b + 84), heap_off = L32(b + 88), cc = L32(b + 92), root = L32(b + 96);
  uint32_t flags = L16(b + 106);
  if (vol_len == 0 || heap_off == 0 || cc == 0 || heap_off >= vol_len || root < 2 || root >= cc + 2 || heap_off + cc * spc > vol_len || fat_off + fat_len > heap_off)
    return fail(Errc::Corrupt, "bad-boot-sector", "implausible exFAT parameters");
  if (bitmap_too_big(vol_len, opt)) return fail(Errc::NoMemory, "bitmap-too-large", "the bitmap would be too large");
  if (dev.size() / bps < vol_len) return fail(Errc::Corrupt, "fs-larger-than-device", "the filesystem is larger than the device");
  uint64_t cbytes = bps * spc;

  m->fs = "EXFAT";
  m->block_size = static_cast<uint32_t>(bps);
  m->total_blocks = vol_len;
  m->device_size = vol_len * bps;
  m->bits.assign(static_cast<size_t>((vol_len + 7) / 8), 0);
  m->set_range(0, heap_off);
  if (flags & 0x2) m->warnings.push_back("the volume dirty flag is set");

  // ルートディレクトリ (FAT チェーン) から割り当てビットマップのエントリ (0x81) を探す
  Bytes cl, fatb;
  uint64_t bm_first = 0, bm_len = 0;
  bool found = false;
  uint64_t c = root;
  for (uint64_t hops = 0; hops <= cc && !found; hops++) {
    s = rd(dev, (heap_off + (c - 2) * spc) * bps, static_cast<size_t>(cbytes), &cl);
    if (!s.ok()) return io_fail(s, "the root directory");
    bool end = false;
    for (size_t o = 0; o + 32 <= cl.size(); o += 32) {
      uint8_t t = cl[o];
      if (t == 0) { end = true; break; }
      if (t == 0x81 && (cl[o + 1] & 1) == 0) {
        bm_first = L32(&cl[o + 20]);
        bm_len = L64(&cl[o + 24]);
        found = true;
        break;
      }
    }
    if (found || end) break;
    s = rd(dev, fat_off * bps + c * 4, 4, &fatb);
    if (!s.ok()) return io_fail(s, "the FAT");
    uint32_t nx = L32(fatb.data());
    if (nx < 2 || nx >= cc + 2) break;
    c = nx;
  }
  if (!found) return fail(Errc::Corrupt, "no-allocation-bitmap", "the root directory has no allocation bitmap entry");
  uint64_t need = (cc + 7) / 8;
  if (bm_first < 2 || bm_first >= cc + 2 || bm_len < need || bm_first - 2 + (need + cbytes - 1) / cbytes > cc) return fail(Errc::Corrupt, "bad-allocation-bitmap", "the allocation bitmap is out of range");
  Bytes bm;
  s = rd(dev, (heap_off + (bm_first - 2) * spc) * bps, static_cast<size_t>(need), &bm);
  if (!s.ok()) return io_fail(s, "the allocation bitmap");
  for (uint64_t i = 0; i < cc; i++)
    if ((bm[static_cast<size_t>(i >> 3)] >> (i & 7)) & 1) m->set_range(heap_off + i * spc, spc);
  m->count_used();
  return good();
}

// ------------------------------------------------------------------------------------------------------------ NTFS
// MFT レコードを読み、更新シーケンス (fixup) を適用する。
FsStatus ntfs_record(BlockDevice& dev, uint64_t off, uint32_t rs, Bytes* rec) {
  Status s = rd(dev, off, rs, rec);
  if (!s.ok()) return io_fail(s, "an MFT record");
  Bytes& r = *rec;
  if (rs < 48 || std::memcmp(r.data(), "FILE", 4) != 0) return fail(Errc::Corrupt, "bad-mft-record", "the MFT record has no FILE signature");
  uint32_t usa_ofs = L16(&r[4]), usa_cnt = L16(&r[6]);
  if (usa_cnt < 2 || usa_ofs + usa_cnt * 2ull > rs || (usa_cnt - 1) * 512ull > rs) return fail(Errc::Corrupt, "bad-mft-record", "bad update sequence array");
  uint16_t usn = L16(&r[usa_ofs]);
  for (uint32_t i = 1; i < usa_cnt; i++) {
    size_t p = i * 512 - 2;
    if (p + 2 > rs) break;
    if (L16(&r[p]) != usn) return fail(Errc::Corrupt, "bad-mft-record", "update sequence mismatch (torn record)");
    r[p] = r[usa_ofs + i * 2];
    r[p + 1] = r[usa_ofs + i * 2 + 1];
  }
  return good();
}

struct NtfsAttr { const uint8_t* p = nullptr; uint32_t len = 0; };
// 名前なしの type 属性を探す。見つからなければ p == nullptr。範囲外は Corrupt。
FsStatus ntfs_find_attr(const Bytes& r, uint32_t type, NtfsAttr* out) {
  out->p = nullptr;
  uint32_t o = L16(&r[20]);
  uint32_t used = L32(&r[24]);
  if (used > r.size()) used = static_cast<uint32_t>(r.size());
  for (int guard = 0; guard < 64; guard++) {
    if (o + 8 > used) return fail(Errc::Corrupt, "bad-mft-record", "attribute runs past the record");
    uint32_t t = L32(&r[o]);
    if (t == 0xFFFFFFFFu) return good();
    uint32_t len = L32(&r[o + 4]);
    if (len < 16 || len % 8 || o + len > used) return fail(Errc::Corrupt, "bad-mft-record", "bad attribute length");
    if (t == type && r[o + 9] == 0) {
      out->p = &r[o];
      out->len = len;
      return good();
    }
    o += len;
  }
  return fail(Errc::Corrupt, "bad-mft-record", "too many attributes");
}

FsStatus build_ntfs(BlockDevice& dev, const ExtractOptions& opt, UsedMap* m) {
  Bytes bs0;
  Status s = rd(dev, 0, 512, &bs0);
  if (!s.ok()) return io_fail(s, "the boot sector");
  const uint8_t* b = bs0.data();
  if (std::memcmp(b + 3, "NTFS    ", 8) != 0) return fail(Errc::Corrupt, "bad-boot-sector", "not an NTFS boot sector");
  uint64_t bps = L16(b + 11);
  uint64_t spc = b[13] <= 0x80 ? b[13] : (1ull << (256 - b[13]));
  uint64_t ts = L64(b + 40), mft_lcn = L64(b + 48);
  int8_t cpr = static_cast<int8_t>(b[64]);
  if (bps < 512 || bps > 4096 || (bps & (bps - 1)) || spc == 0 || (spc & (spc - 1)) || ts == 0) return fail(Errc::Corrupt, "bad-boot-sector", "implausible NTFS BPB");
  uint64_t cs = bps * spc;
  if (cs > (1u << 21)) return fail(Errc::Corrupt, "bad-boot-sector", "cluster size too large");
  uint64_t rs = cpr > 0 ? static_cast<uint64_t>(cpr) * cs : (cpr >= -20 ? (1ull << -cpr) : 0);
  if (rs < 1024 || rs > 65536 || (rs & (rs - 1))) return fail(Errc::Corrupt, "bad-boot-sector", "bad MFT record size");
  uint64_t clusters = ts / spc;
  if (clusters == 0 || mft_lcn >= clusters) return fail(Errc::Corrupt, "bad-boot-sector", "the MFT is outside the volume");
  if (bitmap_too_big(clusters, opt)) return fail(Errc::NoMemory, "bitmap-too-large", "the bitmap would be too large");
  if (dev.size() / bps < ts) return fail(Errc::Corrupt, "fs-larger-than-device", "the filesystem is larger than the device");
  uint64_t mft_off = mft_lcn * cs;

  m->fs = "NTFS";
  m->block_size = static_cast<uint32_t>(cs);
  m->total_blocks = clusters;
  m->device_size = ts * bps + (dev.size() >= (ts + 1) * bps ? bps : 0);
  m->bits.assign(static_cast<size_t>((clusters + 7) / 8), 0);

  // $Volume (レコード 3) の dirty フラグ
  bool dirty = false;
  {
    Bytes r;
    FsStatus f = ntfs_record(dev, mft_off + 3 * rs, static_cast<uint32_t>(rs), &r);
    if (f.ok()) {
      NtfsAttr a;
      if (ntfs_find_attr(r, 0x70, &a).ok() && a.p && a.p[8] == 0 && a.len >= 32) dirty = (L16(a.p + L16(a.p + 20) + 10) & 1) != 0;
    }
  }
  if (dirty) {
    if (!opt.full_if_unclean) return fail(Errc::Corrupt, "fs-not-clean", "the NTFS volume is marked dirty (unclean shutdown or hibernation)");
    m->warnings.push_back("volume dirty: every cluster is copied");
    make_all_used(m);
    m->count_used();
    return good();
  }

  // $Bitmap (レコード 6) の $DATA
  Bytes rec;
  FsStatus f = ntfs_record(dev, mft_off + 6 * rs, static_cast<uint32_t>(rs), &rec);
  if (!f.ok()) return f;
  NtfsAttr a;
  f = ntfs_find_attr(rec, 0x80, &a);
  if (!f.ok()) return f;
  if (!a.p) return fail(Errc::Unsupported, "bitmap-attribute-list", "$Bitmap has no $DATA in its base record (attribute list is not supported)");
  uint64_t need = (clusters + 7) / 8;
  Bytes bm;
  if (a.p[8] == 0) {  // 常駐
    uint32_t vlen = L32(a.p + 16), voff = L16(a.p + 20);
    if (voff + static_cast<uint64_t>(vlen) > a.len || vlen < need) return fail(Errc::Corrupt, "bad-bitmap", "resident $Bitmap is too short");
    bm.assign(a.p + voff, a.p + voff + need);
  } else {
    if (a.len < 64 || L64(a.p + 16) != 0) return fail(Errc::Unsupported, "bitmap-fragmented", "the $Bitmap data attribute does not start at VCN 0");
    uint64_t real = L64(a.p + 48);
    uint32_t ro = L16(a.p + 32);
    if (real < need || ro >= a.len) return fail(Errc::Corrupt, "bad-bitmap", "$Bitmap is shorter than the cluster count");
    bm.assign(static_cast<size_t>(need), 0);
    uint64_t got = 0;  // 読んだバイト数
    int64_t lcn = 0;
    const uint8_t* p = a.p + ro;
    const uint8_t* end = a.p + a.len;
    while (p < end && *p && got < need) {
      unsigned ls = *p & 0xF, os = *p >> 4;
      p++;
      if (ls == 0 || ls > 8 || os > 8 || p + ls + os > end) return fail(Errc::Corrupt, "bad-runlist", "bad run list");
      uint64_t len = 0;
      for (unsigned i = 0; i < ls; i++) len |= static_cast<uint64_t>(p[i]) << (8 * i);
      p += ls;
      int64_t delta = 0;
      if (os) {
        for (unsigned i = 0; i < os; i++) delta |= static_cast<int64_t>(p[i]) << (8 * i);
        if (p[os - 1] & 0x80 && os < 8) delta |= -(int64_t(1) << (8 * os));
        p += os;
        lcn += delta;
      } else {
        return fail(Errc::Corrupt, "bad-runlist", "sparse $Bitmap");
      }
      if (lcn < 0 || len == 0 || static_cast<uint64_t>(lcn) >= clusters || len > clusters - static_cast<uint64_t>(lcn)) return fail(Errc::Corrupt, "bad-runlist", "run is outside the volume");
      uint64_t take = std::min<uint64_t>(len * cs, need - got);
      Bytes tmp;
      s = rd(dev, static_cast<uint64_t>(lcn) * cs, static_cast<size_t>(take), &tmp);
      if (!s.ok()) return io_fail(s, "$Bitmap");
      std::memcpy(&bm[static_cast<size_t>(got)], tmp.data(), static_cast<size_t>(take));
      got += take;
    }
    if (got < need) return fail(Errc::Corrupt, "bad-runlist", "the run list does not cover $Bitmap");
  }
  for (uint64_t i = 0; i < clusters; i++)
    if ((bm[static_cast<size_t>(i >> 3)] >> (i & 7)) & 1) m->set(i);
  m->count_used();
  return good();
}
}  // namespace

FsStatus build_used_map(BlockDevice& dev, const ExtractOptions& opt, UsedMap* out) {
  try {
    Status io;
    FsType t = probe_filesystem(dev, 0, dev.size(), &io);
    if (!io.ok()) return io_fail(io, "the filesystem signature");
    UsedMap m;
    FsStatus r;
    switch (t) {
      case FsType::Ext2: case FsType::Ext3: case FsType::Ext4: r = build_ext(dev, opt, &m); break;
      case FsType::Fat12: case FsType::Fat16: case FsType::Fat32: r = build_fat(dev, opt, &m); break;
      case FsType::Exfat: r = build_exfat(dev, opt, &m); break;
      case FsType::Ntfs: r = build_ntfs(dev, opt, &m); break;
      case FsType::BitLocker: case FsType::Luks:
        return fail(Errc::Unsupported, "encrypted", "the volume is encrypted: unlock it first, then back up the decrypted device");
      default:
        return fail(Errc::Unsupported, "unsupported-fs", std::string("backup is not supported for ") + fs_type_name(t));
    }
    if (r.ok()) *out = std::move(m);
    return r;
  } catch (const std::bad_alloc&) {
    return fail(Errc::NoMemory, "no-memory", "out of memory");
  }
}

}  // namespace usbmem
