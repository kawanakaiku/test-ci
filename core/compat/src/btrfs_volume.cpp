// SPDX-License-Identifier: GPL-2.0-or-later
// Btrfs 読み取り専用リーダー。ディスク上の形式 (btrfs wiki "On-disk Format" / include/uapi/linux/btrfs_tree.h の構造体の並び) を直接読む。
// カーネルの fs/btrfs (約 16 万行: トランザクション / delayed ref / extent_io / bio / ワークキュー) は載せず、読み取りに必要な部分だけを自前で持つ。
// 方針: ディスク上の値は一切信用しない (範囲・桁あふれ・ループを毎回検査)。メタデータはノードごとにチェックサムを検証し、DUP のもう一方のコピーへフォールバックする。
// 書き込み・tree-log の再生・修復はしない。docs/btrfs-port.md
#include "usbmem/btrfs_volume.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>

#include "usbmem/crypto.h"
#include "usbmem/sha256.h"

extern "C" {
int usbmem_btrfs_zlib_inflate(const void* src, size_t srclen, void* dst, size_t dstcap, size_t* outlen);
int usbmem_btrfs_lzo_segment(const void* src, size_t srclen, void* dst, size_t* dstlen);
int usbmem_btrfs_zstd_inflate(const void* src, size_t srclen, void* dst, size_t dstcap, size_t* outlen);
}

namespace usbmem {
namespace {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

inline u16 le16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
inline u32 le32(const u8* p) { return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24); }
inline u64 le64(const u8* p) { return static_cast<u64>(le32(p)) | (static_cast<u64>(le32(p + 4)) << 32); }

// ---- ディスク上の定数
constexpr u64 kSbOffsets[3] = {0x10000ull, 0x4000000ull, 0x4000000000ull};
constexpr size_t kSbSize = 4096;
constexpr size_t kHdr = 101;       // btrfs_header
constexpr size_t kItemSz = 25;     // btrfs_item (key 17 + offset 4 + size 4)
constexpr size_t kPtrSz = 33;      // btrfs_key_ptr (key 17 + blockptr 8 + generation 8)
constexpr u32 kMaxNode = 65536;
constexpr int kMaxLevel = 8;
constexpr u64 kMaxRam = 2u << 20;  // 展開後の extent の上限 (カーネルは 128 KiB)

enum : u8 {
  kInodeItem = 1, kInodeRef = 12, kDirItem = 84, kDirIndex = 96, kExtentData = 108, kExtentCsum = 128,
  kRootItem = 132, kRootRef = 156, kChunkItem = 228,
};
constexpr u64 kRootTreeObj = 1, kExtentTreeObj = 2, kChunkTreeObj = 3, kFsTreeObj = 5, kRootTreeDirObj = 6, kCsumTreeObj = 7, kFirstFree = 256;
constexpr u64 kExtentCsumObj = static_cast<u64>(-10);
constexpr u64 kFirstChunkObj = 256;

constexpr u64 kIncompatKnown = (1ull << 0) | (1ull << 1) | (1ull << 2) | (1ull << 3) | (1ull << 4) | (1ull << 5) | (1ull << 6) | (1ull << 7) |
                               (1ull << 8) | (1ull << 9) | (1ull << 10) | (1ull << 11) | (1ull << 16);
constexpr u64 kBgRaid0 = 8, kBgRaid1 = 0x10, kBgDup = 0x20, kBgRaid10 = 0x40, kBgRaid5 = 0x80, kBgRaid6 = 0x100, kBgRaid1c3 = 0x200, kBgRaid1c4 = 0x400;

struct Key { u64 obj; u8 type; u64 off; };
inline Key rdkey(const u8* p) { return Key{le64(p), p[8], le64(p + 9)}; }
inline int kcmp(const Key& a, const Key& b) {
  if (a.obj != b.obj) return a.obj < b.obj ? -1 : 1;
  if (a.type != b.type) return a.type < b.type ? -1 : 1;
  if (a.off != b.off) return a.off < b.off ? -1 : 1;
  return 0;
}

// ---- チェックサム (crc32c / xxhash64 / sha256 / blake2b-256)
struct Crc32cTable {
  u32 t[256];
  Crc32cTable() {
    for (u32 i = 0; i < 256; i++) {
      u32 c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
      t[i] = c;
    }
  }
};
const Crc32cTable& crc_tab() { static const Crc32cTable t; return t; }
u32 crc32c_raw(u32 crc, const u8* p, size_t n) {   // 前後の反転なし (カーネルの crc32c() と同じ)
  const u32* t = crc_tab().t;
  while (n--) crc = t[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  return crc;
}
u32 name_hash(const u8* p, size_t n) { return crc32c_raw(0xFFFFFFFEu, p, n); }   // btrfs_name_hash = crc32c(~1, name)

inline u64 rotl64(u64 x, int r) { return (x << r) | (x >> (64 - r)); }
u64 xxh64(const u8* p, size_t len, u64 seed) {
  constexpr u64 P1 = 11400714785074694791ULL, P2 = 14029467366897019727ULL, P3 = 1609587929392839161ULL, P4 = 9650029242287828579ULL, P5 = 2870177450012600261ULL;
  auto round = [&](u64 acc, u64 in) { acc += in * P2; acc = rotl64(acc, 31); return acc * P1; };
  auto merge = [&](u64 acc, u64 val) { val = round(0, val); acc ^= val; return acc * P1 + P4; };
  const u8* end = p + len;
  u64 h;
  if (len >= 32) {
    u64 v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
    const u8* lim = end - 32;
    do {
      v1 = round(v1, le64(p)); v2 = round(v2, le64(p + 8)); v3 = round(v3, le64(p + 16)); v4 = round(v4, le64(p + 24));
      p += 32;
    } while (p <= lim);
    h = rotl64(v1, 1) + rotl64(v2, 7) + rotl64(v3, 12) + rotl64(v4, 18);
    h = merge(h, v1); h = merge(h, v2); h = merge(h, v3); h = merge(h, v4);
  } else {
    h = seed + P5;
  }
  h += len;
  while (p + 8 <= end) { h ^= round(0, le64(p)); h = rotl64(h, 27) * P1 + P4; p += 8; }
  if (p + 4 <= end) { h ^= static_cast<u64>(le32(p)) * P1; h = rotl64(h, 23) * P2 + P3; p += 4; }
  while (p < end) { h ^= static_cast<u64>(*p++) * P5; h = rotl64(h, 11) * P1; }
  h ^= h >> 33; h *= P2; h ^= h >> 29; h *= P3; h ^= h >> 32;
  return h;
}

size_t csum_size_of(u16 type) {
  switch (type) { case 0: return 4; case 1: return 8; case 2: return 32; case 3: return 32; default: return 0; }
}
void calc_csum(u16 type, const u8* p, size_t n, u8* out) {
  switch (type) {
    case 0: { u32 c = ~crc32c_raw(0xFFFFFFFFu, p, n); for (int i = 0; i < 4; i++) out[i] = static_cast<u8>(c >> (8 * i)); break; }
    case 1: { u64 x = xxh64(p, n, 0); for (int i = 0; i < 8; i++) out[i] = static_cast<u8>(x >> (8 * i)); break; }
    case 2: { auto d = Sha256::hash(p, n); std::memcpy(out, d.data(), 32); break; }
    default: crypto::blake2b(p, n, out, 32); break;
  }
}

// ---- 構造
struct Stripe { u64 devid, off; };
struct Chunk { u64 start = 0, len = 0, type = 0; std::vector<Stripe> stripes; };

struct Node {
  std::vector<u8> d;
  u8 level = 0;
  u32 n = 0;
  Key key(int i) const { return rdkey(&d[kHdr + static_cast<size_t>(i) * (level ? kPtrSz : kItemSz)]); }
  u64 child(int i) const { return le64(&d[kHdr + static_cast<size_t>(i) * kPtrSz + 17]); }
  const u8* item(int i, u32* sz) const {
    const u8* it = &d[kHdr + static_cast<size_t>(i) * kItemSz];
    *sz = le32(it + 21);
    return &d[kHdr + le32(it + 17)];
  }
};
using NodeP = std::shared_ptr<const Node>;

struct Cursor {
  std::vector<std::pair<NodeP, int>> st;
  bool valid() const { return !st.empty(); }
  Key key() const { return st.back().first->key(st.back().second); }
  const u8* item(u32* sz) const { return st.back().first->item(st.back().second, sz); }
};

struct Subvol { u64 id = 0, bytenr = 0, flags = 0; int level = 0; };

struct Inode { u64 size = 0, nbytes = 0, flags = 0; u32 nlink = 0, mode = 0; int64_t mtime = 0; };

constexpr unsigned kDtByFt[9] = {0, 8, 4, 2, 6, 1, 12, 10, 0};   // BTRFS_FT_* → DT_*

}  // namespace

struct BtrfsVolume::Impl {
  BlockDevicePtr dev;
  std::mutex mu;
  BtrfsMountOptions opt;

