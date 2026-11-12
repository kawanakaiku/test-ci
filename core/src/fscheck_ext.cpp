// SPDX-License-Identifier: Apache-2.0
// ext2/3/4 の読み取り専用の整合性検査 (e2fsck -fn 相当の主要部分)。何も書かない。
//
// 実装した検査 (e2fsck のパスに対応):
//   スーパーブロック / グループ記述子の妥当性とチェックサム、
//   パス 1: 全 inode の走査 (モード・ブロックマップ / extent 木・i_blocks・i_size・重複ブロック・拡張属性ブロック・inline_data)、
//   パス 2: ディレクトリブロックの構造 (rec_len・名前・参照先 inode・file type・重複名・リーフのチェックサム)、
//   パス 3: ディレクトリの接続性 (ルートから到達できるか・".." の整合・lost+found)、
//   パス 4: リンクカウント、
//   パス 5: ブロック / inode ビットマップと空きカウント。
// 未対応 (検査をやめて Incomplete): bigalloc / meta_bg / 未知の incompat 機能。htree の内部ハッシュ順序は検査しない。
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "fscheck_internal.h"

namespace usbmem {
namespace fsck {
namespace {

// ---------------------------------------------------------------- チェックサム

struct Crc32cTable {
  uint32_t t[256];
  Crc32cTable() {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
      t[i] = c;
    }
  }
};
// 生の crc32c (ext4 は ~ をかけない): crc = update(crc, data)
uint32_t crc32c(uint32_t crc, const void* data, size_t n) {
  static const Crc32cTable T;
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < n; i++) crc = T.t[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return crc;
}
uint16_t crc16(uint16_t crc, const void* data, size_t n) {
  static uint16_t T[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; i++) {
      uint16_t c = static_cast<uint16_t>(i);
      for (int k = 0; k < 8; k++) c = (c & 1) ? static_cast<uint16_t>(0xA001u ^ (c >> 1)) : static_cast<uint16_t>(c >> 1);
      T[i] = c;
    }
    init = true;
  }
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < n; i++) crc = static_cast<uint16_t>((crc >> 8) ^ T[(crc ^ p[i]) & 0xFF]);
  return crc;
}

// ---------------------------------------------------------------- 定数

constexpr uint32_t COMPAT_HAS_JOURNAL = 0x4, COMPAT_RESIZE_INODE = 0x10, COMPAT_DIR_INDEX = 0x20, COMPAT_SPARSE_SUPER2 = 0x200;
constexpr uint32_t INCOMPAT_FILETYPE = 0x2, INCOMPAT_RECOVER = 0x4, INCOMPAT_JOURNAL_DEV = 0x8, INCOMPAT_META_BG = 0x10,
                   INCOMPAT_EXTENTS = 0x40, INCOMPAT_64BIT = 0x80, INCOMPAT_MMP = 0x100, INCOMPAT_FLEX_BG = 0x200,
                   INCOMPAT_EA_INODE = 0x400, INCOMPAT_DIRDATA = 0x1000, INCOMPAT_CSUM_SEED = 0x2000, INCOMPAT_LARGEDIR = 0x4000,
                   INCOMPAT_INLINE_DATA = 0x8000, INCOMPAT_ENCRYPT = 0x10000, INCOMPAT_CASEFOLD = 0x20000;
constexpr uint32_t INCOMPAT_KNOWN = 0x2 | 0x4 | 0x8 | 0x10 | 0x40 | 0x80 | 0x100 | 0x200 | 0x400 | 0x1000 | 0x2000 | 0x4000 | 0x8000 | 0x10000 | 0x20000;
constexpr uint32_t RO_SPARSE_SUPER = 0x1, RO_LARGE_FILE = 0x2, RO_HUGE_FILE = 0x8, RO_GDT_CSUM = 0x10, RO_DIR_NLINK = 0x20,
                   RO_EXTRA_ISIZE = 0x40, RO_QUOTA = 0x100, RO_BIGALLOC = 0x200, RO_METADATA_CSUM = 0x400, RO_PROJECT = 0x2000, RO_VERITY = 0x8000;
constexpr uint32_t GD_INODE_UNINIT = 1, GD_BLOCK_UNINIT = 2;
constexpr uint32_t INO_EXTENTS_FL = 0x80000, INO_INLINE_DATA_FL = 0x10000000, INO_HUGE_FILE_FL = 0x40000, INO_INDEX_FL = 0x1000;

struct Sb {
  uint32_t inodes_count = 0;
  uint64_t blocks_count = 0, free_blocks = 0;
  uint32_t free_inodes = 0, first_data_block = 0, log_bs = 0, bpg = 0, ipg = 0, rev = 0, first_ino = 11;
  uint16_t inode_size = 128, desc_size = 32, state = 0, minor = 0, reserved_gdt = 0;
  uint32_t compat = 0, incompat = 0, ro = 0, journal_inum = 0, last_orphan = 0, csum_seed = 0, bs = 1024;
  uint8_t uuid[16] = {0};
  uint8_t csum_type = 0;
  uint32_t groups = 0, backup_bgs[2] = {0, 0};
  uint32_t internal_inos[4] = {0, 0, 0, 0};  // orphan_file / quota (ディレクトリから参照されない内部 inode)
  bool csum_sb = false, csum_gd16 = false;
  bool has(uint32_t f, int which) const { return (which == 0 ? compat : which == 1 ? incompat : ro) & f; }
};

struct Gd {
  uint64_t block_bitmap = 0, inode_bitmap = 0, inode_table = 0;
  uint32_t free_blocks = 0, free_inodes = 0, used_dirs = 0, itable_unused = 0;
  uint16_t flags = 0, csum = 0;
  uint32_t bb_csum = 0, ib_csum = 0;
};

struct DirRec {
  std::vector<uint64_t> blocks;  // 論理順の物理ブロック (0 = 穴)
  bool inl = false;
  std::vector<uint8_t> inl_data;  // inline_data のとき: i_block 60 バイト + system.data の値
  uint64_t size = 0;
  uint32_t gen = 0;
};

class ExtChk {
 public:
  ExtChk(Ctx& c, FsType t) : c_(c), v_(c.vol), want_(t) {}
  Status run();

 private:
  Ctx& c_;
  Vol& v_;
  FsType want_;
  Sb s_;
  std::vector<Gd> gd_;
  Bits found_, imap_used_;
  std::vector<uint16_t> counted_;
  bool have_counted_ = false;
  std::unordered_map<uint32_t, DirRec> dirs_;
  std::unordered_map<uint32_t, uint32_t> parent_by_entry_, dotdot_;
  std::unordered_map<uint64_t, uint32_t> ea_refs_;       // EA ブロック -> この走査で見た参照数
  std::unordered_map<uint64_t, uint32_t> ea_refcount_;   // EA ブロック -> h_refcount
  std::unordered_set<uint64_t> ea_blocks_;
  std::unordered_map<std::string, uint32_t> root_names_;
  uint64_t free_blocks_counted_ = 0;
  uint64_t used_inodes_ = 0;
  uint32_t dir_count_ = 0;
  bool root_ok_ = false;
  bool journal_ok_ = true;
  std::vector<uint32_t> meta_cnt_;
  uint64_t journal_first_ = 0;
  bool journal_seen_ = false;

  uint64_t blk_off(uint64_t b) const { return b * s_.bs; }
  bool blk_valid(uint64_t b) const { return b >= s_.first_data_block && b < s_.blocks_count; }
  bool is64() const { return s_.has(INCOMPAT_64BIT, 1); }
  bool csum_meta() const { return s_.has(RO_METADATA_CSUM, 2); }
  bool csum_gdt() const { return s_.has(RO_GDT_CSUM, 2) && !csum_meta(); }
  uint32_t group_of_block(uint64_t b) const { return static_cast<uint32_t>((b - s_.first_data_block) / s_.bpg); }
  uint64_t group_first_block(uint32_t g) const { return s_.first_data_block + static_cast<uint64_t>(g) * s_.bpg; }
  uint64_t group_blocks(uint32_t g) const {
    uint64_t start = group_first_block(g);
    return std::min<uint64_t>(s_.bpg, s_.blocks_count - start);
  }
  bool group_has_super(uint32_t g) const;
  uint32_t itable_blocks() const { return static_cast<uint32_t>((static_cast<uint64_t>(s_.ipg) * s_.inode_size + s_.bs - 1) / s_.bs); }

