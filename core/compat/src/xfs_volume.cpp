// SPDX-License-Identifier: GPL-2.0-or-later
// XFS の読み取り専用リーダ。ディスク形式 (スーパーブロック / AG / inode / extent / bmbt / ディレクトリ各形式 / シンボリックリンク / ログ) を
// Linux の fs/xfs の形式定義 (xfs_format.h, xfs_da_format.h, xfs_log_format.h, xfs_log_recover.c) を仕様として読み、独立に実装したもの。
// ログの再生は持たない。書き込み (USBMEM_NTFS_WRITE のビルドだけ) は xfs_write*.inc: ログ (カーネルが再生できる形式) 経由のコミット。docs/xfs-port.md / docs/xfs-write.md
#include "usbmem/xfs_volume.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <ctime>
#include <vector>

namespace usbmem {
namespace {

// ---- バイト列の読み出し (XFS はビッグエンディアン。CRC だけリトルエンディアン) ----
inline uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
inline uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
inline uint64_t be64(const uint8_t* p) { return (uint64_t(be32(p)) << 32) | be32(p + 4); }
inline uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

// ---- CRC-32C (Castagnoli)。XFS のメタデータ CRC は、CRC フィールドを 0 にして計算した標準の CRC-32C をリトルエンディアンで保存したもの ----
struct CrcTable {
  uint32_t t[8][256];
  CrcTable() {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : (c >> 1);
      t[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; i++)
      for (int s = 1; s < 8; s++) t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFF];
  }
};
const CrcTable& crc_table() { static const CrcTable t; return t; }
uint32_t crc_update(uint32_t c, const uint8_t* p, size_t n) {
  const CrcTable& T = crc_table();
  while (n >= 8) {
    uint32_t a = c ^ le32(p), b = le32(p + 4);
    c = T.t[7][a & 0xFF] ^ T.t[6][(a >> 8) & 0xFF] ^ T.t[5][(a >> 16) & 0xFF] ^ T.t[4][a >> 24] ^
        T.t[3][b & 0xFF] ^ T.t[2][(b >> 8) & 0xFF] ^ T.t[1][(b >> 16) & 0xFF] ^ T.t[0][b >> 24];
    p += 8; n -= 8;
  }
  while (n--) c = (c >> 8) ^ T.t[0][(c ^ *p++) & 0xFF];
  return c;
}
// buf[0..len) の、cksum_off から 4 バイトを 0 として計算した CRC が、そこに保存された値と一致するか
bool cksum_ok(const uint8_t* buf, size_t len, size_t cksum_off) {
  if (len < cksum_off + 4) return false;
  static const uint8_t zero[4] = {0, 0, 0, 0};
  uint32_t c = 0xFFFFFFFFu;
  c = crc_update(c, buf, cksum_off);
  c = crc_update(c, zero, 4);
  c = crc_update(c, buf + cksum_off + 4, len - cksum_off - 4);
  return ~c == le32(buf + cksum_off);
}

// ---- 定数 (ディスク形式) ----
constexpr uint32_t kSbMagic = 0x58465342;      // 'XFSB'
constexpr uint16_t kInodeMagic = 0x494e;       // 'IN'
constexpr uint32_t kBmapMagic = 0x424d4150;    // 'BMAP'
constexpr uint32_t kBmapCrcMagic = 0x424d4133; // 'BMA3'
constexpr uint32_t kDir2Block = 0x58443242, kDir2Data = 0x58443244, kDir3Block = 0x58444233, kDir3Data = 0x58444433;
constexpr uint32_t kSymlinkMagic = 0x58534c4d; // 'XSLM'
constexpr uint32_t kLogMagic = 0xFEEDbabeu;
constexpr uint8_t kFmtDev = 0, kFmtLocal = 1, kFmtExtents = 2, kFmtBtree = 3;
constexpr uint64_t kNullFsb = ~0ULL;
constexpr uint32_t kIncompatFtype = 1u << 0, kIncompatSpinodes = 1u << 1, kIncompatMetaUuid = 1u << 2, kIncompatBigtime = 1u << 3,
                   kIncompatNeedsRepair = 1u << 4, kIncompatNrext64 = 1u << 5, kIncompatExchrange = 1u << 6, kIncompatParent = 1u << 7,
                   kIncompatMetadir = 1u << 8;
constexpr uint32_t kIncompatKnown = kIncompatFtype | kIncompatSpinodes | kIncompatMetaUuid | kIncompatBigtime | kIncompatNeedsRepair |
                                    kIncompatNrext64 | kIncompatExchrange | kIncompatParent | kIncompatMetadir;
constexpr size_t kInodeCoreV2 = 100, kInodeCoreV3 = 176;
constexpr size_t kInodeCrcOff = 100;
constexpr size_t kBtreeLHdrV4 = 24, kBtreeLHdrV5 = 72, kBtreeLCrcOff = 64;
constexpr size_t kDirDataHdrV4 = 16, kDirDataHdrV5 = 64, kDirCrcOff = 4;
constexpr size_t kSymlinkHdr = 56, kSymlinkCrcOff = 12;
constexpr uint64_t kDirLeafOffsetBytes = 1ULL << 35;   // XFS_DIR2_LEAF_OFFSET: データ空間はこれより手前 (32 GiB)
constexpr size_t kMaxExtents = 16u << 20;              // 1 ファイルの extent 数の上限 (メモリ保護)
constexpr size_t kMaxSymlink = 1024;
constexpr unsigned kMaxBtreeLevel = 12;

constexpr uint32_t S_FMT = 0170000, S_DIR = 0040000, S_REG = 0100000, S_LNK = 0120000, S_CHR = 0020000, S_BLK = 0060000, S_FIFO = 0010000, S_SOCK = 0140000;

unsigned ftype_to_dtype(unsigned ft) {
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
unsigned mode_to_dtype(uint32_t mode) {
  switch (mode & S_FMT) {
    case S_DIR: return 4; case S_REG: return 8; case S_LNK: return 10; case S_CHR: return 2; case S_BLK: return 6; case S_FIFO: return 1; case S_SOCK: return 12;
  }
  return 0;
}

struct Ext { uint64_t off, fsb, len; bool unwritten; };

struct Inode {
  uint64_t ino = 0;
  uint32_t mode = 0, nlink = 0;
  uint8_t version = 0, format = 0;
  int64_t mtime = 0;
  uint64_t size = 0, nblocks = 0, nextents = 0, flags2 = 0;
  uint16_t flags = 0;
  std::vector<uint8_t> dfork;   // データフォークの生バイト
  std::shared_ptr<std::vector<Ext>> exts;   // 遅延読み込み (extent / bmbt)
};

}  // namespace

struct XfsVolume::Impl {
  BlockDevicePtr dev;
  std::mutex mu;
  uint64_t dev_size = 0;