  // スーパーブロック
  std::array<u8, kSbSize> sb{};
  u32 sectorsize = 0, nodesize = 0;
  u16 csum_type = 0;
  size_t csum_len = 0;
  u64 root_bytenr = 0, chunk_root = 0, incompat = 0, devid = 0, generation = 0;
  int root_level = 0, chunk_level = 0;
  std::array<u8, 16> meta_fsid{};
  std::string label_;
  bool log_ignored = false;

  // スーパーブロックの各コピーの状態 (書き込みマウントの検査用: ミラーが壊れている / 世代が違うときは書かない)
  bool sb_inrange[3] = {false, false, false}, sb_valid_seen[3] = {false, false, false};
  u64 sb_gen_seen[3] = {0, 0, 0};

  std::map<u64, Chunk> chunks;
  std::unordered_map<u64, NodeP> ncache;
  size_t ncache_max = 64;

  std::vector<Subvol> subvols;
  std::map<u64, u32> subvol_slot;
  u64 mount_subvol = kFsTreeObj;

  bool csum_loaded = false, csum_present = false;
  Subvol csum_root;

  // 展開済み extent の小さなキャッシュ
  struct Ext { u64 key = 0; std::shared_ptr<std::vector<u8>> data; };
  Ext ecache[4];
  int enext = 0;

  // ---- 低レベル I/O
  int devread(u64 phys, void* buf, size_t n) {
    if (!range_ok(dev->size(), phys, n)) return -EIO;
    Status st = dev->read_at(phys, buf, n);
    return st.ok() ? 0 : -EIO;
  }

  const Chunk* find_chunk(u64 logical) const {
    auto it = chunks.upper_bound(logical);
    if (it == chunks.begin()) return nullptr;
    --it;
    const Chunk& c = it->second;
    if (logical - c.start >= c.len) return nullptr;
    return &c;
  }
  // 論理 [logical, logical+n) が 1 つの chunk に収まるときの、各コピーの物理位置
  int map_logical(u64 logical, u64 n, std::vector<u64>* phys) const {
    const Chunk* c = find_chunk(logical);
    if (!c || n > c->len - (logical - c->start)) return -EIO;
    phys->clear();
    for (const Stripe& s : c->stripes) {
      if (s.devid != devid) continue;
      u64 p = s.off + (logical - c->start);
      if (p < s.off) return -EIO;
      phys->push_back(p);
    }
    return phys->empty() ? -EIO : 0;
  }

  // ---- ノード
  int validate_node(Node& nd, u64 logical, int expect_level) {
    const std::vector<u8>& d = nd.d;
    if (le64(&d[48]) != logical) return -EIO;
    if (std::memcmp(&d[32], meta_fsid.data(), 16) != 0) return -EIO;
    nd.level = d[100];
    nd.n = le32(&d[96]);
    if (nd.level >= kMaxLevel) return -EIO;
    if (expect_level >= 0 && nd.level != expect_level) return -EIO;
    if (nd.level == 0) {
      size_t body = nodesize - kHdr;
      if (static_cast<u64>(nd.n) * kItemSz > body) return -EIO;
      Key prev{0, 0, 0};
      for (u32 i = 0; i < nd.n; i++) {
        const u8* it = &d[kHdr + static_cast<size_t>(i) * kItemSz];
        u64 off = le32(it + 17), sz = le32(it + 21);
        if (off < static_cast<u64>(nd.n) * kItemSz || off + sz > body) return -EIO;
        Key k = rdkey(it);
        if (i && kcmp(prev, k) > 0) return -EIO;
        prev = k;
      }
    } else {
      if (static_cast<u64>(nd.n) * kPtrSz + kHdr > nodesize || nd.n == 0) return -EIO;
      Key prev{0, 0, 0};
      for (u32 i = 0; i < nd.n; i++) {
        Key k = nd.key(static_cast<int>(i));
        if (i && kcmp(prev, k) > 0) return -EIO;
        if (nd.child(static_cast<int>(i)) == 0) return -EIO;
        prev = k;
      }
    }
    return 0;
  }

