// SPDX-License-Identifier: Apache-2.0
// F2FS の読み取り専用リーダー。オンディスク形式 (スーパーブロック / チェックポイント / NAT / ノード木 / ディレクトリエントリ / 圧縮クラスタ) から書いた独自実装で、
// Linux の fs/f2fs のコードは含まない (docs/f2fs-port.md)。ブロックサイズ 4 KiB、1 セグメント 512 ブロックの標準形式のみ。
// 書き込み (USBMEM_NTFS_WRITE のビルドで mount_rw したときだけ。docs/f2fs-write.md): ログ構造 + チェックポイントの規則に従い、すべての更新を使用中でないブロックへ書き、
// 毎回 2 世代のチェックポイント パックの旧い側へ完全なチェックポイントを書く (roll-forward 用の fsync ログ・orphan inode は書かない)。それ以外は常に読み取り専用。
#include "usbmem/f2fs_volume.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>

extern "C" int usbmem_f2fs_decompress(int alg, const void* src, size_t clen, void* dst, size_t dstlen);

namespace usbmem {
namespace {

constexpr uint32_t kMagic = 0xF2F52010u;
constexpr uint32_t kBlk = 4096;
constexpr uint32_t kSegBlks = 512;               // 1 << log_blocks_per_seg (標準は 9)
constexpr uint32_t kReservedNodes = 3;
constexpr uint32_t kDefAddrsPerInode = (kBlk - 360 - 20 - 24) / 4;   // 923
constexpr uint32_t kAddrsPerBlock = (kBlk - 24) / 4;                 // 1018
constexpr uint32_t kNidsPerBlock = kAddrsPerBlock;
constexpr uint32_t kNatPerBlock = kBlk / 9;                          // 455
constexpr uint32_t kNullAddr = 0, kNewAddr = 0xFFFFFFFFu, kCompressAddr = 0xFFFFFFFEu;
constexpr size_t kCompHdr = 24;   // 圧縮クラスタの先頭: clen(4) + chksum(4) + reserved(16)。chksum は検証しない
constexpr int kEUCLEAN = 117, kEOPNOTSUPP = 95;

// フィーチャ
constexpr uint32_t kFeatEncrypt = 0x1, kFeatBlkzoned = 0x2, kFeatExtraAttr = 0x8, kFeatFlexXattr = 0x40, kFeatVerity = 0x400,
                   kFeatSbChksum = 0x800, kFeatCasefold = 0x1000, kFeatCompression = 0x2000, kFeatDeviceAlias = 0x8000, kFeatPackedSsa = 0x10000;
constexpr uint32_t kFeatKnownMask = 0x1FFFF;
// チェックポイントの印
constexpr uint32_t kCpUmount = 0x1, kCpOrphan = 0x2, kCpCompactSum = 0x4, kCpError = 0x8, kCpFsck = 0x10, kCpCrcRecovery = 0x40,
                   kCpNocrcRecovery = 0x200, kCpLargeNat = 0x400, kCpQuotaNeedFsck = 0x800, kCpResizefs = 0x4000;
// inode
constexpr uint8_t kInlXattr = 0x01, kInlData = 0x02, kInlDentry = 0x04, kInlDataExist = 0x08, kInlDots = 0x10, kInlExtra = 0x20, kInlComprReleased = 0x80;
constexpr uint32_t kFlComprFl = 0x4, kFlEncryptFl = 0x800, kFlCasefoldFl = 0x40000000;

inline uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t le32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24); }
inline uint64_t le64(const uint8_t* p) { return static_cast<uint64_t>(le32(p)) | (static_cast<uint64_t>(le32(p + 4)) << 32); }

// カーネルの crc32_le (反転なし、種を直接渡す)
uint32_t crc32_le(uint32_t crc, const uint8_t* p, size_t n) {
  static uint32_t table[256];
  static std::once_flag once;
  std::call_once(once, [] {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
      table[i] = c;
    }
  });
  for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return crc;
}

using Blk = std::shared_ptr<const std::vector<uint8_t>>;

struct Inode {
  uint32_t nid = 0;
  Blk blk;                      // inode ブロック
  uint16_t mode = 0;
  uint8_t inl = 0;              // i_inline
  uint32_t nlink = 0, flags = 0;
  uint64_t size = 0;
  int64_t mtime = 0;
  uint32_t extra_words = 0;     // i_extra_isize / 4
  uint32_t xattr_words = 0;     // インラインの xattr 領域 (4 バイト単位)
  uint32_t addrs_in_inode = 0;  // inode 内のブロック番号の数 (圧縮ファイルはクラスタ境界に切り下げ)
  uint32_t addrs_per_block = kAddrsPerBlock;
  uint32_t max_inline = 0;      // MAX_INLINE_DATA (バイト)
  bool compressed = false;
  uint8_t calg = 0, clog = 0;
  bool is_dir() const { return (mode & 0170000) == 0040000; }
  bool is_reg() const { return (mode & 0170000) == 0100000; }
  bool is_lnk() const { return (mode & 0170000) == 0120000; }
  bool inline_data() const { return (inl & kInlData) != 0; }
  bool inline_dentry() const { return (inl & kInlDentry) != 0; }
  const uint8_t* addr_base() const { return blk->data() + 360 + 4 * extra_words; }
  const uint8_t* inline_region() const { return addr_base() + 4; }   // DEF_INLINE_RESERVED_SIZE (1 ワード) の後
  uint32_t nid_at(int i) const { return le32(blk->data() + kBlk - 24 - 20 + 4 * i); }
};

struct NatEnt { uint32_t ino = 0, blkaddr = 0; uint8_t version = 0; };

struct DirCache {
  std::vector<FsDirEntry> ents;
  std::unordered_map<std::string, uint64_t> idx;
};

unsigned dtype_of(uint8_t ft) {
  switch (ft) {
    case 1: return 8;    // DT_REG
    case 2: return 4;    // DT_DIR
    case 3: return 2;    // DT_CHR
    case 4: return 6;    // DT_BLK
    case 5: return 1;    // DT_FIFO
    case 6: return 12;   // DT_SOCK
    case 7: return 10;   // DT_LNK
    default: return 0;
  }
}


#ifdef USBMEM_NTFS_WRITE
constexpr uint32_t kSitPerBlock = 55, kSitEntSize = 74, kNr = 214;
constexpr uint32_t kFtReg = 1, kFtDir = 2, kFtLnk = 7;
inline void put16(uint8_t* p, uint32_t v) { p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8); }
inline void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>(v >> (8 * i)); }
inline void put64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = static_cast<uint8_t>(v >> (8 * i)); }
// SIT / NAT のビットマップは MSB 先頭 (f2fs_test_bit)、dentry のビットマップは LSB 先頭 (test_bit_le)
inline bool bm_msb_get(const uint8_t* m, uint32_t i) { return (m[i >> 3] & (0x80u >> (i & 7))) != 0; }
inline void bm_msb_set(uint8_t* m, uint32_t i) { m[i >> 3] |= static_cast<uint8_t>(0x80u >> (i & 7)); }
inline void bm_msb_clr(uint8_t* m, uint32_t i) { m[i >> 3] &= static_cast<uint8_t>(~(0x80u >> (i & 7))); }
inline bool bm_lsb_get(const uint8_t* m, uint32_t i) { return (m[i >> 3] & (1u << (i & 7))) != 0; }
inline void bm_lsb_set(uint8_t* m, uint32_t i) { m[i >> 3] |= static_cast<uint8_t>(1u << (i & 7)); }
inline void bm_lsb_clr(uint8_t* m, uint32_t i) { m[i >> 3] &= static_cast<uint8_t>(~(1u << (i & 7))); }

