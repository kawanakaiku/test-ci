// SPDX-License-Identifier: Apache-2.0
// ext3 / ext4 の mkfs (書き込む内容を組み立てるだけ)。ext4 のオンディスク形式の公開仕様 (kernel.org の ext4 Data Structures and Algorithms) から自前で書く。
// e2fsprogs・カーネルのコードは使わない。構成の考え方 (既定の機能・ジャーナルの大きさ・iノード比) は mke2fs の既定に合わせるが、ビット単位の一致は目指さない。
//
// 構成 (ext4 の既定): has_journal / extent / huge_file / flex_bg / metadata_csum / dir_nlink / extra_isize (+ resize_inode / dir_index / filetype / sparse_super / large_file / ext_attr)、
//   64bit は 2^32 ブロック以上のときだけ自動。iノード 256 バイト。グループ記述子は 32 バイト (64bit のとき 64 バイト)。
//   flex_bg: 16 グループ分のビットマップ・iノードテーブルを先頭グループに並べる。metadata_csum: 2 つ目以降のグループは BLOCK_UNINIT / INODE_UNINIT、
//   iノードテーブルは遅延ゼロ化 (ITABLE_ZEROED を立てない。カーネルの lazy_itable_init と同じ状態)。
// ext3: ブロックマップ + ジャーナル (extent なし)。
#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

#include "mkfs_internal.h"
#include "usbmem/crc32.h"
#include "usbmem/partition_edit.h"  // make_random_guid

