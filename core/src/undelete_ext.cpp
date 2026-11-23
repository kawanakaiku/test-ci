// SPDX-License-Identifier: Apache-2.0
// ext2/3/4 の削除ファイル: inode テーブルを走査し、「削除済み (dtime ≠ 0・リンク数 0・inode ビットマップが空き)・通常ファイル・サイズが残る・
// データの位置 (ブロックポインタ / extent) が残る」ものを列挙する。
//
// 限界 (重要): Linux カーネルの ext2/3/4 は削除時に i_size を 0 にし、ブロックポインタ / extent も消す (ext4 は extent の本体をゼロにする)。
// したがって通常のカーネルの削除では、inode だけからはデータの位置が分からず、ここでは候補にならない。候補になるのは
// debugfs (`rm`) や e2fsprogs の libext2fs・カーネル以外の実装で削除されたファイル、サイズと位置が残った inode など。ジャーナルから削除前の
// inode を探す方法は対象外 (docs/undelete-carving.md)。
// 名前: 削除でディレクトリエントリの inode 番号がゼロにされる実装ではファイル名との対応が失われる。inode 番号が残っているエントリ
// (前のエントリの余白に残る) が見つかれば、名前と親ディレクトリを復元する。見つからなければ "inode_<番号>"。
// 仕様: ext4 のディスクレイアウト (kernel.org の ext4 Data Structures and Algorithms) による。
#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <unordered_map>

#include "undelete_internal.h"

namespace usbmem {
namespace ud {
namespace {

struct Group { uint64_t ibitmap = 0, itable = 0; uint16_t flags = 0; uint32_t itable_unused = 0; };

struct ExtGeo {
  uint32_t bs = 0, ipg = 0, isize = 0, groups = 0, incompat = 0, ro = 0, compat = 0, desc_size = 32, first_data = 0, rev = 0, inodes_count = 0;
  uint64_t blocks = 0;
  bool group_csum = false;
  std::vector<Group> g;
};

bool has_super(uint32_t g, uint32_t compat, uint32_t ro, const uint32_t* backup) {
  if (g == 0) return true;
  if (compat & 0x200) return g == backup[0] || g == backup[1];
  if (!(ro & 1)) return true;
  if (g == 1) return true;
  if (!(g & 1)) return false;
  for (uint32_t b : {3u, 5u, 7u}) {
    uint64_t p = b;
    while (p < g) p *= b;
    if (p == g) return true;
  }
  return false;
}

class ExtScanner {
 public:
  ExtScanner(Ctx& c, ExtGeo& g) : c_(c), g_(g) {}

  Status run() {
    const uint64_t total = static_cast<uint64_t>(g_.groups);
    for (uint32_t gi = 0; gi < g_.groups; gi++) {
      if (c_.cancelled()) return Status::Make(Errc::Cancelled);
      c_.progress("scan", gi, total);
      Status s = scan_group(gi);
      if (!s.ok()) return s;
      if (c_.full()) break;
    }
    c_.progress("scan", total, total);
    if (!cand_.empty()) {
      Status s = walk_dirs();
      if (!s.ok() && s.code != Errc::Corrupt) return s;
    }
    for (Cand& cd : cand_) {
      auto it = names_.find(cd.ino);
      if (it != names_.end()) {
        cd.f.name = it->second.first;
        cd.f.path = it->second.second + "/" + it->second.first;
      } else {
        cd.f.name = "inode_" + std::to_string(cd.ino);
        cd.f.path = "/?/" + cd.f.name;
        cd.f.name_partial = true;
      }
      c_.add(std::move(cd.f));
    }
    return Status::Ok();
  }

 private:
  struct Cand { uint32_t ino; DeletedFile f; };

  Status read_blocks(uint64_t blk, uint32_t n, Bytes* out) { return c_.rd(blk * g_.bs, static_cast<size_t>(n) * g_.bs, out); }
  bool blk_ok(uint64_t b) const { return b >= g_.first_data && b < g_.blocks; }