// f2fs_dentry_hash (TEA、非 casefold)
void tea_transform(uint32_t buf[4], const uint32_t in[4]) {
  uint32_t sum = 0, b0 = buf[0], b1 = buf[1], a = in[0], b = in[1], c = in[2], d = in[3];
  for (int n = 0; n < 16; n++) {
    sum += 0x9E3779B9u;
    b0 += ((b1 << 4) + a) ^ (b1 + sum) ^ ((b1 >> 5) + b);
    b1 += ((b0 << 4) + c) ^ (b0 + sum) ^ ((b0 >> 5) + d);
  }
  buf[0] += b0; buf[1] += b1;
}
void str2hashbuf(const uint8_t* msg, size_t len, uint32_t* buf, int num) {
  uint32_t pad = static_cast<uint32_t>(len) | (static_cast<uint32_t>(len) << 8);
  pad |= pad << 16;
  uint32_t val = pad;
  if (len > static_cast<size_t>(num) * 4) len = static_cast<size_t>(num) * 4;
  for (size_t i = 0; i < len; i++) {
    if ((i % 4) == 0) val = pad;
    val = msg[i] + (val << 8);
    if ((i % 4) == 3) { *buf++ = val; val = pad; num--; }
  }
  if (--num >= 0) *buf++ = val;
  while (--num >= 0) *buf++ = pad;
}
uint32_t dentry_hash(const uint8_t* name, size_t len) {
  if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.')) return 0;
  uint32_t buf[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u}, in[4];
  const uint8_t* p = name;
  size_t n = len;
  while (n > 0) {
    str2hashbuf(p, n, in, 4);
    tea_transform(buf, in);
    p += 16;
    if (n <= 16) break;
    n -= 16;
  }
  return buf[0];   // カーネルの F2FS_HASH_COL_BIT は 1ULL<<63 で、32 ビットの hash には効かない (上位ビットも残す。fsck.f2fs も同じ)
}
inline uint32_t dir_buckets(uint32_t level, uint32_t dir_level) { return (level + dir_level < 31) ? (1u << (level + dir_level)) : (1u << 30); }
inline uint32_t bucket_blocks(uint32_t level) { return level < 31 ? 2 : 4; }
inline uint64_t dir_block_index(uint32_t level, uint32_t dir_level, uint32_t idx) {
  uint64_t b = 0;
  for (uint32_t i = 0; i < level; i++) b += static_cast<uint64_t>(dir_buckets(i, dir_level)) * bucket_blocks(i);
  return b + static_cast<uint64_t>(idx) * bucket_blocks(level);
}

// 1 つのセグメントの状態 (SIT エントリそのもの)
struct Seg { uint16_t vb = 0; uint8_t type = 0; uint8_t map[64] = {0}; uint64_t mtime = 0; };
// dentry 領域 (通常ブロックまたはインライン) への窓
struct DBuf {
  uint8_t *bm, *de, *fn; uint32_t nr;
  bool used(uint32_t i) const { return bm_lsb_get(bm, i); }
  int room(uint32_t slots) const {   // 連続して空いている slots 個の先頭 (なければ -1)
    uint32_t run = 0;
    for (uint32_t i = 0; i < nr; i++) {
      if (used(i)) { run = 0; continue; }
      if (++run == slots) return static_cast<int>(i + 1 - slots);
    }
    return -1;
  }
  int find(uint32_t hash, const std::string& name, uint32_t* ino, uint8_t* ft) const {   // スロット番号 / -1 / 負の errno (破損)
    for (uint32_t i = 0; i < nr;) {
      if (!used(i)) { i++; continue; }
      const uint8_t* e = de + 11 * i;
      uint32_t nlen = le16(e + 8), slots = (nlen + 7) / 8;
      if (nlen == 0 || nlen > 255 || i + slots > nr) return -kEUCLEAN;
      if (le32(e) == hash && nlen == name.size() && memcmp(fn + 8 * i, name.data(), nlen) == 0) { if (ino) *ino = le32(e + 4); if (ft) *ft = e[10]; return static_cast<int>(i); }
      i += slots;
    }
    return -1;
  }
  void put(uint32_t slot, uint32_t hash, const std::string& name, uint32_t ino, uint8_t ft) const {
    uint8_t* e = de + 11 * slot;
    put32(e, hash); put32(e + 4, ino); put16(e + 8, static_cast<uint32_t>(name.size())); e[10] = ft;
    uint32_t slots = static_cast<uint32_t>((name.size() + 7) / 8);
    memset(fn + 8 * slot, 0, 8 * slots);
    memcpy(fn + 8 * slot, name.data(), name.size());
    for (uint32_t k = 0; k < slots; k++) bm_lsb_set(bm, slot + k);
  }
  void del(uint32_t slot) const {
    uint32_t nlen = le16(de + 11 * slot + 8), slots = (nlen + 7) / 8;
    for (uint32_t k = 0; k < slots; k++) bm_lsb_clr(bm, slot + k);
  }
  bool only_dots() const {   // "." と ".." (スロット 0, 1) だけか
    for (uint32_t i = 2; i < nr; i++) if (used(i)) return false;
    return true;
  }
  bool empty() const { for (uint32_t i = 0; i < nr; i++) if (used(i)) return false; return true; }
};
inline DBuf dbuf_blk(uint8_t* p) { return {p, p + 30, p + 30 + 11 * kNr, kNr}; }
#endif

}  // namespace

struct F2fsVolume::Impl {
  BlockDevicePtr dev;
  std::mutex mu;
  uint64_t dev_blocks = 0;
  // スーパーブロック
  uint32_t feature = 0, root_ino = 0, node_ino = 0, meta_ino = 0;
  uint32_t seg0 = 0, cp_blkaddr = 0, nat_blkaddr = 0, main_blkaddr = 0, max_blkaddr = 0, nat_blocks = 0, max_nid = 0;
  uint64_t block_count = 0;
  // チェックポイント
  std::vector<uint8_t> ckpt;
  uint32_t cp_flags = 0, cp_start = 0;
  uint64_t cp_ver = 0;
  const uint8_t* nat_bitmap = nullptr;
  uint32_t nat_bitmap_bytes = 0;
  std::unordered_map<uint32_t, NatEnt> nat_journal;
  bool recovery_ignored = false;
  // キャッシュ
  static constexpr size_t kCacheCap = 2048;
  std::list<uint32_t> lru;
  std::unordered_map<uint32_t, std::pair<Blk, std::list<uint32_t>::iterator>> cache;
  std::list<uint32_t> dir_lru;
  std::unordered_map<uint32_t, std::shared_ptr<DirCache>> dirs;
  // 展開済みクラスタの 1 つ分
  uint32_t cl_nid = 0; uint64_t cl_idx = ~0ull; std::vector<uint8_t> cl_buf;

  bool valid_data_blk(uint32_t a) const { return a >= main_blkaddr && a < max_blkaddr; }
  bool has_dirty_pages() const {
#ifdef USBMEM_NTFS_WRITE
    return !dpages.empty();
#else
    return false;
#endif
  }

  int read_raw(uint32_t blk, void* out) {
    if (blk >= dev_blocks) return -EIO;
    Status st = dev->read_at(static_cast<uint64_t>(blk) * kBlk, out, kBlk);
    return st.ok() ? 0 : -EIO;
  }
  int get(uint32_t blk, Blk* out) {
    auto it = cache.find(blk);
    if (it != cache.end()) {
      lru.splice(lru.begin(), lru, it->second.second);
      *out = it->second.first;
      return 0;
    }
    auto v = std::make_shared<std::vector<uint8_t>>(kBlk);
    int rc = read_raw(blk, v->data());
    if (rc) return rc;
    if (cache.size() >= kCacheCap) { cache.erase(lru.back()); lru.pop_back(); }
    lru.push_front(blk);
    cache[blk] = {v, lru.begin()};
    *out = v;
    return 0;
  }