  Status superblock();
  Status group_descriptors();
  Status mark_metadata();
  Status pass1();
  Status scan_inode(uint32_t ino, const uint8_t* raw, bool* in_use);
  Status scan_extent_node(const uint8_t* node, size_t node_bytes, int depth_left, uint32_t ino, uint32_t gen, struct Scan& sc, bool root);
  Status scan_blockmap(uint32_t ino, const uint8_t* iblock, struct Scan& sc);
  Status walk_indirect(uint64_t blk, int level, uint64_t base, uint32_t ino, struct Scan& sc);
  bool claim_block(uint64_t b, uint32_t ino, struct Scan& sc, const char* what);
  Status pass2();
  Status check_dir_block(uint32_t ino, const uint8_t* buf, size_t len, uint64_t file_off, bool is_first, bool leaf_tail, uint32_t gen, DirRec& dr, std::unordered_set<std::string>& names, bool* saw_dot, bool* saw_dotdot, bool inline_part);
  void count_ref(uint32_t target) { if (have_counted_ && target < counted_.size() && counted_[target] != 0xFFFF) counted_[target]++; }
  Status pass3();
  Status pass4();
  Status pass5();
  Status journal();
  uint32_t inode_csum(uint32_t ino, const uint8_t* raw) const;
  Status read_inode_raw(uint32_t ino, std::vector<uint8_t>* out);
  bool ea_in_inode(const uint8_t* raw, std::vector<uint8_t>* system_data) const;
};

struct Scan {
  uint64_t blocks = 0;        // 数えたブロック (データ + 間接 / extent 木ノード)
  uint64_t last_lblock = 0;   // 初期化済みデータの最後の論理ブロック + 1
  uint64_t dups = 0, dup_first = 0;
  uint64_t bad = 0;
  bool is_dir = false;
  std::vector<uint64_t>* dir_blocks = nullptr;
  uint64_t dir_cap = 1u << 22;
  bool dir_trunc = false;
  uint64_t next_logical = 0;   // block map: 次の論理ブロック番号
  uint64_t prev_end = 0;       // extent: 直前の extent の終わり
  bool any_extent = false;
  uint64_t first_phys = 0;     // 論理ブロック 0 の物理ブロック (ジャーナル inode 用)
};

bool ExtChk::group_has_super(uint32_t g) const {
  if (g == 0) return true;
  if (s_.has(COMPAT_SPARSE_SUPER2, 0)) return g == s_.backup_bgs[0] || g == s_.backup_bgs[1];
  if (!s_.has(RO_SPARSE_SUPER, 2)) return true;
  if (g == 1) return true;
  for (uint32_t b : {3u, 5u, 7u}) {
    uint64_t p = b;
    while (p < g) p *= b;
    if (p == g) return true;
  }
  return false;
}

Status ExtChk::superblock() {
  uint8_t sb[1024];
  Status st = v_.read(1024, sb, 1024);
  if (!st.ok()) return st;
  if (le16(sb + 0x38) != 0xEF53) { c_.error("ext-magic", "the superblock magic number is wrong", 1024 + 0x38); return Status::Make(Errc::Corrupt); }
  s_.inodes_count = le32(sb);
  s_.blocks_count = le32(sb + 4);
  s_.free_blocks = le32(sb + 12);
  s_.free_inodes = le32(sb + 16);
  s_.first_data_block = le32(sb + 20);
  s_.log_bs = le32(sb + 24);
  s_.bpg = le32(sb + 32);
  s_.ipg = le32(sb + 40);
  s_.state = le16(sb + 0x3A);
  s_.rev = le32(sb + 0x4C);
  if (s_.rev >= 1) { s_.first_ino = le32(sb + 0x54); s_.inode_size = le16(sb + 0x58); }
  s_.compat = le32(sb + 0x5C); s_.incompat = le32(sb + 0x60); s_.ro = le32(sb + 0x64);
  std::memcpy(s_.uuid, sb + 0x68, 16);
  s_.journal_inum = le32(sb + 0xE0);
  s_.last_orphan = le32(sb + 0xE8);
  s_.backup_bgs[0] = le32(sb + 0x24C); s_.backup_bgs[1] = le32(sb + 0x250);
  s_.internal_inos[0] = le32(sb + 0x280); s_.internal_inos[1] = le32(sb + 0x240); s_.internal_inos[2] = le32(sb + 0x244); s_.internal_inos[3] = le32(sb + 0x26C);
  if (s_.incompat & INCOMPAT_64BIT) {
    s_.desc_size = le16(sb + 0xFE);
    s_.blocks_count |= static_cast<uint64_t>(le32(sb + 0x150)) << 32;
    s_.free_blocks |= static_cast<uint64_t>(le32(sb + 0x158)) << 32;
  } else s_.desc_size = 32;
  s_.reserved_gdt = le16(sb + 0xCE);
  s_.csum_type = sb[0x175];
  s_.csum_seed = le32(sb + 0x270);

  if (s_.log_bs > 6) { c_.error("ext-block-size", "the block size shift " + num(s_.log_bs) + " is out of range", 1024 + 24); return Status::Make(Errc::Corrupt); }
  s_.bs = 1024u << s_.log_bs;
  bool bad = false;
  if (s_.incompat & ~INCOMPAT_KNOWN) { c_.incomplete("ext-unsupported-feature", "the file system has unknown incompatible features; it was not checked"); return Status::Make(Errc::Unsupported); }
  if (s_.incompat & INCOMPAT_JOURNAL_DEV) { c_.incomplete("ext-journal-device", "this is an external journal device, not a file system"); return Status::Make(Errc::Unsupported); }
  if (s_.incompat & INCOMPAT_META_BG) { c_.incomplete("ext-meta-bg", "meta_bg is not supported by this checker"); return Status::Make(Errc::Unsupported); }
  if (s_.ro & RO_BIGALLOC) { c_.incomplete("ext-bigalloc", "bigalloc is not supported by this checker"); return Status::Make(Errc::Unsupported); }
  if (s_.rev > 1) { c_.error("ext-revision", "unknown revision " + num(s_.rev), 1024 + 0x4C); return Status::Make(Errc::Corrupt); }
  if (s_.inode_size < 128 || !is_pow2(s_.inode_size) || s_.inode_size > s_.bs) { c_.error("ext-inode-size", "the inode size " + num(s_.inode_size) + " is invalid", 1024 + 0x58); bad = true; }
  if (s_.bpg == 0 || s_.bpg > 8u * s_.bs) { c_.error("ext-blocks-per-group", "blocks per group " + num(s_.bpg) + " is invalid", 1024 + 32); bad = true; }
  if (s_.ipg == 0 || s_.ipg > 8u * s_.bs) { c_.error("ext-inodes-per-group", "inodes per group " + num(s_.ipg) + " is invalid", 1024 + 40); bad = true; }
  if (s_.first_data_block != (s_.bs == 1024 ? 1u : 0u)) { c_.error("ext-first-data-block", "first data block " + num(s_.first_data_block) + " is wrong for this block size", 1024 + 20); bad = true; }
  if (s_.blocks_count == 0 || s_.blocks_count <= s_.first_data_block) { c_.error("ext-blocks-count", "the block count is invalid", 1024 + 4); bad = true; }
  if (bad) return Status::Make(Errc::Corrupt);
  if (s_.blocks_count * s_.bs > v_.size()) c_.error("ext-larger-than-device", "the file system claims " + num(s_.blocks_count) + " blocks but the device is smaller", 1024 + 4);
  if (s_.first_ino < 11) { c_.error("ext-first-ino", "first_ino " + num(s_.first_ino) + " is below 11", 1024 + 0x54); return Status::Make(Errc::Corrupt); }
  uint64_t data_blocks = s_.blocks_count - s_.first_data_block;
  s_.groups = static_cast<uint32_t>((data_blocks + s_.bpg - 1) / s_.bpg);
  if (static_cast<uint64_t>(s_.groups) * s_.ipg != s_.inodes_count) { c_.error("ext-inodes-count", "the inode count (" + num(s_.inodes_count) + ") does not equal groups x inodes per group (" + num(static_cast<uint64_t>(s_.groups) * s_.ipg) + ")", 1024); return Status::Make(Errc::Corrupt); }
  if (s_.has(INCOMPAT_64BIT, 1) && (s_.desc_size < 64 || s_.desc_size > 1024 || !is_pow2(s_.desc_size))) { c_.error("ext-desc-size", "the group descriptor size " + num(s_.desc_size) + " is invalid", 1024 + 0xFE); return Status::Make(Errc::Corrupt); }
  if (s_.free_blocks > s_.blocks_count) c_.error("ext-sb-free-blocks-range", "the free block count exceeds the block count", 1024 + 12);
  if (s_.free_inodes > s_.inodes_count) c_.error("ext-sb-free-inodes-range", "the free inode count exceeds the inode count", 1024 + 16);
  if (s_.state & 2) c_.warn("ext-has-errors", "the file system is marked as having errors", 1024 + 0x3A);
  if (!(s_.state & 1)) { c_.warn("ext-not-clean", "the file system was not cleanly unmounted", 1024 + 0x3A); c_.rep.unclean = true; }
  if (s_.incompat & INCOMPAT_RECOVER) { c_.warn("ext-needs-recovery", "the journal has not been replayed (needs_recovery); a read-only mount needs noload", 1024 + 0x60); c_.rep.unclean = true; c_.rep.needs_journal_recovery = true; }
  if (s_.last_orphan) c_.warn("ext-orphan-list", "the orphan inode list is not empty (inodes were being deleted/truncated)", 1024 + 0xE8);
  if (s_.has(RO_QUOTA, 2) || s_.has(RO_VERITY, 2)) c_.info("ext-feature-note", "quota / verity data is not checked");
  // スーパーブロックのチェックサム
  if (csum_meta()) {
    if (s_.csum_type != 1) c_.error("ext-csum-type", "unknown checksum type " + num(s_.csum_type), 1024 + 0x175);
    uint32_t c = crc32c(0xFFFFFFFFu, sb, 1020);
    if (c != le32(sb + 0x3FC)) c_.error("ext-sb-csum", "the superblock checksum is wrong", 1024 + 0x3FC);
    s_.csum_seed = s_.has(INCOMPAT_CSUM_SEED, 1) ? le32(sb + 0x270) : crc32c(0xFFFFFFFFu, s_.uuid, 16);
  }
  // 種別とラベルの整合
  bool has_j = s_.has(COMPAT_HAS_JOURNAL, 0);
  bool ext4like = (s_.incompat & (INCOMPAT_EXTENTS | INCOMPAT_64BIT | INCOMPAT_FLEX_BG)) || (s_.ro & (RO_HUGE_FILE | RO_DIR_NLINK | RO_EXTRA_ISIZE | RO_GDT_CSUM | RO_METADATA_CSUM));
  FsType actual = ext4like ? FsType::Ext4 : has_j ? FsType::Ext3 : FsType::Ext2;
  if (actual != want_) c_.info("ext-type-note", std::string("the on-disk features say ") + fs_type_name(actual) + ", detected as " + fs_type_name(want_));
  c_.rep.stats.units_total = s_.blocks_count;
  c_.rep.stats.bytes_per_unit = s_.bs;
  return Status::Ok();
}

