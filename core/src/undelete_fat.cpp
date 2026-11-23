// SPDX-License-Identifier: Apache-2.0
// FAT12/16/32 の削除ファイル (ディレクトリエントリの先頭が 0xE5)。
// 削除では FAT のクラスタ連鎖がゼロにされ連鎖の情報は失われるが、ディレクトリエントリには先頭クラスタとサイズが残る。
// そこで「ファイルは連続したクラスタに置かれていた」と仮定してデータを復元する (断片化していたファイルは復元できない)。
// 長いファイル名 (LFN) のエントリも削除され、順序番号の位置に 0xE5 が入る。LFN の各エントリは短い名前のチェックサムを持つので、
// チェックサムが合う唯一の先頭文字を逆算でき、LFN があれば先頭文字を含む名前が全部戻る。8.3 名だけの場合は先頭文字が失われる。
// 仕様: Microsoft の FAT 仕様 (FAT: General Overview of On-Disk Format) による。
#include <algorithm>
#include <deque>
#include <map>
#include <unordered_set>

#include "undelete_internal.h"

namespace usbmem {
namespace ud {
namespace {

struct FatGeo {
  int bits = 0;
  uint32_t bps = 0, spc = 0;
  uint64_t fat_off = 0, fat_bytes = 0, root_off = 0, root_bytes = 0, data_off = 0, clusters = 0, cbytes = 0;
  uint32_t root_clus = 0;
};

// FAT のページキャッシュ付き読み出し
class FatTable {
 public:
  FatTable(Ctx& c, const FatGeo& g) : c_(c), g_(g) {}
  // クラスタ cl の次。読めなければ 0 (空き = 連鎖の終わり扱い)
  uint32_t next(uint32_t cl) {
    uint64_t idx = g_.bits == 12 ? cl + cl / 2 : g_.bits == 16 ? static_cast<uint64_t>(cl) * 2 : static_cast<uint64_t>(cl) * 4;
    uint8_t b[4] = {0, 0, 0, 0};
    int n = g_.bits == 32 ? 4 : 2;
    for (int i = 0; i < n; i++) if (!byte(idx + i, &b[i])) return 0;
    if (g_.bits == 12) { uint32_t v = L16(b); return (cl & 1) ? v >> 4 : v & 0xFFF; }
    if (g_.bits == 16) return L16(b);
    return L32(b) & 0x0FFFFFFF;
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
  const FatGeo& g_;
  std::map<uint64_t, Bytes> pages_;
};

uint8_t short_checksum(const uint8_t* n11) {
  uint8_t s = 0;
  for (int i = 0; i < 11; i++) s = static_cast<uint8_t>(((s & 1) << 7) + (s >> 1) + n11[i]);
  return s;
}

bool bad_short_char(uint8_t ch) {
  if (ch < 0x20 && ch != 0x05) return true;
  return std::strchr("\"*+,/:;<=>?[\\]|", ch) != nullptr && ch != 0;
}

struct Lfn { bool deleted; uint8_t chk; uint16_t ch[13]; };

std::string lfn_name(const std::vector<Lfn>& parts_phys_order, size_t first, size_t count) {
  // 物理順では最後の論理片が先。短い名前のすぐ前 (= 末尾) が最初の 13 文字
  std::vector<uint16_t> u;
  for (size_t k = 0; k < count; k++) {
    const Lfn& l = parts_phys_order[first + count - 1 - k];
    for (int i = 0; i < 13; i++) u.push_back(l.ch[i]);
  }
  size_t n = 0;
  while (n < u.size() && u[n] != 0x0000 && u[n] != 0xFFFF) n++;
  return utf16_to_utf8(u.data(), n);
}

std::string short_name(const uint8_t* e, bool first_unknown, uint8_t first_char) {
  std::string base, ext;
  uint8_t nt = e[12];
  for (int i = 0; i < 8; i++) {
    uint8_t ch = e[i];
    if (i == 0) ch = first_unknown ? first_char : (ch == 0x05 ? 0xE5 : ch);
    base.push_back(static_cast<char>(ch));
  }
  for (int i = 8; i < 11; i++) ext.push_back(static_cast<char>(e[i]));
  while (!base.empty() && base.back() == ' ') base.pop_back();
  while (!ext.empty() && ext.back() == ' ') ext.pop_back();
  auto fix = [](std::string& s, bool lower) {
    for (char& ch : s) {
      uint8_t u = static_cast<uint8_t>(ch);
      if (u >= 0x80) ch = '_';  // OEM コードページは分からない
      else if (lower && ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + 32);
    }
  };
  fix(base, (nt & 0x08) != 0);
  fix(ext, (nt & 0x10) != 0);
  return ext.empty() ? base : base + "." + ext;
}

class FatScanner {
 public:
  FatScanner(Ctx& c, const FatGeo& g) : c_(c), g_(g), fat_(c, g) {}