  // ---- NAT: nid → ブロック番号
  int nat_lookup(uint32_t nid, NatEnt* e) {
    if (nid < kReservedNodes || nid >= max_nid) return -ENOENT;
#ifdef USBMEM_NTFS_WRITE
    { auto d = nat_dirty.find(nid); if (d != nat_dirty.end()) { *e = d->second; return 0; } }
#endif
    auto j = nat_journal.find(nid);
    if (j != nat_journal.end()) { *e = j->second; return 0; }
    uint32_t block_off = nid / kNatPerBlock;
    uint32_t seg_off = block_off / kSegBlks;
    uint32_t addr = nat_blkaddr + (seg_off * kSegBlks * 2) + (block_off % kSegBlks);
    if ((block_off >> 3) >= nat_bitmap_bytes) return -kEUCLEAN;
    if (nat_bitmap[block_off >> 3] & (1u << (7 - (block_off & 7)))) addr += kSegBlks;
    Blk b;
    int rc = get(addr, &b);
    if (rc) return rc;
    const uint8_t* p = b->data() + 9 * (nid % kNatPerBlock);
    e->version = p[0];
    e->ino = le32(p + 1);
    e->blkaddr = le32(p + 5);
    return 0;
  }
  // nid のノードを読む。footer の nid / ino が一致しなければ破損。owner_ino = 0 なら inode ノード (ino == nid)
  int get_node(uint32_t nid, uint32_t owner_ino, Blk* out) {
#ifdef USBMEM_NTFS_WRITE
    { auto it = nodes.find(nid); if (it != nodes.end()) { *out = it->second; return 0; } }
#endif
    NatEnt e;
    int rc = nat_lookup(nid, &e);
    if (rc) return rc;
    if (e.blkaddr == kNullAddr) return -ENOENT;
    if (!valid_data_blk(e.blkaddr)) return -kEUCLEAN;
    Blk b;
    rc = get(e.blkaddr, &b);
    if (rc) return rc;
    const uint8_t* f = b->data() + kBlk - 24;
    if (le32(f) != nid) return -kEUCLEAN;
    if (le32(f + 4) != (owner_ino ? owner_ino : nid)) return -kEUCLEAN;
    *out = b;
    return 0;
  }

  int load_inode(uint32_t nid, Inode* in) {
    Blk b;
    int rc = get_node(nid, 0, &b);
    if (rc) return rc == -ENOENT ? -ENOENT : rc;
    const uint8_t* p = b->data();
    in->nid = nid; in->blk = b;
    in->mode = le16(p);
    in->inl = p[3];
    in->nlink = le32(p + 12);
    in->size = le64(p + 16);
    in->mtime = static_cast<int64_t>(le64(p + 48));
    in->flags = le32(p + 80);
    uint32_t t = in->mode & 0170000;
    if (t != 0040000 && t != 0100000 && t != 0120000 && t != 0020000 && t != 0060000 && t != 0010000 && t != 0140000) return -kEUCLEAN;
    bool has_extra = (in->inl & kInlExtra) != 0;
    uint32_t extra = 0;
    if (has_extra) {
      extra = le16(p + 360);
      if (!(feature & kFeatExtraAttr) || extra < 4 || extra > 4 * (kDefAddrsPerInode / 2) || (extra & 3)) return -kEUCLEAN;
    }
    in->extra_words = extra / 4;
    if (feature & kFeatFlexXattr) in->xattr_words = has_extra ? le16(p + 362) : 0;
    else if ((in->inl & kInlXattr) || (in->inl & kInlDentry)) in->xattr_words = 50;
    else in->xattr_words = 0;
    uint32_t cur = kDefAddrsPerInode - in->extra_words;
    if (in->xattr_words > cur) return -kEUCLEAN;
    in->addrs_in_inode = cur - in->xattr_words;
    in->max_inline = cur - in->xattr_words > 1 ? 4 * (cur - in->xattr_words - 1) : 0;
    if (in->is_reg() && (in->flags & kFlComprFl) && has_extra && (feature & kFeatCompression) && extra >= 36) {
      in->calg = p[360 + 32];
      in->clog = p[360 + 33];
      if (in->clog < 2 || in->clog > 8 || in->calg > 3) return -kEUCLEAN;
      uint32_t csz = 1u << in->clog;
      in->compressed = true;
      in->addrs_in_inode = in->addrs_in_inode / csz * csz;
      in->addrs_per_block = kAddrsPerBlock / csz * csz;
      if (in->addrs_per_block == 0) return -kEUCLEAN;
    }
    return 0;
  }

  // ファイル内ブロック fblk → ブロック番号 (0 = 穴 / 未割り当て、kNewAddr = 確保済みで未書き込み)
  int bmap(const Inode& in, uint64_t fblk, uint32_t* addr) {
    *addr = 0;
    const uint64_t dpi = in.addrs_in_inode, dpb = in.addrs_per_block;
    if (fblk < dpi) { *addr = le32(in.addr_base() + 4 * fblk); return 0; }
    uint64_t b = fblk - dpi;
    uint32_t nid = 0; int level = 0; uint64_t o1 = 0, o2 = 0, o3 = 0;
    const uint64_t ind = dpb * kNidsPerBlock;
    if (b < dpb) { nid = in.nid_at(0); level = 1; o1 = b; }
    else if ((b -= dpb) < dpb) { nid = in.nid_at(1); level = 1; o1 = b; }
    else if ((b -= dpb) < ind) { nid = in.nid_at(2); level = 2; o1 = b / dpb; o2 = b % dpb; }
    else if ((b -= ind) < ind) { nid = in.nid_at(3); level = 2; o1 = b / dpb; o2 = b % dpb; }
    else if ((b -= ind) < ind * kNidsPerBlock) { nid = in.nid_at(4); level = 3; o1 = b / ind; uint64_t r = b % ind; o2 = r / dpb; o3 = r % dpb; }
    else return -EIO;
    if (nid == 0) return 0;
    Blk n;
    int rc = get_node(nid, in.nid, &n);
    if (rc == -ENOENT) return -kEUCLEAN;   // 親が指す nid が未割り当て
    if (rc) return rc;
    for (int l = level; l > 1; l--) {
      uint64_t idx = (l == level) ? o1 : o2;
      uint32_t child = le32(n->data() + 4 * idx);
      if (child == 0) return 0;
      rc = get_node(child, in.nid, &n);
      if (rc == -ENOENT) return -kEUCLEAN;
      if (rc) return rc;
    }
    uint64_t off = level == 1 ? o1 : level == 2 ? o2 : o3;
    *addr = le32(n->data() + 4 * off);
    return 0;
  }