Status ExtChk::group_descriptors() {
  uint64_t gdt_block = s_.first_data_block + 1;
  uint64_t gdt_bytes = static_cast<uint64_t>(s_.groups) * s_.desc_size;
  uint64_t gdt_blocks = (gdt_bytes + s_.bs - 1) / s_.bs;
  if (gdt_blocks + gdt_block > s_.blocks_count) { c_.error("ext-gdt-range", "the group descriptor table does not fit in the file system", blk_off(gdt_block)); return Status::Make(Errc::Corrupt); }
  if (c_.over_budget(static_cast<uint64_t>(s_.groups) * sizeof(Gd))) { c_.incomplete("memory-limit", "too many block groups for the memory budget"); return Status::Make(Errc::Unsupported); }
  gd_.resize(s_.groups);
  std::vector<uint8_t> raw(static_cast<size_t>(gdt_bytes));
  Status st = v_.read(blk_off(gdt_block), raw.data(), raw.size());
  if (!st.ok()) return st;
  uint64_t bad_csum = 0, first_bad = 0;
  for (uint32_t g = 0; g < s_.groups; g++) {
    const uint8_t* d = raw.data() + static_cast<size_t>(g) * s_.desc_size;
    Gd& x = gd_[g];
    x.block_bitmap = le32(d); x.inode_bitmap = le32(d + 4); x.inode_table = le32(d + 8);
    x.free_blocks = le16(d + 12); x.free_inodes = le16(d + 14); x.used_dirs = le16(d + 16);
    x.flags = le16(d + 18); x.itable_unused = le16(d + 28); x.csum = le16(d + 30);
    x.bb_csum = le16(d + 24); x.ib_csum = le16(d + 26);
    if (s_.desc_size >= 64) {
      x.block_bitmap |= static_cast<uint64_t>(le32(d + 32)) << 32;
      x.inode_bitmap |= static_cast<uint64_t>(le32(d + 36)) << 32;
      x.inode_table |= static_cast<uint64_t>(le32(d + 40)) << 32;
      x.free_blocks |= static_cast<uint32_t>(le16(d + 44)) << 16;
      x.free_inodes |= static_cast<uint32_t>(le16(d + 46)) << 16;
      x.used_dirs |= static_cast<uint32_t>(le16(d + 48)) << 16;
      x.itable_unused |= static_cast<uint32_t>(le16(d + 50)) << 16;
      x.bb_csum |= static_cast<uint32_t>(le16(d + 56)) << 16;
      x.ib_csum |= static_cast<uint32_t>(le16(d + 58)) << 16;
    }
    bool ok = true;
    if (csum_meta()) {
      std::vector<uint8_t> t(d, d + s_.desc_size);
      t[30] = t[31] = 0;
      uint32_t gle = static_cast<uint32_t>(g);
      uint8_t gb[4]; put32(gb, gle);
      uint32_t c = crc32c(s_.csum_seed, gb, 4);
      c = crc32c(c, t.data(), t.size());
      ok = static_cast<uint16_t>(c & 0xFFFF) == x.csum;
    } else if (csum_gdt()) {
      uint8_t gb[4]; put32(gb, g);
      uint16_t c = crc16(0xFFFF, s_.uuid, 16);
      c = crc16(c, gb, 4);
      c = crc16(c, d, 30);
      if (s_.has(INCOMPAT_64BIT, 1) && s_.desc_size > 32) c = crc16(c, d + 32, s_.desc_size - 32u);
      ok = c == x.csum;
    }
    if (!ok) { if (!bad_csum) first_bad = g; bad_csum++; }
  }
  if (bad_csum) c_.error("ext-gd-csum", num(bad_csum) + " group descriptor checksum(s) are wrong (first: group " + num(first_bad) + ")", blk_off(gdt_block) + first_bad * s_.desc_size);
  return Status::Ok();
}

Status ExtChk::mark_metadata() {
  // ブロックビットマップ (found_) にファイルシステムのメタデータを登録する
  if (c_.over_budget(Bits::bytes_for(s_.blocks_count) + Bits::bytes_for(s_.inodes_count + 1ull))) { c_.incomplete("memory-limit", "the file system is too large for the memory budget; only the superblock and group descriptors were checked"); return Status::Make(Errc::Unsupported); }
  if (!found_.init(s_.blocks_count) || !imap_used_.init(s_.inodes_count + 1ull)) { c_.incomplete("memory-limit", "not enough memory"); return Status::Make(Errc::Unsupported); }
  uint64_t gdt_blocks = (static_cast<uint64_t>(s_.groups) * s_.desc_size + s_.bs - 1) / s_.bs;
  uint64_t overlap = 0, first_overlap = 0;
  auto mark = [&](uint64_t b, const char* what, uint32_t g) {
    if (!blk_valid(b)) { c_.error("ext-metadata-range", std::string(what) + " block " + num(b) + " of group " + num(g) + " is outside the file system", UINT64_MAX, "group " + num(g)); return; }
    if (found_.get(b)) { if (!overlap) first_overlap = b; overlap++; }
    found_.set(b);
    meta_cnt_[group_of_block(b)]++;
  };
  meta_cnt_.assign(s_.groups, 0);
  for (uint32_t g = 0; g < s_.groups; g++) {
    uint64_t start = group_first_block(g);
    if (group_has_super(g)) {
      mark(start, "superblock", g);  // 1k ブロックでは first_data_block = 1 = スーパーブロック、それ以外はブロック 0 (ブートセクタと同居)
      for (uint64_t i = 0; i < gdt_blocks; i++) mark(start + 1 + i, "group descriptor", g);
      for (uint64_t i = 0; i < s_.reserved_gdt; i++) mark(start + 1 + gdt_blocks + i, "reserved GDT", g);
    }
    const Gd& x = gd_[g];
    mark(x.block_bitmap, "block bitmap", g);
    mark(x.inode_bitmap, "inode bitmap", g);
    if (!blk_valid(x.inode_table) || x.inode_table + itable_blocks() > s_.blocks_count) c_.error("ext-itable-range", "the inode table of group " + num(g) + " is outside the file system", UINT64_MAX, "group " + num(g));
    else for (uint32_t i = 0; i < itable_blocks(); i++) mark(x.inode_table + i, "inode table", g);
    if (!s_.has(INCOMPAT_FLEX_BG, 1)) {
      uint64_t end = start + group_blocks(g);
      if (x.block_bitmap < start || x.block_bitmap >= end || x.inode_bitmap < start || x.inode_bitmap >= end || x.inode_table < start || x.inode_table >= end)
        c_.error("ext-metadata-outside-group", "the bitmaps / inode table of group " + num(g) + " are outside the group (flex_bg is off)", UINT64_MAX, "group " + num(g));
    }
  }
  if (overlap) c_.error("ext-metadata-overlap", num(overlap) + " metadata block(s) overlap each other (first: block " + num(first_overlap) + ")", blk_off(first_overlap));
  for (uint32_t i = 1; i < s_.first_ino && i <= s_.inodes_count; i++) imap_used_.set(i);
  return Status::Ok();
}