  int read_node(u64 logical, int expect_level, NodeP* out) {
#ifdef USBMEM_NTFS_WRITE
    {   // 現在のトランザクションで作った (未書き込みの) ブロックを優先
      auto dit = dirty.find(logical);
      if (dit != dirty.end()) { *out = dit->second; return 0; }
    }
#endif
    auto it = ncache.find(logical);
    if (it != ncache.end() && (expect_level < 0 || it->second->level == expect_level)) { *out = it->second; return 0; }
    std::vector<u64> phys;
    int rc = map_logical(logical, nodesize, &phys);
    if (rc) return rc;
    for (u64 p : phys) {
      auto nd = std::make_shared<Node>();
      nd->d.resize(nodesize);
      if (devread(p, nd->d.data(), nodesize)) continue;
      u8 cs[32];
      calc_csum(csum_type, &nd->d[32], nodesize - 32, cs);
      if (std::memcmp(cs, nd->d.data(), csum_len) != 0) continue;
      if (validate_node(*nd, logical, expect_level)) continue;
      if (ncache.size() >= ncache_max) ncache.clear();
      ncache[logical] = nd;
      *out = nd;
      return 0;
    }
    return -EIO;
  }

  // 位置づけ。le=false: key 以上で最小の項目 / le=true: key 以下で最大の項目。0 = 見つかった、1 = 無い、負 = エラー
  int search(u64 root, int level, const Key& k, bool le, Cursor& c) {
    c.st.clear();
    NodeP n;
    int rc = read_node(root, level, &n);
    if (rc) return rc;
    for (int depth = 0; depth < kMaxLevel + 1; depth++) {
      if (n->level == 0) {
        if (n->n == 0) { c.st.clear(); return 1; }
        int lo = 0, hi = static_cast<int>(n->n);
        while (lo < hi) { int m = (lo + hi) / 2; if (kcmp(n->key(m), k) < 0) lo = m + 1; else hi = m; }
        if (le) {
          int s = (lo < static_cast<int>(n->n) && kcmp(n->key(lo), k) == 0) ? lo : lo - 1;
          if (s < 0) { c.st.clear(); return 1; }
          c.st.push_back({n, s});
          return 0;
        }
        c.st.push_back({n, lo});
        if (lo >= static_cast<int>(n->n)) {
          c.st.back().second = static_cast<int>(n->n) - 1;
          int r = next(c);
          if (r) { c.st.clear(); return r; }
        }
        return 0;
      }
      int lo = 0, hi = static_cast<int>(n->n);   // 最初に key より大きいもの
      while (lo < hi) { int m = (lo + hi) / 2; if (kcmp(n->key(m), k) <= 0) lo = m + 1; else hi = m; }
      int idx = lo - 1;
      if (idx < 0) { if (le) { c.st.clear(); return 1; } idx = 0; }
      c.st.push_back({n, idx});
      NodeP ch;
      rc = read_node(n->child(idx), n->level - 1, &ch);
      if (rc) { c.st.clear(); return rc; }
      n = ch;
    }
    c.st.clear();
    return -EIO;
  }

  // 次の項目へ。0 = 進んだ、1 = 末尾、負 = エラー
  int next(Cursor& c) {
    if (!c.valid()) return 1;
    auto& lf = c.st.back();
    if (lf.second + 1 < static_cast<int>(lf.first->n)) { lf.second++; return 0; }
    size_t i = c.st.size() - 1;
    while (i > 0) {
      i--;
      auto& a = c.st[i];
      if (a.second + 1 < static_cast<int>(a.first->n)) {
        a.second++;
        c.st.resize(i + 1);
        for (;;) {
          NodeP ch;
          int rc = read_node(c.st.back().first->child(c.st.back().second), c.st.back().first->level - 1, &ch);
          if (rc) return rc;
          c.st.push_back({ch, 0});
          if (ch->level == 0) return ch->n ? 0 : -EIO;
        }
      }
    }
    return 1;
  }

  // ---- ルート / サブボリューム
  int load_root_item(u64 id, Subvol* sv) {
    if (id == kRootTreeObj) { sv->id = id; sv->bytenr = root_bytenr; sv->level = root_level; return 0; }
    if (id == kChunkTreeObj) { sv->id = id; sv->bytenr = chunk_root; sv->level = chunk_level; return 0; }
    Cursor c;
    int rc = search(root_bytenr, root_level, Key{id, kRootItem, ~0ull}, true, c);
    if (rc < 0) return rc;
    if (rc > 0 || c.key().obj != id || c.key().type != kRootItem) return -ENOENT;
    u32 sz;
    const u8* p = c.item(&sz);
    if (sz < 239) return -EIO;
    sv->id = id;
    sv->flags = le64(p + 208);
    sv->bytenr = le64(p + 176);
    sv->level = p[238];
    if (sv->bytenr == 0 || sv->level >= kMaxLevel) return -EIO;
    return 0;
  }
  int slot_for(u64 id, u32* slot) {
    auto it = subvol_slot.find(id);
    if (it != subvol_slot.end()) { *slot = it->second; return 0; }
    if (subvols.size() >= 65535) return -EMFILE;
    Subvol sv;
    int rc = load_root_item(id, &sv);
    if (rc) return rc;
    subvols.push_back(sv);
    subvol_slot[id] = static_cast<u32>(subvols.size() - 1);
    *slot = static_cast<u32>(subvols.size() - 1);
    return 0;
  }
  static u64 make_ino(u32 slot, u64 obj) { return (static_cast<u64>(slot) << 48) | obj; }
  int split_ino(u64 ino, Subvol** sv, u64* obj) {
    u32 slot = static_cast<u32>(ino >> 48);
    if (slot >= subvols.size()) return -ENOENT;
    *sv = &subvols[slot];
    *obj = ino & ((1ull << 48) - 1);
    return 0;
  }

  // ---- 項目の読み取り
  int get_inode(const Subvol& sv, u64 obj, Inode* in) {
    Cursor c;
    int rc = search(sv.bytenr, sv.level, Key{obj, kInodeItem, 0}, true, c);
    if (rc < 0) return rc;
    if (rc > 0) return -ENOENT;
    Key k = c.key();
    if (k.obj != obj || k.type != kInodeItem) return -ENOENT;
    u32 sz;
    const u8* p = c.item(&sz);
    if (sz < 160) return -EIO;
    in->size = le64(p + 16);
    in->nbytes = le64(p + 24);
    in->nlink = le32(p + 40);
    in->mode = le32(p + 52);
    in->flags = le64(p + 64);
    in->mtime = static_cast<int64_t>(le64(p + 136));
    return 0;
  }

  struct DirEnt { Key loc; u8 type; std::string name; };
  // DIR_ITEM / DIR_INDEX 項目 (1 つの項目に複数入ることがある) を解釈
  static int parse_dir_item(const u8* p, u32 sz, std::vector<DirEnt>* out) {
    u32 cur = 0;
    while (cur + 30 <= sz) {
      u32 data_len = le16(p + cur + 25), name_len = le16(p + cur + 27);
      if (name_len == 0 || name_len > 255 || data_len > 65535) return -EIO;
      if (static_cast<u64>(cur) + 30 + name_len + data_len > sz) return -EIO;
      DirEnt e;
      e.loc = rdkey(p + cur);
      e.type = p[cur + 29];
      e.name.assign(reinterpret_cast<const char*>(p + cur + 30), name_len);
      out->push_back(std::move(e));
      cur += 30 + name_len + data_len;
    }
    return cur == sz ? 0 : -EIO;
  }

