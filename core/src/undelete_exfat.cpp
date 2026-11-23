// SPDX-License-Identifier: Apache-2.0
// exFAT の削除ファイル。ファイルは「エントリセット」(File 0x85 + Stream Extension 0xC0 + File Name 0xC1 ...) で、
// 削除すると各エントリの型の最上位ビット (InUse) が落ちる (0x05 / 0x40 / 0x41)。セットのチェックサム (SetChecksum) は
// 型バイトを含めて計算されているので、InUse ビットを戻して検算すれば、削除済みのエントリセットが本物かを確かめられる (再利用された領域の取り違えを防ぐ)。
// Stream の NoFatChain フラグ (GeneralSecondaryFlags の bit 1) が立つファイルは連続領域に置かれているので、データの位置が確実。
// 立っていないファイルは FAT の連鎖が削除で消えるので、連続と仮定する (確度は下がる)。
// 仕様: Microsoft の exFAT ファイルシステム仕様による。
#include <deque>
#include <map>
#include <unordered_set>

#include "undelete_internal.h"

namespace usbmem {
namespace ud {
namespace {

struct ExGeo {
  uint32_t bps = 0, spc = 0;
  uint64_t fat_off = 0, fat_bytes = 0, heap_off = 0, clusters = 0, cbytes = 0;
  uint32_t root_clus = 0;
  uint64_t dev_size = 0;
  uint64_t cluster_off(uint32_t cl) const { return heap_off + static_cast<uint64_t>(cl - 2) * cbytes; }
  bool valid_cluster(uint64_t cl) const { return cl >= 2 && cl < clusters + 2; }
};

class ExFat {
 public:
  ExFat(Ctx& c, const ExGeo& g) : c_(c), g_(g) {}
  uint32_t next(uint32_t cl) {
    uint64_t idx = static_cast<uint64_t>(cl) * 4;
    uint8_t b[4];
    for (int i = 0; i < 4; i++) if (!byte(idx + i, &b[i])) return 0;
    return L32(b);
  }
 private:
  bool byte(uint64_t i, uint8_t* out) {
    if (i >= g_.fat_bytes) return false;
    uint64_t pg = i >> 12;
    auto it = pages_.find(pg);
    if (it == pages_.end()) {
      if (pages_.size() > 256) pages_.clear();
      Bytes b;
      size_t len = static_cast<size_t>(std::min<uint64_t>(4096, g_.fat_bytes - (pg << 12)));
      if (!c_.rd(g_.fat_off + (pg << 12), len, &b).ok()) return false;
      it = pages_.emplace(pg, std::move(b)).first;
    }
    *out = it->second[static_cast<size_t>(i & 4095)];
    return true;
  }
  Ctx& c_;
  const ExGeo& g_;
  std::map<uint64_t, Bytes> pages_;
};

class ExScanner {
 public:
  ExScanner(Ctx& c, const ExGeo& g) : c_(c), g_(g), fat_(c, g) {}

  Status run() {
    pending_.push_back({g_.root_clus, 0, false, false, "", false, 0});
    while (!pending_.empty()) {
      if (c_.cancelled()) return Status::Make(Errc::Cancelled);
      if (c_.full()) break;
      Dir d = std::move(pending_.front());
      pending_.pop_front();
      Bytes data;
      if (!read_dir(d, &data)) { if (fatal_.code != Errc::Ok) return fatal_; continue; }
      done_++;
      c_.progress("scan", done_, done_ + pending_.size());
      parse(data, d);
      if (fatal_.code != Errc::Ok) return fatal_;
    }
    return Status::Ok();
  }

 private:
  struct Dir { uint32_t first; uint64_t len; bool nofat; bool deleted; std::string path; bool known_len; int depth; };

  bool read_dir(const Dir& d, Bytes* out) {
    out->clear();
    const uint64_t cap = 64ull << 20;
    if (!g_.valid_cluster(d.first)) return false;
    bool contiguous = d.nofat || (d.deleted && fat_.next(d.first) == 0);
    uint64_t want = d.known_len ? std::min<uint64_t>(d.len, cap) : cap;
    if (contiguous) {
      if (d.len == 0 && d.known_len) return false;
      uint64_t n = (want + g_.cbytes - 1) / g_.cbytes;
      for (uint64_t k = 0; k < n && d.first + k < g_.clusters + 2; k++) {
        Bytes b;
        Status s = c_.rd(g_.cluster_off(static_cast<uint32_t>(d.first + k)), static_cast<size_t>(g_.cbytes), &b);
        if (!s.ok()) { if (Ctx::fatal(s)) fatal_ = s; break; }
        out->insert(out->end(), b.begin(), b.end());
        if (!d.known_len) { bool end = false; for (size_t o = 0; o + 32 <= b.size(); o += 32) if (b[o] == 0) { end = true; break; } if (end) break; }
      }
    } else {
      uint32_t cl = d.first;
      for (uint64_t n = 0; n < g_.clusters && g_.valid_cluster(cl) && out->size() < cap; n++) {
        Bytes b;
        Status s = c_.rd(g_.cluster_off(cl), static_cast<size_t>(g_.cbytes), &b);
        if (!s.ok()) { if (Ctx::fatal(s)) fatal_ = s; break; }
        out->insert(out->end(), b.begin(), b.end());
        uint32_t nx = fat_.next(cl);
        if (nx == cl) break;
        cl = nx;
      }
    }
    return !out->empty();
  }