  // スーパーブロック
  bool v5 = false, has_ftype = false;
  uint32_t bs = 0, agblocks = 0, agcount = 0, inodesize = 0, inopblock = 0, sectsize = 0, logblocks = 0;
  uint8_t bslog = 0, agblklog = 0, inopblog = 0, dirblklog = 0;
  uint64_t dblocks = 0, rootino = 0, logstart = 0;
  uint32_t dirblksize = 0, incompat = 0, ro_compat = 0, versionnum = 0;
  bool journal_ignored = false;
#ifndef USBMEM_NTFS_WRITE
  bool lg_ok = false; uint64_t lg_head = 0, lg_rhead = 0, lg_prev = 0; uint32_t lg_cycle = 0;   // log_state が埋める (書き込みビルドだけが使う)
#endif

  // メタデータのページキャッシュ
  static constexpr size_t kPage = 16384, kMaxPages = 128;
  struct Page { std::vector<uint8_t> d; uint64_t stamp = 0; };
  std::map<uint64_t, Page> pages;
  uint64_t tick = 0;

  // ディレクトリの名前 → inode 表 (lookup の高速化。直近の数ディレクトリだけ。巨大なディレクトリは作らず毎回走査する)
  struct DirMap { uint64_t ino; std::unordered_map<std::string, uint64_t> m; };
  std::vector<std::shared_ptr<DirMap>> dirmaps;
  static constexpr size_t kMaxDirMaps = 4, kMaxDirMapEntries = 1u << 19;

  // inode キャッシュ
  std::unordered_map<uint64_t, std::shared_ptr<Inode>> icache;
  size_t cached_exts = 0;

  // ---- デバイス I/O ----
  int dev_read(uint64_t off, void* buf, size_t len) {
    if (len == 0) return 0;
    if (!range_ok(dev_size, off, len)) return -EIO;
    Status st = dev->read_at(off, buf, len);
    return st.ok() ? 0 : -EIO;
  }
  // メタデータ読み出し (16 KiB ページ単位でキャッシュ)
  int read_meta(uint64_t off, void* out, size_t len) {
    uint8_t* o = static_cast<uint8_t*>(out);
    if (!range_ok(dev_size, off, len)) return -EIO;
    const uint64_t off0 = off; const size_t len0 = len; (void)off0; (void)len0;
    while (len) {
      uint64_t base = off & ~uint64_t(kPage - 1);
      auto it = pages.find(base);
      if (it == pages.end()) {
        if (pages.size() >= kMaxPages) {   // 最も古いものを捨てる
          auto victim = pages.begin();
          for (auto j = pages.begin(); j != pages.end(); ++j) if (j->second.stamp < victim->second.stamp) victim = j;
          pages.erase(victim);
        }
        size_t n = static_cast<size_t>(std::min<uint64_t>(kPage, dev_size - base));
        Page pg; pg.d.resize(n);
        int rc = dev_read(base, pg.d.data(), n);
        if (rc) return rc;
        it = pages.emplace(base, std::move(pg)).first;
      }
      it->second.stamp = ++tick;
      size_t in = static_cast<size_t>(off - base);
      if (in >= it->second.d.size()) return -EIO;
      size_t n = std::min(len, it->second.d.size() - in);
      std::memcpy(o, it->second.d.data() + in, n);
      o += n; off += n; len -= n;
    }
#ifdef USBMEM_NTFS_WRITE
    overlay(off0, static_cast<uint8_t*>(out), len0);   // まだコミットしていない書き込みを重ねる
#endif
    return 0;
  }

  // ---- アドレス変換 ----
  // FSB (上位 AG 番号 + 下位 AG 内ブロック) → ファイルシステム先頭からの物理ブロック番号。範囲外なら false
  bool fsb_to_blk(uint64_t fsb, uint64_t* blk) const {
    uint64_t agno = fsb >> agblklog;
    uint64_t agbno = fsb & ((1ULL << agblklog) - 1);
    if (agno >= agcount || agbno >= agblocks) return false;
    uint64_t b = agno * agblocks + agbno;
    if (b >= dblocks) return false;
    *blk = b;
    return true;
  }
  // [fsb, fsb+len) が 1 つの AG 内に収まり、データ領域の中か
  bool extent_ok(uint64_t fsb, uint64_t len) const {
    uint64_t blk;
    if (len == 0 || !fsb_to_blk(fsb, &blk)) return false;
    uint64_t agbno = fsb & ((1ULL << agblklog) - 1);
    if (len > agblocks - agbno) return false;
    return blk + len <= dblocks;
  }

  // ---- inode ----
  int get_inode(uint64_t ino, std::shared_ptr<Inode>* out) {
    auto it = icache.find(ino);
    if (it != icache.end()) { *out = it->second; return 0; }
    unsigned shift = agblklog + inopblog;
    if (shift >= 64 || (ino >> shift) >= agcount) return -ENOENT;
    uint64_t agbno = (ino >> inopblog) & ((1ULL << agblklog) - 1);
    uint32_t slot = static_cast<uint32_t>(ino & ((1ULL << inopblog) - 1));
    if (agbno >= agblocks || slot >= inopblock) return -ENOENT;
    uint64_t blk = (ino >> shift) * agblocks + agbno;
    if (blk >= dblocks) return -ENOENT;
    std::vector<uint8_t> d(inodesize);
    int rc = read_meta(blk * bs + uint64_t(slot) * inodesize, d.data(), inodesize);
    if (rc) return rc;
    if (be16(d.data()) != kInodeMagic) return -EUCLEAN;
    auto in = std::make_shared<Inode>();
    in->ino = ino;
    in->version = d[4];
    if (in->version != 2 && in->version != 3) return -EUCLEAN;   // v1 inode (旧形式) は未対応
    if (v5 && in->version != 3) return -EUCLEAN;
    size_t core = kInodeCoreV2;
    if (in->version == 3) {
      core = kInodeCoreV3;
      if (inodesize < core) return -EUCLEAN;
      if (!cksum_ok(d.data(), inodesize, kInodeCrcOff)) return -EBADMSG;
      if (be64(d.data() + 152) != ino) return -EUCLEAN;
      in->flags2 = be64(d.data() + 120);
    }
    in->mode = be16(d.data() + 2);
    if (in->mode == 0) return -ENOENT;   // 空き inode
    in->format = d[5];
    in->nlink = be32(d.data() + 16);
    in->size = be64(d.data() + 56);
    in->nblocks = be64(d.data() + 64);
    in->flags = be16(d.data() + 90);
    in->nextents = (in->flags2 & (1u << 4)) ? be64(d.data() + 24) : be32(d.data() + 76);   // NREXT64
    if (in->size > uint64_t(INT64_MAX)) return -EUCLEAN;
    const uint8_t* tm = d.data() + 40;
    if (in->flags2 & (1u << 3)) {   // BIGTIME: 1901-12-13 を起点とするナノ秒
      uint64_t t = be64(tm);
      in->mtime = static_cast<int64_t>(t / 1000000000ULL) - 2147483648LL;
    } else {
      in->mtime = static_cast<int32_t>(be32(tm));
    }
    size_t litino = inodesize - core;
    uint8_t forkoff = d[82];
    size_t dsize = forkoff ? size_t(forkoff) * 8 : litino;
    if (dsize > litino) return -EUCLEAN;
    in->dfork.assign(d.begin() + core, d.begin() + core + dsize);
    // 種別とフォーク形式の整合性
    switch (in->mode & S_FMT) {
      case S_DIR: if (in->format != kFmtLocal && in->format != kFmtExtents && in->format != kFmtBtree) return -EUCLEAN; break;
      case S_REG: if (in->format != kFmtExtents && in->format != kFmtBtree) return -EUCLEAN; break;
      case S_LNK: if (in->format != kFmtLocal && in->format != kFmtExtents) return -EUCLEAN; break;
      case S_CHR: case S_BLK: case S_FIFO: case S_SOCK: if (in->format != kFmtDev) return -EUCLEAN; break;
      default: return -EUCLEAN;
    }
    if (in->format == kFmtLocal && in->size > in->dfork.size()) return -EUCLEAN;
    if (icache.size() >= 8192) icache.clear();
    icache[ino] = in;
    *out = in;
    return 0;
  }