  // 論理ブロック → 物理ブロックの対応 (runs: {lblk, pblk, len}。pblk 0 = 穴)。失敗なら false
  struct Run { uint64_t l, p, n; };
  bool map_blocks(const uint8_t* ib, uint32_t flags, uint64_t nblocks, std::vector<Run>* runs, Status* io) {
    if (flags & 0x80000) return map_extent_node(ib, 60, 0, runs, io, 0);
    if (nblocks > (1u << 24)) return false;
    // ブロックポインタ形式
    std::vector<uint32_t> list;
    list.reserve(static_cast<size_t>(nblocks));
    for (int i = 0; i < 12 && list.size() < nblocks; i++) list.push_back(L32(ib + 4 * i));
    bool bad = false;
    auto indirect = [&](auto&& self, uint32_t blk, int level) -> void {
      if (bad || list.size() >= nblocks) return;
      uint64_t per = g_.bs / 4;
      if (blk == 0) {  // 穴 (ゼロのポインタ): この段が覆う論理ブロックの数だけ 0 を並べる
        uint64_t span = 1;
        for (int k = 0; k < level; k++) span *= per;
        for (uint64_t k = 0; k < span && list.size() < nblocks; k++) list.push_back(0);
        return;
      }
      if (!blk_ok(blk)) { bad = true; return; }
      Bytes b;
      Status s = read_blocks(blk, 1, &b);
      if (!s.ok()) { *io = s; bad = true; return; }
      for (uint64_t k = 0; k < per && list.size() < nblocks && !bad; k++) {
        uint32_t v = L32(&b[static_cast<size_t>(k * 4)]);
        if (level == 1) list.push_back(v);
        else self(self, v, level - 1);
      }
    };
    if (list.size() < nblocks) indirect(indirect, L32(ib + 48), 1);
    if (list.size() < nblocks) indirect(indirect, L32(ib + 52), 2);
    if (list.size() < nblocks) indirect(indirect, L32(ib + 56), 3);
    if (bad) return false;
    for (uint64_t l = 0; l < list.size(); l++) {
      uint32_t p = list[static_cast<size_t>(l)];
      if (p && !blk_ok(p)) return false;
      if (!runs->empty() && runs->back().p && p && runs->back().p + runs->back().n == p && runs->back().l + runs->back().n == l) runs->back().n++;
      else if (!runs->empty() && !runs->back().p && !p && runs->back().l + runs->back().n == l) runs->back().n++;
      else runs->push_back({l, p, 1});
    }
    return true;
  }

  bool map_extent_node(const uint8_t* n, size_t avail, int depth_seen, std::vector<Run>* runs, Status* io, uint64_t) {
    if (avail < 12 || L16(n) != 0xF30A || depth_seen > 5) return false;
    unsigned entries = L16(n + 2), max = L16(n + 4), depth = L16(n + 6);
    if (entries > max || 12 + static_cast<size_t>(entries) * 12 > avail || depth > 5) return false;
    for (unsigned i = 0; i < entries; i++) {
      const uint8_t* e = n + 12 + 12 * i;
      if (depth == 0) {
        uint64_t lb = L32(e), len = L16(e + 4), pb = (static_cast<uint64_t>(L16(e + 6)) << 32) | L32(e + 8);
        bool uninit = len > 32768;
        if (uninit) len -= 32768;
        if (len == 0 || !blk_ok(pb) || pb + len > g_.blocks) return false;
        runs->push_back({lb, uninit ? 0 : pb, len});  // 未初期化 extent はゼロとして読まれる (穴)
      } else {
        uint64_t leaf = L32(e + 4) | (static_cast<uint64_t>(L16(e + 8)) << 32);
        if (!blk_ok(leaf)) return false;
        Bytes b;
        Status s = read_blocks(leaf, 1, &b);
        if (!s.ok()) { *io = s; return false; }
        if (static_cast<unsigned>(L16(b.data() + 6)) + 1 != depth) return false;
        if (!map_extent_node(b.data(), b.size(), depth_seen + 1, runs, io, 0)) return false;
      }
    }
    return true;
  }