  // 子の ino (サブボリュームへの項目はその root ディレクトリ) を決める。ENOENT = 参照先が無い (読み飛ばす)
  int child_ino(u32 slot, const DirEnt& e, u64* ino, unsigned* dtype) {
    unsigned dt = e.type < 9 ? kDtByFt[e.type] : 0;
    if (e.loc.type == kInodeItem) {
      if (e.loc.obj >= (1ull << 48)) return -EOVERFLOW;
      *ino = make_ino(slot, e.loc.obj);
    } else if (e.loc.type == kRootItem) {
      u32 s2;
      int rc = slot_for(e.loc.obj, &s2);
      if (rc) return rc;
      *ino = make_ino(s2, kFirstFree);
      dt = 4;
    } else {
      return -ENOENT;
    }
    if (dtype) *dtype = dt;
    return 0;
  }

  // ---- データの読み取り
  u64 sec_mask() const { return static_cast<u64>(sectorsize) - 1; }

  // csum ツリーから [start, start + nsec * sectorsize) の各セクタのチェックサムを集める。have[i] = そのセクタに csum がある
  int fetch_csums(u64 start, size_t nsec, std::vector<u8>* csums, std::vector<u8>* have) {
    csums->assign(nsec * csum_len, 0);
    have->assign(nsec, 0);
    int rc = 0;
    if (!csum_loaded) {
      rc = load_root_item(kCsumTreeObj, &csum_root);
      if (rc && rc != -ENOENT) return rc;
      csum_present = (rc == 0);
      csum_loaded = true;
    }
    if (!csum_present) return 0;
    const Subvol& cs = csum_root;
    u64 end = start + static_cast<u64>(nsec) * sectorsize;
    Cursor c;
    rc = search(cs.bytenr, cs.level, Key{kExtentCsumObj, kExtentCsum, start}, true, c);
    if (rc < 0) return rc;
    if (rc > 0) { rc = search(cs.bytenr, cs.level, Key{kExtentCsumObj, kExtentCsum, start}, false, c); if (rc) return rc < 0 ? rc : 0; }
    for (int guard = 0; guard < 1 << 20; guard++) {
      Key k = c.key();
      if (k.obj == kExtentCsumObj && k.type == kExtentCsum) {
        if (k.off >= end) break;
        u32 sz;
        const u8* p = c.item(&sz);
        u64 cnt = sz / csum_len;
        for (u64 i = 0; i < cnt; i++) {
          u64 addr = k.off + i * sectorsize;
          if (addr < start) continue;
          if (addr >= end) break;
          size_t idx = static_cast<size_t>((addr - start) / sectorsize);
          std::memcpy(&(*csums)[idx * csum_len], p + i * csum_len, csum_len);
          (*have)[idx] = 1;
        }
      } else if (kcmp(k, Key{kExtentCsumObj, kExtentCsum, start}) > 0) {
        break;
      }
      rc = next(c);
      if (rc) return rc < 0 ? rc : 0;
    }
    return 0;
  }

  // 論理 [logical, logical+n) を読む (logical / n はセクタ境界)。verify なら csum を検査し、不一致なら別コピーを試す
  int read_logical_aligned(u64 logical, size_t n, u8* out, bool verify) {
    std::vector<u64> phys;
    int rc = map_logical(logical, n, &phys);
    if (rc) return rc;
    size_t nsec = n / sectorsize;
    std::vector<u8> csums, have;
    bool any = false;
    if (verify && opt.verify_data) {
      rc = fetch_csums(logical, nsec, &csums, &have);
      if (rc) return rc;
      for (u8 h : have) if (h) { any = true; break; }
    }
    for (u64 p : phys) {
      if (devread(p, out, n)) continue;
      if (!any) return 0;
      bool ok = true;
      u8 cs[32];
      for (size_t i = 0; i < nsec && ok; i++) {
        if (!have[i]) continue;
        calc_csum(csum_type, out + i * sectorsize, sectorsize, cs);
        if (std::memcmp(cs, &csums[i * csum_len], csum_len) != 0) ok = false;
      }
      if (ok) return 0;
    }
    return -EIO;
  }

  // 論理 [logical, logical+n) の任意の範囲を読む
  int read_logical(u64 logical, size_t n, u8* out, bool verify) {
    constexpr size_t kWin = 1u << 20;
    while (n) {
      u64 a = logical & ~sec_mask();
      size_t head = static_cast<size_t>(logical - a);
      size_t take = std::min<size_t>(n, kWin - head);
      u64 e = (logical + take + sec_mask()) & ~sec_mask();
      std::vector<u8> tmp(static_cast<size_t>(e - a));
      int rc = read_logical_aligned(a, tmp.size(), tmp.data(), verify);
      if (rc) return rc;
      std::memcpy(out, tmp.data() + head, take);
      out += take; logical += take; n -= take;
    }
    return 0;
  }

  int decompress(u8 comp, const u8* src, size_t srclen, size_t ram, std::vector<u8>* out) {
    if (ram == 0 || ram > kMaxRam) return -EIO;
    out->assign(ram, 0);
    size_t got = 0;
    int rc;
    if (comp == 1) {
      rc = usbmem_btrfs_zlib_inflate(src, srclen, out->data(), ram, &got);
      if (rc == -ENOSPC) rc = 0;   // 想定より長い分は使わない
    } else if (comp == 3) {
      rc = usbmem_btrfs_zstd_inflate(src, srclen, out->data(), ram, &got);
      if (rc == -ENOSPC) rc = 0;
    } else if (comp == 2) {
      // LZO: 先頭 4 バイトが全長 (自分を含む)。以降は セクタごとに [4 バイト長][LZO1X データ] を並べ、セクタ境界を跨がない
      // (セクタの残りが 4 バイト未満なら次のセクタ先頭へ詰める)。各セグメントは最大 sectorsize バイトに展開される
      if (srclen < 4) return -EIO;
      u64 tot = le32(src);
      if (tot > srclen || tot < 4) return -EIO;
      u64 in = 4;
      size_t outpos = 0;
      rc = 0;
      while (in < tot) {
        u64 left = sectorsize - (in % sectorsize);
        if (left < 4) { in += left; continue; }
        if (in + 4 > tot) { rc = -EIO; break; }
        u32 seg = le32(src + in);
        in += 4;
        if (seg == 0 || in + seg > tot) { rc = -EIO; break; }
        size_t dl = std::min<size_t>(sectorsize, ram - outpos);
        if (dl == 0) break;
        rc = usbmem_btrfs_lzo_segment(src + in, seg, out->data() + outpos, &dl);
        if (rc) break;
        outpos += dl;
        in += seg;
      }
      got = outpos;
    } else {
      return -EOPNOTSUPP;
    }
    (void)got;
    return rc ? -EIO : 0;
  }