  // ---- extent 一覧 ----
  bool parse_rec(const uint8_t* r, Ext* e) const {
    uint64_t l0 = be64(r), l1 = be64(r + 8);
    e->unwritten = (l0 >> 63) != 0;
    e->off = (l0 >> 9) & ((1ULL << 54) - 1);
    e->fsb = ((l0 & 0x1FF) << 43) | (l1 >> 21);
    e->len = l1 & 0x1FFFFF;
    if (e->off + e->len > (1ULL << 62) / bs) return false;   // 論理バイト位置が 2^62 未満 (掛け算のオーバーフローで穴の長さが 0 になりハングするのを防ぐ)
    return extent_ok(e->fsb, e->len);
  }
  int load_extents(Inode* in) {
    if (in->exts) return 0;
    auto v = std::make_shared<std::vector<Ext>>();
    const std::vector<uint8_t>& df = in->dfork;
    auto push = [&](const uint8_t* r) -> int {
      Ext e;
      if (!parse_rec(r, &e)) return -EUCLEAN;
      if (!v->empty()) {
        const Ext& p = v->back();
        if (e.off < p.off + p.len) return -EUCLEAN;   // 昇順・非重複
      }
      if (v->size() >= kMaxExtents) return -EFBIG;
      v->push_back(e);
      return 0;
    };
    if (in->format == kFmtExtents) {
      if (in->nextents > df.size() / 16) return -EUCLEAN;
      for (uint64_t i = 0; i < in->nextents; i++) { int rc = push(df.data() + i * 16); if (rc) return rc; }
    } else if (in->format == kFmtBtree) {
      if (df.size() < 4) return -EUCLEAN;
      unsigned level = be16(df.data());
      unsigned numrecs = be16(df.data() + 2);
      size_t maxrecs = (df.size() - 4) / 16;
      if (level == 0 || level > kMaxBtreeLevel || numrecs == 0 || numrecs > maxrecs) return -EUCLEAN;
      uint64_t fsb = be64(df.data() + 4 + maxrecs * 8);   // 左端のポインタ
      std::set<uint64_t> seen;
      size_t hdr = v5 ? kBtreeLHdrV5 : kBtreeLHdrV4;
      std::vector<uint8_t> blk(bs);
      uint64_t limit = in->nblocks + 4;
      while (true) {
        if (fsb == kNullFsb) break;
        uint64_t b;
        if (!fsb_to_blk(fsb, &b)) return -EUCLEAN;
        if (!seen.insert(fsb).second || seen.size() > limit) return -EUCLEAN;   // 循環 / 過大
        int rc = read_meta(b * bs, blk.data(), bs);
        if (rc) return rc;
        uint32_t magic = be32(blk.data());
        if (magic != (v5 ? kBmapCrcMagic : kBmapMagic)) return -EUCLEAN;
        if (v5) {
          if (!cksum_ok(blk.data(), bs, kBtreeLCrcOff)) return -EBADMSG;
          if (be64(blk.data() + 24) != b * (bs / 512)) return -EUCLEAN;   // bb_blkno
          if (be64(blk.data() + 56) != in->ino) return -EUCLEAN;         // bb_owner
        }
        unsigned lvl = be16(blk.data() + 4), nrec = be16(blk.data() + 6);
        if (lvl >= level) return -EUCLEAN;
        if (lvl > 0) {   // 内部ノード: 左端の子へ降りる (level は 1 つずつ減る)
          if (lvl != level - 1) return -EUCLEAN;
          size_t maxr = (bs - hdr) / 16;
          if (nrec == 0 || nrec > maxr) return -EUCLEAN;
          level = lvl;
          fsb = be64(blk.data() + hdr + maxr * 8);
          continue;
        }
        // 葉: 右隣へ辿りながら集める
        if (level != 1) return -EUCLEAN;   // 途中の段が抜けている
        size_t maxr = (bs - hdr) / 16;
        if (nrec > maxr) return -EUCLEAN;
        for (unsigned i = 0; i < nrec; i++) { rc = push(blk.data() + hdr + size_t(i) * 16); if (rc) return rc; }
        fsb = be64(blk.data() + 16);   // bb_rightsib
        level = 1;
      }
    } else {
      return -EUCLEAN;
    }
    cached_exts += v->size();
    if (cached_exts > (4u << 20)) {   // キャッシュ全体の extent 数が多すぎたら破棄 (今のものは保持)
      for (auto& kv : icache) if (kv.second.get() != in) kv.second->exts.reset();
      cached_exts = v->size();
    }
    in->exts = v;
    return 0;
  }
  // 論理ブロック fb を含む extent を探す
  const Ext* find_ext(const std::vector<Ext>& v, uint64_t fb) const {
    size_t lo = 0, hi = v.size();
    while (lo < hi) {   // off <= fb を満たす最後の要素
      size_t mid = (lo + hi) / 2;
      if (v[mid].off <= fb) lo = mid + 1; else hi = mid;
    }
    if (lo == 0) return nullptr;
    const Ext& e = v[lo - 1];
    return fb < e.off + e.len ? &e : nullptr;
  }
  // fb 以降で最初に始まる extent の開始 (なければ UINT64_MAX)
  uint64_t next_ext_start(const std::vector<Ext>& v, uint64_t fb) const {
    auto it = std::upper_bound(v.begin(), v.end(), fb, [](uint64_t x, const Ext& e) { return x < e.off; });
    return it == v.end() ? UINT64_MAX : it->off;
  }