  // ---- 読み取り
  // ファイル ino の fblk 番目のブロックの未書き込み (ダーティ) ページ。書き込み可でマウントしていなければ常に nullptr
  const std::vector<uint8_t>* dirty_page(uint32_t ino, uint64_t fblk) const {
#ifdef USBMEM_NTFS_WRITE
    if (!dpages.empty()) { auto it = dpages.find((static_cast<uint64_t>(ino) << 32) | fblk); if (it != dpages.end()) return &it->second; }
#endif
    (void)ino; (void)fblk;
    return nullptr;
  }
  int read_block_into(uint32_t addr, uint8_t* dst, size_t from, size_t n) {   // addr のブロックの [from, from+n) を dst へ (穴 / 未書き込みはゼロ)
    if (addr == kNullAddr || addr == kNewAddr) { memset(dst, 0, n); return 0; }
    if (!valid_data_blk(addr)) return -kEUCLEAN;
    Blk b;
    int rc = get(addr, &b);
    if (rc) return rc;
    memcpy(dst, b->data() + from, n);
    return 0;
  }
  int load_cluster(const Inode& in, uint64_t cidx) {
    if (cl_nid == in.nid && cl_idx == cidx) return 0;
    cl_nid = 0; cl_idx = ~0ull;
    const uint32_t csz = 1u << in.clog;
    std::vector<uint32_t> a(csz);
    for (uint32_t i = 0; i < csz; i++) { int rc = bmap(in, cidx * csz + i, &a[i]); if (rc) return rc; }
    cl_buf.assign(static_cast<size_t>(csz) * kBlk, 0);
    if (a[0] == kCompressAddr) {
      uint32_t cnt = 0;
      while (cnt + 1 < csz && a[cnt + 1] != kNullAddr && a[cnt + 1] != kNewAddr) cnt++;
      if (cnt == 0) return -kEUCLEAN;
      std::vector<uint8_t> cbuf(static_cast<size_t>(cnt) * kBlk);
      for (uint32_t i = 0; i < cnt; i++) {
        if (!valid_data_blk(a[i + 1])) return -kEUCLEAN;
        int rc = read_raw(a[i + 1], cbuf.data() + static_cast<size_t>(i) * kBlk);
        if (rc) return rc;
      }
      uint32_t clen = le32(cbuf.data());
      if (clen == 0 || static_cast<uint64_t>(clen) + kCompHdr > cbuf.size()) return -kEUCLEAN;
      int n = usbmem_f2fs_decompress(in.calg, cbuf.data() + kCompHdr, clen, cl_buf.data(), cl_buf.size());
      if (n < 0) return n == -EOPNOTSUPP ? -kEOPNOTSUPP : -kEUCLEAN;
      if (static_cast<size_t>(n) != cl_buf.size()) return -kEUCLEAN;
    } else {
      for (uint32_t i = 0; i < csz; i++) {
        if (a[i] == kCompressAddr) return -kEUCLEAN;
        int rc = read_block_into(a[i], cl_buf.data() + static_cast<size_t>(i) * kBlk, 0, kBlk);
        if (rc) return rc;
      }
    }
    cl_nid = in.nid; cl_idx = cidx;
    return 0;
  }
  int64_t read_file(const Inode& in, uint64_t off, uint8_t* buf, size_t len) {
    if (off >= in.size || len == 0) return 0;
    len = static_cast<size_t>(std::min<uint64_t>(len, in.size - off));
    if (in.flags & kFlEncryptFl) return -kEOPNOTSUPP;
    if (in.inline_data() && !in.compressed) {
      uint64_t avail = std::min<uint64_t>(in.size, in.max_inline);
      if (off >= avail) { memset(buf, 0, len); return static_cast<int64_t>(len); }   // 範囲外はゼロ
      size_t n = static_cast<size_t>(std::min<uint64_t>(len, avail - off));
      memcpy(buf, in.inline_region() + off, n);
      if (n < len) memset(buf + n, 0, len - n);
      return static_cast<int64_t>(len);
    }
    if (in.compressed && (in.inl & kInlComprReleased)) return -kEOPNOTSUPP;
    size_t done = 0;
    while (done < len) {
      uint64_t pos = off + done;
      uint64_t fblk = pos / kBlk;
      size_t boff = static_cast<size_t>(pos % kBlk);
      if (in.compressed) {
        const uint32_t csz = 1u << in.clog;
        uint64_t cidx = fblk / csz;
        int rc = load_cluster(in, cidx);
        if (rc) return rc;
        size_t coff = static_cast<size_t>(pos - cidx * csz * kBlk);
        size_t n = std::min<size_t>(len - done, cl_buf.size() - coff);
        memcpy(buf + done, cl_buf.data() + coff, n);
        done += n;
        continue;
      }
      if (const std::vector<uint8_t>* dp = dirty_page(in.nid, fblk)) {
        size_t n = std::min<size_t>(len - done, kBlk - boff);
        memcpy(buf + done, dp->data() + boff, n);
        done += n;
        continue;
      }
      uint32_t addr;
      int rc = bmap(in, fblk, &addr);
      if (rc) return rc;
      if (boff == 0 && len - done >= kBlk && addr != kNullAddr && addr != kNewAddr && !has_dirty_pages()) {
        // ブロック整列の部分は、連続するブロック番号をまとめて 1 回で読む
        size_t maxblk = (len - done) / kBlk;
        size_t run = 1;
        while (run < maxblk && run < 256) {
          uint32_t nx;
          if (bmap(in, fblk + run, &nx) || nx != addr + run) break;
          run++;
        }
        if (!valid_data_blk(addr) || !valid_data_blk(addr + static_cast<uint32_t>(run) - 1)) return -kEUCLEAN;
        if (addr + run > dev_blocks) return -EIO;
        Status st = dev->read_at(static_cast<uint64_t>(addr) * kBlk, buf + done, run * kBlk);
        if (!st.ok()) return -EIO;
        done += run * kBlk;
        continue;
      }
      size_t n = std::min<size_t>(len - done, kBlk - boff);
      rc = read_block_into(addr, buf + done, boff, n);
      if (rc) return rc;
      done += n;
    }
    return static_cast<int64_t>(len);
  }

  // ---- ディレクトリ
  static void add_ent(std::vector<FsDirEntry>* out, const uint8_t* name, uint16_t nlen, uint32_t ino, uint8_t ft) {
    if ((nlen == 1 && name[0] == '.') || (nlen == 2 && name[0] == '.' && name[1] == '.')) return;
    out->push_back({std::string(reinterpret_cast<const char*>(name), nlen), ino, dtype_of(ft)});
  }
  // 1 つの dentry 領域 (通常ブロックまたはインライン) を走査する。nr = スロット数、bm = ビットマップ、de = dentry 配列、fn = ファイル名配列
  int scan_dentries(const uint8_t* bm, const uint8_t* de, const uint8_t* fn, uint32_t nr, std::vector<FsDirEntry>* out) {
    uint32_t i = 0;
    while (i < nr) {
      if (!(bm[i >> 3] & (1u << (i & 7)))) { i++; continue; }
      const uint8_t* d = de + 11 * i;
      uint32_t ino = le32(d + 4);
      uint16_t nlen = le16(d + 8);
      uint8_t ft = d[10];
      uint32_t slots = (nlen + 7) / 8;
      if (nlen == 0 || nlen > 255 || i + slots > nr) return -kEUCLEAN;
      if (ino < kReservedNodes || ino >= max_nid) return -kEUCLEAN;
      add_ent(out, fn + 8 * i, nlen, ino, ft);
      i += slots;
    }
    return 0;
  }
  int list_dir(const Inode& in, std::vector<FsDirEntry>* out) {
    out->clear();
    if (in.flags & kFlEncryptFl) return -kEOPNOTSUPP;
    if (in.flags & kFlCasefoldFl) return -kEOPNOTSUPP;
    if (in.inline_dentry()) {
      uint32_t maxi = in.max_inline;
      uint32_t nr = maxi * 8 / ((11 + 8) * 8 + 1);
      uint32_t bms = (nr + 7) / 8;
      if (nr == 0 || bms + 19u * nr > maxi) return -kEUCLEAN;
      uint32_t reserved = maxi - 19 * nr - bms;
      const uint8_t* r = in.inline_region();
      return scan_dentries(r, r + bms + reserved, r + bms + reserved + 11 * nr, nr, out);
    }
    uint64_t nblk = (in.size + kBlk - 1) / kBlk;
    if (nblk > block_count) return -kEUCLEAN;
    constexpr uint32_t kNr = 214;
    for (uint64_t b = 0; b < nblk; b++) {
      if (const std::vector<uint8_t>* dp = dirty_page(in.nid, b)) {
        int rc = scan_dentries(dp->data(), dp->data() + 30, dp->data() + 30 + 11 * kNr, kNr, out);
        if (rc) return rc;
        continue;
      }
      uint32_t addr;
      int rc = bmap(in, b, &addr);
      if (rc) return rc;
      if (addr == kNullAddr || addr == kNewAddr) continue;
      if (!valid_data_blk(addr)) return -kEUCLEAN;
      Blk blk;
      rc = get(addr, &blk);
      if (rc) return rc;
      const uint8_t* p = blk->data();
      rc = scan_dentries(p, p + 30, p + 30 + 11 * kNr, kNr, out);
      if (rc) return rc;
    }
    return 0;
  }
  int dir_of(uint32_t nid, const Inode& in, std::shared_ptr<DirCache>* out) {
    auto it = dirs.find(nid);
    if (it != dirs.end()) { *out = it->second; return 0; }
    auto dc = std::make_shared<DirCache>();
    int rc = list_dir(in, &dc->ents);
    if (rc) return rc;
    for (size_t i = 0; i < dc->ents.size(); i++) dc->idx.emplace(dc->ents[i].name, i);
    if (dirs.size() >= 8) { dirs.erase(dir_lru.back()); dir_lru.pop_back(); }
    dir_lru.push_front(nid);
    dirs[nid] = dc;
    *out = dc;
    return 0;
  }