  struct Ext2 { u8 type = 0, comp = 0; u64 ram = 0, disk = 0, disk_len = 0, off = 0, num = 0; const u8* inl = nullptr; u32 inl_len = 0; u64 start = 0; };

  int parse_extent(const Key& k, const u8* p, u32 sz, Ext2* e) {
    if (sz < 21) return -EIO;
    e->start = k.off;
    e->ram = le64(p + 8);
    e->comp = p[16];
    if (p[17] != 0 || le16(p + 18) != 0) return -EOPNOTSUPP;   // 暗号化 / other_encoding
    e->type = p[20];
    if (e->type == 0) {
      e->inl = p + 21;
      e->inl_len = sz - 21;
      e->num = e->ram;
      if (e->ram == 0 || e->ram > kMaxRam) return -EIO;
    } else if (e->type == 1 || e->type == 2) {
      if (sz < 53) return -EIO;
      e->disk = le64(p + 21);
      e->disk_len = le64(p + 29);
      e->off = le64(p + 37);
      e->num = le64(p + 45);
      if (e->num == 0 || e->start + e->num < e->start) return -EIO;
      if (e->disk_len > (1ull << 40) || e->off > (1ull << 40)) return -EIO;
    } else {
      return -EIO;
    }
    return 0;
  }

  // 圧縮された regular extent の展開結果を (キャッシュ経由で) 得る
  int get_decompressed(const Ext2& e, bool verify, std::shared_ptr<std::vector<u8>>* out) {
    u64 ck = e.disk ^ (static_cast<u64>(e.comp) << 56);
    for (auto& c : ecache) if (c.data && c.key == ck && c.data->size() == e.ram) { *out = c.data; return 0; }
    if (e.disk == 0 || e.disk_len == 0 || e.disk_len > kMaxRam * 2 || (e.disk & sec_mask()) || (e.disk_len & sec_mask())) return -EIO;
    std::vector<u8> raw(static_cast<size_t>(e.disk_len));
    int rc = read_logical_aligned(e.disk, raw.size(), raw.data(), verify);
    if (rc) return rc;
    auto d = std::make_shared<std::vector<u8>>();
    rc = decompress(e.comp, raw.data(), raw.size(), static_cast<size_t>(e.ram), d.get());
    if (rc) return rc;
    ecache[enext] = Ext{ck, d};
    enext = (enext + 1) % 4;
    *out = d;
    return 0;
  }

  int64_t read_file(const Subvol& sv, u64 obj, const Inode& in, u64 off, u8* buf, size_t len) {
    if (off >= in.size) return 0;
    u64 end = std::min<u64>(in.size, off + len);
    if (end < off) end = in.size;
    bool verify = (in.flags & 3) == 0;   // NODATASUM / NODATACOW は csum なし
    u64 pos = off;
    while (pos < end) {
      Cursor c;
      int rc = search(sv.bytenr, sv.level, Key{obj, kExtentData, pos}, true, c);
      if (rc < 0) return rc;
      bool have = false;
      Ext2 e;
      if (rc == 0) {
        Key k = c.key();
        if (k.obj == obj && k.type == kExtentData) {
          u32 sz;
          const u8* p = c.item(&sz);
          rc = parse_extent(k, p, sz, &e);
          if (rc) return rc;
          have = true;
          if (pos >= e.start + e.num) {
            have = false;   // この extent より後ろ: 次の extent までは穴
            if (e.type == 0) e.inl = nullptr;
          }
        }
      }
      if (have) {
        u64 n = std::min<u64>(end, e.start + e.num) - pos;
        u64 rel = pos - e.start;
        u8* dst = buf + (pos - off);
        if (e.type == 0) {
          if (e.comp == 0) {
            std::memset(dst, 0, static_cast<size_t>(n));
            if (rel < e.inl_len) std::memcpy(dst, e.inl + rel, static_cast<size_t>(std::min<u64>(n, e.inl_len - rel)));
          } else {
            std::vector<u8> d;
            rc = decompress(e.comp, e.inl, e.inl_len, static_cast<size_t>(e.ram), &d);
            if (rc) return rc;
            std::memcpy(dst, d.data() + rel, static_cast<size_t>(n));
          }
        } else if (e.type == 2 || e.disk == 0) {
          std::memset(dst, 0, static_cast<size_t>(n));   // prealloc / 穴
        } else if (e.comp == 0) {
          if (e.disk + e.off < e.disk) return -EIO;
          rc = read_logical(e.disk + e.off + rel, static_cast<size_t>(n), dst, verify);
          if (rc) return rc;
        } else {
          std::shared_ptr<std::vector<u8>> d;
          rc = get_decompressed(e, verify, &d);
          if (rc) return rc;
          u64 o = e.off + rel;
          if (o > d->size()) return -EIO;
          u64 avail = d->size() - o;
          std::memset(dst, 0, static_cast<size_t>(n));
          std::memcpy(dst, d->data() + o, static_cast<size_t>(std::min<u64>(n, avail)));
        }
        pos += n;
        continue;
      }
      // 穴: 次の extent の開始 (または end) まで 0
      u64 hole_end = end;
      Cursor c2;
      rc = search(sv.bytenr, sv.level, Key{obj, kExtentData, pos}, false, c2);
      if (rc < 0) return rc;
      if (rc == 0) {
        Key k = c2.key();
        if (k.obj == obj && k.type == kExtentData && k.off < hole_end && k.off > pos) hole_end = k.off;
        else if (k.obj == obj && k.type == kExtentData && k.off <= pos) {
          // 到達しないはず (le 検索が拾う)。壊れた木での無限ループ防止
          return -EIO;
        }
      }
      std::memset(buf + (pos - off), 0, static_cast<size_t>(hole_end - pos));
      pos = hole_end;
    }
    return static_cast<int64_t>(end - off);
  }

  // ---- マウント
  int read_super() {
    int best = -1;
    std::array<u8, kSbSize> cand{};
    u64 best_gen = 0;
    for (int i = 0; i < 3; i++) {
      sb_inrange[i] = false; sb_valid_seen[i] = false; sb_gen_seen[i] = 0;
      if (!range_ok(dev->size(), kSbOffsets[i], kSbSize)) continue;
      sb_inrange[i] = true;
      if (devread(kSbOffsets[i], cand.data(), kSbSize)) continue;
      if (std::memcmp(&cand[0x40], "_BHRfS_M", 8) != 0) continue;
      if (le64(&cand[0x30]) != kSbOffsets[i]) continue;
      u16 ct = le16(&cand[196]);
      size_t cl = csum_size_of(ct);
      if (cl == 0) continue;
      u8 cs[32];
      calc_csum(ct, &cand[32], kSbSize - 32, cs);
      if (std::memcmp(cs, cand.data(), cl) != 0) continue;
      u64 gen = le64(&cand[0x48]);
      sb_valid_seen[i] = true; sb_gen_seen[i] = gen;
      if (best < 0 || gen > best_gen) { best = i; best_gen = gen; sb = cand; }
    }
    return best < 0 ? -EINVAL : 0;
  }