  Status run() {
    // ルート
    Bytes root;
    if (g_.bits == 32) {
      if (!read_chain(g_.root_clus, &root)) c_.warn("root directory unreadable");
      else enqueue_parse(root, "", false, 0);
    } else {
      Status s = c_.rd(g_.root_off, static_cast<size_t>(g_.root_bytes), &root);
      if (!s.ok()) { if (Ctx::fatal(s)) return s; c_.warn("root directory unreadable"); }
      else enqueue_parse(root, "", false, 0);
    }
    while (!pending_.empty()) {
      if (c_.cancelled()) return Status::Make(Errc::Cancelled);
      if (c_.full()) break;
      Dir d = std::move(pending_.front());
      pending_.pop_front();
      Bytes data;
      bool ok = d.deleted ? read_deleted_dir(d.first, &data) : read_chain(d.first, &data);
      dirs_done_++;
      c_.progress("scan", dirs_done_, dirs_done_ + pending_.size());
      if (!ok) continue;
      parse(data, d.path, d.deleted, d.depth);
      if (fatal_.code != Errc::Ok) return fatal_;
    }
    return Status::Ok();
  }

 private:
  struct Dir { uint32_t first; std::string path; bool deleted; int depth; };

  void enqueue_parse(const Bytes& d, const std::string& path, bool del, int depth) { parse(d, path, del, depth); }

  bool read_chain(uint32_t first, Bytes* out) {
    out->clear();
    uint32_t cl = first;
    for (uint64_t n = 0; n < g_.clusters && cl >= 2 && cl < g_.clusters + 2; n++) {
      Bytes b;
      Status s = c_.rd(g_.data_off + static_cast<uint64_t>(cl - 2) * g_.cbytes, static_cast<size_t>(g_.cbytes), &b);
      if (!s.ok()) { if (Ctx::fatal(s)) fatal_ = s; return !out->empty(); }
      out->insert(out->end(), b.begin(), b.end());
      if (out->size() > (4u << 20)) break;  // FAT のディレクトリは最大 2 MiB (65536 エントリ)
      uint32_t nx = fat_.next(cl);
      if (nx == cl) break;
      cl = nx;
    }
    return !out->empty();
  }

  // 削除されたディレクトリ: FAT の連鎖は失われているので、先頭クラスタから連続と仮定して読む。先頭が "." ".." でなければ (再利用された) 使わない
  bool read_deleted_dir(uint32_t first, Bytes* out) {
    out->clear();
    if (first < 2 || first >= g_.clusters + 2) return false;
    for (uint32_t k = 0; k < 16 && first + k < g_.clusters + 2; k++) {
      Bytes b;
      Status s = c_.rd(g_.data_off + static_cast<uint64_t>(first + k - 2) * g_.cbytes, static_cast<size_t>(g_.cbytes), &b);
      if (!s.ok()) { if (Ctx::fatal(s)) fatal_ = s; break; }
      out->insert(out->end(), b.begin(), b.end());
      // 終端 (0x00) が現れたら以降は読まない
      bool end = false;
      for (size_t o = 0; o + 32 <= b.size(); o += 32) if (b[o] == 0) { end = true; break; }
      if (end) break;
    }
    if (out->size() < 64) return false;
    const uint8_t* d = out->data();
    bool dot = d[0] == '.' && d[1] == ' ' && (d[11] & 0x10);
    bool dotdot = d[32] == '.' && d[33] == '.' && (d[43] & 0x10);
    uint32_t dc = L16(d + 26) | (g_.bits == 32 ? static_cast<uint32_t>(L16(d + 20)) << 16 : 0u);
    return dot && dotdot && dc == first;
  }