  // ---- ファイルの読み出し ----
  int64_t read_data(Inode* in, uint64_t off, uint8_t* buf, size_t len) {
    if (off >= in->size) return 0;
    len = static_cast<size_t>(std::min<uint64_t>(len, in->size - off));
    int rc = load_extents(in);
    if (rc) return rc;
    const std::vector<Ext>& v = *in->exts;
    size_t done = 0;
    while (done < len) {
      uint64_t pos = off + done;
      uint64_t fb = pos / bs;
      uint32_t in_blk = static_cast<uint32_t>(pos % bs);
      const Ext* e = find_ext(v, fb);
      if (!e) {   // 穴: 次の extent までゼロ
        uint64_t nxt = next_ext_start(v, fb);
        uint64_t hole_end = nxt == UINT64_MAX ? UINT64_MAX : nxt * bs;
        size_t n = static_cast<size_t>(std::min<uint64_t>(len - done, hole_end - pos));
        std::memset(buf + done, 0, n);
        done += n;
        continue;
      }
      uint64_t ext_end = (e->off + e->len) * bs;
      size_t n = static_cast<size_t>(std::min<uint64_t>(len - done, ext_end - pos));
      if (e->unwritten) {
        std::memset(buf + done, 0, n);
      } else {
        uint64_t blk;
        if (!fsb_to_blk(e->fsb, &blk)) return -EUCLEAN;
        uint64_t phys = (blk + (fb - e->off)) * bs + in_blk;
        rc = dev_read(phys, buf + done, n);
        if (rc) return rc;
      }
      done += n;
    }
    return static_cast<int64_t>(done);
  }

  // ---- ディレクトリ ----
  struct Ent { const uint8_t* name; size_t nlen; uint64_t ino; unsigned ftype; };
  // cb が true を返したら打ち切る。include_dots: "." と ".." も返す
  template <class F>
  int walk_dir(Inode* dir, bool include_dots, F&& cb) {
    if ((dir->mode & S_FMT) != S_DIR) return -ENOTDIR;
    if (dir->format == kFmtLocal) return walk_sf(dir, include_dots, cb);
    int rc = load_extents(dir);
    if (rc) return rc;
    const std::vector<Ext>& v = *dir->exts;
    const uint64_t leaf_fb = kDirLeafOffsetBytes / bs;
    const unsigned per = 1u << dirblklog;   // 1 ディレクトリブロックあたりの fs ブロック数
    std::vector<uint8_t> blk(dirblksize);
    int64_t last_db = -1;
    for (const Ext& e : v) {
      if (e.off >= leaf_fb) break;
      uint64_t db0 = e.off / per, db1 = (e.off + e.len - 1) / per;
      for (uint64_t db = std::max<int64_t>(db0, last_db + 1); db <= db1; db++) {
        last_db = static_cast<int64_t>(db);
        if (db * per >= leaf_fb) return 0;
        // ディレクトリブロックの fs ブロックをすべて読む (穴があるブロックは飛ばす)
        bool hole = false;
        for (unsigned k = 0; k < per && !hole; k++) {
          const Ext* x = find_ext(v, db * per + k);
          uint64_t b;
          if (!x || x->unwritten || !fsb_to_blk(x->fsb + (db * per + k - x->off), &b)) { hole = true; break; }
          rc = read_meta(b * bs, blk.data() + size_t(k) * bs, bs);
          if (rc) return rc;
        }
        if (hole) continue;
        bool stop = false;
        rc = scan_dirblock(dir, db, blk.data(), include_dots, cb, &stop);
        if (rc) return rc;
        if (stop) return 0;
      }
    }
    return 0;
  }

  template <class F>
  int scan_dirblock(Inode* dir, uint64_t db, uint8_t* p, bool include_dots, F& cb, bool* stop) {
    uint32_t magic = be32(p);
    bool is_block = magic == (v5 ? kDir3Block : kDir2Block);
    bool is_data = magic == (v5 ? kDir3Data : kDir2Data);
    if (!is_block && !is_data) return -EUCLEAN;
    size_t hdr = v5 ? kDirDataHdrV5 : kDirDataHdrV4;
    if (v5) {
      if (!cksum_ok(p, dirblksize, kDirCrcOff)) return -EBADMSG;
      if (be64(p + 40) != dir->ino) return -EUCLEAN;   // owner
    }
    (void)db;
    size_t end = dirblksize;
    if (is_block) {   // 末尾: [葉エントリ 8 バイト × count][tail: count, stale]
      if (dirblksize < hdr + 8) return -EUCLEAN;
      uint32_t count = be32(p + dirblksize - 8);
      uint64_t leaf_bytes = uint64_t(count) * 8 + 8;
      if (leaf_bytes > dirblksize - hdr) return -EUCLEAN;
      end = dirblksize - static_cast<size_t>(leaf_bytes);
    }
    size_t o = hdr;
    while (o < end) {
      if (end - o < 8) return -EUCLEAN;
      if (be16(p + o) == 0xFFFF) {   // 未使用領域
        size_t len = be16(p + o + 2);
        if (len < 8 || (len & 7) || len > end - o) return -EUCLEAN;
        o += len;
        continue;
      }
      uint64_t inum = be64(p + o);
      size_t nlen = p[o + 8];
      size_t ent = (8 + 1 + nlen + (has_ftype ? 1 : 0) + 2 + 7) & ~size_t(7);
      if (nlen == 0 || ent > end - o) return -EUCLEAN;
      unsigned ft = has_ftype ? p[o + 9 + nlen] : 0;
      const uint8_t* nm = p + o + 9;
      bool dot = (nlen == 1 && nm[0] == '.') || (nlen == 2 && nm[0] == '.' && nm[1] == '.');
      if (!dot || include_dots) {
        if (cb(Ent{nm, nlen, inum, ft})) { *stop = true; return 0; }
      }
      o += ent;
    }
    return 0;
  }