  // ---- マウント
  int parse_super(const uint8_t* s) {
    if (le32(s) != kMagic) return -EINVAL;
    feature = le32(s + 2180);
    if (le16(s + 4) != 1) return -kEOPNOTSUPP;
    uint32_t log_blk = le32(s + 16);
    if (log_blk != 12) return (log_blk > 12 && log_blk <= 16) ? -kEOPNOTSUPP : -kEUCLEAN;
    if (le32(s + 20) != 9) return -kEOPNOTSUPP;
    uint32_t lss = le32(s + 8), lspb = le32(s + 12);
    if (lss < 9 || lss > 12 || lss + lspb != log_blk) return -kEUCLEAN;
    if (feature & kFeatSbChksum) {
      if (le32(s + 32) != 3068 || crc32_le(kMagic, s, 3068) != le32(s + 3068)) return -kEUCLEAN;
    }
    if (feature & kFeatEncrypt) return -kEOPNOTSUPP;
    if (feature & kFeatVerity) return -kEOPNOTSUPP;
    if (feature & kFeatCasefold) return -kEOPNOTSUPP;
    if (feature & (kFeatBlkzoned | kFeatDeviceAlias | kFeatPackedSsa)) return -kEOPNOTSUPP;
    if (feature & ~kFeatKnownMask) return -kEOPNOTSUPP;
    for (int d = 0; d < 8; d++) if (s[2201 + 68 * d] != 0) return -kEOPNOTSUPP;   // 複数デバイス
    block_count = le64(s + 36);
    uint32_t segs_per_sec = le32(s + 24), secs_per_zone = le32(s + 28), section_count = le32(s + 44), seg_count = le32(s + 48);
    uint32_t seg_ckpt = le32(s + 52), seg_sit = le32(s + 56), seg_nat = le32(s + 60), seg_ssa = le32(s + 64), seg_main = le32(s + 68);
    seg0 = le32(s + 72); cp_blkaddr = le32(s + 76);
    uint32_t sit = le32(s + 80); nat_blkaddr = le32(s + 84);
    uint32_t ssa = le32(s + 88); main_blkaddr = le32(s + 92);
    root_ino = le32(s + 96); node_ino = le32(s + 100); meta_ino = le32(s + 104);
    uint32_t cp_payload = le32(s + 1664);
    if (segs_per_sec == 0 || secs_per_zone == 0 || section_count == 0 || seg_count == 0 || seg_count > 4194304u) return -kEUCLEAN;
    if (seg_ckpt == 0 || seg_sit == 0 || seg_nat == 0 || seg_ssa == 0 || seg_main == 0) return -kEUCLEAN;
    if (static_cast<uint64_t>(seg_ckpt) + seg_sit + seg_nat + seg_ssa + seg_main > seg_count) return -kEUCLEAN;
    if (static_cast<uint64_t>(section_count) * segs_per_sec > seg_main) return -kEUCLEAN;
    // 領域は seg0 から ckpt → sit → nat → ssa → main の順に並ぶ
    uint64_t exp = seg0;
    if (cp_blkaddr != exp) return -kEUCLEAN;
    exp += static_cast<uint64_t>(seg_ckpt) * kSegBlks; if (sit != exp) return -kEUCLEAN;
    exp += static_cast<uint64_t>(seg_sit) * kSegBlks; if (nat_blkaddr != exp) return -kEUCLEAN;
    exp += static_cast<uint64_t>(seg_nat) * kSegBlks; if (ssa != exp) return -kEUCLEAN;
    exp += static_cast<uint64_t>(seg_ssa) * kSegBlks; if (main_blkaddr != exp) return -kEUCLEAN;
    uint64_t end = static_cast<uint64_t>(seg0) + static_cast<uint64_t>(seg_count) * kSegBlks;
    if (end > 0xFFFFFFF0ull || main_blkaddr + static_cast<uint64_t>(seg_main) * kSegBlks > end) return -kEUCLEAN;
    max_blkaddr = static_cast<uint32_t>(end);
    if (block_count == 0 || block_count > end || block_count > dev_blocks) return -kEUCLEAN;
    if (cp_payload > kSegBlks - 16) return -kEUCLEAN;
    nat_blocks = (seg_nat >> 1) * kSegBlks;
    if (nat_blocks == 0) return -kEUCLEAN;
    uint64_t mn = static_cast<uint64_t>(kNatPerBlock) * nat_blocks;
    max_nid = mn > 0xFFFFFFF0ull ? 0xFFFFFFF0u : static_cast<uint32_t>(mn);
    if (root_ino < kReservedNodes || root_ino >= max_nid || node_ino == 0 || meta_ino == 0) return -kEUCLEAN;
    sb_cp_payload = cp_payload;
    sit_blkaddr = sit; ssa_blkaddr = ssa; sb_seg_main = seg_main; sb_seg_sit = seg_sit; sb_segs_per_sec = segs_per_sec; sb_secs_per_zone = secs_per_zone;
    memcpy(sb_uuid, s + 108, 16);
    return 0;
  }
  int rw_refusal = 0;   // mount_rw で書き込み可にできなかった理由 (errno)。0 = 書き込み可 / 読み取り専用で開いただけ
  uint32_t sb_cp_payload = 0, sit_blkaddr = 0, ssa_blkaddr = 0, sb_seg_main = 0, sb_seg_sit = 0, sb_segs_per_sec = 0, sb_secs_per_zone = 0;
  uint8_t sb_uuid[16] = {0};

#ifdef USBMEM_NTFS_WRITE
  // ======== 書き込み (f2fs_write.inc。docs/f2fs-write.md) ========
  // 設計: 変更はすべてメモリ上 (ダーティ ノード nodes / ダーティ ページ dpages / NAT・SIT・SSA の差分) に溜め、sync() で 1 回の完全なチェックポイントとしてコミットする。
  // コミット前に書くのは「前回のチェックポイントから見て使われていないブロック」だけ。解放したブロック / セグメントはコミットまで再利用しない (cvb)。
  using NodeVec = std::shared_ptr<std::vector<uint8_t>>;
  static constexpr int kLogHotData = 0, kLogWarmData = 1, kLogColdData = 2, kLogHotNode = 3, kLogWarmNode = 4, kLogColdNode = 5;
  struct Cur { uint32_t segno = 0, blkoff = 0; };
  bool rw = false;                  // 書き込み可でマウントした
  bool broken = false;              // 操作の途中で失敗した: これ以上チェックポイントしない (コミット済みの状態は無傷のまま)
  bool dirty = false;               // 未コミットの変更がある
  uint32_t main_segs = 0, free_segs = 0, seg_hint = 0, nid_hint = kReservedNodes;
  uint64_t valid_blocks = 0, user_blocks = 0, pend_blocks = 0;   // pend_blocks = 予約済みで未配置 (NEW_ADDR) のデータブロック数
  uint32_t valid_nodes = 0, valid_inodes = 0, new_extra = 0;
  uint64_t mount_time = 0, cp_elapsed = 0;
  std::vector<Seg> segs;            // 現在 (未コミットを含む) のセグメント表
  std::vector<uint16_t> cvb;        // コミット済みの有効ブロック数 (空きセグメントの判定にだけ使う)
  std::set<uint32_t> committed_cur; // 最後にコミットしたチェックポイントのカレントセグメント (コミットまで再利用しない)
  std::set<uint32_t> sit_dirty;     // 変更した SIT ブロック番号
  Cur cur[6];
  std::unordered_map<uint32_t, std::vector<uint8_t>> ssa;   // サマリ ブロック (セグメント番号 → 4 KiB)。カレントの 6 つは常に持つ
  std::set<uint32_t> ssa_dirty;
  std::map<uint32_t, NodeVec> nodes;            // ダーティ ノード (nid → ブロック)。コミット時に新しい場所へ書く
  std::map<uint32_t, NatEnt> nat_dirty;         // 変更した NAT エントリ (ジャーナルの内容を含む)
  std::set<uint32_t> freed_nids;                // このエポックで解放した nid (コミットまで再割り当てしない)
  std::map<uint64_t, std::vector<uint8_t>> dpages;   // ダーティなデータ / ディレクトリ ページ ((ino << 32) | ブロック番号)
  long acc_iblocks = 0;                         // truncate 中に解放したブロック数 (i_blocks から引く)
  uint32_t gen_state = 1;
  static constexpr size_t kMaxDirtyPages = 1024;
  static constexpr uint32_t kReserveSegs = 8;