uint32_t ExtChk::inode_csum(uint32_t ino, const uint8_t* raw) const {
  std::vector<uint8_t> t(raw, raw + s_.inode_size);
  uint32_t gen = le32(raw + 100);
  t[0x7C] = t[0x7D] = 0;
  bool hi = s_.inode_size > 128 && le16(raw + 128) >= 4;
  if (hi) t[0x82] = t[0x83] = 0;
  uint8_t b[4];
  put32(b, ino);
  uint32_t c = crc32c(s_.csum_seed, b, 4);
  put32(b, gen);
  c = crc32c(c, b, 4);
  c = crc32c(c, t.data(), t.size());
  return c;
}

// inode 内の拡張属性 (extra_isize の後) から system.data の値を取り出す。inline_data 用
bool ExtChk::ea_in_inode(const uint8_t* raw, std::vector<uint8_t>* out) const {
  if (s_.inode_size <= 128) return false;
  uint16_t extra = le16(raw + 128);
  size_t off = 128u + extra;
  if (off + 4 > s_.inode_size || le32(raw + off) != 0xEA020000u) return false;
  size_t first = off + 4;
  size_t p = first;
  while (p + 16 <= s_.inode_size) {
    if (le32(raw + p) == 0) break;
    uint8_t nl = raw[p], idx = raw[p + 1];
    uint16_t vo = le16(raw + p + 2);
    uint32_t vs = le32(raw + p + 8);
    if (p + 16 + nl > s_.inode_size) break;
    if (idx == 7 && nl == 4 && !std::memcmp(raw + p + 16, "data", 4)) {
      if (first + vo + vs <= s_.inode_size) { out->assign(raw + first + vo, raw + first + vo + vs); return true; }
      return false;
    }
    p += (16u + nl + 3u) & ~3u;
  }
  return false;
}

bool ExtChk::claim_block(uint64_t b, uint32_t ino, Scan& sc, const char* what) {
  if (!blk_valid(b)) {
    sc.bad++;
    if (sc.bad <= 3) c_.error("ext-block-range", std::string(what) + " block number " + num(b) + " is outside the file system", UINT64_MAX, "inode " + num(ino));
    return false;
  }
  if (found_.get(b)) {
    if (!sc.dups) sc.dup_first = b;
    sc.dups++;
    return true;
  }
  found_.set(b);
  sc.blocks++;
  return true;
}

Status ExtChk::walk_indirect(uint64_t blk, int level, uint64_t base, uint32_t ino, Scan& sc) {
  // blk は間接ブロック (level 1 = 直接ブロックの配列、2 = 二重、3 = 三重)
  if (c_.stop()) return Status::Ok();
  if (!claim_block(blk, ino, sc, "indirect")) return Status::Ok();
  std::vector<uint8_t> buf(s_.bs);
  Status st = v_.read(blk_off(blk), buf.data(), buf.size());
  if (!st.ok()) return st;
  uint64_t per = s_.bs / 4;
  uint64_t span = 1;
  for (int i = 1; i < level; i++) span *= per;
  for (uint64_t i = 0; i < per; i++) {
    uint32_t b = le32(buf.data() + 4 * i);
    if (!b) continue;
    uint64_t lb = base + i * span;
    if (level == 1) {
      if (claim_block(b, ino, sc, "data")) {
        if (sc.dir_blocks) {
          if (lb < sc.dir_cap) { if (sc.dir_blocks->size() <= lb) sc.dir_blocks->resize(static_cast<size_t>(lb + 1), 0); (*sc.dir_blocks)[static_cast<size_t>(lb)] = b; }
          else sc.dir_trunc = true;
        }
        if (lb + 1 > sc.last_lblock) sc.last_lblock = lb + 1;
      }
    } else {
      st = walk_indirect(b, level - 1, lb, ino, sc);
      if (!st.ok()) return st;
    }
  }
  return Status::Ok();
}

Status ExtChk::scan_blockmap(uint32_t ino, const uint8_t* ib, Scan& sc) {
  for (int i = 0; i < 12; i++) {
    uint32_t b = le32(ib + 4 * i);
    if (!b) continue;
    if (claim_block(b, ino, sc, "data")) {
      if (i == 0) sc.first_phys = b;
      if (sc.dir_blocks) { if (sc.dir_blocks->size() <= static_cast<size_t>(i)) sc.dir_blocks->resize(static_cast<size_t>(i) + 1, 0); (*sc.dir_blocks)[static_cast<size_t>(i)] = b; }
      if (static_cast<uint64_t>(i) + 1 > sc.last_lblock) sc.last_lblock = static_cast<uint64_t>(i) + 1;
    }
  }
  uint64_t per = s_.bs / 4;
  uint32_t b = le32(ib + 48);
  Status st;
  if (b) { st = walk_indirect(b, 1, 12, ino, sc); if (!st.ok()) return st; }
  b = le32(ib + 52);
  if (b) { st = walk_indirect(b, 2, 12 + per, ino, sc); if (!st.ok()) return st; }
  b = le32(ib + 56);
  if (b) { st = walk_indirect(b, 3, 12 + per + per * per, ino, sc); if (!st.ok()) return st; }
  return Status::Ok();
}

Status ExtChk::scan_extent_node(const uint8_t* node, size_t node_bytes, int depth_left, uint32_t ino, uint32_t gen, Scan& sc, bool root) {
  if (c_.stop()) return Status::Ok();
  std::string obj = "inode " + num(ino);
  if (le16(node) != 0xF30A) { c_.error("ext-extent-magic", "an extent tree node has a bad magic number", UINT64_MAX, obj); sc.bad++; return Status::Ok(); }
  uint16_t entries = le16(node + 2), max = le16(node + 4), depth = le16(node + 6);
  size_t cap = (node_bytes - 12) / 12;
  if (max > cap || entries > max) { c_.error("ext-extent-entries", "an extent tree node has " + num(entries) + " entries (max " + num(max) + ", room for " + num(cap) + ")", UINT64_MAX, obj); sc.bad++; return Status::Ok(); }
  if (depth != depth_left) { c_.error("ext-extent-depth", "an extent tree node has depth " + num(depth) + ", expected " + num(static_cast<uint64_t>(depth_left)), UINT64_MAX, obj); sc.bad++; return Status::Ok(); }
  if (depth > 5) { c_.error("ext-extent-depth", "the extent tree is too deep", UINT64_MAX, obj); sc.bad++; return Status::Ok(); }
  (void)root;
  uint64_t prev_lb = 0;
  bool have_prev = false;
  for (uint16_t i = 0; i < entries; i++) {
    const uint8_t* e = node + 12 + 12ull * i;
    uint32_t lb = le32(e);
    if (have_prev && lb <= prev_lb && depth > 0) { c_.error("ext-extent-order", "extent index entries are not in ascending order", UINT64_MAX, obj); sc.bad++; return Status::Ok(); }
    if (depth == 0) {
      uint16_t len = le16(e + 4);
      bool unwritten = len > 32768;
      uint32_t n = unwritten ? len - 32768u : len;
      uint64_t start = le32(e + 8) | (static_cast<uint64_t>(le16(e + 6)) << 32);
      if (n == 0) { c_.error("ext-extent-zero-len", "an extent has zero length", UINT64_MAX, obj); sc.bad++; continue; }
      if (sc.any_extent && lb < sc.prev_end) { c_.error("ext-extent-overlap", "extents overlap or are out of order (logical block " + num(lb) + ")", UINT64_MAX, obj); sc.bad++; }
      sc.any_extent = true;
      if (lb == 0 && !sc.first_phys) sc.first_phys = start;
      sc.prev_end = static_cast<uint64_t>(lb) + n;
      if (!blk_valid(start) || start + n > s_.blocks_count) { c_.error("ext-extent-range", "an extent [" + num(start) + ", +" + num(n) + ") is outside the file system", UINT64_MAX, obj); sc.bad++; continue; }
      for (uint32_t k = 0; k < n; k++) {
        if (found_.get(start + k)) { if (!sc.dups) sc.dup_first = start + k; sc.dups++; }
        else { found_.set(start + k); sc.blocks++; }
        if (sc.dir_blocks) {
          uint64_t l = static_cast<uint64_t>(lb) + k;
          if (l < sc.dir_cap) { if (sc.dir_blocks->size() <= l) sc.dir_blocks->resize(static_cast<size_t>(l + 1), 0); (*sc.dir_blocks)[static_cast<size_t>(l)] = start + k; }
          else sc.dir_trunc = true;
        }
        if ((k & 0xFFFF) == 0xFFFF && c_.stop()) return Status::Ok();
      }
      if (!unwritten && static_cast<uint64_t>(lb) + n > sc.last_lblock) sc.last_lblock = static_cast<uint64_t>(lb) + n;
    } else {
      uint64_t leaf = le32(e + 4) | (static_cast<uint64_t>(le16(e + 8)) << 32);
      if (!blk_valid(leaf)) { c_.error("ext-extent-range", "an extent index points outside the file system (block " + num(leaf) + ")", UINT64_MAX, obj); sc.bad++; continue; }
      if (found_.get(leaf)) { if (!sc.dups) sc.dup_first = leaf; sc.dups++; continue; }
      found_.set(leaf);
      sc.blocks++;
      std::vector<uint8_t> buf(s_.bs);
      Status st = v_.read(blk_off(leaf), buf.data(), buf.size());
      if (!st.ok()) return st;
      if (csum_meta()) {
        size_t tail = 12 + 12ull * le16(buf.data() + 4);
        if (tail + 4 <= buf.size()) {
          uint8_t b4[4];
          put32(b4, ino);
          uint32_t c = crc32c(s_.csum_seed, b4, 4);
          put32(b4, gen);
          c = crc32c(c, b4, 4);
          c = crc32c(c, buf.data(), tail);
          if (c != le32(buf.data() + tail)) c_.error("ext-extent-csum", "an extent tree block checksum is wrong (block " + num(leaf) + ")", blk_off(leaf), obj);
        }
      }
      uint64_t save_end = sc.prev_end;
      bool save_any = sc.any_extent;
      st = scan_extent_node(buf.data(), buf.size(), depth_left - 1, ino, gen, sc, false);
      sc.prev_end = std::max(save_end, sc.prev_end);
      sc.any_extent = save_any || sc.any_extent;
      if (!st.ok()) return st;
    }
    prev_lb = lb;
    have_prev = true;
  }
  return Status::Ok();
}