namespace usbmem {
namespace mkfsimpl {

namespace {

constexpr uint32_t COMPAT_HAS_JOURNAL = 0x4, COMPAT_EXT_ATTR = 0x8, COMPAT_RESIZE_INODE = 0x10, COMPAT_DIR_INDEX = 0x20;
constexpr uint32_t INCOMPAT_FILETYPE = 0x2, INCOMPAT_EXTENTS = 0x40, INCOMPAT_64BIT = 0x80, INCOMPAT_FLEX_BG = 0x200;
constexpr uint32_t RO_SPARSE_SUPER = 0x1, RO_LARGE_FILE = 0x2, RO_HUGE_FILE = 0x8, RO_GDT_CSUM = 0x10, RO_DIR_NLINK = 0x20, RO_EXTRA_ISIZE = 0x40,
                   RO_METADATA_CSUM = 0x400;
constexpr uint32_t GD_INODE_UNINIT = 1, GD_BLOCK_UNINIT = 2, GD_ITABLE_ZEROED = 4;
constexpr uint32_t INO_EXTENTS_FL = 0x80000;

bool has_backup(uint64_t g) {
  if (g <= 1) return true;
  for (uint64_t base : {3ull, 5ull, 7ull}) {
    uint64_t v = base;
    while (v < g) v *= base;
    if (v == g) return true;
  }
  return false;
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

uint32_t c32c(uint32_t crc, const void* d, size_t n) { return crc32c_raw(crc, d, n); }
void be32(uint8_t* p, uint32_t v) { p[0] = static_cast<uint8_t>(v >> 24); p[1] = static_cast<uint8_t>(v >> 16); p[2] = static_cast<uint8_t>(v >> 8); p[3] = static_cast<uint8_t>(v); }

// 使用済みブロックの区間集合 (整列・結合済み)。確保は「goal 以降の最初の空き」から。
struct Iv { uint64_t s, e; };
class Used {
 public:
  void push_raw(uint64_t s, uint64_t e) { if (e > s) v_.push_back({s, e}); }
  void finalize() {
    std::sort(v_.begin(), v_.end(), [](const Iv& a, const Iv& b) { return a.s < b.s; });
    std::vector<Iv> m;
    for (auto& x : v_) {
      if (!m.empty() && x.s <= m.back().e) m.back().e = std::max(m.back().e, x.e);
      else m.push_back(x);
    }
    v_.swap(m);
  }
  void add(uint64_t s, uint64_t e) {
    if (e <= s) return;
    size_t i = static_cast<size_t>(std::lower_bound(v_.begin(), v_.end(), s, [](const Iv& a, uint64_t x) { return a.e < x; }) - v_.begin());
    size_t j = i;
    uint64_t ns = s, ne = e;
    while (j < v_.size() && v_[j].s <= e) { ns = std::min(ns, v_[j].s); ne = std::max(ne, v_[j].e); j++; }
    v_.erase(v_.begin() + static_cast<std::ptrdiff_t>(i), v_.begin() + static_cast<std::ptrdiff_t>(j));
    v_.insert(v_.begin() + static_cast<std::ptrdiff_t>(i), Iv{ns, ne});
  }
  // [goal, limit) で最初の空き区間
  bool first_free(uint64_t goal, uint64_t limit, uint64_t* s, uint64_t* e) const {
    uint64_t p = goal;
    for (;;) {
      if (p >= limit) return false;
      size_t i = static_cast<size_t>(std::upper_bound(v_.begin(), v_.end(), p, [](uint64_t x, const Iv& a) { return x < a.e; }) - v_.begin());
      if (i < v_.size() && v_[i].s <= p) { p = v_[i].e; continue; }
      *s = p;
      *e = std::min<uint64_t>(i < v_.size() ? v_[i].s : limit, limit);
      return true;
    }
  }
  // n ブロックを goal 以降 (足りなければ先頭に戻って) 確保し、区間の並びで返す。足りなければ false (何も確保しない)
  bool alloc(uint64_t goal, uint64_t n, uint64_t lo, uint64_t limit, std::vector<Iv>* out) {
    std::vector<Iv> got;
    uint64_t left = n;
    for (int pass = 0; pass < 2 && left; pass++) {
      uint64_t p = pass == 0 ? goal : lo, end = pass == 0 ? limit : std::min(goal, limit);
      while (left) {
        uint64_t s, e;
        if (!first_free(p, end, &s, &e)) break;
        uint64_t take = std::min<uint64_t>(e - s, left);
        got.push_back({s, s + take});
        left -= take;
        p = s + take;
        // 確保済みとして一時的に扱うため、仮に追加しておく
        add(s, s + take);
      }
    }
    if (left) {
      // 巻き戻す
      for (auto& g : got) remove(g.s, g.e);
      return false;
    }
    *out = got;
    return true;
  }
  void remove(uint64_t s, uint64_t e) {
    for (size_t i = 0; i < v_.size(); i++) {
      if (v_[i].e <= s || v_[i].s >= e) continue;
      Iv a = v_[i];
      v_.erase(v_.begin() + static_cast<std::ptrdiff_t>(i));
      size_t at = i;
      if (a.s < s) v_.insert(v_.begin() + static_cast<std::ptrdiff_t>(at++), Iv{a.s, s});
      if (a.e > e) v_.insert(v_.begin() + static_cast<std::ptrdiff_t>(at), Iv{e, a.e});
      return;
    }
  }
  const std::vector<Iv>& iv() const { return v_; }

 private:
  std::vector<Iv> v_;
};

struct Feat {
  bool has_journal = false, extent = false, flex_bg = false, meta_csum = false, gdt_csum = false, f64 = false, huge_file = false, dir_nlink = false,
       extra_isize = false, resize_inode = false, dir_index = false, sparse = false, large_file = false, filetype = false, ext_attr = false;
};

const std::vector<std::pair<const char*, bool Feat::*>>& feat_table() {
  static const std::vector<std::pair<const char*, bool Feat::*>> t = {
      {"has_journal", &Feat::has_journal}, {"extent", &Feat::extent},       {"flex_bg", &Feat::flex_bg},       {"metadata_csum", &Feat::meta_csum},
      {"uninit_bg", &Feat::gdt_csum},      {"64bit", &Feat::f64},           {"huge_file", &Feat::huge_file},   {"dir_nlink", &Feat::dir_nlink},
      {"extra_isize", &Feat::extra_isize}, {"resize_inode", &Feat::resize_inode}, {"dir_index", &Feat::dir_index}, {"sparse_super", &Feat::sparse},
      {"large_file", &Feat::large_file},   {"filetype", &Feat::filetype},   {"ext_attr", &Feat::ext_attr}};
  return t;
}

// 既定: ext3 = ジャーナル + 基本。ext4 = ext3 + extent / huge_file / flex_bg / metadata_csum / dir_nlink / extra_isize (64bit は容量で決める)
Feat default_feat(bool ext4) {
  Feat f;
  f.has_journal = f.resize_inode = f.dir_index = f.sparse = f.large_file = f.filetype = f.ext_attr = true;
  if (ext4) f.extent = f.huge_file = f.flex_bg = f.meta_csum = f.dir_nlink = f.extra_isize = true;
  return f;
}

}  // namespace

// 選べる機能の一覧 (UI 用)。"name" = 既定で有効
std::vector<std::string> ext_feature_list(bool ext4) {
  std::vector<std::string> out;
  Feat d = default_feat(ext4);
  for (auto& kv : feat_table()) {
    std::string n = kv.first;
    if (!ext4 && (n == "extent" || n == "flex_bg" || n == "metadata_csum" || n == "uninit_bg" || n == "64bit" || n == "huge_file" || n == "dir_nlink" || n == "extra_isize")) continue;
    out.push_back((d.*(kv.second) ? "" : "^") + n);
  }
  return out;
}

namespace {

bool parse_features(bool ext4, const std::string& spec, Feat* f, std::string* why, bool* f64_explicit) {
  *f = default_feat(ext4);
  *f64_explicit = false;
  size_t pos = 0;
  while (pos <= spec.size() && !spec.empty()) {
    size_t c = spec.find(',', pos);
    std::string tok = spec.substr(pos, c == std::string::npos ? std::string::npos : c - pos);
    pos = c == std::string::npos ? spec.size() + 1 : c + 1;
    if (tok.empty()) continue;
    bool off = tok[0] == '^' || tok[0] == '-';
    std::string name = off || tok[0] == '+' ? tok.substr(1) : tok;
    bool found = false;
    for (auto& kv : feat_table()) {
      if (name != kv.first) continue;
      found = true;
      if (!ext4 && (name == "extent" || name == "flex_bg" || name == "metadata_csum" || name == "uninit_bg" || name == "64bit" || name == "huge_file" || name == "dir_nlink" || name == "extra_isize")) {
        *why = "the feature '" + name + "' is an ext4 feature; use ext4 (ext3 is the block-map file system with a journal)";
        return false;
      }
      f->*(kv.second) = !off;
      if (name == "64bit") *f64_explicit = true;
    }
    if (!found) { *why = "unknown or unsupported feature '" + name + "'"; return false; }
  }
  if (f->meta_csum) f->gdt_csum = false;   // metadata_csum が uninit_bg を兼ねる (mke2fs と同じ)
  return true;
}

}  // namespace

void build_ext(const Params& p, Build* b, bool ext4) {
  Feat F;
  std::string why;
  bool f64_explicit = false;
  if (!parse_features(ext4, p.opt.features, &F, &why, &f64_explicit)) { b->error("bad-features", why); return; }
  if (ext4 && !F.extent) { b->error("bad-features", "ext4 needs the extent feature (without it the file system is ext3; choose ext3)"); return; }
  if (F.f64 && !F.extent) { b->error("bad-features", "64bit needs extent"); return; }
  if (F.dir_nlink && !ext4) { b->error("bad-features", "dir_nlink is an ext4 feature"); return; }

  // ---- ブロックサイズ・iノード
  const uint64_t len_bytes = p.length;
  auto size_class = [&](uint64_t bytes) { return bytes < (3ull << 20) ? 0 : bytes < (512ull << 20) ? 1 : bytes < (4ull << 40) ? 2 : bytes < (16ull << 40) ? 3 : 4; };
  const int sc = size_class(len_bytes);
  uint32_t B = p.opt.cluster_size ? p.opt.cluster_size : (sc <= 1 ? 1024u : 4096u);
  if (B != 1024 && B != 2048 && B != 4096) { b->error("bad-block-size", "the ext block size must be 1024, 2048 or 4096"); return; }
  uint32_t isz = p.opt.inode_size ? p.opt.inode_size : 256;
  if (isz != 128 && isz != 256) { b->error("bad-inode-size", "the inode size must be 128 or 256"); return; }
  if (F.extra_isize && isz < 256) { b->error("bad-inode-size", "extra_isize needs 256-byte inodes"); return; }
  if (isz < 256 && ext4) { b->error("bad-inode-size", "ext4 needs 256-byte inodes"); return; }
  uint32_t ratio = p.opt.inode_ratio ? p.opt.inode_ratio : (sc == 0 ? 8192u : sc == 1 ? 4096u : sc == 2 ? 16384u : sc == 3 ? 32768u : 65536u);
  if (ratio < B || ratio > (64u << 20)) { b->error("bad-inode-ratio", "the bytes-per-inode ratio must be between the block size and 64 MiB"); return; }
  if (p.opt.reserved_percent > 50) { b->error("bad-reserved", "the reserved block percentage must be 0 to 50"); return; }
  if (p.opt.label.size() > 16 || p.opt.label.find('\0') != std::string::npos) { b->error("bad-label", "the label must be at most 16 bytes"); return; }

  uint64_t N = len_bytes / B;
  if (N >= 0xFFFFFFFFull) {
    if (f64_explicit && !F.f64) { b->error("too-large", "this size needs the 64bit feature (more than 2^32 - 1 blocks); remove ^64bit or use a larger block size"); return; }
    if (!F.extent) { b->error("too-large", "ext3 (block map) cannot address more than 2^32 blocks; use a larger block size or ext4"); return; }
    F.f64 = true;
    F.resize_inode = false;   // resize inode のブロック番号は 32 ビット (mke2fs も 2^32 ブロック以上では使わない)
  }
  if (N > (1ull << 48)) { b->error("too-large", "the volume is larger than ext4 can address"); return; }
  const uint32_t fdb = B == 1024 ? 1 : 0;
  const uint64_t bpg = 8ull * B;
  const uint32_t gdsz = F.f64 ? 64 : 32;
  const uint64_t ipb = B / isz;
  const bool csum = F.meta_csum || F.gdt_csum;
  if (N < 2048 && F.has_journal) { b->error("too-small", "a file system with a journal needs at least 2048 blocks (" + std::to_string(2048ull * B / 1024) + " KiB at this block size)"); return; }
  if (N < 64) { b->error("too-small", "the volume is too small for ext (needs at least 64 blocks)"); return; }

  // ---- グループ数・iノード数・予約 GDT
  uint64_t G = 0, ipg = 0, gdb = 0, itb = 0, rsv = 0;
  const uint32_t flexsz = F.flex_bg ? 16 : 1;
  for (int guard = 0; guard < 16; guard++) {
    G = ceil_div(N - fdb, bpg);
    uint64_t target = ceil_div(N * B, ratio);
    ipg = ceil_div(target, G);
    ipg = std::max<uint64_t>(ipg, 16);
    ipg = std::min<uint64_t>(ipg, 8ull * B);
    ipg = align_up(ipg, std::max<uint64_t>(ipb, 8));
    if (ipg > 8ull * B) ipg -= std::max<uint64_t>(ipb, 8);
    itb = ipg / ipb;
    gdb = ceil_div(G * gdsz, B);
    rsv = 0;
    if (F.resize_inode) {
      uint64_t max_blocks = 0xFFFFFFFFull;
      if (N < max_blocks / 1024) max_blocks = N * 1024;
      uint64_t rsv_groups = ceil_div(max_blocks - fdb, bpg);
      uint64_t gdpb = B / gdsz;
      uint64_t r = ceil_div(rsv_groups, gdpb);
      rsv = r > gdb ? r - gdb : 0;
      if (rsv > B / 4) rsv = B / 4;
    }
    uint64_t last = (N - fdb) - (G - 1) * bpg;
    uint64_t ovh = (has_backup(G - 1) ? 1 + gdb + rsv : 0) + 2 + itb;
    if (G > 1 && last < ovh + 50) { N = fdb + (G - 1) * bpg; continue; }
    break;
  }
  if (ipg < 16 || ipg * G < 16) { b->error("too-small", "too few inodes"); return; }
  if (ipg * G > 0xFFFFFFFFull) { b->error("too-large", "too many inodes; use a larger bytes-per-inode ratio"); return; }
  if (!F.f64 && N > 0xFFFFFFFFull) { b->error("too-large", "too many blocks without 64bit"); return; }
  const uint32_t first_ino = 11;
  auto gstart = [&](uint64_t g) { return fdb + g * bpg; };
  auto gsize = [&](uint64_t g) { return g + 1 == G ? (N - fdb) - g * bpg : bpg; };
  auto hdr = [&](uint64_t g) -> uint64_t { return has_backup(g) ? 1 + gdb + rsv : 0; };

  // ---- テーブルの配置
  std::vector<uint64_t> bb(G), ib(G), itab(G);
  Used U;
  U.push_raw(0, fdb);   // 1 KiB ブロックのときの先頭のブートブロック (どのグループにも属さない)
  for (uint64_t g = 0; g < G; g++) U.push_raw(gstart(g), gstart(g) + hdr(g));
  if (F.flex_bg) {
    for (uint64_t h = 0; h < G; h += flexsz) {
      uint64_t n = std::min<uint64_t>(flexsz, G - h);
      uint64_t base = gstart(h) + hdr(h);
      if (n * (2 + itb) + hdr(h) > gsize(h)) { b->error("too-small", "the volume is too small for flex_bg with this block size"); return; }
      for (uint64_t s = 0; s < n; s++) {
        bb[h + s] = base + s;
        ib[h + s] = base + n + s;
        itab[h + s] = base + 2 * n + s * itb;
      }
      U.push_raw(base, base + n * (2 + itb));
    }
  } else {
    for (uint64_t g = 0; g < G; g++) {
      if (hdr(g) + 2 + itb >= gsize(g)) { b->error("too-small", "the last block group is too small"); return; }
      bb[g] = gstart(g) + hdr(g);
      ib[g] = bb[g] + 1;
      itab[g] = bb[g] + 2;
      U.push_raw(bb[g], bb[g] + 2 + itb);
    }
  }
  U.finalize();
  if (gsize(0) < hdr(0) + 2 + itb + 40) { b->error("too-small", "the volume is too small for ext with this block size"); return; }

  auto id = make_random_guid(p.opt.seed);
  if (!p.opt.uuid.empty()) {
    if (!parse_uuid(p.opt.uuid, id.data())) { b->error("bad-uuid", "the UUID must look like xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"); return; }
  }
  auto hs = make_random_guid(p.opt.seed ? p.opt.seed ^ 0x5bd1e995u : 0);
  const uint32_t now = static_cast<uint32_t>(p.now);
  const uint32_t fs_seed = c32c(0xFFFFFFFFu, id.data(), 16);

  // ---- 確保: resize inode の DIND / ルート / lost+found / ジャーナル
  const uint32_t nlf = static_cast<uint32_t>(std::min<uint64_t>(12, 16384 / B));
  const uint64_t limit = N;
  std::vector<Iv> dind_run, root_run, lf_run;
  if (F.resize_inode && rsv) {
    if (!U.alloc(0, 1, fdb, limit, &dind_run)) { b->error("too-small", "no space for the resize inode"); return; }
  }
  if (!U.alloc(0, 1, fdb, limit, &root_run) || !U.alloc(0, nlf, fdb, limit, &lf_run)) { b->error("too-small", "no space for the root directory"); return; }

  // ジャーナルの大きさ (mke2fs の既定: ブロック数から決める)
  uint64_t jblocks = 0;
  if (F.has_journal) {
    if (p.opt.journal_size) {
      jblocks = p.opt.journal_size / B;
      if (p.opt.journal_size % B) { b->error("bad-journal-size", "the journal size must be a multiple of the block size"); return; }
      if (jblocks < 1024) { b->error("bad-journal-size", "the journal must be at least 1024 blocks (" + std::to_string(1024ull * B / 1024) + " KiB at this block size)"); return; }
      if (jblocks > 10240000ull) { b->error("bad-journal-size", "the journal is larger than 10240000 blocks"); return; }
      if (jblocks > N / 2) { b->error("bad-journal-size", "the journal would take more than half of the volume"); return; }
    } else {
      jblocks = N < 32768 ? 1024 : N < 256 * 1024 ? 4096 : N < 512 * 1024 ? 8192 : N < 4096 * 1024 ? 16384 : N < 8192 * 1024 ? 32768 : N < 16384 * 1024 ? 65536 : N < 32768 * 1024 ? 131072 : 262144;
    }
    if (!F.extent) {
      uint64_t apb = B / 4;
      if (jblocks > 12 + apb + apb * apb) { b->error("bad-journal-size", "the journal is too large for a block-mapped (ext3) inode"); return; }
    }
  }
  std::vector<Iv> jruns;
  Iv jleaf{0, 0};
  uint64_t jmeta_extra = 0;   // 間接ブロック (ext3) / 木のブロック (ext4)
  if (F.has_journal) {
    // mke2fs と同じく、ブロック番号の中ほど (1 つ前のグループの先頭) を目標にする
    uint64_t gmid = ((N - fdb) / 2) / bpg;
    if (F.flex_bg && gmid > flexsz) gmid &= ~static_cast<uint64_t>(flexsz - 1);
    uint64_t gs = gmid > 0 ? gmid - 1 : 0;
    uint64_t goal = gstart(gs);
    if (F.extent) {
      std::vector<Iv> leaf;
      // 先に区間の数を見積もる: 空きを一時的に確保して数え、4 本を超えるなら木のブロックを足して取り直す
      std::vector<Iv> tmp;
      if (!U.alloc(goal, jblocks, fdb, limit, &tmp)) { b->error("too-small", "no space for the journal (" + std::to_string(jblocks) + " blocks)"); return; }
      for (auto& r : tmp) U.remove(r.s, r.e);
      size_t pieces = 0;
      for (auto& r : tmp) pieces += static_cast<size_t>(ceil_div(r.e - r.s, 32768));
      if (pieces > 4) {
        if (!U.alloc(goal, 1, fdb, limit, &leaf)) { b->error("too-small", "no space for the journal"); return; }
        jleaf = leaf[0];
        jmeta_extra = 1;
        if (!U.alloc(goal, jblocks, fdb, limit, &jruns)) { b->error("too-small", "no space for the journal (" + std::to_string(jblocks) + " blocks)"); return; }
        pieces = 0;
        for (auto& r : jruns) pieces += static_cast<size_t>(ceil_div(r.e - r.s, 32768));
        if (pieces > (B - 12) / 12) { b->error("too-small", "the free space is too fragmented for the journal"); return; }
      } else {
        jruns = tmp;
        for (auto& r : jruns) U.add(r.s, r.e);
      }
    } else {
      uint64_t apb = B / 4, need = jblocks;
      uint64_t meta = 0;
      if (jblocks > 12) { meta++; uint64_t rest = jblocks - 12; if (rest > apb) { meta++; rest -= apb; meta += ceil_div(rest, apb); } }
      need += meta;
      jmeta_extra = meta;
      if (!U.alloc(goal, need, fdb, limit, &jruns)) { b->error("too-small", "no space for the journal (" + std::to_string(need) + " blocks)"); return; }
    }
  }

  // ---- グループごとの使用数
  std::vector<uint64_t> used_g(G, 0);
  for (auto& iv : U.iv()) {
    uint64_t s = std::max<uint64_t>(iv.s, fdb), e = iv.e;
    while (s < e) {
      uint64_t g = (s - fdb) / bpg;
      uint64_t ge = std::min<uint64_t>(e, gstart(g) + gsize(g));
      used_g[g] += ge - s;
      s = ge;
    }
  }
  std::vector<uint32_t> flags(G, 0), itable_unused(G, 0);
  std::vector<uint64_t> free_blocks(G);
  std::vector<uint32_t> free_inodes(G);
  uint64_t total_free = 0;
  for (uint64_t g = 0; g < G; g++) {
    free_blocks[g] = gsize(g) - used_g[g];
    free_inodes[g] = static_cast<uint32_t>(ipg - (g == 0 ? first_ino : 0));
    total_free += free_blocks[g];
    if (csum) {
      // BLOCK_UNINIT: 使用中のブロックが「自分の控えと、自分のテーブル (自分の中にあるもの)」だけのグループ。最初と最後は除く
      uint64_t own = hdr(g);
      for (uint64_t t : {bb[g], ib[g]}) if (t >= gstart(g) && t < gstart(g) + gsize(g)) own++;
      if (itab[g] >= gstart(g) && itab[g] < gstart(g) + gsize(g)) own += itb;
      if (g != 0 && g != G - 1 && used_g[g] == own) flags[g] |= GD_BLOCK_UNINIT;
      if (g != 0) { flags[g] |= GD_INODE_UNINIT; itable_unused[g] = static_cast<uint32_t>(ipg); }
      else { flags[g] |= GD_ITABLE_ZEROED; itable_unused[g] = static_cast<uint32_t>(ipg - first_ino); }
    }
  }

  // ---- スーパーブロック
  uint32_t compat = 0, incompat = 0, ro = 0;
  if (F.has_journal) compat |= COMPAT_HAS_JOURNAL;
  if (F.ext_attr) compat |= COMPAT_EXT_ATTR;
  if (F.resize_inode) compat |= COMPAT_RESIZE_INODE;
  if (F.dir_index) compat |= COMPAT_DIR_INDEX;
  if (F.filetype) incompat |= INCOMPAT_FILETYPE;
  if (F.extent) incompat |= INCOMPAT_EXTENTS;
  if (F.f64) incompat |= INCOMPAT_64BIT;
  if (F.flex_bg) incompat |= INCOMPAT_FLEX_BG;
  if (F.sparse) ro |= RO_SPARSE_SUPER;
  if (F.large_file) ro |= RO_LARGE_FILE;
  if (F.huge_file) ro |= RO_HUGE_FILE;
  if (F.gdt_csum) ro |= RO_GDT_CSUM;
  if (F.dir_nlink) ro |= RO_DIR_NLINK;
  if (F.extra_isize) ro |= RO_EXTRA_ISIZE;
  if (F.meta_csum) ro |= RO_METADATA_CSUM;
  if (!ext4) {   // ext3 は ext3 と判別される機能だけ
    if (F.gdt_csum) { b->error("bad-features", "uninit_bg is an ext4 feature"); return; }
  }

  std::vector<uint8_t> sb(1024, 0);
  p32(&sb[0], static_cast<uint32_t>(ipg * G));
  p32(&sb[4], static_cast<uint32_t>(N));
  const uint64_t rblocks = N * p.opt.reserved_percent / 100;
  p32(&sb[8], static_cast<uint32_t>(rblocks));
  p32(&sb[12], static_cast<uint32_t>(total_free));
  if (F.f64) { p32(&sb[0x150], static_cast<uint32_t>(N >> 32)); p32(&sb[0x154], static_cast<uint32_t>(rblocks >> 32)); p32(&sb[0x158], static_cast<uint32_t>(total_free >> 32)); }
  p32(&sb[16], static_cast<uint32_t>(ipg * G - first_ino));
  p32(&sb[20], fdb);
  p32(&sb[24], ilog2(B) - 10);
  p32(&sb[28], ilog2(B) - 10);
  p32(&sb[32], static_cast<uint32_t>(bpg));
  p32(&sb[36], static_cast<uint32_t>(bpg));
  p32(&sb[40], static_cast<uint32_t>(ipg));
  p32(&sb[48], now);
  p16(&sb[54], 0xFFFF);
  p16(&sb[56], 0xEF53);
  p16(&sb[58], 1);
  p16(&sb[60], 1);
  p32(&sb[64], now);
  p32(&sb[76], 1);
  p32(&sb[84], first_ino);
  p16(&sb[88], isz);
  p32(&sb[92], compat);
  p32(&sb[96], incompat);
  p32(&sb[100], ro);
  std::memcpy(&sb[104], id.data(), 16);
  std::memcpy(&sb[120], p.opt.label.data(), p.opt.label.size());
  p16(&sb[0xCE], static_cast<uint32_t>(rsv));
  if (F.has_journal) p32(&sb[0xE0], 8);
  std::memcpy(&sb[236], hs.data(), 16);
  sb[0xFC] = 1;   // half_md4
  p16(&sb[0xFE], F.f64 ? 64 : 0);
  p32(&sb[0x100], 0x000C);   // user_xattr + acl
  p32(&sb[0x108], now);
  if (isz > 128) { p16(&sb[0x15C], 32); p16(&sb[0x15E], 32); }
  p32(&sb[0x160], 1);   // signed_directory_hash
  if (F.flex_bg) sb[0x174] = 4;
  if (F.meta_csum) sb[0x175] = 1;

  // ---- iノード組み立ての道具
  const bool big_ino = isz > 128;
  auto inode_seed = [&](uint32_t ino) { uint8_t t[4]; p32(t, ino); uint32_t c = c32c(fs_seed, t, 4); p32(t, 0); return c32c(c, t, 4); };
  auto mk_inode = [&](uint32_t ino, uint16_t mode, uint16_t links, uint64_t size, uint64_t blocks512, uint32_t iflags, const uint8_t* iblock) {
    std::vector<uint8_t> in(isz, 0);
    p16(&in[0], mode);
    p32(&in[4], static_cast<uint32_t>(size));
    p32(&in[8], now); p32(&in[12], now); p32(&in[16], now);
    p16(&in[26], links);
    p32(&in[28], static_cast<uint32_t>(blocks512));
    p32(&in[32], iflags);
    if (iblock) std::memcpy(&in[40], iblock, 60);
    p32(&in[108], static_cast<uint32_t>(size >> 32));
    if (big_ino) { p16(&in[128], 32); p32(&in[144], now); }
    if (F.meta_csum) {
      uint32_t c = c32c(inode_seed(ino), in.data(), isz);   // csum 欄は 0 の状態
      p16(&in[124], c & 0xFFFF);
      if (big_ino) p16(&in[130], c >> 16);
    }
    return in;
  };
  // 区間の並び → 論理ブロック 0 から続くエクステント (index > 0 の leaf 経由も)
  auto extent_tree_iblock = [&](const std::vector<Iv>& runs, const Iv* leaf_blk, std::vector<uint8_t>* leaf_data, uint32_t ino) {
    std::vector<uint8_t> ib60(60, 0);
    struct E { uint32_t lblk; uint32_t len; uint64_t pblk; };
    std::vector<E> list;
    uint32_t l = 0;
    for (auto& r : runs) {
      uint64_t s = r.s, left = r.e - r.s;
      while (left) {
        uint32_t len = static_cast<uint32_t>(std::min<uint64_t>(left, 32768));
        list.push_back({l, len, s});
        l += len; s += len; left -= len;
      }
    }
    auto put_ext = [](uint8_t* d, const E& e) {
      p32(d, e.lblk); p16(d + 4, e.len); p16(d + 6, static_cast<uint32_t>(e.pblk >> 32)); p32(d + 8, static_cast<uint32_t>(e.pblk));
    };
    p16(&ib60[0], 0xF30A);
    p16(&ib60[4], 4);
    if (!leaf_blk) {
      p16(&ib60[2], static_cast<uint32_t>(list.size()));
      for (size_t i = 0; i < list.size(); i++) put_ext(&ib60[12 + 12 * i], list[i]);
    } else {
      p16(&ib60[2], 1);
      p16(&ib60[6], 1);   // depth
      p32(&ib60[12], 0);
      p32(&ib60[16], static_cast<uint32_t>(leaf_blk->s));
      p16(&ib60[20], static_cast<uint32_t>(leaf_blk->s >> 32));
      leaf_data->assign(B, 0);
      uint32_t maxe = (B - 12) / 12;
      p16(&(*leaf_data)[0], 0xF30A);
      p16(&(*leaf_data)[2], static_cast<uint32_t>(list.size()));
      p16(&(*leaf_data)[4], maxe);
      for (size_t i = 0; i < list.size(); i++) put_ext(&(*leaf_data)[12 + 12 * i], list[i]);
      if (F.meta_csum) {
        uint32_t c = c32c(inode_seed(ino), leaf_data->data(), 12 + 12 * static_cast<size_t>(maxe));
        p32(&(*leaf_data)[12 + 12 * static_cast<size_t>(maxe)], c);
      }
    }
    return ib60;
  };

  const std::string st_groups = "group-metadata", st_journal = "journal", st_dirs = "root-directories", st_backup = "superblock-backups", st_primary = "primary-superblock";
  b->stage(st_backup);
  b->stage(st_groups);
  b->stage(st_journal);
  b->stage(st_dirs);
  b->stage(st_primary);

  // ---- ジャーナル (iノード 8) とブロックの内容
  std::vector<uint8_t> jino;
  std::vector<std::pair<uint64_t, std::vector<uint8_t>>> blk_data;   // 物理ブロック → 内容 (小さな領域)
  uint8_t jiblock[60] = {0};
  if (F.has_journal) {
    uint64_t jsum_blocks = jblocks + jmeta_extra;
    if (F.extent) {
      std::vector<uint8_t> leaf;
      std::vector<uint8_t> ib60 = extent_tree_iblock(jruns, jleaf.e ? &jleaf : nullptr, &leaf, 8);
      std::memcpy(jiblock, ib60.data(), 60);
      if (jleaf.e) blk_data.push_back({jleaf.s, leaf});
    } else {
      // ブロックマップ: [直接 12][IND][apb][DIND][IND][apb]... の順に確保した列を、先頭から割り当てる
      uint64_t apb = B / 4;
      std::vector<Iv> runs = jruns;
      size_t ri = 0;
      uint64_t rpos = runs.empty() ? 0 : runs[0].s;
      auto next = [&]() { uint64_t v = rpos++; if (rpos == runs[ri].e) { ri++; if (ri < runs.size()) rpos = runs[ri].s; } return v; };
      std::vector<std::pair<uint64_t, uint64_t>> data_runs;   // 物理のデータ区間 (ゼロ埋め)
      bool first_data = true;
      auto note_data = [&](uint64_t pb) {
        if (first_data) { first_data = false; return; }   // 論理 0 は jbd2 のスーパーブロック (別に書く)
        if (!data_runs.empty() && data_runs.back().second == pb) data_runs.back().second = pb + 1; else data_runs.push_back({pb, pb + 1});
      };
      uint64_t left = jblocks;
      for (uint64_t i = 0; i < 12 && left; i++, left--) { uint64_t pb = next(); note_data(pb); p32(&jiblock[4 * i], static_cast<uint32_t>(pb)); }
      auto fill_ind = [&](uint64_t n, uint64_t* ind_pb) {
        uint64_t ib_pb = next();
        std::vector<uint8_t> blk(B, 0);
        for (uint64_t k = 0; k < n; k++) { uint64_t pb = next(); note_data(pb); p32(&blk[4 * k], static_cast<uint32_t>(pb)); }
        blk_data.push_back({ib_pb, std::move(blk)});
        *ind_pb = ib_pb;
      };
      if (left) {
        uint64_t n = std::min<uint64_t>(left, apb), ind;
        fill_ind(n, &ind);
        p32(&jiblock[48], static_cast<uint32_t>(ind));
        left -= n;
      }
      if (left) {
        uint64_t dind_pb = next();
        std::vector<uint8_t> dblk(B, 0);
        for (uint64_t k = 0; left && k < apb; k++) {
          uint64_t n = std::min<uint64_t>(left, apb), ind;
          fill_ind(n, &ind);
          p32(&dblk[4 * k], static_cast<uint32_t>(ind));
          left -= n;
        }
        p32(&jiblock[52], static_cast<uint32_t>(dind_pb));
        blk_data.push_back({dind_pb, std::move(dblk)});
      }
      for (auto& dr : data_runs) b->zero(st_journal, dr.first * B, (dr.second - dr.first) * B, "journal-body");
    }
    // ジャーナルの先頭ブロック (jbd2 のスーパーブロック) の物理位置: 論理 0
    uint64_t first_pb = jruns.empty() ? 0 : jruns[0].s;
    if (!F.extent) first_pb = g32(jiblock);
    std::vector<uint8_t> jsb(B, 0);
    be32(&jsb[0], 0xC03B3998u);
    be32(&jsb[4], 4);
    be32(&jsb[12], B);
    be32(&jsb[16], static_cast<uint32_t>(jblocks));
    be32(&jsb[20], 1);
    be32(&jsb[24], 1);
    be32(&jsb[0x40], 1);
    std::memcpy(&jsb[0x100], id.data(), 16);
    if (F.extent) {
      // ジャーナルの本体 (ゼロ) は先頭ブロックを除く
      uint64_t lpos = 0;
      for (auto& r : jruns) {
        uint64_t s = r.s, e = r.e;
        if (lpos == 0) s += 1;   // 先頭のブロックは jbd2 のスーパーブロック
        lpos += r.e - r.s;
        if (e > s) b->zero(st_journal, s * B, (e - s) * B, "journal-body");
      }
    }
    b->data(st_journal, first_pb * B, std::move(jsb), "journal-superblock");
    for (auto& kv : blk_data) b->data(st_journal, kv.first * B, kv.second, "journal-map-block");
    jino = mk_inode(8, 0x8180, 1, jblocks * B, jsum_blocks * B / 512, F.extent ? INO_EXTENTS_FL : 0, jiblock);
    // スーパーブロックにジャーナルのバックアップ (iノードの i_block と大きさ)
    std::memcpy(&sb[0x10C], jiblock, 60);
    p32(&sb[0x10C + 60], static_cast<uint32_t>((jblocks * B) >> 32));
    p32(&sb[0x10C + 64], static_cast<uint32_t>(jblocks * B));
    sb[0xFD] = 1;   // jnl_backup_type = blocks
  }

  // ---- 予約 GDT (resize inode) と DIND
  std::vector<uint8_t> resize_ino;
  std::vector<std::pair<uint64_t, std::vector<uint8_t>>> rsv_blocks;
  if (F.resize_inode && rsv) {
    uint64_t apb = B / 4;
    std::vector<uint8_t> dind(B, 0);
    uint64_t nback = 0;
    for (uint64_t g = 1; g < G; g++) if (has_backup(g)) nback++;
    for (uint64_t i = 0; i < rsv; i++) {
      uint64_t pblk = fdb + 1 + gdb + i;
      p32(&dind[4 * ((gdb + i) % apb)], static_cast<uint32_t>(pblk));
      std::vector<uint8_t> ind(B, 0);
      uint64_t k = 0;
      for (uint64_t g = 1; g < G; g++) if (has_backup(g)) p32(&ind[4 * (k++)], static_cast<uint32_t>(pblk + g * bpg));
      rsv_blocks.push_back({pblk, std::move(ind)});
    }
    uint8_t ib60[60] = {0};
    p32(&ib60[4 * 13], static_cast<uint32_t>(dind_run[0].s));
    uint64_t rsz = (apb * apb + apb + 12) * B;
    resize_ino = mk_inode(7, 0x8180, 1, rsz, (1 + rsv + rsv * nback) * B / 512, 0, ib60);
    b->data(st_dirs, dind_run[0].s * B, std::move(dind), "resize-inode-dind");
  }

  // ---- ルートと lost+found
  const uint8_t ftdir = F.filetype ? 2 : 0;
  auto finish_dir = [&](std::vector<uint8_t>& d, uint32_t dir_ino) {
    if (F.meta_csum) {
      std::memset(&d[B - 12], 0, 12);
      p16(&d[B - 12 + 4], 12);
      d[B - 12 + 7] = 0xDE;
      uint32_t c = c32c(inode_seed(dir_ino), d.data(), B - 12);
      p32(&d[B - 12 + 8], c);
    }
  };
  const uint32_t tail = F.meta_csum ? 12 : 0;
  std::vector<uint8_t> root_blk(B, 0);
  p32(&root_blk[0], 2); p16(&root_blk[4], 12); root_blk[6] = 1; root_blk[7] = ftdir; root_blk[8] = '.';
  p32(&root_blk[12], 2); p16(&root_blk[16], 12); root_blk[18] = 2; root_blk[19] = ftdir; root_blk[20] = '.'; root_blk[21] = '.';
  p32(&root_blk[24], 11); p16(&root_blk[28], B - 24 - tail); root_blk[30] = 10; root_blk[31] = ftdir; std::memcpy(&root_blk[32], "lost+found", 10);
  finish_dir(root_blk, 2);
  b->data(st_dirs, root_run[0].s * B, std::move(root_blk), "root-directory");
  // lost+found: 先頭のブロックに . と ..、残りは空のエントリ 1 つ
  std::vector<uint64_t> lf_blocks;
  for (auto& r : lf_run) for (uint64_t x = r.s; x < r.e; x++) lf_blocks.push_back(x);
  for (uint32_t k = 0; k < nlf; k++) {
    std::vector<uint8_t> d(B, 0);
    if (k == 0) {
      p32(&d[0], 11); p16(&d[4], 12); d[6] = 1; d[7] = ftdir; d[8] = '.';
      p32(&d[12], 2); p16(&d[16], B - 12 - tail); d[18] = 2; d[19] = ftdir; d[20] = '.'; d[21] = '.';
    } else {
      p16(&d[4], B - tail);
    }
    finish_dir(d, 11);
    b->data(st_dirs, lf_blocks[k] * B, std::move(d), "lost+found");
  }
  std::vector<uint8_t> root_ino, lf_ino;
  {
    uint8_t ib60[60] = {0};
    if (F.extent) {
      std::vector<uint8_t> t = extent_tree_iblock(root_run, nullptr, nullptr, 2);
      std::memcpy(ib60, t.data(), 60);
      root_ino = mk_inode(2, 0x41ED, 3, B, B / 512, INO_EXTENTS_FL, ib60);
      std::memset(ib60, 0, 60);
      t = extent_tree_iblock(lf_run, nullptr, nullptr, 11);
      std::memcpy(ib60, t.data(), 60);
      lf_ino = mk_inode(11, 0x41C0, 2, static_cast<uint64_t>(nlf) * B, static_cast<uint64_t>(nlf) * B / 512, INO_EXTENTS_FL, ib60);
    } else {
      p32(&ib60[0], static_cast<uint32_t>(root_run[0].s));
      root_ino = mk_inode(2, 0x41ED, 3, B, B / 512, 0, ib60);
      std::memset(ib60, 0, 60);
      for (uint32_t k = 0; k < nlf; k++) p32(&ib60[4 * k], static_cast<uint32_t>(lf_blocks[k]));
      lf_ino = mk_inode(11, 0x41C0, 2, static_cast<uint64_t>(nlf) * B, static_cast<uint64_t>(nlf) * B / 512, 0, ib60);
    }
  }

  // ---- グループ記述子
  auto put_gd = [&](uint8_t* d, uint64_t g, const uint8_t* bbm, const uint8_t* ibm) {
    p32(d, static_cast<uint32_t>(bb[g]));
    p32(d + 4, static_cast<uint32_t>(ib[g]));
    p32(d + 8, static_cast<uint32_t>(itab[g]));
    p16(d + 12, static_cast<uint32_t>(free_blocks[g] & 0xFFFF));
    p16(d + 14, free_inodes[g] & 0xFFFF);
    p16(d + 16, g == 0 ? 2 : 0);
    p16(d + 18, flags[g]);
    p16(d + 28, itable_unused[g] & 0xFFFF);
    if (F.f64) {
      p32(d + 32, static_cast<uint32_t>(bb[g] >> 32));
      p32(d + 36, static_cast<uint32_t>(ib[g] >> 32));
      p32(d + 40, static_cast<uint32_t>(itab[g] >> 32));
      p16(d + 44, static_cast<uint32_t>(free_blocks[g] >> 16));
      p16(d + 46, free_inodes[g] >> 16);
      p16(d + 50, itable_unused[g] >> 16);
    }
    if (F.meta_csum) {
      if (bbm) { uint32_t c = c32c(fs_seed, bbm, B); p16(d + 24, c & 0xFFFF); if (F.f64) p16(d + 56, c >> 16); }
      if (ibm) { uint32_t c = c32c(fs_seed, ibm, static_cast<size_t>(ipg / 8)); p16(d + 26, c & 0xFFFF); if (F.f64) p16(d + 58, c >> 16); }
    }
    uint8_t gn[4];
    p32(gn, static_cast<uint32_t>(g));
    uint16_t cs = 0;
    if (F.meta_csum) {
      uint32_t c = c32c(fs_seed, gn, 4);
      c = c32c(c, d, 0x1E);
      uint8_t z[2] = {0, 0};
      c = c32c(c, z, 2);
      if (gdsz > 0x20) c = c32c(c, d + 0x20, gdsz - 0x20);
      cs = static_cast<uint16_t>(c & 0xFFFF);
    } else if (F.gdt_csum) {
      uint16_t c = crc16(0xFFFF, id.data(), 16);
      c = crc16(c, gn, 4);
      c = crc16(c, d, 0x1E);
      if (gdsz > 0x20) c = crc16(c, d + 0x20, gdsz - 0x20);
      cs = c;
    }
    p16(d + 0x1E, cs);
  };

  // ---- ビットマップ (UNINIT でないグループ) と iノードテーブル
  std::vector<uint8_t> gdt(static_cast<size_t>(gdb * B), 0);
  {
    // 区間を group 順に引くための添字
    const auto& iv = U.iv();
    size_t cur = 0;
    for (uint64_t g = 0; g < G; g++) {
      std::vector<uint8_t> bbm, ibm;
      bool need_b = !(flags[g] & GD_BLOCK_UNINIT), need_i = !(flags[g] & GD_INODE_UNINIT);
      uint64_t gs = gstart(g), ge = gs + gsize(g);
      if (need_b) {
        bbm.assign(B, 0);
        while (cur > 0 && iv[cur - 1].e > gs) cur--;
        for (size_t k = cur; k < iv.size() && iv[k].s < ge; k++) {
          uint64_t s = std::max<uint64_t>(iv[k].s, gs), e = std::min<uint64_t>(iv[k].e, ge);
          for (uint64_t x = s; x < e; x++) bbm[static_cast<size_t>((x - gs) / 8)] |= static_cast<uint8_t>(1u << ((x - gs) % 8));
        }
        for (uint64_t i = gsize(g); i < 8ull * B; i++) bbm[static_cast<size_t>(i / 8)] |= static_cast<uint8_t>(1u << (i % 8));
      }
      while (cur < iv.size() && iv[cur].e <= gs) cur++;
      if (need_i) {
        ibm.assign(B, 0);
        if (g == 0) for (uint32_t i = 0; i < first_ino; i++) ibm[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
        for (uint64_t i = ipg; i < 8ull * B; i++) ibm[static_cast<size_t>(i / 8)] |= static_cast<uint8_t>(1u << (i % 8));
      }
      put_gd(&gdt[static_cast<size_t>(g * gdsz)], g, need_b ? bbm.data() : nullptr, need_i ? ibm.data() : nullptr);
      if (need_b) b->data(st_groups, bb[g] * B, std::move(bbm), "block-bitmap");
      if (need_i) b->data(st_groups, ib[g] * B, std::move(ibm), "inode-bitmap");
      // iノードテーブル: UNINIT でないものはゼロ埋め (グループ 0 の先頭は予約 iノードを書く)
      if (!(flags[g] & GD_INODE_UNINIT) || !csum) {
        if (g == 0) {
          uint64_t head = std::min<uint64_t>(align_up(static_cast<uint64_t>(first_ino) * isz, B), itb * B);
          b->zero(st_groups, itab[g] * B + head, itb * B - head, "inode-table");
        } else {
          b->zero(st_groups, itab[g] * B, itb * B, "inode-table");
        }
      }
    }
  }
  {
    uint64_t head = std::min<uint64_t>(align_up(static_cast<uint64_t>(first_ino) * isz, B), itb * B);
    std::vector<uint8_t> h(static_cast<size_t>(head), 0);
    std::memcpy(&h[static_cast<size_t>(1) * isz], root_ino.data(), isz);
    if (!resize_ino.empty()) std::memcpy(&h[static_cast<size_t>(6) * isz], resize_ino.data(), isz);
    if (!jino.empty()) std::memcpy(&h[static_cast<size_t>(7) * isz], jino.data(), isz);
    std::memcpy(&h[static_cast<size_t>(10) * isz], lf_ino.data(), isz);
    b->data(st_dirs, itab[0] * B, std::move(h), "inode-table-head");
  }
  for (auto& kv : rsv_blocks) b->data(st_dirs, kv.first * B, kv.second, "reserved-gdt-block");

  // ---- スーパーブロックの確定 (チェックサム) と控え
  auto finish_sb = [&](std::vector<uint8_t>& s, uint32_t group_nr) {
    p16(&s[90], group_nr);
    if (F.meta_csum) {
      p32(&s[0x3FC], 0);
      uint32_t c = c32c(0xFFFFFFFFu, s.data(), 1020);
      p32(&s[0x3FC], c);
    }
  };
  for (uint64_t g = 1; g < G; g++) {
    if (!has_backup(g)) continue;
    std::vector<uint8_t> hd(static_cast<size_t>((1 + gdb) * B), 0);
    std::vector<uint8_t> s2 = sb;
    finish_sb(s2, static_cast<uint32_t>(g));
    std::memcpy(hd.data(), s2.data(), 1024);
    std::memcpy(hd.data() + B, gdt.data(), gdt.size());
    b->data(st_backup, gstart(g) * B, std::move(hd), "superblock-backup");
  }
  {
    finish_sb(sb, 0);
    const uint64_t gdt_off = std::max<uint64_t>(B, 2048);
    std::vector<uint8_t> hd(static_cast<size_t>(gdt_off + gdt.size()), 0);
    std::memcpy(&hd[1024], sb.data(), 1024);
    std::memcpy(&hd[static_cast<size_t>(gdt_off)], gdt.data(), gdt.size());
    b->data(st_primary, 0, std::move(hd), "primary-superblock-and-gdt");
  }

  auto n = [](uint64_t v) { return std::to_string(v); };
  char uu[40];
  std::snprintf(uu, sizeof uu, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7],
                id[8], id[9], id[10], id[11], id[12], id[13], id[14], id[15]);
  std::string fl;
  for (auto& kv : feat_table()) if (F.*(kv.second)) fl += (fl.empty() ? "" : ",") + std::string(kv.first);
  b->layout = {{"blockSize", n(B)}, {"blockCount", n(N)}, {"groups", n(G)}, {"blocksPerGroup", n(bpg)}, {"inodesPerGroup", n(ipg)}, {"inodeCount", n(ipg * G)},
               {"inodeSize", n(isz)}, {"inodeTableBlocksPerGroup", n(itb)}, {"gdtBlocks", n(gdb)}, {"reservedGdtBlocks", n(rsv)}, {"freeBlocks", n(total_free)},
               {"journalBlocks", n(jblocks)}, {"features", fl}, {"uuid", uu}};
  if (N * B < p.length) b->warn("unused-tail", n(p.length - N * B) + " bytes at the end of the range do not fill a whole block (or a whole last group) and are not used");
  if (!F.has_journal) b->warn("no-journal", "the file system has no journal; this app mounts it read-only (journal-less ext4 is not written)");
  uint64_t zero_bytes = 0;
  for (uint64_t g = 0; g < G; g++) if (!(flags[g] & GD_INODE_UNINIT)) zero_bytes += itb * B;
  if (F.has_journal && !p.opt.lazy_journal_init) zero_bytes += jblocks * B;
  if (zero_bytes > (1ull << 30)) b->warn("slow", "about " + n(zero_bytes >> 20) + " MiB of zeros are written (inode tables / journal); it can take a long time");
  b->changes.push_back(std::string("create an ") + (ext4 ? "ext4" : "ext3") + " file system: " + n(N) + " blocks of " + n(B) + " bytes in " + n(G) + " group(s), " + n(ipg * G) + " inodes" +
                       (F.has_journal ? ", journal " + n(jblocks * B >> 20) + " MiB" : ""));
  if (p.opt.lazy_journal_init && F.has_journal) {
    // ジャーナル本体のゼロ埋めを取り除く (スーパーブロックと間接ブロックだけ書く)
    auto& st = b->stage(st_journal);
    std::vector<MkfsRegion> keep;
    for (auto& r : st.regions) if (!(r.data.empty() && r.what == "journal-body")) keep.push_back(std::move(r));
    st.regions = std::move(keep);
    b->warn("lazy-journal", "the journal area is not cleared (lazy_journal_init); old data in it is ignored because the journal is marked empty");
  }
}

}  // namespace mkfsimpl
}  // namespace usbmem