  uint8_t* nat_bm_w() { return const_cast<uint8_t*>(nat_bitmap); }
  uint8_t* sit_bm_w();
  const uint8_t* sit_bm() { return sit_bm_w(); }
  uint64_t seg_time() const;
  void drop_cache(uint32_t a);
  int write_blocks(uint32_t a, const void* d, uint32_t n);
  bool in_main(uint32_t a) const { return a >= main_blkaddr && a < main_blkaddr + main_segs * kSegBlks; }
  int invalidate(uint32_t a);
  bool is_cur(uint32_t s) const;
  bool seg_is_free(uint32_t s) const;
  int alloc_seg(uint32_t* out);
  void init_ssa(uint32_t segno, int type);
  int alloc_blk(int type, uint32_t owner_nid, uint8_t ver, uint16_t ofs, uint32_t* addr);
  int nat_get(uint32_t nid, NatEnt* e);
  int alloc_nid(uint32_t* out);
  bool space_ok(uint64_t blocks) const;
  int mut_node(uint32_t nid, uint32_t owner, NodeVec* out);
  int new_node(uint32_t ino, uint32_t ofs, bool cold, uint32_t* out);
  void free_node(uint32_t nid);
  int iblocks(uint32_t ino, long delta);
  int slot_ptr(uint32_t ino, uint64_t fblk, bool create, uint8_t** slot, uint32_t* dn_nid, uint32_t* ofs_in_node);
  int reserve_block(uint32_t ino, uint64_t fblk);
  int dpage_mut(uint32_t ino, uint64_t fblk, bool full_overwrite, std::vector<uint8_t>** out);
  int writeback_pages();
  int maybe_writeback() { return dpages.size() > kMaxDirtyPages ? writeback_pages() : 0; }
  void free_data_addr(uint32_t a);
  int trunc_node(uint32_t nid, int level, uint32_t ino, uint64_t base, uint64_t from);
  int truncate_blocks(uint32_t ino, uint64_t from_blk);
  int free_inode(uint32_t ino);
  void drop_dpages(uint32_t ino, uint64_t from_blk);
  int make_inode(uint16_t mode, uint32_t pino, const std::string& name, uint32_t uid, uint32_t gid, uint32_t* out);
  void set_footer(uint8_t* p, uint32_t nid, uint32_t ino, uint32_t flag);
  void finalize_inode(uint8_t* p, uint32_t nid);
  int touch(uint32_t nid, bool mtime);
  // ディレクトリ
  int inline_dbuf(const Inode& in, uint8_t* inode_base, DBuf* d);
  int dir_find(uint32_t dnid, const std::string& name, uint32_t* ino, uint8_t* ft, bool* found);
  int dir_add(uint32_t dnid, const std::string& name, uint32_t ino, uint8_t ft);
  int dir_del(uint32_t dnid, const std::string& name);
  int dir_set_ino(uint32_t dnid, const std::string& name, uint32_t ino);   // 既存エントリの ino を差し替える (".." の付け替え)
  int dir_convert_inline(uint32_t dnid);
  int dir_is_empty(uint32_t dnid, bool* empty);
  int dir_blk_read(const Inode& d, uint64_t b, Blk* out, bool* hole);
  void dir_cache_add(uint32_t dnid, const std::string& name, uint32_t ino, uint8_t ft);
  int check_name(const std::string& name);
  int check_mutable_dir(uint32_t dnid, Inode* d);
  // 公開操作の本体
  int op_create(uint64_t dir, const std::string& name, uint32_t mode, uint64_t* ino, uint16_t type, const std::string* target);
  int op_unlink(uint64_t dir, const std::string& name, bool want_dir);
  int op_rename(uint64_t od, const std::string& on, uint64_t nd, const std::string& nn);
  int op_link(uint64_t dir, const std::string& name, uint64_t existing);
  int64_t op_write(uint64_t ino, uint64_t off, const void* buf, size_t len, bool inplace);
  int op_truncate(uint64_t ino, uint64_t size);
  int convert_inline_file(uint32_t ino);
  // チェックポイント
  int flush_nodes();
  int roll_full_cursegs();
  int write_ssa_blocks();
  int flush_nat();
  int flush_sit();
  int write_cp_pack();
  int checkpoint();
  int load_write_state();
  int load_sit();
  int load_cursegs();
  void reset_write_state();
#endif