  // runs (論理順に整列) をバイトの extents にする。nblocks を超える部分は切る
  void runs_to_extents(std::vector<Run>& runs, uint64_t nblocks, std::vector<FileExtent>* out) {
    std::sort(runs.begin(), runs.end(), [](const Run& a, const Run& b) { return a.l < b.l; });
    uint64_t pos = 0;
    for (const Run& r : runs) {
      if (r.l >= nblocks) break;
      if (r.l > pos) push_extent(out, 0, (r.l - pos) * g_.bs, true);
      else if (r.l < pos) continue;  // 重なり (壊れた木)。先のものを優先
      uint64_t n = std::min<uint64_t>(r.n, nblocks - r.l);
      if (r.p) push_extent(out, r.p * g_.bs, n * g_.bs, false);
      else push_extent(out, 0, n * g_.bs, true);
      pos = r.l + n;
    }
    if (pos < nblocks) push_extent(out, 0, (nblocks - pos) * g_.bs, true);
  }

  Status scan_group(uint32_t gi) {
    const Group& gr = g_.g[gi];
    if (g_.group_csum && (gr.flags & 0x1)) return Status::Ok();  // INODE_UNINIT
    uint32_t usable = g_.ipg;
    if (g_.group_csum && gr.itable_unused <= g_.ipg) usable = g_.ipg - gr.itable_unused;
    if (!usable) return Status::Ok();
    Bytes ibm;
    if (!blk_ok(gr.ibitmap) || !blk_ok(gr.itable)) return Status::Ok();
    Status s = read_blocks(gr.ibitmap, 1, &ibm);
    if (!s.ok()) { if (Ctx::fatal(s)) return s; c_.warn("inode bitmap unreadable in group " + std::to_string(gi)); return Status::Ok(); }
    const uint64_t chunk_inodes = std::max<uint64_t>(1, (1u << 20) / g_.isize);
    Bytes tbl;
    for (uint64_t base = 0; base < usable; base += chunk_inodes) {
      uint64_t cnt = std::min<uint64_t>(chunk_inodes, usable - base);
      s = c_.rd(gr.itable * g_.bs + base * g_.isize, static_cast<size_t>(cnt * g_.isize), &tbl);
      if (!s.ok()) { if (Ctx::fatal(s)) return s; c_.warn("inode table unreadable in group " + std::to_string(gi)); continue; }
      for (uint64_t k = 0; k < cnt; k++) {
        uint64_t idx = base + k;
        const uint8_t* in = &tbl[static_cast<size_t>(k * g_.isize)];
        c_.res.entries_seen++;
        uint16_t mode = L16(in);
        if ((mode & 0xF000) != 0x8000) continue;  // 通常ファイルだけ
        uint32_t dtime = L32(in + 20);
        if (!dtime || L16(in + 26) != 0) continue;
        if ((ibm[static_cast<size_t>(idx >> 3)] >> (idx & 7)) & 1) continue;  // inode は再利用されている
        uint64_t ino = static_cast<uint64_t>(gi) * g_.ipg + idx + 1;
        if (ino < 11 && ino != 2) continue;
        uint32_t flags = L32(in + 32);
        uint64_t size = L32(in + 4) | (g_.rev >= 1 ? static_cast<uint64_t>(L32(in + 108)) << 32 : 0);
        if (size == 0 && !c_.opt.include_empty) continue;
        if (size == 0) continue;
        if (flags & (0x10000000 /*inline data*/ | 0x800 /*encrypted*/ | 0x4 /*compressed*/)) { c_.res.skipped_invalid++; continue; }
        uint64_t nblocks = (size + g_.bs - 1) / g_.bs;
        std::vector<Run> runs;
        Status io;
        if (!map_blocks(in + 40, flags, nblocks, &runs, &io)) {
          if (Ctx::fatal(io)) return io;
          c_.res.skipped_invalid++;
          continue;
        }
        bool any = false;
        for (const Run& r : runs) any = any || r.p != 0;
        if (!any) { c_.res.skipped_invalid++; continue; }  // 位置が残っていない (カーネルによる削除)
        DeletedFile f;
        f.fs = c_.res.fs;
        f.size = size;
        f.mtime = L32(in + 16);
        f.node = ino;
        f.layout_exact = true;
        runs_to_extents(runs, nblocks, &f.extents);
        cand_.push_back({static_cast<uint32_t>(ino), std::move(f)});
        cand_ino_.insert(static_cast<uint32_t>(ino));
        if (cand_.size() >= c_.opt.max_files) { c_.res.truncated = true; return Status::Ok(); }
      }
    }
    return Status::Ok();
  }