Status ExtChk::scan_inode(uint32_t ino, const uint8_t* raw, bool* in_use) {
  *in_use = false;
  uint16_t mode = le16(raw);
  uint16_t links = le16(raw + 26);
  uint32_t dtime = le32(raw + 20);
  uint32_t flags = le32(raw + 32);
  std::string obj = "inode " + num(ino);
  bool reserved = ino < s_.first_ino;
  bool special_noscan = ino == 7;  // resize inode: ブロックは予約 GDT で登録済み
  if (links == 0 && !(ino == s_.journal_inum && s_.has(COMPAT_HAS_JOURNAL, 0) && mode != 0)) {
    if (reserved) *in_use = true;   // 予約 inode は常に使用中扱い (ビットマップとの比較用)
    else if (mode != 0 && dtime == 0 && !all_zero(raw, s_.inode_size))
      c_.warn("ext-deleted-no-dtime", "the inode has link count 0 but no deletion time", UINT64_MAX, obj);
    return Status::Ok();
  }
  *in_use = true;
  if (ino == s_.journal_inum && s_.has(COMPAT_HAS_JOURNAL, 0) && links == 0) c_.error("ext-journal-invalid", "the journal inode has link count 0 (e2fsck would treat the journal as invalid)", UINT64_MAX, obj);
  if (dtime && !reserved) c_.warn("ext-dtime-set", "an in-use inode has a deletion time", UINT64_MAX, obj);
  if (csum_meta()) {
    uint32_t c = inode_csum(ino, raw);
    uint32_t stored = le16(raw + 0x7C);
    bool hi = s_.inode_size > 128 && le16(raw + 128) >= 4;
    if (hi) stored |= static_cast<uint32_t>(le16(raw + 0x82)) << 16; else c &= 0xFFFF;
    if (c != stored && !all_zero(raw, s_.inode_size)) c_.error("ext-inode-csum", "the inode checksum is wrong", UINT64_MAX, obj);
  }
  uint16_t fmt = mode & 0xF000;
  bool is_dir = fmt == 0x4000, is_reg = fmt == 0x8000, is_lnk = fmt == 0xA000;
  bool is_dev = fmt == 0x2000 || fmt == 0x6000 || fmt == 0x1000 || fmt == 0xC000;
  if (!(is_dir || is_reg || is_lnk || is_dev)) {
    if (!(reserved && mode == 0)) c_.error("ext-bad-mode", "the inode has an invalid file type in its mode (" + hex(mode) + ")", UINT64_MAX, obj);
    return Status::Ok();
  }
  uint64_t size = le32(raw + 4);
  if (is_dir || (s_.ro & RO_LARGE_FILE)) size |= static_cast<uint64_t>(le32(raw + 108)) << 32;
  uint32_t gen = le32(raw + 100);
  uint64_t file_acl = le32(raw + 104) | (static_cast<uint64_t>(le16(raw + 118)) << 32);
  uint64_t i_blocks = le32(raw + 28);
  if (s_.ro & RO_HUGE_FILE) i_blocks |= static_cast<uint64_t>(le16(raw + 116)) << 32;
  bool huge_units = (s_.ro & RO_HUGE_FILE) && (flags & INO_HUGE_FILE_FL);
  uint64_t per_blk = huge_units ? 1 : s_.bs / 512;
  uint64_t ea_blocks = 0;
  const uint8_t* ib = raw + 40;

  Scan sc;
  DirRec dr;
  if (is_dir) { sc.is_dir = true; sc.dir_blocks = &dr.blocks; }
  bool inline_data = (flags & INO_INLINE_DATA_FL) && s_.has(INCOMPAT_INLINE_DATA, 1);
  bool scanned = false;
  bool fast_symlink = is_lnk && !(flags & INO_EXTENTS_FL) && !inline_data && size < 60 && (i_blocks - (file_acl ? per_blk : 0)) == 0;
  if (special_noscan) {
    // リサイズ inode: 予約 GDT ブロックはグループのメタデータとして登録済み。自身の二重間接ブロックだけを数える
    uint32_t dind = le32(ib + 52);
    if (dind && s_.has(COMPAT_RESIZE_INODE, 0)) claim_block(dind, ino, sc, "resize");
  } else if (is_dev || fast_symlink) {
    // ブロックを持たない
  } else if (inline_data) {
    if (is_dir) {
      dr.inl = true;
      dr.inl_data.assign(ib, ib + 60);
      std::vector<uint8_t> ea;
      if (ea_in_inode(raw, &ea)) dr.inl_data.insert(dr.inl_data.end(), ea.begin(), ea.end());
    }
  } else if (flags & INO_EXTENTS_FL) {
    if (!s_.has(INCOMPAT_EXTENTS, 1)) c_.error("ext-extents-flag", "the inode uses extents but the file system does not have the extents feature", UINT64_MAX, obj);
    else {
      Status st = scan_extent_node(ib, 60, le16(ib + 6), ino, gen, sc, true);
      if (!st.ok()) return st;
      scanned = true;
    }
  } else {
    Status st = scan_blockmap(ino, ib, sc);
    if (!st.ok()) return st;
    scanned = true;
  }
  (void)scanned;
  // 拡張属性ブロック
  if (file_acl) {
    if (!blk_valid(file_acl)) c_.error("ext-ea-range", "the extended attribute block " + num(file_acl) + " is outside the file system", UINT64_MAX, obj);
    else {
      ea_blocks = 1;
      if (!ea_blocks_.count(file_acl)) {
        if (found_.get(file_acl)) { sc.dups++; if (!sc.dup_first) sc.dup_first = file_acl; }
        else { found_.set(file_acl); ea_blocks_.insert(file_acl); }
        std::vector<uint8_t> eb(s_.bs);
        Status st = v_.read(blk_off(file_acl), eb.data(), eb.size());
        if (!st.ok()) return st;
        if (le32(eb.data()) != 0xEA020000u) c_.error("ext-ea-magic", "the extended attribute block has a bad magic number", blk_off(file_acl), obj);
        else ea_refcount_[file_acl] = le32(eb.data() + 4);
      }
      ea_refs_[file_acl]++;
    }
  }
  if (sc.dups) {
    c_.error("ext-dup-block", num(sc.dups) + " block(s) are claimed by more than one file or by metadata (first: block " + num(sc.dup_first) + ")", blk_off(sc.dup_first), obj);
  }
  if (sc.bad) c_.error("ext-bad-blocks", num(sc.bad) + " invalid block reference(s)", UINT64_MAX, obj);
  // i_blocks
  uint64_t expect = (sc.blocks + ea_blocks) * per_blk;
  if (!sc.bad && !sc.dups && !special_noscan && i_blocks != expect) c_.error("ext-bad-i_blocks", "i_blocks is " + num(i_blocks) + " but the allocated blocks add up to " + num(expect), UINT64_MAX, obj);
  // i_size
  if (!sc.bad && !sc.dups) {
    if (is_dir) {
      if (!inline_data) {
        uint64_t nb = sc.last_lblock;
        if (size != nb * s_.bs) c_.error("ext-dir-size", "the directory size " + num(size) + " does not match its " + num(nb) + " block(s)", UINT64_MAX, obj);
      }
    } else if (is_reg && !inline_data) {
      if (sc.last_lblock && size < (sc.last_lblock - 1) * s_.bs) c_.error("ext-file-size", "the file size " + num(size) + " is smaller than its last data block (" + num(sc.last_lblock) + ")", UINT64_MAX, obj);
    }
  }
  if (ino == s_.journal_inum && s_.has(COMPAT_HAS_JOURNAL, 0)) { journal_seen_ = true; journal_first_ = sc.first_phys; if (!is_reg) c_.error("ext-journal-inode", "the journal inode is not a regular file", UINT64_MAX, obj); }
  if (is_dir) {
    dr.size = size;
    dr.gen = gen;
    if (ino == 2) root_ok_ = true;
    dirs_[ino] = std::move(dr);
    dir_count_++;
    c_.rep.stats.directories++;
  } else if (is_reg) c_.rep.stats.files++;
  else if (is_lnk) c_.rep.stats.symlinks++;
  else c_.rep.stats.other++;
  if (ino == 2 && !is_dir) c_.error("ext-root-not-dir", "the root inode is not a directory", UINT64_MAX, obj);
  return Status::Ok();
}