  // チェックポイントパックの検証。version を返す
  int validate_cp(uint32_t addr, Blk* first, uint64_t* ver) {
    Blk b;
    int rc = get(addr, &b);
    if (rc) return rc;
    const uint8_t* p = b->data();
    uint32_t off = le32(p + 164);
    if (off < 192 || off > kBlk - 4) return -kEUCLEAN;
    uint32_t crc = crc32_le(kMagic, p, off);
    if (off < kBlk - 4) crc = crc32_le(crc, p + off + 4, kBlk - off - 4);
    if (crc != le32(p + off)) return -kEUCLEAN;
    uint32_t total = le32(p + 136);
    if (total > kSegBlks || total <= 2) return -kEUCLEAN;
    uint64_t v1 = le64(p);
    Blk b2;
    rc = get(addr + total - 1, &b2);
    if (rc) return rc;
    const uint8_t* q = b2->data();
    uint32_t off2 = le32(q + 164);
    if (off2 < 192 || off2 > kBlk - 4) return -kEUCLEAN;
    uint32_t crc2 = crc32_le(kMagic, q, off2);
    if (off2 < kBlk - 4) crc2 = crc32_le(crc2, q + off2 + 4, kBlk - off2 - 4);
    if (crc2 != le32(q + off2) || le64(q) != v1) return -kEUCLEAN;
    *first = b; *ver = v1;
    return 0;
  }
  int load_checkpoint() {
    Blk c1, c2;
    uint64_t v1 = 0, v2 = 0;
    int r1 = validate_cp(cp_blkaddr, &c1, &v1);
    int r2 = validate_cp(cp_blkaddr + kSegBlks, &c2, &v2);
    if (r1 == -EIO && r2 == -EIO) return -EIO;
    bool use2;
    if (!r1 && !r2) use2 = static_cast<int64_t>(v2 - v1) > 0;
    else if (!r1) use2 = false;
    else if (!r2) use2 = true;
    else return -kEUCLEAN;
    cp_start = use2 ? cp_blkaddr + kSegBlks : cp_blkaddr;
    Blk& cur = use2 ? c2 : c1;
    cp_ver = use2 ? v2 : v1;
    ckpt.assign(static_cast<size_t>(1 + sb_cp_payload) * kBlk, 0);
    memcpy(ckpt.data(), cur->data(), kBlk);
    for (uint32_t i = 1; i <= sb_cp_payload; i++) {
      int rc = read_raw(cp_start + i, ckpt.data() + static_cast<size_t>(i) * kBlk);
      if (rc) return rc;
    }
    const uint8_t* c = ckpt.data();
    cp_flags = le32(c + 132);
    uint32_t sit_bytes = le32(c + 156), nat_bytes = le32(c + 160);
    uint32_t off;
    if (cp_flags & kCpLargeNat) off = 192 + 4;
    else if (sb_cp_payload > 0) off = 192;
    else off = 192 + sit_bytes;
    if (nat_bytes == 0 || off + static_cast<uint64_t>(nat_bytes) > ckpt.size()) return -kEUCLEAN;
    if (static_cast<uint64_t>(nat_bytes) * 8 < nat_blocks) return -kEUCLEAN;
    nat_bitmap = c + off;
    nat_bitmap_bytes = nat_bytes;
    return 0;
  }
  // NAT ジャーナル (カレントセグメントのサマリに入っている最新の NAT 更新) を読む
  int load_nat_journal() {
    const uint8_t* c = ckpt.data();
    uint32_t total = le32(c + 136), start_sum = le32(c + 140);
    if (total <= 2 || total > kSegBlks) return -kEUCLEAN;
    Blk b;
    const uint8_t* j;
    if (cp_flags & kCpCompactSum) {
      if (start_sum == 0 || start_sum >= total) return -kEUCLEAN;
      int rc = get(cp_start + start_sum, &b);
      if (rc) return rc;
      j = b->data();
    } else {
      uint32_t base = (cp_flags & kCpUmount) ? 6 : 3;   // NR_CURSEG_PERSIST_TYPE / NR_CURSEG_DATA_TYPE
      if (total < base + 1) return -kEUCLEAN;
      int rc = get(cp_start + total - (base + 1), &b);   // HOT_DATA (type 0) のサマリブロック
      if (rc) return rc;
      j = b->data() + 7 * 512;
    }
    uint32_t n = le16(j);
    const uint32_t jsize = kBlk - 5 - 7 * 512;           // 507
    uint32_t maxn = (jsize - 2) / 13;
    if (n > maxn) return -kEUCLEAN;
    for (uint32_t i = 0; i < n; i++) {
      const uint8_t* e = j + 2 + 13 * i;
      NatEnt ne; ne.version = e[4]; ne.ino = le32(e + 5); ne.blkaddr = le32(e + 9);
      nat_journal[le32(e)] = ne;
    }
    return 0;
  }
  // 最後のチェックポイントのあとに fsync されたノードが書かれていて、回復 (roll-forward) が必要か
  bool need_roll_forward(int* err) {
    const uint8_t* c = ckpt.data();
    uint32_t segno = le32(c + 36 + 4 * 1);     // cur_node_segno[1] = 暖かいノード
    uint32_t blkoff = le16(c + 68 + 2 * 1);    // cur_node_blkoff[1]
    uint64_t addr = static_cast<uint64_t>(main_blkaddr) + static_cast<uint64_t>(segno) * kSegBlks + blkoff;
    for (int steps = 0; steps < 65536; steps++) {
      if (addr < main_blkaddr || addr >= max_blkaddr) return false;
      Blk b;
      int rc = get(static_cast<uint32_t>(addr), &b);
      if (rc) { *err = rc; return false; }
      const uint8_t* f = b->data() + kBlk - 24;
      uint64_t nv = le64(f + 12);
      uint64_t want = cp_ver;
      bool ok;
      if (cp_flags & kCpNocrcRecovery) ok = (want << 32) == (nv << 32);
      else {
        if (cp_flags & kCpCrcRecovery) {
          uint32_t off = le32(c + 164);
          want |= static_cast<uint64_t>(le32(c + off)) << 32;
        }
        ok = want == nv;
      }
      if (!ok) return false;
      if (le32(f + 8) & (1u << 1)) return true;      // FSYNC マーク
      addr = le32(f + 20);
    }
    return true;
  }

  int mount_all(bool allow_noload) {
    dev_blocks = dev->size() / kBlk;
    int first_err = 0, rc = -EINVAL;
    for (int copy = 0; copy < 2; copy++) {
      std::vector<uint8_t> blkbuf(kBlk);
      int r = read_raw(static_cast<uint32_t>(copy), blkbuf.data());
      if (r) { if (!first_err) first_err = r; continue; }
      std::vector<uint8_t> sb(blkbuf.begin() + 1024, blkbuf.end());
      sb.resize(3072 + 16, 0);
      r = parse_super(sb.data());
      if (!r) { rc = 0; break; }
      if (!first_err) first_err = r;
    }
    if (rc) return first_err ? first_err : rc;
    rc = load_checkpoint();
    if (rc) return rc;
    rc = load_nat_journal();
    if (rc) return rc;
    bool risky = (cp_flags & (kCpError | kCpFsck | kCpQuotaNeedFsck | kCpResizefs)) != 0;
    int err = 0;
    bool rf = need_roll_forward(&err);
    if (err) return err;
    if (rf) risky = true;
    if (risky) {
      if (!allow_noload) return -kEUCLEAN;
      recovery_ignored = true;
    }
    // ルート
    Inode r;
    rc = load_inode(root_ino, &r);
    if (rc) return rc == -ENOENT ? -kEUCLEAN : rc;
    if (!r.is_dir()) return -kEUCLEAN;
    return 0;
  }
};

#ifdef USBMEM_NTFS_WRITE
#include "f2fs_write.inc"
#include "f2fs_write_ops.inc"
#include "f2fs_write_cp.inc"
#endif

std::unique_ptr<F2fsVolume> F2fsVolume::mount(BlockDevicePtr dev, int* err, const F2fsMountOptions& opts) {
  std::unique_ptr<F2fsVolume> vol(new F2fsVolume());
  vol->impl_.reset(new Impl());
  vol->impl_->dev = std::move(dev);
  int rc = vol->impl_->mount_all(opts.allow_noload);
  if (rc) {
    if (err) *err = -rc;
    return nullptr;
  }
  return vol;
}

std::unique_ptr<F2fsVolume> F2fsVolume::mount_rw(BlockDevicePtr dev, int* err) {
  bool ro_dev = dev && dev->is_read_only();
  // 読み取りのマウント条件 (チェックポイント / roll-forward の検査) をそのまま通す。要回復のボリュームは書き込み可どころかここで EUCLEAN。
  std::unique_ptr<F2fsVolume> vol = mount(std::move(dev), err, F2fsMountOptions());
  if (!vol) return nullptr;
#ifdef USBMEM_NTFS_WRITE
  if (ro_dev) { vol->impl_->rw_refusal = EROFS; return vol; }
  int rc = vol->impl_->load_write_state();
  if (rc) { vol->impl_->reset_write_state(); vol->impl_->rw_refusal = -rc; }   // 書き込みのホワイトリスト外 / 不整合: 読み取り専用のまま開く
#else
  (void)ro_dev;
  vol->impl_->rw_refusal = EROFS;
#endif
  return vol;
}
int F2fsVolume::write_refusal() const { return impl_->rw_refusal; }
bool F2fsVolume::writable() const {
#ifdef USBMEM_NTFS_WRITE
  return impl_->rw && !impl_->broken;
#else
  return false;
#endif
}
F2fsVolume::~F2fsVolume() {
#ifdef USBMEM_NTFS_WRITE
  if (impl_ && impl_->rw && impl_->dirty && !impl_->broken) impl_->checkpoint();   // 最後の安全網 (アプリは先に sync() する)
#endif
}
bool F2fsVolume::recovery_ignored() const { return impl_->recovery_ignored; }
uint64_t F2fsVolume::root() const { return impl_->root_ino; }