  // inode 番号から inode を読む
  bool read_inode(uint32_t ino, Bytes* out, Status* io) {
    if (ino == 0 || ino > g_.inodes_count) return false;
    uint32_t gi = (ino - 1) / g_.ipg, idx = (ino - 1) % g_.ipg;
    if (gi >= g_.groups || !blk_ok(g_.g[gi].itable)) return false;
    *io = c_.rd(g_.g[gi].itable * g_.bs + static_cast<uint64_t>(idx) * g_.isize, g_.isize, out);
    return io->ok();
  }

  // ルートから生きているディレクトリをたどり、各ブロックの余白に残る削除エントリから (inode → 名前, 親パス) を集める
  Status walk_dirs() {
    struct D { uint32_t ino; std::string path; };
    std::deque<D> q;
    std::set<uint32_t> seen;
    q.push_back({2, ""});
    seen.insert(2);
    size_t dirs = 0;
    while (!q.empty() && dirs < 200000) {
      if (c_.cancelled()) return Status::Make(Errc::Cancelled);
      D d = q.front();
      q.pop_front();
      dirs++;
      Bytes in;
      Status io;
      if (!read_inode(d.ino, &in, &io)) { if (Ctx::fatal(io)) return io; continue; }
      if ((L16(in.data()) & 0xF000) != 0x4000) continue;
      uint64_t size = L32(in.data() + 4);
      uint32_t flags = L32(in.data() + 32);
      if (flags & 0x10000000) continue;
      uint64_t nblocks = (size + g_.bs - 1) / g_.bs;
      if (nblocks == 0 || nblocks > 65536) continue;
      std::vector<Run> runs;
      if (!map_blocks(in.data() + 40, flags, nblocks, &runs, &io)) { if (Ctx::fatal(io)) return io; continue; }
      std::vector<FileExtent> ex;
      runs_to_extents(runs, nblocks, &ex);
      for (const FileExtent& e : ex) {
        if (e.hole) continue;
        for (uint64_t off = 0; off < e.length; off += g_.bs) {
          Bytes blk;
          Status s = c_.rd(e.offset + off, g_.bs, &blk);
          if (!s.ok()) { if (Ctx::fatal(s)) return s; continue; }
          parse_dir_block(blk, d.path, &q, &seen);
        }
      }
    }
    return Status::Ok();
  }

  template <class Q>
  void parse_dir_block(const Bytes& b, const std::string& path, Q* q, std::set<uint32_t>* seen) {
    size_t pos = 0;
    while (pos + 8 <= b.size()) {
      const uint8_t* p = &b[pos];
      uint32_t ino = L32(p);
      uint32_t rl = L16(p + 4);
      if (rl == 0 && b.size() == g_.bs) rl = 65536;  // 64 KiB ブロックの rec_len のゼロ表現
      unsigned nl = p[6];
      if (rl < 8 || (rl & 3) || pos + rl > b.size() || 8 + nl > rl) return;
      if (ino != 0 && nl) {
        std::string name(reinterpret_cast<const char*>(p + 8), nl);
        bool dotted = name == "." || name == "..";
        if (!dotted && p[7] == 2 && seen->insert(ino).second) q->push_back({ino, path + "/" + name});
        size_t real = (8 + nl + 3) & ~static_cast<size_t>(3);
        // 余白に残っている削除済みエントリ
        size_t sp = real;
        while (sp + 12 <= rl) {
          const uint8_t* s = p + sp;
          uint32_t i2 = L32(s);
          unsigned r2 = L16(s + 4), n2 = s[6];
          if (r2 < 8 || (r2 & 3) || sp + r2 > rl || 8 + n2 > r2 || n2 == 0) break;
          if (i2 != 0 && cand_ino_.count(i2)) {
            std::string nm(reinterpret_cast<const char*>(s + 8), n2);
            if (nm.find('/') == std::string::npos && nm.find('\0') == std::string::npos && !names_.count(i2)) names_[i2] = {nm, path};
          }
          sp += r2;
        }
      }
      pos += rl;
    }
  }