Status ExtChk::pass1() {
  c_.progress("inodes", 0, s_.inodes_count);
  std::vector<uint8_t> buf;
  uint32_t ino = 0;
  for (uint32_t g = 0; g < s_.groups; g++) {
    const Gd& x = gd_[g];
    uint32_t in_group = s_.ipg;
    bool uninit_ok = csum_meta() || csum_gdt();
    if (uninit_ok && (x.flags & GD_INODE_UNINIT)) { ino += s_.ipg; continue; }
    if (uninit_ok && x.itable_unused <= s_.ipg) in_group = s_.ipg - x.itable_unused;
    if (!blk_valid(x.inode_table) || x.inode_table + itable_blocks() > s_.blocks_count) { ino += s_.ipg; continue; }
    // inode テーブルを 1 MiB ずつ読む
    uint32_t per_chunk = std::max<uint32_t>(1, (1u << 20) / s_.inode_size);
    for (uint32_t i = 0; i < in_group;) {
      uint32_t n = std::min(per_chunk, in_group - i);
      buf.resize(static_cast<size_t>(n) * s_.inode_size);
      Status st = v_.read(blk_off(x.inode_table) + static_cast<uint64_t>(i) * s_.inode_size, buf.data(), buf.size());
      if (!st.ok()) return st;
      for (uint32_t k = 0; k < n; k++) {
        uint32_t cur = ino + i + k + 1;
        if (!c_.count_object()) return Status::Ok();
        bool used = false;
        st = scan_inode(cur, buf.data() + static_cast<size_t>(k) * s_.inode_size, &used);
        if (!st.ok()) return st;
        if (used) { imap_used_.set(cur); used_inodes_++; }
      }
      i += n;
    }
    ino += s_.ipg;
    c_.progress("inodes", ino, s_.inodes_count);
  }
  // 拡張属性ブロックの参照数
  for (auto& kv : ea_refs_) {
    auto it = ea_refcount_.find(kv.first);
    if (it != ea_refcount_.end() && it->second != kv.second)
      c_.error("ext-ea-refcount", "extended attribute block " + num(kv.first) + " has reference count " + num(it->second) + " but is used by " + num(kv.second) + " inode(s)", blk_off(kv.first));
  }
  if (!root_ok_) c_.error("ext-no-root", "the root inode (2) is missing or not a directory", UINT64_MAX, "inode 2");
  return Status::Ok();
}


Status ExtChk::read_inode_raw(uint32_t ino, std::vector<uint8_t>* out) {
  if (ino == 0 || ino > s_.inodes_count) return Status::Make(Errc::OutOfRange);
  uint32_t g = (ino - 1) / s_.ipg, idx = (ino - 1) % s_.ipg;
  const Gd& x = gd_[g];
  if (!blk_valid(x.inode_table)) return Status::Make(Errc::OutOfRange);
  out->resize(s_.inode_size);
  return v_.read(blk_off(x.inode_table) + static_cast<uint64_t>(idx) * s_.inode_size, out->data(), out->size());
}

// ---------------------------------------------------------------- パス 2: ディレクトリ

Status ExtChk::check_dir_block(uint32_t ino, const uint8_t* buf, size_t len, uint64_t file_off, bool is_first, bool leaf_tail, uint32_t gen,
                               DirRec& dr, std::unordered_set<std::string>& names, bool* saw_dot, bool* saw_dotdot, bool inline_part) {
  (void)gen; (void)dr;
  std::string obj = "inode " + num(ino);
  size_t limit = leaf_tail ? len - 12 : len;
  size_t off = 0;
  int index = 0;
  bool casefold = s_.has(INCOMPAT_CASEFOLD, 1);
  bool ft_feature = s_.has(INCOMPAT_FILETYPE, 1);
  while (off < limit) {
    if (off + 8 > limit) { c_.error("ext-dir-truncated", "a directory entry is cut off at the end of the block", file_off + off, obj); return Status::Ok(); }
    uint32_t t = le32(buf + off);
    uint32_t rec = le16(buf + off + 4);
    if (s_.bs >= 65536 && (rec == 0 || rec == 65536)) rec = static_cast<uint32_t>(len);
    uint8_t nl = buf[off + 6], ft = buf[off + 7];
    if (rec < 12 || (rec & 3) || off + rec > limit || static_cast<uint32_t>(nl) + 8 > rec) {
      c_.error("ext-dir-entry", "a directory entry has an invalid record length " + num(rec) + " (name length " + num(nl) + ")", file_off + off, obj);
      return Status::Ok();
    }
    bool in_first = is_first && !inline_part;
    if (in_first && index == 0 && !(t != 0 && nl == 1 && buf[off + 8] == '.')) { c_.error("ext-dir-no-dot", "the first entry of the directory is not '.'", file_off + off, obj); }
    if (in_first && index == 1 && !(t != 0 && nl == 2 && buf[off + 8] == '.' && buf[off + 9] == '.')) { c_.error("ext-dir-no-dotdot", "the second entry of the directory is not '..'", file_off + off, obj); }
    if (t != 0) {
      std::string name(reinterpret_cast<const char*>(buf + off + 8), nl);
      std::string eo = obj + " entry \"" + name + "\"";
      bool is_dot = nl == 1 && name[0] == '.', is_dd = nl == 2 && name == "..";
      if (t > s_.inodes_count) c_.error("ext-dir-bad-inode", "the entry refers to inode " + num(t) + " which does not exist", file_off + off, eo);
      else {
        if (t < s_.first_ino && t != 2) c_.error("ext-dir-reserved-inode", "the entry refers to reserved inode " + num(t), file_off + off, eo);
        else if (!imap_used_.get(t)) c_.error("ext-dir-unused-inode", "the entry refers to inode " + num(t) + " which is not in use", file_off + off, eo);
        else {
          count_ref(t);
          bool target_is_dir = dirs_.count(t) != 0;
          if (ft_feature) {
            uint16_t mode = 0;
            if (target_is_dir) mode = 0x4000;
            else { std::vector<uint8_t> raw; Status st = read_inode_raw(t, &raw); if (!st.ok()) return st; mode = le16(raw.data()); }
            uint8_t want = 0;
            switch (mode & 0xF000) { case 0x8000: want = 1; break; case 0x4000: want = 2; break; case 0x2000: want = 3; break; case 0x6000: want = 4; break; case 0x1000: want = 5; break; case 0xC000: want = 6; break; case 0xA000: want = 7; break; default: break; }
            if (ft == 0) c_.warn("ext-dir-filetype-unset", "the entry has no file type set", file_off + off, eo);
            else if (want && ft != want) c_.error("ext-dir-filetype", "the entry's file type " + num(ft) + " does not match the inode (" + num(want) + ")", file_off + off, eo);
          }
          if (!is_dot && !is_dd && target_is_dir) {
            auto it = parent_by_entry_.find(t);
            if (it != parent_by_entry_.end()) c_.error("ext-dir-multiple-links", "directory inode " + num(t) + " is linked more than once", file_off + off, eo);
            else parent_by_entry_[t] = ino;
          }
        }
      }
      if (is_dot) { if (in_first && index == 0) { *saw_dot = true; if (t != ino) c_.error("ext-dir-dot-target", "'.' does not point to the directory itself", file_off + off, obj); } else if (!inline_part) c_.error("ext-dir-dot-misplaced", "'.' appears in the wrong place", file_off + off, obj); }
      else if (is_dd) { if (in_first && index == 1) { *saw_dotdot = true; dotdot_[ino] = t; } else if (!inline_part) c_.error("ext-dir-dotdot-misplaced", "'..' appears in the wrong place", file_off + off, obj); }
      else {
        if (nl == 0) c_.error("ext-dir-empty-name", "an entry has an empty name", file_off + off, obj);
        for (char ch : name) if (ch == '/' || ch == 0) { c_.error("ext-dir-bad-name", "an entry name contains '/' or NUL", file_off + off, eo); break; }
        if (!casefold && !s_.has(INCOMPAT_ENCRYPT, 1) && !names.insert(name).second) c_.error("ext-dir-duplicate", "duplicate directory entry", file_off + off, eo);
        if (ino == 2 && t <= s_.inodes_count) root_names_[name] = t;
      }
    }
    off += rec;
    index++;
  }
  if (off != limit) c_.error("ext-dir-chain", "the directory entries do not end at the end of the block", file_off + off, obj);
  return Status::Ok();
}