  int add_chunk(u64 start, const u8* p, size_t avail) {
    if (avail < 48) return -EIO;
    u16 ns = le16(p + 44);
    if (ns == 0 || ns > 64 || avail < 48 + static_cast<size_t>(ns) * 32) return -EIO;
    Chunk c;
    c.start = start;
    c.len = le64(p);
    c.type = le64(p + 24);
    if (c.len == 0 || start + c.len < start) return -EIO;
    if (c.type & (kBgRaid0 | kBgRaid10 | kBgRaid5 | kBgRaid6)) return -EOPNOTSUPP;
    for (u16 i = 0; i < ns; i++) c.stripes.push_back(Stripe{le64(p + 48 + i * 32), le64(p + 56 + i * 32)});
    // 自分のデバイス以外にしかコピーが無い (RAID1 系で別デバイスのみ) chunk は読めない
    bool mine = false;
    for (auto& s : c.stripes) if (s.devid == devid) mine = true;
    if (!mine) return -EOPNOTSUPP;
    (void)kBgRaid1; (void)kBgDup; (void)kBgRaid1c3; (void)kBgRaid1c4;
    chunks[start] = std::move(c);
    return 0;
  }

  int mount(const BtrfsMountOptions& o) {
    opt = o;
    int rc = read_super();
    if (rc) return rc;
    csum_type = le16(&sb[196]);
    csum_len = csum_size_of(csum_type);
    sectorsize = le32(&sb[0x90]);
    nodesize = le32(&sb[0x94]);
    incompat = le64(&sb[0xbc]);
    root_bytenr = le64(&sb[0x50]);
    chunk_root = le64(&sb[0x58]);
    generation = le64(&sb[0x48]);
    root_level = sb[198];
    chunk_level = sb[199];
    devid = le64(&sb[201]);
    u64 num_devices = le64(&sb[0x88]);
    auto pow2 = [](u64 v) { return v && !(v & (v - 1)); };
    if (!pow2(sectorsize) || sectorsize < 512 || sectorsize > 65536) return -EIO;
    if (!pow2(nodesize) || nodesize < sectorsize || nodesize > kMaxNode || nodesize < 4096) return -EIO;
    if (root_level >= kMaxLevel || chunk_level >= kMaxLevel || root_bytenr == 0 || chunk_root == 0) return -EIO;
    if (incompat & ~kIncompatKnown) return -EOPNOTSUPP;
    if (incompat & ((1ull << 7))) return -EOPNOTSUPP;   // RAID56
    if (num_devices != 1) return -EOPNOTSUPP;           // 複数デバイスは未対応
    if (le64(&sb[0x38]) & (1ull << 32)) { /* SEEDING: 読み取り専用のシードデバイス。単独で読める */ }
    if (incompat & (1ull << 10)) std::memcpy(meta_fsid.data(), &sb[571], 16); else std::memcpy(meta_fsid.data(), &sb[0x20], 16);
    {
      const char* l = reinterpret_cast<const char*>(&sb[299]);
      size_t n = 0;
      while (n < 256 && l[n]) n++;
      label_.assign(l, n);
    }
    ncache_max = std::max<size_t>(16, (8u << 20) / nodesize);

    // sys_chunk_array
    u32 sys = le32(&sb[0xa0]);
    if (sys > 2048) return -EIO;
    const u8* a = &sb[811];
    u32 cur = 0;
    while (cur < sys) {
      if (sys - cur < 17 + 48) return -EIO;
      Key k = rdkey(a + cur);
      if (k.type != kChunkItem) return -EIO;
      u16 ns = le16(a + cur + 17 + 44);
      size_t need = 17 + 48 + static_cast<size_t>(ns) * 32;
      if (ns == 0 || need > sys - cur) return -EIO;
      rc = add_chunk(k.off, a + cur + 17, need - 17);
      if (rc) return rc;
      cur += static_cast<u32>(need);
    }
    // chunk tree
    {
      Cursor c;
      rc = search(chunk_root, chunk_level, Key{kFirstChunkObj, kChunkItem, 0}, false, c);
      if (rc < 0) return rc;
      while (rc == 0) {
        Key k = c.key();
        if (k.obj != kFirstChunkObj || k.type != kChunkItem) break;
        u32 sz;
        const u8* p = c.item(&sz);
        rc = add_chunk(k.off, p, sz);
        if (rc) return rc;
        rc = next(c);
        if (rc < 0) return rc;
      }
    }
    if (chunks.empty()) return -EIO;
    // tree-log
    if (le64(&sb[0x60]) != 0) {
      if (!opt.allow_noload) return -EUCLEAN;
      log_ignored = true;
    }
    // ルート
    u64 want = opt.subvol_id;
    if (want == 0) {
      want = kFsTreeObj;
      std::string nm = "default";
      Cursor c;
      u32 h = name_hash(reinterpret_cast<const u8*>(nm.data()), nm.size());
      rc = search(root_bytenr, root_level, Key{kRootTreeDirObj, kDirItem, h}, true, c);
      if (rc < 0) return rc;
      if (rc == 0) {
        Key k = c.key();
        if (k.obj == kRootTreeDirObj && k.type == kDirItem && k.off == h) {
          u32 sz;
          const u8* p = c.item(&sz);
          std::vector<DirEnt> es;
          if (parse_dir_item(p, sz, &es) == 0) for (auto& e : es) if (e.name == nm && e.loc.type == kRootItem) want = e.loc.obj;
        }
      }
    }
    u32 slot;
    rc = slot_for(want, &slot);
    if (rc) return rc == -ENOENT ? -EIO : rc;
    mount_subvol = want;
    // ルートディレクトリの inode が読めること
    Inode ri;
    rc = get_inode(subvols[slot], kFirstFree, &ri);
    if (rc) return rc == -ENOENT ? -EIO : rc;
    if ((ri.mode & 0170000) != 0040000) return -EIO;
    return 0;
  }

#ifdef USBMEM_NTFS_WRITE
#include "btrfs_write.inc"
#include "btrfs_write_ops.inc"
#endif
};

// ---- 公開 API

std::unique_ptr<BtrfsVolume> BtrfsVolume::mount(BlockDevicePtr dev, int* err, const BtrfsMountOptions& opts) {
  std::unique_ptr<BtrfsVolume> v(new BtrfsVolume());
  try {
    v->impl_.reset(new Impl());
    v->impl_->dev = std::move(dev);
    int rc = v->impl_->mount(opts);
    if (rc) {
      if (err) *err = -rc;
      return nullptr;
    }
  } catch (...) {
    if (err) *err = ENOMEM;
    return nullptr;
  }
  return v;
}
BtrfsVolume::~BtrfsVolume() {
#ifdef USBMEM_NTFS_WRITE
  // 未コミットの変更があればコミットする (失敗しても例外は出さない。確定済みの状態は壊れない)
  try {
    if (impl_) {
      std::lock_guard<std::mutex> lk(impl_->mu);
      if (impl_->writable_ && impl_->txn_open) impl_->commit_txn();
    }
  } catch (...) {
  }
#endif
}

uint64_t BtrfsVolume::root() const { return Impl::make_ino(0, kFirstFree); }
uint64_t BtrfsVolume::mounted_subvolume() const { return impl_->mount_subvol; }
bool BtrfsVolume::log_ignored() const { return impl_->log_ignored; }
std::string BtrfsVolume::label() const { return impl_->label_; }

#define BTRFS_GUARD(...) \
  try { std::lock_guard<std::mutex> lk(impl_->mu); __VA_ARGS__ } catch (...) { return -EIO; }

int BtrfsVolume::lookup(uint64_t dir, const std::string& name, uint64_t* ino) {
  BTRFS_GUARD(
    Impl& m = *impl_;
    Subvol* sv; u64 obj;
    int rc = m.split_ino(dir, &sv, &obj);
    if (rc) return rc;
    if (name == ".") { *ino = dir; return 0; }
    if (name == "..") {
      if (obj == kFirstFree) { *ino = dir; return 0; }
      Cursor c;
      rc = m.search(sv->bytenr, sv->level, Key{obj, kInodeRef, 0}, false, c);
      if (rc < 0) return rc;
      if (rc > 0 || c.key().obj != obj || c.key().type != kInodeRef) return -ENOENT;
      *ino = Impl::make_ino(static_cast<u32>(dir >> 48), c.key().off);
      return 0;
    }
    if (name.empty() || name.size() > 255) return -ENOENT;
    u32 h = name_hash(reinterpret_cast<const u8*>(name.data()), name.size());
    Cursor c;
    rc = m.search(sv->bytenr, sv->level, Key{obj, kDirItem, h}, true, c);
    if (rc < 0) return rc;
    if (rc > 0) return -ENOENT;
    Key k = c.key();
    if (k.obj != obj || k.type != kDirItem || k.off != h) return -ENOENT;
    u32 sz;
    const u8* p = c.item(&sz);
    std::vector<Impl::DirEnt> es;
    rc = Impl::parse_dir_item(p, sz, &es);
    if (rc) return rc;
    for (auto& e : es) {
      if (e.name != name) continue;
      rc = m.child_ino(static_cast<u32>(dir >> 48), e, ino, nullptr);
      return rc;
    }
    return -ENOENT;
  )
}

int BtrfsVolume::stat(uint64_t ino, FsStat* st) {
  BTRFS_GUARD(
    Impl& m = *impl_;
    Subvol* sv; u64 obj;
    int rc = m.split_ino(ino, &sv, &obj);
    if (rc) return rc;
    Inode in;
    rc = m.get_inode(*sv, obj, &in);
    if (rc) return rc;
    st->ino = ino;
    st->size = in.size;
    st->mode = in.mode;
    st->nlink = in.nlink;
    st->mtime = in.mtime;
    return 0;
  )
}

int BtrfsVolume::readdir(uint64_t dir, std::vector<FsDirEntry>* out) {
  BTRFS_GUARD(
    Impl& m = *impl_;
    out->clear();
    Subvol* sv; u64 obj;
    int rc = m.split_ino(dir, &sv, &obj);
    if (rc) return rc;
    Inode in;
    rc = m.get_inode(*sv, obj, &in);
    if (rc) return rc;
    if ((in.mode & 0170000) != 0040000) return -ENOTDIR;
    Cursor c;
    rc = m.search(sv->bytenr, sv->level, Key{obj, kDirIndex, 0}, false, c);
    if (rc < 0) return rc;
    while (rc == 0) {
      Key k = c.key();
      if (k.obj != obj || k.type != kDirIndex) break;
      u32 sz;
      const u8* p = c.item(&sz);
      std::vector<Impl::DirEnt> es;
      int r2 = Impl::parse_dir_item(p, sz, &es);
      if (r2) return r2;
      for (auto& e : es) {
        u64 ci; unsigned dt;
        int r3 = m.child_ino(static_cast<u32>(dir >> 48), e, &ci, &dt);
        if (r3 == -ENOENT) continue;
        if (r3) return r3;
        out->push_back(FsDirEntry{e.name, ci, dt});
      }
      rc = m.next(c);
      if (rc < 0) return rc;
    }
    return 0;
  )
}

int64_t BtrfsVolume::read(uint64_t ino, uint64_t off, void* buf, size_t len) {
  BTRFS_GUARD(
    Impl& m = *impl_;
    Subvol* sv; u64 obj;
    int rc = m.split_ino(ino, &sv, &obj);
    if (rc) return rc;
    Inode in;
    rc = m.get_inode(*sv, obj, &in);
    if (rc) return rc;
    u32 fmt = in.mode & 0170000;
    if (fmt == 0040000) return -EISDIR;
    if (fmt != 0100000 && fmt != 0120000) return -EINVAL;   // シンボリックリンクはリンク先の文字列 (他の FS と同じ)
    return m.read_file(*sv, obj, in, off, static_cast<u8*>(buf), len);
  )
}

int BtrfsVolume::readlink(uint64_t ino, std::string* target) {
  BTRFS_GUARD(
    Impl& m = *impl_;
    Subvol* sv; u64 obj;
    int rc = m.split_ino(ino, &sv, &obj);
    if (rc) return rc;
    Inode in;
    rc = m.get_inode(*sv, obj, &in);
    if (rc) return rc;
    if ((in.mode & 0170000) != 0120000) return -EINVAL;
    if (in.size == 0 || in.size > 4095) return -EIO;
    std::string s(static_cast<size_t>(in.size), '\0');
    int64_t n = m.read_file(*sv, obj, in, 0, reinterpret_cast<u8*>(&s[0]), s.size());
    if (n < 0) return static_cast<int>(n);
    if (static_cast<u64>(n) != in.size) return -EIO;
    *target = std::move(s);
    return 0;
  )
}

int BtrfsVolume::list_subvolumes(std::vector<BtrfsSubvolume>* out) {
  BTRFS_GUARD(
    Impl& m = *impl_;
    out->clear();
    auto add = [&](u64 id, u64 parent, u64 dirid, const std::string& name) {
      BtrfsSubvolume s;
      s.id = id; s.parent_id = parent; s.dirid = dirid; s.name = name;
      Subvol sv;
      if (m.load_root_item(id, &sv) == 0) s.read_only = (sv.flags & 1) != 0;
      out->push_back(std::move(s));
    };
    add(kFsTreeObj, 0, 0, "");
    Cursor c;
    int rc = m.search(m.root_bytenr, m.root_level, Key{0, 0, 0}, false, c);
    if (rc < 0) return rc;
    while (rc == 0 && out->size() < 65535) {
      Key k = c.key();
      if (k.type == kRootRef) {
        u32 sz;
        const u8* p = c.item(&sz);
        if (sz >= 18) {
          u16 nl = le16(p + 16);
          if (18u + nl <= sz) add(k.off, k.obj, le64(p), std::string(reinterpret_cast<const char*>(p + 18), nl));
        }
      }
      rc = m.next(c);
      if (rc < 0) return rc;
    }
    return 0;
  )
}

// ---- 書き込み系の公開 API (USBMEM_NTFS_WRITE のビルドだけ実体。そうでなければ全て EROFS)
#ifdef USBMEM_NTFS_WRITE
std::unique_ptr<BtrfsVolume> BtrfsVolume::mount_rw(BlockDevicePtr dev, int* err) {
  auto v = mount(std::move(dev), err, BtrfsMountOptions{});
  if (!v) return nullptr;
  try {
    std::lock_guard<std::mutex> lk(v->impl_->mu);
    std::string why = v->impl_->rw_load();
    if (why.empty()) v->impl_->writable_ = true;
    else v->impl_->ro_reason = why;
  } catch (...) {
    v->impl_->ro_reason = "書き込み可の検査中に例外";
  }
  return v;
}
bool BtrfsVolume::writable() const { return impl_->writable_; }
std::string BtrfsVolume::read_only_reason() const { return impl_->ro_reason; }
int BtrfsVolume::sync() { BTRFS_GUARD(return impl_->do_sync();) }
int BtrfsVolume::create(uint64_t dir, const std::string& name, uint32_t mode, uint64_t* ino) {
  BTRFS_GUARD(Impl& m = *impl_; return m.run_op([&] { return m.op_create(dir, name, mode, ino); });)
}
int BtrfsVolume::mkdir(uint64_t dir, const std::string& name, uint32_t mode, uint64_t* ino) {
  BTRFS_GUARD(Impl& m = *impl_; return m.run_op([&] { return m.op_mkdir(dir, name, mode, ino); });)
}
int BtrfsVolume::unlink(uint64_t dir, const std::string& name) {
  BTRFS_GUARD(Impl& m = *impl_; return m.run_op([&] { return m.op_unlink(dir, name); }, 0, true);)
}
int BtrfsVolume::rmdir(uint64_t dir, const std::string& name) {
  BTRFS_GUARD(Impl& m = *impl_; return m.run_op([&] { return m.op_rmdir(dir, name); }, 0, true);)
}
int BtrfsVolume::rename(uint64_t od, const std::string& on, uint64_t nd, const std::string& nn) {
  BTRFS_GUARD(Impl& m = *impl_; return m.run_op([&] { return m.op_rename(od, on, nd, nn); });)
}
int BtrfsVolume::symlink(uint64_t dir, const std::string& name, const std::string& target, uint64_t* ino) {
  BTRFS_GUARD(Impl& m = *impl_; return m.run_op([&] { return m.op_symlink(dir, name, target, ino); });)
}
int BtrfsVolume::link(uint64_t dir, const std::string& name, uint64_t target_ino) {
  BTRFS_GUARD(Impl& m = *impl_; return m.run_op([&] { return m.op_link(dir, name, target_ino); });)
}
int BtrfsVolume::truncate(uint64_t ino, uint64_t size) {
  BTRFS_GUARD(
    Impl& m = *impl_;
    if (ino >> 48) return m.writable_ ? -EOPNOTSUPP : -EROFS;
    if (size > (1ull << 62)) return m.writable_ ? -EFBIG : -EROFS;
    return m.run_op([&] { return m.truncate_file(ino, size); }, size > 0 ? 2 * static_cast<u64>(m.sectorsize) : 0, size == 0);
  )
}
int64_t BtrfsVolume::write(uint64_t ino, uint64_t off, const void* buf, size_t len) {
  BTRFS_GUARD(return impl_->write_all(ino, off, buf, len);)
}
int64_t BtrfsVolume::write_inplace(uint64_t ino, uint64_t off, const void* buf, size_t len) {
  BTRFS_GUARD(
    Impl& m = *impl_;
    if (!m.writable_) return m.aborted ? -EIO : -EROFS;
    if (ino >> 48) return -EOPNOTSUPP;
    std::vector<u8> iv;
    int rc = m.inode_get(ino, &iv);
    if (rc) return rc;
    u32 fmt = Impl::i_mode(iv) & 0170000;
    if (fmt == 0040000) return -EISDIR;
    if (fmt != 0100000) return -EINVAL;
    if (off + len < off || off + len > Impl::i_size(iv)) return -EINVAL;
    return m.write_all(ino, off, buf, len);
  )
}
#else
std::unique_ptr<BtrfsVolume> BtrfsVolume::mount_rw(BlockDevicePtr, int* err) {
  if (err) *err = EROFS;
  return nullptr;
}
bool BtrfsVolume::writable() const { return false; }
std::string BtrfsVolume::read_only_reason() const { return "このビルドは書き込み非対応 (USBMEM_NTFS_WRITE=OFF)"; }
int BtrfsVolume::sync() { return -EROFS; }
int BtrfsVolume::create(uint64_t, const std::string&, uint32_t, uint64_t*) { return -EROFS; }
int BtrfsVolume::mkdir(uint64_t, const std::string&, uint32_t, uint64_t*) { return -EROFS; }
int BtrfsVolume::unlink(uint64_t, const std::string&) { return -EROFS; }
int BtrfsVolume::rmdir(uint64_t, const std::string&) { return -EROFS; }
int BtrfsVolume::rename(uint64_t, const std::string&, uint64_t, const std::string&) { return -EROFS; }
int BtrfsVolume::symlink(uint64_t, const std::string&, const std::string&, uint64_t*) { return -EROFS; }
int BtrfsVolume::link(uint64_t, const std::string&, uint64_t) { return -EROFS; }
int BtrfsVolume::truncate(uint64_t, uint64_t) { return -EROFS; }
int64_t BtrfsVolume::write(uint64_t, uint64_t, const void*, size_t) { return -EROFS; }
int64_t BtrfsVolume::write_inplace(uint64_t, uint64_t, const void*, size_t) { return -EROFS; }
#endif

}  // namespace usbmem