  Ctx& c_;
  ExtGeo& g_;
  std::vector<Cand> cand_;
  std::set<uint32_t> cand_ino_;
  std::unordered_map<uint32_t, std::pair<std::string, std::string>> names_;
};

}  // namespace

Status scan_ext(Ctx& c) {
  Bytes sb;
  Status s = c.rd(1024, 1024, &sb);
  if (!s.ok()) return s;
  if (L16(&sb[56]) != 0xEF53) return Status::Make(Errc::Corrupt);
  ExtGeo g;
  g.compat = L32(&sb[92]); g.incompat = L32(&sb[96]); g.ro = L32(&sb[100]);
  bool is64 = (g.incompat & 0x80) != 0;
  g.blocks = L32(&sb[4]) | (is64 ? static_cast<uint64_t>(L32(&sb[336])) << 32 : 0);
  uint32_t lbs = L32(&sb[24]);
  if (lbs > 6) return Status::Make(Errc::Corrupt);
  g.bs = 1024u << lbs;
  g.first_data = L32(&sb[20]);
  uint32_t bpg = L32(&sb[32]);
  g.ipg = L32(&sb[40]);
  g.inodes_count = L32(&sb[0]);
  g.rev = L32(&sb[76]);
  g.isize = g.rev >= 1 ? L16(&sb[88]) : 128;
  g.desc_size = is64 ? L16(&sb[254]) : 32;
  uint32_t first_meta_bg = L32(&sb[260]);
  bool meta_bg = (g.incompat & 0x10) != 0;
  uint32_t backup[2] = {L32(&sb[0x24c]), L32(&sb[0x250])};
  g.group_csum = (g.ro & (0x10 | 0x400)) != 0;
  if (!bpg || !g.ipg || g.isize < 128 || g.isize > g.bs || !g.blocks || g.first_data >= g.blocks || g.first_data > 1) return Status::Make(Errc::Corrupt);
  if (g.desc_size < 32 || (is64 && g.desc_size < 64) || g.desc_size > g.bs || (g.desc_size & (g.desc_size - 1))) return Status::Make(Errc::Corrupt);
  if ((g.ro & 0x200) != 0) { c.warn("bigalloc is not supported by the undelete scan"); return Status::Make(Errc::Unsupported); }
  uint64_t groups64 = (g.blocks - g.first_data + bpg - 1) / bpg;
  if (groups64 > (1u << 20)) return Status::Make(Errc::Corrupt);
  g.groups = static_cast<uint32_t>(groups64);
  if (c.dev.size() / g.bs < g.blocks) return Status::Make(Errc::Corrupt);
  if (g.incompat & 0x4) c.warn("the journal has not been replayed (needs_recovery): the inode tables may be out of date");
  uint64_t dpb = g.bs / g.desc_size;
  g.g.resize(g.groups);
  Bytes gd;
  uint64_t gd_block = UINT64_MAX;
  for (uint32_t gi = 0; gi < g.groups; gi++) {
    uint64_t di = gi / dpb;
    if (di != gd_block) {
      uint64_t loc;
      if (!meta_bg || di < first_meta_bg) loc = g.first_data + di + 1;
      else {
        uint64_t bg = dpb * di;
        loc = g.first_data + bg * bpg + (has_super(static_cast<uint32_t>(std::min<uint64_t>(bg, 0xFFFFFFFFu)), g.compat, g.ro, backup) ? 1 : 0);
      }
      if (loc >= g.blocks) return Status::Make(Errc::Corrupt);
      s = c.rd(loc * g.bs, g.bs, &gd);
      if (!s.ok()) return s;
      gd_block = di;
    }
    const uint8_t* d = &gd[static_cast<size_t>((gi % dpb) * g.desc_size)];
    Group& gr = g.g[gi];
    gr.ibitmap = L32(d + 4);
    gr.itable = L32(d + 8);
    gr.flags = L16(d + 18);
    gr.itable_unused = L16(d + 28);
    if (is64 && g.desc_size >= 64) {
      gr.ibitmap |= static_cast<uint64_t>(L32(d + 0x24)) << 32;
      gr.itable |= static_cast<uint64_t>(L32(d + 0x28)) << 32;
      gr.itable_unused |= static_cast<uint32_t>(L16(d + 0x32)) << 16;
    }
  }
  ExtScanner sc(c, g);
  return sc.run();
}

}  // namespace ud
}  // namespace usbmem