  void parse(const Bytes& d, const Dir& dir) {
    for (size_t off = 0; off + 32 <= d.size(); off += 32) {
      const uint8_t* e = &d[off];
      if (e[0] == 0x00) break;
      if (e[0] != 0x85 && e[0] != 0x05) continue;
      unsigned sec = e[1];
      if (sec < 2 || off + 32 * (sec + 1) > d.size()) continue;
      // SetChecksum の検算 (削除済みは各エントリの型の InUse ビットを戻す)
      uint16_t want = L16(e + 2), chk = 0;
      for (size_t i = 0; i < 32 * (sec + 1); i++) {
        if (i == 2 || i == 3) continue;
        uint8_t b = e[i];
        if (i % 32 == 0) b |= 0x80;
        chk = static_cast<uint16_t>(((chk << 15) | (chk >> 1)) + b);
      }
      if (chk != want) continue;
      const uint8_t* st = e + 32;
      if ((st[0] & 0x7F) != 0x40) continue;
      unsigned name_len = st[3];
      unsigned name_ents = (name_len + 14) / 15;
      if (name_len == 0 || name_ents > sec - 1) continue;
      std::vector<uint16_t> u;
      bool ok = true;
      for (unsigned k = 0; k < name_ents; k++) {
        const uint8_t* ne = e + 64 + 32 * k;
        if ((ne[0] & 0x7F) != 0x41) { ok = false; break; }
        for (int i = 0; i < 15 && u.size() < name_len; i++) u.push_back(L16(ne + 2 + 2 * i));
      }
      if (!ok) continue;
      c_.res.entries_seen++;
      bool deleted = e[0] == 0x05;
      std::string name = utf16_to_utf8(u.data(), u.size());
      std::string full = dir.path + "/" + name;
      uint16_t attr = L16(e + 4);
      uint8_t flags = st[1];
      uint64_t vdl = L64(st + 8), len = L64(st + 24);
      uint32_t first = L32(st + 20);
      bool nofat = (flags & 2) != 0;
      off += 32 * sec;  // セットの残りを読み飛ばす
      if (attr & 0x10) {
        if (dir.depth >= 24 || !first || !visited_.insert(first).second) continue;
        pending_.push_back({first, len, nofat, deleted || dir.deleted, full, true, dir.depth + 1});
        continue;
      }
      if (!deleted) continue;
      if (len == 0 && !c_.opt.include_empty) continue;
      DeletedFile f;
      f.fs = "exfat";
      f.name = name;
      f.path = full;
      f.in_deleted_dir = dir.deleted;
      f.size = len;
      f.mtime = dos_to_unix(static_cast<uint16_t>(L32(e + 12) >> 16), static_cast<uint16_t>(L32(e + 12) & 0xFFFF));
      f.node = first;
      uint64_t n = (len + g_.cbytes - 1) / g_.cbytes;
      if (len && (!g_.valid_cluster(first) || first + n - 1 > g_.clusters + 1 || len > g_.clusters * g_.cbytes)) {
        c_.res.skipped_invalid++;
        continue;
      }
      if (len) {
        uint64_t data_len = std::min(vdl, len);  // ValidDataLength を超える部分はゼロとして読まれる
        push_extent(&f.extents, g_.cluster_off(first), std::min(data_len, n * g_.cbytes), false);
        if (len > data_len) push_extent(&f.extents, 0, len - data_len, true);
      }
      f.layout_exact = nofat || n <= 1;
      if (!nofat && n > 1) f.notes.push_back("fat-chain-lost");
      c_.add(std::move(f));
      if (c_.full()) return;
    }
  }

  Ctx& c_;
  const ExGeo& g_;
  ExFat fat_;
  std::deque<Dir> pending_;
  std::unordered_set<uint32_t> visited_;
  uint64_t done_ = 0;
  Status fatal_;
};

}  // namespace

Status scan_exfat(Ctx& c) {
  Bytes b;
  Status s = c.rd(0, 512, &b);
  if (!s.ok()) return s;
  ExGeo g;
  if (std::memcmp(&b[3], "EXFAT   ", 8) != 0) return Status::Make(Errc::Corrupt);
  uint32_t bshift = b[108], sshift = b[109];
  if (bshift < 9 || bshift > 12 || sshift > 25 - bshift) return Status::Make(Errc::Corrupt);
  g.bps = 1u << bshift;
  g.spc = 1u << sshift;
  g.cbytes = static_cast<uint64_t>(g.bps) << sshift;
  g.fat_off = static_cast<uint64_t>(L32(&b[80])) * g.bps;
  g.fat_bytes = static_cast<uint64_t>(L32(&b[84])) * g.bps;
  g.heap_off = static_cast<uint64_t>(L32(&b[88])) * g.bps;
  g.clusters = L32(&b[92]);
  g.root_clus = L32(&b[96]);
  g.dev_size = c.dev.size();
  if (!g.clusters || g.heap_off + g.clusters * g.cbytes > g.dev_size || !g.valid_cluster(g.root_clus)) return Status::Make(Errc::Corrupt);
  ExScanner sc(c, g);
  return sc.run();
}

}  // namespace ud
}  // namespace usbmem