  template <class F>
  int walk_sf(Inode* dir, bool include_dots, F& cb) {
    const std::vector<uint8_t>& df = dir->dfork;
    size_t size = static_cast<size_t>(dir->size);
    if (size > df.size() || size < 2) return -EUCLEAN;
    unsigned count = df[0], i8 = df[1];
    size_t isz = i8 ? 8 : 4;
    size_t o = 2;
    if (o + isz > size) return -EUCLEAN;
    uint64_t parent = isz == 8 ? be64(&df[o]) : be32(&df[o]);
    o += isz;
    if (include_dots) {
      static const uint8_t dot[2] = {'.', '.'};
      if (cb(Ent{dot, 1, dir->ino, has_ftype ? 2u : 0u})) return 0;
      if (cb(Ent{dot, 2, parent, has_ftype ? 2u : 0u})) return 0;
    }
    for (unsigned i = 0; i < count; i++) {
      if (o + 3 > size) return -EUCLEAN;
      size_t nlen = df[o];
      size_t need = 3 + nlen + (has_ftype ? 1 : 0) + isz;
      if (nlen == 0 || o + need > size) return -EUCLEAN;
      const uint8_t* nm = &df[o + 3];
      unsigned ft = has_ftype ? df[o + 3 + nlen] : 0;
      const uint8_t* ip = &df[o + 3 + nlen + (has_ftype ? 1 : 0)];
      uint64_t inum = isz == 8 ? be64(ip) : be32(ip);
      if (cb(Ent{nm, nlen, inum, ft})) return 0;
      o += need;
    }
    return 0;
  }

  // dir の名前表を (なければ作って) 返す。作れない (大きすぎる) ときは null を返す。エラーは *rc
  std::shared_ptr<DirMap> get_dirmap(Inode* dir, int* rc) {
    for (auto& d : dirmaps) if (d->ino == dir->ino) return d;
    auto dm = std::make_shared<DirMap>();
    dm->ino = dir->ino;
    bool too_big = false;
    *rc = walk_dir(dir, true, [&](const Ent& e) {
      if (dm->m.size() >= kMaxDirMapEntries) { too_big = true; return true; }
      dm->m.emplace(std::string(reinterpret_cast<const char*>(e.name), e.nlen), e.ino);
      return false;
    });
    if (*rc || too_big) return nullptr;
    if (dirmaps.size() >= kMaxDirMaps) dirmaps.erase(dirmaps.begin());
    dirmaps.push_back(dm);
    return dm;
  }

  // ---- シンボリックリンク ----
  int read_symlink(Inode* in, std::string* out) {
    if (in->size == 0 || in->size > kMaxSymlink) return -EUCLEAN;
    size_t size = static_cast<size_t>(in->size);
    if (in->format == kFmtLocal) {
      out->assign(reinterpret_cast<const char*>(in->dfork.data()), size);
      return 0;
    }
    int rc = load_extents(in);
    if (rc) return rc;
    out->clear();
    std::vector<uint8_t> blk(bs);
    for (const Ext& e : *in->exts) {
      for (uint64_t k = 0; k < e.len && out->size() < size; k++) {
        uint64_t b;
        if (!fsb_to_blk(e.fsb + k, &b)) return -EUCLEAN;
        rc = read_meta(b * bs, blk.data(), bs);
        if (rc) return rc;
        size_t take, start = 0;
        if (v5) {
          if (be32(blk.data()) != kSymlinkMagic) return -EUCLEAN;
          if (!cksum_ok(blk.data(), bs, kSymlinkCrcOff)) return -EBADMSG;
          if (be64(blk.data() + 32) != in->ino) return -EUCLEAN;       // sl_owner
          if (be32(blk.data() + 4) != out->size()) return -EUCLEAN;    // sl_offset
          take = be32(blk.data() + 8);                                 // sl_bytes
          start = kSymlinkHdr;
          if (take > bs - kSymlinkHdr) return -EUCLEAN;
        } else {
          take = std::min<size_t>(size - out->size(), bs);
        }
        take = std::min(take, size - out->size());
        out->append(reinterpret_cast<const char*>(blk.data()) + start, take);
      }
    }
    return out->size() == size ? 0 : -EUCLEAN;
  }