Status ExtChk::pass2() {
  std::vector<uint32_t> keys;
  keys.reserve(dirs_.size());
  for (auto& kv : dirs_) keys.push_back(kv.first);
  std::sort(keys.begin(), keys.end());
  uint64_t done = 0;
  std::vector<uint8_t> buf(s_.bs);
  for (uint32_t ino : keys) {
    if (c_.stop()) return Status::Ok();
    DirRec& dr = dirs_[ino];
    std::string obj = "inode " + num(ino);
    std::unordered_set<std::string> names;
    bool saw_dot = false, saw_dotdot = false;
    if (dr.inl) {
      uint32_t parent = le32(dr.inl_data.data());
      saw_dot = saw_dotdot = true;
      dotdot_[ino] = parent;
      count_ref(ino);
      if (parent == 0 || parent > s_.inodes_count || !imap_used_.get(parent)) c_.error("ext-dir-bad-parent", "the inline directory's parent inode " + num(parent) + " is invalid", UINT64_MAX, obj);
      else count_ref(parent);
      Status st = check_dir_block(ino, dr.inl_data.data() + 4, 56, 0, false, false, dr.gen, dr, names, &saw_dot, &saw_dotdot, true);
      if (!st.ok()) return st;
      if (dr.inl_data.size() > 60) {
        st = check_dir_block(ino, dr.inl_data.data() + 60, dr.inl_data.size() - 60, 60, false, false, dr.gen, dr, names, &saw_dot, &saw_dotdot, true);
        if (!st.ok()) return st;
      }
    } else {
      if (dr.blocks.empty()) c_.error("ext-dir-no-blocks", "the directory has no blocks", UINT64_MAX, obj);
      for (size_t i = 0; i < dr.blocks.size(); i++) {
        uint64_t b = dr.blocks[i];
        if (b == 0) { c_.error("ext-dir-hole", "the directory has a hole at block " + num(i), UINT64_MAX, obj); continue; }
        Status st = v_.read(blk_off(b), buf.data(), buf.size());
        if (!st.ok()) return st;
        bool leaf_tail = false;
        if (csum_meta()) {
          // dx の内部ノードは先頭が「inode=0, rec_len=ブロック全体」の偽エントリ。リーフには末尾 12 バイトにチェックサムエントリ
          bool dx_interior = le32(buf.data()) == 0 && (le16(buf.data() + 4) == s_.bs || (s_.bs >= 65536 && le16(buf.data() + 4) == 0));
          const uint8_t* tl = buf.data() + s_.bs - 12;
          bool has_tail = le32(tl) == 0 && le16(tl + 4) == 12 && tl[6] == 0 && tl[7] == 0xDE;
          if (!dx_interior) {
            if (!has_tail) {
              // dx ルートブロック ('.' と '..' が先頭で、'..' の rec_len が残り全部) は dx_tail を内部に持つ
              bool dx_root = i == 0 && s_.has(COMPAT_DIR_INDEX, 0) && le16(buf.data() + 16) == s_.bs - 12;
              if (!dx_root) c_.error("ext-dir-no-csum-tail", "a directory block has no checksum tail", blk_off(b), obj);
            } else {
              uint8_t b4[4];
              put32(b4, ino);
              uint32_t c = crc32c(s_.csum_seed, b4, 4);
              put32(b4, dr.gen);
              c = crc32c(c, b4, 4);
              c = crc32c(c, buf.data(), s_.bs - 12);
              if (c != le32(tl + 8)) c_.error("ext-dir-csum", "a directory block checksum is wrong", blk_off(b), obj);
              leaf_tail = true;
            }
          }
        }
        st = check_dir_block(ino, buf.data(), s_.bs, blk_off(b), i == 0, leaf_tail, dr.gen, dr, names, &saw_dot, &saw_dotdot, false);
        if (!st.ok()) return st;
      }
      if (!dr.blocks.empty() && dr.blocks[0] != 0) {
        if (!saw_dot) c_.error("ext-dir-missing-dot", "the directory has no '.' entry", UINT64_MAX, obj);
        if (!saw_dotdot) c_.error("ext-dir-missing-dotdot", "the directory has no '..' entry", UINT64_MAX, obj);
      }
    }
    if ((++done & 255) == 0) c_.progress("directories", done, dir_count_);
  }
  return Status::Ok();
}

// ---------------------------------------------------------------- パス 3: 接続性

Status ExtChk::pass3() {
  if (!root_ok_) return Status::Ok();
  auto d2 = dotdot_.find(2);
  if (d2 != dotdot_.end() && d2->second != 2) c_.error("ext-root-dotdot", "the root directory's '..' does not point to itself", UINT64_MAX, "inode 2");
  std::unordered_map<uint32_t, uint8_t> state;  // 1 = 調査中, 2 = 到達可能, 3 = 不可
  std::vector<uint32_t> keys;
  for (auto& kv : dirs_) keys.push_back(kv.first);
  std::sort(keys.begin(), keys.end());
  state[2] = 2;
  for (uint32_t d : keys) {
    if (d == 2) continue;
    std::vector<uint32_t> path;
    uint32_t cur = d;
    uint8_t res = 0;
    while (true) {
      auto it = state.find(cur);
      if (it != state.end()) { res = it->second == 1 ? 3 : it->second; break; }
      state[cur] = 1;
      path.push_back(cur);
      auto p = parent_by_entry_.find(cur);
      if (p == parent_by_entry_.end()) { res = 3; break; }
      cur = p->second;
      if (!dirs_.count(cur)) { res = 3; break; }
    }
    for (uint32_t x : path) state[x] = res;
    if (state[d] == 3) {
      if (!parent_by_entry_.count(d)) c_.error("ext-unconnected-dir", "directory inode " + num(d) + " is not reachable from the root (no entry refers to it)", UINT64_MAX, "inode " + num(d));
      else c_.error("ext-dir-loop", "directory inode " + num(d) + " is part of a loop or hangs off an unreachable directory", UINT64_MAX, "inode " + num(d));
    }
    auto dd = dotdot_.find(d);
    auto pe = parent_by_entry_.find(d);
    if (dd != dotdot_.end() && pe != parent_by_entry_.end() && dd->second != pe->second)
      c_.error("ext-dotdot-wrong", "'..' of directory inode " + num(d) + " points to inode " + num(dd->second) + " but its parent is inode " + num(pe->second), UINT64_MAX, "inode " + num(d));
  }
  auto lf = root_names_.find("lost+found");
  if (lf == root_names_.end()) c_.warn("ext-no-lost-found", "lost+found does not exist in the root directory", UINT64_MAX, "/");
  else if (!dirs_.count(lf->second)) c_.error("ext-lost-found-not-dir", "lost+found is not a directory", UINT64_MAX, "/lost+found");
  return Status::Ok();
}

// ---------------------------------------------------------------- パス 4: リンクカウント

Status ExtChk::pass4() {
  if (!have_counted_) { c_.info("ext-links-skipped", "the link counts were not checked (memory budget)"); return Status::Ok(); }
  uint64_t unattached = 0;
  uint32_t first_un = 0;
  std::vector<uint8_t> raw;
  for (uint32_t ino = 1; ino <= s_.inodes_count; ino++) {
    if (!imap_used_.get(ino)) continue;
    if (ino < s_.first_ino && ino != 2) continue;
    if (ino == s_.journal_inum || ino == s_.internal_inos[0] || ino == s_.internal_inos[1] || ino == s_.internal_inos[2] || ino == s_.internal_inos[3]) continue;
    Status st = read_inode_raw(ino, &raw);
    if (!st.ok()) return st;
    uint16_t links = le16(raw.data() + 26);
    if (links == 0) continue;
    uint16_t cnt = counted_[ino];
    bool is_dir = (le16(raw.data()) & 0xF000) == 0x4000;
    if (cnt == 0) { if (!unattached) first_un = ino; unattached++; continue; }
    if (cnt == links) continue;
    if (is_dir && links == 1 && (s_.ro & RO_DIR_NLINK) && cnt > 1) continue;
    if (cnt == 0xFFFF) continue;
    c_.error("ext-link-count", "the link count is " + num(links) + " but " + num(cnt) + " directory entries refer to it", UINT64_MAX, "inode " + num(ino));
    if ((ino & 0xFFFF) == 0 && c_.stop()) return Status::Ok();
  }
  if (unattached) c_.error("ext-unattached-inode", num(unattached) + " inode(s) are in use but no directory entry refers to them (first: inode " + num(first_un) + ")", UINT64_MAX, "inode " + num(first_un));
  return Status::Ok();
}

// ---------------------------------------------------------------- パス 5: ビットマップ