  void parse(const Bytes& d, const std::string& path, bool parent_deleted, int depth) {
    std::vector<Lfn> pend;
    for (size_t off = 0; off + 32 <= d.size(); off += 32) {
      const uint8_t* e = &d[off];
      if (e[0] == 0x00) break;
      uint8_t attr = e[11];
      if ((attr & 0x3F) == 0x0F) {
        if (e[12] != 0 || L16(e + 26) != 0) { pend.clear(); continue; }
        Lfn l;
        l.deleted = e[0] == 0xE5;
        l.chk = e[13];
        for (int i = 0; i < 5; i++) l.ch[i] = L16(e + 1 + 2 * i);
        for (int i = 0; i < 6; i++) l.ch[5 + i] = L16(e + 14 + 2 * i);
        for (int i = 0; i < 2; i++) l.ch[11 + i] = L16(e + 28 + 2 * i);
        pend.push_back(l);
        continue;
      }
      std::vector<Lfn> lfn;
      lfn.swap(pend);
      if ((attr & 0x08) && !(attr & 0x10)) continue;  // ボリュームラベル
      c_.res.entries_seen++;
      bool deleted = e[0] == 0xE5;
      if (e[0] == '.' && (e[1] == ' ' || (e[1] == '.' && e[2] == ' '))) continue;
      bool is_dir = (attr & 0x10) != 0;

      std::string name;
      bool partial = false;
      if (!deleted) {
        uint8_t sum = short_checksum(e);
        bool ok = !lfn.empty();
        for (const Lfn& l : lfn) ok = ok && !l.deleted && l.chk == sum;
        name = ok ? lfn_name(lfn, 0, lfn.size()) : short_name(e, false, 0);
        if (name.empty()) name = short_name(e, false, 0);
      } else {
        // 削除: 直前の連続した削除済み LFN のうち、チェックサムが揃っているもの
        size_t cnt = 0;
        uint8_t chk = 0;
        if (!lfn.empty() && lfn.back().deleted) {
          chk = lfn.back().chk;
          while (cnt < lfn.size() && lfn[lfn.size() - 1 - cnt].deleted && lfn[lfn.size() - 1 - cnt].chk == chk) cnt++;
        }
        int first_char = -1;
        if (cnt) {
          // 先頭文字の候補は 256 通り。チェックサムが合うのは (回転と加算が全単射なので) ちょうど 1 つ
          uint8_t tmp[11];
          std::memcpy(tmp, e, 11);
          for (int cch = 0; cch < 256; cch++) {
            tmp[0] = static_cast<uint8_t>(cch);
            if (short_checksum(tmp) == chk) { first_char = cch; break; }
          }
        }
        if (cnt && first_char >= 0 && !bad_short_char(static_cast<uint8_t>(first_char))) {
          name = lfn_name(lfn, lfn.size() - cnt, cnt);
          if (name.empty()) { name = short_name(e, true, static_cast<uint8_t>(first_char)); }
        } else {
          name = short_name(e, true, '_');
          partial = true;
        }
      }
      uint32_t first = L16(e + 26) | (g_.bits == 32 ? static_cast<uint32_t>(L16(e + 20)) << 16 : 0u);
      std::string full = path + "/" + name;
      if (is_dir) {
        if (depth >= 24 || !first) continue;
        if (!visited_.insert(first).second) continue;
        pending_.push_back({first, full, deleted || parent_deleted, depth + 1});
        continue;
      }
      if (!deleted) continue;
      uint64_t size = L32(e + 28);
      if (size == 0 && !c_.opt.include_empty) continue;
      DeletedFile f;
      f.fs = c_.res.fs;
      f.name = name;
      f.path = full;
      f.name_partial = partial;
      f.in_deleted_dir = parent_deleted;
      f.size = size;
      f.mtime = dos_to_unix(L16(e + 24), L16(e + 22));
      f.node = first;
      uint64_t n = (size + g_.cbytes - 1) / g_.cbytes;
      if (size && (first < 2 || first + n - 1 > g_.clusters + 1)) {
        c_.res.skipped_invalid++;
        continue;
      }
      if (size) push_extent(&f.extents, g_.data_off + static_cast<uint64_t>(first - 2) * g_.cbytes, n * g_.cbytes, false);
      f.layout_exact = n <= 1;
      c_.add(std::move(f));
      if (c_.full()) return;
    }
  }

  Ctx& c_;
  const FatGeo& g_;
  FatTable fat_;
  std::deque<Dir> pending_;
  std::unordered_set<uint32_t> visited_;
  uint64_t dirs_done_ = 0;
  Status fatal_;
};

}  // namespace

Status scan_fat(Ctx& c) {
  Bytes b;
  Status s = c.rd(0, 512, &b);
  if (!s.ok()) return s;
  FatGeo g;
  g.bps = L16(&b[11]);
  g.spc = b[13];
  uint32_t rsvd = L16(&b[14]), nfats = b[16], root_ents = L16(&b[17]);
  uint64_t tot = L16(&b[19]) ? L16(&b[19]) : L32(&b[32]);
  uint64_t fatsz = L16(&b[22]) ? L16(&b[22]) : L32(&b[36]);
  if (g.bps < 512 || g.spc == 0 || nfats == 0 || fatsz == 0 || tot == 0) return Status::Make(Errc::Corrupt);
  g.root_bytes = static_cast<uint64_t>(root_ents) * 32;
  uint64_t root_secs = (g.root_bytes + g.bps - 1) / g.bps;
  g.fat_off = static_cast<uint64_t>(rsvd) * g.bps;
  g.fat_bytes = fatsz * g.bps;
  g.root_off = g.fat_off + nfats * g.fat_bytes;
  g.data_off = g.root_off + root_secs * g.bps;
  if (tot * g.bps > c.dev.size() || rsvd + nfats * fatsz + root_secs >= tot) return Status::Make(Errc::Corrupt);
  g.cbytes = static_cast<uint64_t>(g.spc) * g.bps;
  g.clusters = (tot - (g.data_off / g.bps)) / g.spc;
  g.root_clus = L32(&b[44]);
  g.bits = (root_ents == 0 && L16(&b[22]) == 0) ? 32 : (g.clusters < 4085 ? 12 : 16);
  if (g.bits == 32 && (g.root_clus < 2 || g.root_clus >= g.clusters + 2)) return Status::Make(Errc::Corrupt);
  FatScanner sc(c, g);
  return sc.run();
}

}  // namespace ud
}  // namespace usbmem