  // ---- ログの状態 (読み取りだけ。再生はしない) ----
  // 0 = クリーン (再生すべき更新なし) / 1 = 未再生の更新がある、または判定できない / 負 = I/O エラー
  int log_state() {
    uint64_t start_blk;
    if (!fsb_to_blk(logstart, &start_blk)) return 1;
    const uint64_t nbb = uint64_t(logblocks) * (bs / 512);
    if (nbb < 2) return 1;
    const uint64_t base = start_blk * bs;
    if (!range_ok(dev_size, base, nbb * 512)) return 1;
    auto cycle_at = [&](uint64_t bb, uint32_t* c) -> int {
      uint8_t b[8];
      int rc = dev_read(base + bb * 512, b, 8);
      if (rc) return rc;
      *c = be32(b) == kLogMagic ? be32(b + 4) : be32(b);
      return 0;
    };
    uint32_t first = 0, last = 0;
    int rc = cycle_at(0, &first); if (rc) return rc;
    rc = cycle_at(nbb - 1, &last); if (rc) return rc;
    if (first == 0) return 0;   // 一度も書かれていないログ
    const uint64_t window = std::min<uint64_t>(nbb, 4096);
    // [from, from+n) の中で、周期が c のブロックのうち最初のもの (なければ UINT64_MAX)
    auto first_with_cycle = [&](uint64_t from, uint64_t n, uint32_t c, uint64_t* hit) -> int {
      std::vector<uint8_t> buf(static_cast<size_t>(n) * 512);
      int r = dev_read(base + from * 512, buf.data(), buf.size());
      if (r) return r;
      *hit = UINT64_MAX;
      for (uint64_t i = 0; i < n; i++) {
        const uint8_t* p = buf.data() + i * 512;
        uint32_t cc = be32(p) == kLogMagic ? be32(p + 4) : be32(p);
        if (cc == c) { *hit = from + i; break; }
      }
      return 0;
    };
    uint64_t head;
    if (last == 0) {
      // 末尾が未使用: 周期 0 が始まる最初のブロックが head (二分探索)
      uint64_t lo = 0, hi = nbb - 1;
      while (hi - lo > 1) {
        uint64_t mid = (lo + hi) / 2;
        uint32_t c; rc = cycle_at(mid, &c); if (rc) return rc;
        if (c == 0) hi = mid; else lo = mid;
      }
      head = hi;
    } else if (first == last) {
      // 全体が同じ周期。末尾付近に 1 つ前の周期の「穴」があればそこが head
      head = nbb;
      uint64_t from = nbb - window, hit;
      rc = first_with_cycle(from, window, last - 1, &hit); if (rc) return rc;
      if (hit != UINT64_MAX) head = hit;
      if (head == nbb) head = 0;
    } else {
      // 先頭が last+1、末尾が last: last 周期が始まる最初のブロックを二分探索
      uint64_t lo = 0, hi = nbb - 1;
      while (hi - lo > 1) {
        uint64_t mid = (lo + hi) / 2;
        uint32_t c; rc = cycle_at(mid, &c); if (rc) return rc;
        if (c == last) hi = mid; else lo = mid;
      }
      head = hi;
      uint64_t hit;
      if (head >= window) {
        rc = first_with_cycle(head - window, window, last, &hit); if (rc) return rc;
        if (hit != UINT64_MAX) head = hit;
      } else {
        uint64_t tail_part = window - head;
        rc = first_with_cycle(nbb - tail_part, tail_part, last - 1, &hit); if (rc) return rc;
        if (hit != UINT64_MAX) head = hit;
        else {
          rc = first_with_cycle(0, head, last, &hit); if (rc) return rc;
          if (hit != UINT64_MAX) head = hit;
        }
      }
    }
    if (head == 0) {
      uint32_t c0; rc = cycle_at(0, &c0); if (rc) return rc;
      if (c0 == 0) return 0;
    }
    // head から手前へ最後のレコードヘッダ (magic 0xFEEDbabe) を探す
    uint64_t rhead = UINT64_MAX;
    {
      std::vector<uint8_t> hb(512);
      uint64_t scanned = 0, i = head;
      const uint64_t max_scan = std::min<uint64_t>(nbb, 1100);   // 1 レコードは最大 256 KiB (512 BB) + ヘッダ。それより手前にヘッダがなければ破損として未再生扱い
      while (scanned < max_scan) {
        i = i == 0 ? nbb - 1 : i - 1;
        scanned++;
        rc = dev_read(base + i * 512, hb.data(), 4); if (rc) return rc;
        if (be32(hb.data()) == kLogMagic) { rhead = i; break; }
      }
    }
    if (rhead == UINT64_MAX) return 1;
    std::vector<uint8_t> h(512);
    rc = dev_read(base + rhead * 512, h.data(), 512); if (rc) return rc;
    // xlog_rec_header: magic 0, cycle 4, version 8, len 12, lsn 16, tail_lsn 24, crc 32, prev_block 36, num_logops 40, cycle_data[64] 44, fmt 300, uuid 304, size 320
    uint32_t h_version = be32(h.data() + 8), h_len = be32(h.data() + 12);
    uint64_t tail_lsn = be64(h.data() + 24);
    uint32_t num_ops = be32(h.data() + 40);
    uint32_t iclog_size = be32(h.data() + 320);
    uint64_t hblks = 1;
    if ((h_version & 2) && iclog_size > 32768) hblks = (iclog_size + 32767) / 32768;
    uint64_t after = rhead + hblks + (uint64_t(h_len) + 511) / 512;
    if (after >= nbb) after -= nbb;
    uint64_t tail_blk = tail_lsn & 0xFFFFFFFFu;
    if (after == head && num_ops == 1) {
      uint64_t d = rhead + hblks; if (d >= nbb) d -= nbb;
      uint8_t op[12];
      rc = dev_read(base + d * 512, op, 12); if (rc) return rc;
      if (op[9] & 0x20) tail_blk = head;   // XLOG_UNMOUNT_TRANS: 正常にアンマウントされた
    }
    lg_head = head; lg_rhead = rhead; lg_prev = rhead;
    lg_cycle = be32(h.data() + 4) + (head <= rhead ? 1 : 0);
    lg_ok = true;
    return tail_blk == head ? 0 : 1;
  }

#ifdef USBMEM_NTFS_WRITE
#include "xfs_write.inc"
#include "xfs_write_alloc.inc"
#include "xfs_write_ops.inc"
#endif
};

// ---- マウント ----
std::unique_ptr<XfsVolume> XfsVolume::mount(BlockDevicePtr dev, int* err, const XfsMountOptions& opts) {
  auto fail = [&](int e) -> std::unique_ptr<XfsVolume> { if (err) *err = e; return nullptr; };
  if (!dev) return fail(EINVAL);
  std::unique_ptr<XfsVolume> vol(new XfsVolume());
  vol->impl_.reset(new Impl());
  Impl& I = *vol->impl_;
  I.dev = dev;
  I.dev_size = dev->size();
  std::vector<uint8_t> sb(512);
  if (I.dev_size < 4096) return fail(EINVAL);
  if (I.dev_read(0, sb.data(), 512)) return fail(EIO);
  if (be32(sb.data()) != kSbMagic) return fail(EINVAL);
  I.versionnum = be16(sb.data() + 100);
  unsigned vnum = I.versionnum & 0xF;
  if (vnum != 4 && vnum != 5) return fail(EOPNOTSUPP);
  I.v5 = vnum == 5;
  I.bs = be32(sb.data() + 4);
  I.dblocks = be64(sb.data() + 8);
  uint64_t rblocks = be64(sb.data() + 16), rextents = be64(sb.data() + 24);
  I.logstart = be64(sb.data() + 48);
  I.rootino = be64(sb.data() + 56);
  I.agblocks = be32(sb.data() + 84);
  I.agcount = be32(sb.data() + 88);
  I.logblocks = be32(sb.data() + 96);
  I.sectsize = be16(sb.data() + 102);
  I.inodesize = be16(sb.data() + 104);
  I.inopblock = be16(sb.data() + 106);
  I.bslog = sb[120]; I.inopblog = sb[123]; I.agblklog = sb[124];
  uint8_t inprogress = sb[126];
  I.dirblklog = sb[192];
  uint32_t features2 = be32(sb.data() + 200);
  // 基本の妥当性 (シフト量は先に範囲を確かめる: 壊れた値で未定義動作にならないように)
  if (I.bslog > 16 || I.inopblog > 16 || I.dirblklog > 16) return fail(EINVAL);
  if (I.bs < 512 || I.bs > 65536 || (I.bs & (I.bs - 1)) || I.bs != (1u << I.bslog)) return fail(EINVAL);
  if (I.sectsize < 512 || I.sectsize > 32768 || (I.sectsize & (I.sectsize - 1)) || I.sectsize > I.bs) return fail(EINVAL);
  if (I.inodesize < 256 || I.inodesize > 2048 || (I.inodesize & (I.inodesize - 1)) || I.inodesize > I.bs) return fail(EINVAL);
  if (I.inopblock != I.bs / I.inodesize || I.inopblock != (1u << I.inopblog)) return fail(EINVAL);
  if (I.agcount == 0 || I.agblocks == 0 || I.dblocks == 0 || I.agblklog > 32) return fail(EINVAL);
  if ((1ULL << I.agblklog) < I.agblocks || (I.agblklog > 0 && (1ULL << (I.agblklog - 1)) >= I.agblocks)) return fail(EINVAL);
  if (uint64_t(I.agblocks) * (I.agcount - 1) >= I.dblocks || uint64_t(I.agblocks) * I.agcount < I.dblocks) return fail(EINVAL);
  if (I.dblocks > I.dev_size / I.bs) return fail(EINVAL);   // デバイスが小さすぎる
  if (I.bslog + I.dirblklog > 16) return fail(EINVAL);
  I.dirblksize = I.bs << I.dirblklog;
  if (I.v5) {
    if (I.sectsize > sb.size()) sb.resize(I.sectsize);
    if (I.sectsize > 512 && I.dev_read(0, sb.data(), I.sectsize)) return fail(EIO);
    if (!cksum_ok(sb.data(), I.sectsize, 224)) return fail(EUCLEAN);   // sb_crc
    if (I.inodesize < 512) return fail(EINVAL);
    I.ro_compat = be32(sb.data() + 212);
    I.incompat = be32(sb.data() + 216);
    if (I.incompat & kIncompatNeedsRepair) return fail(EUCLEAN);       // xfs_repair が必要
    if (I.incompat & ~kIncompatKnown) return fail(EOPNOTSUPP);          // 未知の incompat 機能
    I.has_ftype = (I.incompat & kIncompatFtype) != 0;
  } else {
    if (!(I.versionnum & 0x0020)) return fail(EOPNOTSUPP);   // NLINKBIT なし = v1 inode
    if (!(I.versionnum & 0x2000)) return fail(EOPNOTSUPP);   // DIRV2BIT なし = v1 ディレクトリ
    if (I.versionnum & 0x0200) return fail(EOPNOTSUPP);      // SHAREDBIT
    I.has_ftype = (I.versionnum & 0x8000) && (features2 & 0x200);
  }
  if (inprogress) return fail(EUCLEAN);   // mkfs 中
  if (rblocks || rextents) return fail(EOPNOTSUPP);   // リアルタイム領域 (別デバイス) は未対応
  if (I.logstart == 0) return fail(EOPNOTSUPP);       // 外部ログデバイスは未対応
  if (I.logblocks == 0 || !I.extent_ok(I.logstart, I.logblocks)) return fail(EUCLEAN);
  // ログ: 未再生の更新があれば拒否 (再生はしない)
  int ls = I.log_state();
  if (ls < 0) return fail(-ls);
  if (ls == 1) {
    if (!opts.allow_noload) return fail(EUCLEAN);
    I.journal_ignored = true;
  }
  // ルート
  std::shared_ptr<Inode> root;
  int rc = I.get_inode(I.rootino, &root);
  if (rc) return fail(rc == -ENOENT ? EUCLEAN : -rc);
  if ((root->mode & S_FMT) != S_DIR) return fail(EUCLEAN);
  return vol;
}

XfsVolume::~XfsVolume() {
#ifdef USBMEM_NTFS_WRITE
  if (impl_) {
    try {
      std::lock_guard<std::mutex> g(impl_->mu);
      if (impl_->rw) impl_->do_sync();
    } catch (...) {}
  }
#endif
}
uint64_t XfsVolume::root() const { return impl_->rootino; }
bool XfsVolume::journal_ignored() const { return impl_->journal_ignored; }

std::string XfsVolume::describe_features() const {
  const Impl& I = *impl_;
  char b[256];
  snprintf(b, sizeof b, "v%d bs=%u isize=%u agcount=%u agblocks=%u dirblk=%u ftype=%d incompat=0x%x rocompat=0x%x%s", I.v5 ? 5 : 4, I.bs, I.inodesize, I.agcount, I.agblocks,
           I.dirblksize, I.has_ftype ? 1 : 0, I.incompat, I.ro_compat, I.journal_ignored ? " journal-ignored" : "");
  return b;
}

int XfsVolume::stat(uint64_t ino, FsStat* st) {
  Impl& I = *impl_;
  std::lock_guard<std::mutex> g(I.mu);
  std::shared_ptr<Inode> in;
  int rc = I.get_inode(ino, &in);
  if (rc) return rc;
  st->ino = ino; st->size = in->size; st->mode = in->mode; st->nlink = in->nlink; st->mtime = in->mtime;
  return 0;
}

int XfsVolume::lookup(uint64_t dir, const std::string& name, uint64_t* ino) {
  Impl& I = *impl_;
  std::lock_guard<std::mutex> g(I.mu);
  std::shared_ptr<Inode> d;
  int rc = I.get_inode(dir, &d);
  if (rc) return rc;
  if ((d->mode & S_FMT) != S_DIR) return -ENOTDIR;
  if (name.empty() || name.size() > 255) return -ENOENT;
  uint64_t found = 0; bool hit = false;
  if (d->format != kFmtLocal) {   // sf 以外は名前表を使う (同じディレクトリへの繰り返しの lookup が速い)
    int mrc = 0;
    std::shared_ptr<Impl::DirMap> dm = I.get_dirmap(d.get(), &mrc);
    if (mrc) return mrc;
    if (dm) {
      auto it = dm->m.find(name);
      if (it == dm->m.end()) return -ENOENT;
      *ino = it->second;
      return 0;
    }
  }
  rc = I.walk_dir(d.get(), true, [&](const Impl::Ent& e) {
    if (e.nlen == name.size() && std::memcmp(e.name, name.data(), e.nlen) == 0) { found = e.ino; hit = true; return true; }
    return false;
  });
  if (rc) return rc;
  if (!hit) return -ENOENT;
  *ino = found;
  return 0;
}

int XfsVolume::readdir(uint64_t dir, std::vector<FsDirEntry>* out) {
  Impl& I = *impl_;
  std::lock_guard<std::mutex> g(I.mu);
  out->clear();
  std::shared_ptr<Inode> d;
  int rc = I.get_inode(dir, &d);
  if (rc) return rc;
  std::vector<FsDirEntry> tmp;
  bool need_inode = false;
  rc = I.walk_dir(d.get(), false, [&](const Impl::Ent& e) {
    unsigned dt = ftype_to_dtype(e.ftype);
    if (dt == 0) need_inode = true;
    tmp.push_back({std::string(reinterpret_cast<const char*>(e.name), e.nlen), e.ino, dt});
    return false;
  });
  if (rc) return rc;
  if (need_inode) {   // ftype のない (v4) ファイルシステム: inode から種別を得る
    for (auto& e : tmp) {
      if (e.dtype) continue;
      std::shared_ptr<Inode> x;
      if (I.get_inode(e.ino, &x) == 0) e.dtype = mode_to_dtype(x->mode);
    }
  }
  *out = std::move(tmp);
  return 0;
}

int64_t XfsVolume::read(uint64_t ino, uint64_t off, void* buf, size_t len) {
  Impl& I = *impl_;
  std::lock_guard<std::mutex> g(I.mu);
  std::shared_ptr<Inode> in;
  int rc = I.get_inode(ino, &in);
  if (rc) return rc;
  switch (in->mode & S_FMT) {
    case S_DIR: return -EISDIR;
    case S_REG: break;
    case S_LNK: {
      std::string t;
      rc = I.read_symlink(in.get(), &t);
      if (rc) return rc;
      if (off >= t.size()) return 0;
      size_t n = std::min<size_t>(len, t.size() - static_cast<size_t>(off));
      std::memcpy(buf, t.data() + off, n);
      return static_cast<int64_t>(n);
    }
    default: return -EINVAL;
  }
  if (in->flags & 1) return -EOPNOTSUPP;   // XFS_DIFLAG_REALTIME
  if (len == 0) return 0;
  return I.read_data(in.get(), off, static_cast<uint8_t*>(buf), len);
}

int XfsVolume::readlink(uint64_t ino, std::string* target) {
  Impl& I = *impl_;
  std::lock_guard<std::mutex> g(I.mu);
  std::shared_ptr<Inode> in;
  int rc = I.get_inode(ino, &in);
  if (rc) return rc;
  if ((in->mode & S_FMT) != S_LNK) return -EINVAL;
  return I.read_symlink(in.get(), target);
}

// ---- 書き込み系の公開 API (USBMEM_NTFS_WRITE のビルドだけ実体。そうでなければ全て EROFS)
#ifdef USBMEM_NTFS_WRITE
std::unique_ptr<XfsVolume> XfsVolume::mount_rw(BlockDevicePtr dev, int* err) {
  auto v = mount(std::move(dev), err, XfsMountOptions{});
  if (!v) return nullptr;
  try {
    std::lock_guard<std::mutex> g(v->impl_->mu);
    std::string why = v->impl_->prepare_rw();
    if (why.empty()) v->impl_->rw = true; else v->impl_->ro_reason = why;
  } catch (...) {
    v->impl_->ro_reason = "書き込み可の検査中に例外";
  }
  return v;
}
bool XfsVolume::writable() const { return impl_->rw; }
std::string XfsVolume::read_only_reason() const { return impl_->ro_reason; }
#define XFS_GUARD(body) try { std::lock_guard<std::mutex> g(impl_->mu); body } catch (const std::bad_alloc&) { return -ENOMEM; } catch (...) { return -EIO; }
int XfsVolume::sync() { XFS_GUARD(return impl_->do_sync();) }
int XfsVolume::create(uint64_t dir, const std::string& name, uint32_t mode, uint64_t* ino) {
  XFS_GUARD(Impl& m = *impl_; return m.run_op([&] { return m.new_node(dir, name, mode, 1, nullptr, ino); });)
}
int XfsVolume::mkdir(uint64_t dir, const std::string& name, uint32_t mode, uint64_t* ino) {
  XFS_GUARD(Impl& m = *impl_; return m.run_op([&] { return m.new_node(dir, name, mode, 2, nullptr, ino); });)
}
int XfsVolume::symlink(uint64_t dir, const std::string& name, const std::string& target, uint64_t* ino) {
  XFS_GUARD(Impl& m = *impl_; return m.run_op([&] { return m.new_node(dir, name, 0777, 7, &target, ino); });)
}
int64_t XfsVolume::write(uint64_t ino, uint64_t off, const void* buf, size_t len) {
  XFS_GUARD(
    Impl& m = *impl_;
    int64_t wr = 0;
    int rc = m.run_op([&] { return m.write_file(ino, off, static_cast<const uint8_t*>(buf), len, false, &wr); });
    return rc < 0 ? rc : wr;)
}
int64_t XfsVolume::write_inplace(uint64_t ino, uint64_t off, const void* buf, size_t len) {
  XFS_GUARD(
    Impl& m = *impl_;
    int64_t wr = 0;
    int rc = m.run_op([&] { return m.write_file(ino, off, static_cast<const uint8_t*>(buf), len, true, &wr); });
    return rc < 0 ? rc : wr;)
}
int XfsVolume::unlink(uint64_t, const std::string&) { return impl_->rw ? -EOPNOTSUPP : -EROFS; }
int XfsVolume::rmdir(uint64_t, const std::string&) { return impl_->rw ? -EOPNOTSUPP : -EROFS; }
int XfsVolume::rename(uint64_t, const std::string&, uint64_t, const std::string&) { return impl_->rw ? -EOPNOTSUPP : -EROFS; }
int XfsVolume::truncate(uint64_t, uint64_t) { return impl_->rw ? -EOPNOTSUPP : -EROFS; }
int XfsVolume::link(uint64_t, const std::string&, uint64_t) { return impl_->rw ? -EOPNOTSUPP : -EROFS; }
#else
std::unique_ptr<XfsVolume> XfsVolume::mount_rw(BlockDevicePtr, int* err) {
  if (err) *err = EROFS;
  return nullptr;
}
bool XfsVolume::writable() const { return false; }
std::string XfsVolume::read_only_reason() const { return "このビルドは書き込み非対応 (USBMEM_NTFS_WRITE=OFF)"; }
int XfsVolume::sync() { return -EROFS; }
int XfsVolume::create(uint64_t, const std::string&, uint32_t, uint64_t*) { return -EROFS; }
int XfsVolume::mkdir(uint64_t, const std::string&, uint32_t, uint64_t*) { return -EROFS; }
int XfsVolume::unlink(uint64_t, const std::string&) { return -EROFS; }
int XfsVolume::rmdir(uint64_t, const std::string&) { return -EROFS; }
int XfsVolume::rename(uint64_t, const std::string&, uint64_t, const std::string&) { return -EROFS; }
int XfsVolume::symlink(uint64_t, const std::string&, const std::string&, uint64_t*) { return -EROFS; }
int XfsVolume::link(uint64_t, const std::string&, uint64_t) { return -EROFS; }
int XfsVolume::truncate(uint64_t, uint64_t) { return -EROFS; }
int64_t XfsVolume::write(uint64_t, uint64_t, const void*, size_t) { return -EROFS; }
int64_t XfsVolume::write_inplace(uint64_t, uint64_t, const void*, size_t) { return -EROFS; }
#endif

}  // namespace usbmem