static bool ino_ok(uint64_t ino) { return ino <= 0xFFFFFFFFull; }

int F2fsVolume::lookup(uint64_t dir, const std::string& name, uint64_t* ino) {
  std::lock_guard<std::mutex> g(impl_->mu);
  if (!ino_ok(dir)) return -ENOENT;
  Inode d;
  int rc = impl_->load_inode(static_cast<uint32_t>(dir), &d);
  if (rc) return rc;
  if (!d.is_dir()) return -ENOTDIR;
  if (name.empty() || name.size() > 255) return -ENOENT;
  if (name == ".") { *ino = dir; return 0; }
  std::shared_ptr<DirCache> dc;
  rc = impl_->dir_of(d.nid, d, &dc);
  if (rc) return rc;
  if (name == "..") {   // 親は inode の i_pino
    *ino = le32(d.blk->data() + 84);
    return 0;
  }
  auto it = dc->idx.find(name);
  if (it == dc->idx.end()) return -ENOENT;
  *ino = dc->ents[it->second].ino;
  return 0;
}

int F2fsVolume::stat(uint64_t ino, FsStat* st) {
  std::lock_guard<std::mutex> g(impl_->mu);
  if (!ino_ok(ino)) return -ENOENT;
  Inode in;
  int rc = impl_->load_inode(static_cast<uint32_t>(ino), &in);
  if (rc) return rc;
  st->ino = ino; st->size = in.size; st->mode = in.mode; st->nlink = in.nlink; st->mtime = in.mtime;
  return 0;
}

int F2fsVolume::readdir(uint64_t dir, std::vector<FsDirEntry>* out) {
  std::lock_guard<std::mutex> g(impl_->mu);
  out->clear();
  if (!ino_ok(dir)) return -ENOENT;
  Inode d;
  int rc = impl_->load_inode(static_cast<uint32_t>(dir), &d);
  if (rc) return rc;
  if (!d.is_dir()) return -ENOTDIR;
  std::shared_ptr<DirCache> dc;
  rc = impl_->dir_of(d.nid, d, &dc);
  if (rc) return rc;
  *out = dc->ents;
  return 0;
}

int64_t F2fsVolume::read(uint64_t ino, uint64_t off, void* buf, size_t len) {
  std::lock_guard<std::mutex> g(impl_->mu);
  if (!ino_ok(ino)) return -ENOENT;
  Inode in;
  int rc = impl_->load_inode(static_cast<uint32_t>(ino), &in);
  if (rc) return rc;
  if (in.is_dir()) return -EISDIR;
  if (!in.is_reg() && !in.is_lnk()) return -EINVAL;
  return impl_->read_file(in, off, static_cast<uint8_t*>(buf), len);
}

int F2fsVolume::readlink(uint64_t ino, std::string* target) {
  std::lock_guard<std::mutex> g(impl_->mu);
  if (!ino_ok(ino)) return -ENOENT;
  Inode in;
  int rc = impl_->load_inode(static_cast<uint32_t>(ino), &in);
  if (rc) return rc;
  if (!in.is_lnk()) return -EINVAL;
  if (in.size == 0 || in.size > 4095) return -kEUCLEAN;
  std::string s(static_cast<size_t>(in.size), '\0');
  int64_t n = impl_->read_file(in, 0, reinterpret_cast<uint8_t*>(&s[0]), s.size());
  if (n < 0) return static_cast<int>(n);
  if (static_cast<uint64_t>(n) != in.size) return -kEUCLEAN;
  *target = std::move(s);
  return 0;
}

#ifdef USBMEM_NTFS_WRITE
int F2fsVolume::sync() { std::lock_guard<std::mutex> g(impl_->mu); if (!impl_->rw) return -30; return impl_->checkpoint(); }
int F2fsVolume::create(uint64_t dir, const std::string& name, uint32_t mode, uint64_t* ino) {
  std::lock_guard<std::mutex> g(impl_->mu);
  return impl_->op_create(dir, name, mode, ino, 0100000, nullptr);
}
int F2fsVolume::mkdir(uint64_t dir, const std::string& name, uint32_t mode, uint64_t* ino) {
  std::lock_guard<std::mutex> g(impl_->mu);
  return impl_->op_create(dir, name, mode, ino, 0040000, nullptr);
}
int F2fsVolume::symlink(uint64_t dir, const std::string& name, const std::string& target, uint64_t* ino) {
  std::lock_guard<std::mutex> g(impl_->mu);
  return impl_->op_create(dir, name, 0777, ino, 0120000, &target);
}
int F2fsVolume::unlink(uint64_t dir, const std::string& name) { std::lock_guard<std::mutex> g(impl_->mu); return impl_->op_unlink(dir, name, false); }
int F2fsVolume::rmdir(uint64_t dir, const std::string& name) { std::lock_guard<std::mutex> g(impl_->mu); return impl_->op_unlink(dir, name, true); }
int F2fsVolume::rename(uint64_t od, const std::string& on, uint64_t nd, const std::string& nn) { std::lock_guard<std::mutex> g(impl_->mu); return impl_->op_rename(od, on, nd, nn); }
int F2fsVolume::link(uint64_t dir, const std::string& name, uint64_t existing) { std::lock_guard<std::mutex> g(impl_->mu); return impl_->op_link(dir, name, existing); }
int64_t F2fsVolume::write(uint64_t ino, uint64_t off, const void* buf, size_t len) { std::lock_guard<std::mutex> g(impl_->mu); return impl_->op_write(ino, off, buf, len, false); }
int64_t F2fsVolume::write_inplace(uint64_t ino, uint64_t off, const void* buf, size_t len) { std::lock_guard<std::mutex> g(impl_->mu); return impl_->op_write(ino, off, buf, len, true); }
int F2fsVolume::truncate(uint64_t ino, uint64_t size) { std::lock_guard<std::mutex> g(impl_->mu); return impl_->op_truncate(ino, size); }
#else
int F2fsVolume::sync() { return -30; }
int F2fsVolume::create(uint64_t, const std::string&, uint32_t, uint64_t*) { return -30; }
int F2fsVolume::mkdir(uint64_t, const std::string&, uint32_t, uint64_t*) { return -30; }
int F2fsVolume::symlink(uint64_t, const std::string&, const std::string&, uint64_t*) { return -30; }
int F2fsVolume::unlink(uint64_t, const std::string&) { return -30; }
int F2fsVolume::rmdir(uint64_t, const std::string&) { return -30; }
int F2fsVolume::rename(uint64_t, const std::string&, uint64_t, const std::string&) { return -30; }
int F2fsVolume::link(uint64_t, const std::string&, uint64_t) { return -30; }
int64_t F2fsVolume::write(uint64_t, uint64_t, const void*, size_t) { return -30; }
int64_t F2fsVolume::write_inplace(uint64_t, uint64_t, const void*, size_t) { return -30; }
int F2fsVolume::truncate(uint64_t, uint64_t) { return -30; }
#endif

}  // namespace usbmem