Status ExtChk::pass5() {
  std::vector<uint8_t> bm(s_.bs);
  std::vector<uint32_t> dirs_per_group(s_.groups, 0);
  for (auto& kv : dirs_) { uint32_t g = (kv.first - 1) / s_.ipg; if (g < s_.groups) dirs_per_group[g]++; }
  uint64_t total_free_blocks = 0, total_free_inodes = 0;
  uint64_t free_wrong = 0, used_wrong = 0, ino_free_wrong = 0, ino_used_wrong = 0;
  uint64_t first_fw = 0, first_uw = 0;
  uint32_t first_ifw = 0;
  uint32_t csum_bad = 0;
  uint32_t group_free_bad = 0;
  uint32_t group_free_blk_first = 0;
  for (uint32_t g = 0; g < s_.groups; g++) {
    if (c_.stop()) return Status::Ok();
    const Gd& x = gd_[g];
    uint64_t start = group_first_block(g), nblk = group_blocks(g);
    uint64_t found_in_group = 0;
    for (uint64_t i = 0; i < nblk; i++) if (found_.get(start + i)) found_in_group++;
    bool uninit_ok = csum_meta() || csum_gdt();
    if (uninit_ok && (x.flags & GD_BLOCK_UNINIT)) {
      if (found_in_group > meta_cnt_[g]) c_.error("ext-uninit-block-group", "group " + num(g) + " is marked BLOCK_UNINIT but has " + num(found_in_group - meta_cnt_[g]) + " block(s) in use", UINT64_MAX, "group " + num(g));
    } else if (blk_valid(x.block_bitmap)) {
      Status st = v_.read(blk_off(x.block_bitmap), bm.data(), bm.size());
      if (!st.ok()) return st;
      for (uint64_t i = 0; i < nblk; i++) {
        bool marked = (bm[static_cast<size_t>(i >> 3)] >> (i & 7)) & 1, used = found_.get(start + i);
        if (used && !marked) { if (!free_wrong) first_fw = start + i; free_wrong++; }
        else if (marked && !used) { if (!used_wrong) first_uw = start + i; used_wrong++; }
      }
      if (csum_meta()) {
        uint32_t c = crc32c(s_.csum_seed, bm.data(), s_.bpg / 8);
        uint32_t stored = x.bb_csum;
        if (s_.desc_size < 64) c &= 0xFFFF;
        if (c != stored) csum_bad++;
      }
    }
    uint64_t exp_free = nblk - found_in_group;
    total_free_blocks += exp_free;
    if (x.free_blocks != exp_free) { if (!group_free_bad) group_free_blk_first = g; group_free_bad++; }
    uint64_t used_in_group = 0;
    uint64_t base = static_cast<uint64_t>(g) * s_.ipg;
    for (uint32_t i = 0; i < s_.ipg; i++) if (imap_used_.get(base + i + 1)) used_in_group++;
    if (uninit_ok && (x.flags & GD_INODE_UNINIT)) {
      if (used_in_group > (g == 0 ? s_.first_ino - 1 : 0)) c_.error("ext-uninit-inode-group", "group " + num(g) + " is marked INODE_UNINIT but has inodes in use", UINT64_MAX, "group " + num(g));
    } else if (blk_valid(x.inode_bitmap)) {
      Status st = v_.read(blk_off(x.inode_bitmap), bm.data(), bm.size());
      if (!st.ok()) return st;
      for (uint32_t i = 0; i < s_.ipg; i++) {
        bool marked = (bm[i >> 3] >> (i & 7)) & 1, used = imap_used_.get(base + i + 1);
        if (used && !marked) { if (!ino_free_wrong) first_ifw = static_cast<uint32_t>(base + i + 1); ino_free_wrong++; }
        else if (marked && !used) ino_used_wrong++;
      }
      if (csum_meta()) {
        uint32_t c = crc32c(s_.csum_seed, bm.data(), s_.ipg / 8);
        uint32_t stored = x.ib_csum;
        if (s_.desc_size < 64) c &= 0xFFFF;
        if (c != stored) csum_bad++;
      }
    }
    uint64_t exp_free_i = s_.ipg - used_in_group;
    total_free_inodes += exp_free_i;
    if (x.free_inodes != exp_free_i || x.used_dirs != dirs_per_group[g]) group_free_bad++;
  }
  if (free_wrong) c_.error("ext-bitmap-free-but-used", num(free_wrong) + " block(s) are in use but marked free in the block bitmap (first: block " + num(first_fw) + ")", blk_off(first_fw));
  if (used_wrong) c_.warn("ext-bitmap-leak", num(used_wrong) + " block(s) are marked used in the block bitmap but belong to nothing (first: block " + num(first_uw) + ")", blk_off(first_uw));
  if (ino_free_wrong) c_.error("ext-inode-bitmap-free-but-used", num(ino_free_wrong) + " inode(s) are in use but marked free in the inode bitmap (first: inode " + num(first_ifw) + ")", UINT64_MAX, "inode " + num(first_ifw));
  if (ino_used_wrong) c_.warn("ext-inode-bitmap-leak", num(ino_used_wrong) + " inode(s) are marked used in the inode bitmap but are not in use", UINT64_MAX);
  if (csum_bad) c_.error("ext-bitmap-csum", num(csum_bad) + " bitmap checksum(s) are wrong", UINT64_MAX);
  if (group_free_bad) c_.warn("ext-gd-free-count", num(group_free_bad) + " group descriptor free block / inode / directory count(s) differ from the counted values (first: group " + num(group_free_blk_first) + ")");
  if (s_.free_blocks != total_free_blocks || s_.free_inodes != total_free_inodes)
    c_.warn("ext-sb-free-count", "the superblock free block / inode counts (" + num(s_.free_blocks) + " / " + num(s_.free_inodes) + ") differ from the counted values (" + num(total_free_blocks) + " / " + num(total_free_inodes) + ")");
  c_.rep.stats.units_used = s_.blocks_count - total_free_blocks;
  return Status::Ok();
}

// ---------------------------------------------------------------- ジャーナル

Status ExtChk::journal() {
  if (!s_.has(COMPAT_HAS_JOURNAL, 0)) return Status::Ok();
  if (s_.journal_inum == 0) { c_.error("ext-no-journal-inum", "the journal feature is set but the journal inode number is 0", UINT64_MAX); return Status::Ok(); }
  if (!journal_seen_) { c_.error("ext-no-journal", "the journal feature is set but the journal inode " + num(s_.journal_inum) + " is not in use", UINT64_MAX); return Status::Ok(); }
  if (!blk_valid(journal_first_)) { c_.error("ext-journal-blocks", "the journal inode has no valid first block", UINT64_MAX); return Status::Ok(); }
  uint8_t j[1024];
  Status st = v_.read(blk_off(journal_first_), j, 1024);
  if (!st.ok()) return st;
  if (be32(j) != 0xC03B3998u) { c_.error("ext-journal-magic", "the journal superblock has a bad magic number", blk_off(journal_first_)); return Status::Ok(); }
  uint32_t type = be32(j + 4);
  if (type != 3 && type != 4) { c_.error("ext-journal-type", "the journal superblock has an unexpected block type " + num(type), blk_off(journal_first_)); return Status::Ok(); }
  if (be32(j + 12) != s_.bs) c_.error("ext-journal-blocksize", "the journal block size differs from the file system block size", blk_off(journal_first_));
  if (be32(j + 28) != 0) {
    c_.warn("ext-journal-dirty", "the journal contains transactions that have not been replayed", blk_off(journal_first_));
    c_.rep.unclean = true;
    c_.rep.needs_journal_recovery = true;
  }
  return Status::Ok();
}

Status ExtChk::run() {
  Status st = superblock();
  if (!st.ok()) return (st.code == Errc::Corrupt || st.code == Errc::Unsupported) ? Status::Ok() : st;
  st = group_descriptors();
  if (!st.ok()) return (st.code == Errc::Corrupt || st.code == Errc::Unsupported) ? Status::Ok() : st;
  st = mark_metadata();
  if (!st.ok()) return (st.code == Errc::Corrupt || st.code == Errc::Unsupported) ? Status::Ok() : st;
  have_counted_ = !c_.over_budget((static_cast<uint64_t>(s_.inodes_count) + 1) * 2 + Bits::bytes_for(s_.blocks_count));
  if (have_counted_) counted_.assign(static_cast<size_t>(s_.inodes_count) + 1, 0);
  st = pass1();
  if (!st.ok() || c_.stop()) return st;
  c_.progress("directories", 0, dir_count_);
  st = pass2();
  if (!st.ok() || c_.stop()) return st;
  st = pass3();
  if (!st.ok() || c_.stop()) return st;
  c_.progress("links", 0, 0);
  st = pass4();
  if (!st.ok() || c_.stop()) return st;
  c_.progress("bitmaps", 0, 0);
  st = pass5();
  if (!st.ok() || c_.stop()) return st;
  return journal();
}

}  // namespace

Status check_ext(Ctx& c, FsType fs) {
  ExtChk k(c, fs);
  return k.run();
}

}  // namespace fsck
}  // namespace usbmem
