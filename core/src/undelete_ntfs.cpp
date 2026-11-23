// SPDX-License-Identifier: Apache-2.0
// NTFS の削除ファイル: $MFT のレコードのうち「使用中」フラグ (0x0001) が落ちたもの。Windows の NTFS ドライバは削除でこのフラグを落とし、
// $Bitmap の対応するクラスタを空きにするが、レコードの中の属性 ($FILE_NAME・$DATA のランリスト・resident のデータ) は残す。
// 名前と親ディレクトリは $FILE_NAME から、データは $DATA のランリスト (非 resident) か値そのもの (resident) から復元する。
// 属性リスト ($ATTRIBUTE_LIST) で別のレコードに分かれたもの・圧縮・暗号化は復元できない (一覧には出し、復元不可と示す)。
// ntfs-3g の削除はレコードの属性を消すので、ここでは候補にならない (Windows の削除が主な対象)。
// 仕様: NTFS のオンディスク形式の公開資料 (Linux-NTFS プロジェクトの文書・Microsoft の公開仕様) による。
#include <algorithm>
#include <map>

#include "undelete_internal.h"

namespace usbmem {
namespace ud {
namespace {

struct NtRun { uint64_t vcn, lcn, len; bool sparse; };

struct NtGeo {
  uint32_t bps = 0, spc = 0;
  uint64_t cbytes = 0, rec_size = 0, mft_size = 0, clusters = 0;
  std::vector<NtRun> mft_runs;
};

bool apply_fixups(uint8_t* r, size_t size) {
  if (size < 48 || std::memcmp(r, "FILE", 4) != 0) return false;
  size_t uo = L16(r + 4), uc = L16(r + 6);
  if (uc < 2 || uo + uc * 2 > size || (uc - 1) * 512 != size) return false;
  uint16_t usn = L16(r + uo);
  for (size_t i = 1; i < uc; i++) {
    size_t pos = i * 512 - 2;
    if (L16(r + pos) != usn) return false;  // 書き込みが途中で途切れたレコード
    r[pos] = r[uo + 2 * i];
    r[pos + 1] = r[uo + 2 * i + 1];
  }
  return true;
}

bool decode_runs(const uint8_t* p, size_t n, std::vector<NtRun>* out, uint64_t start_vcn, uint64_t max_clusters) {
  size_t pos = 0;
  int64_t lcn = 0;
  uint64_t vcn = start_vcn;
  while (pos < n) {
    uint8_t h = p[pos++];
    if (h == 0) return true;
    unsigned lb = h & 0xF, ob = h >> 4;
    if (lb == 0 || lb > 8 || ob > 8 || pos + lb + ob > n) return false;
    uint64_t len = 0;
    for (unsigned i = 0; i < lb; i++) len |= static_cast<uint64_t>(p[pos + i]) << (8 * i);
    pos += lb;
    int64_t off = 0;
    if (ob) {
      uint64_t u = 0;
      for (unsigned i = 0; i < ob; i++) u |= static_cast<uint64_t>(p[pos + i]) << (8 * i);
      if (p[pos + ob - 1] & 0x80 && ob < 8) u |= ~0ull << (8 * ob);  // 符号拡張
      off = static_cast<int64_t>(u);
      pos += ob;
      lcn += off;
    }
    if (len == 0 || len > max_clusters) return false;
    if (ob && (lcn < 0 || static_cast<uint64_t>(lcn) + len > max_clusters)) return false;
    out->push_back({vcn, ob ? static_cast<uint64_t>(lcn) : 0, len, ob == 0});
    vcn += len;
    if (out->size() > 1000000) return false;
  }
  return true;  // 終端 0 が無くても属性の末尾まで読めた
}

uint32_t attr_len(const uint8_t* r, size_t off, size_t used) {
  if (off + 8 > used) return 0;
  uint32_t t = L32(r + off);
  if (t == 0xFFFFFFFFu) return 0;
  uint32_t l = L32(r + off + 4);
  if (l < 16 || (l & 7) || off + l > used) return 0;
  return l;
}

int64_t filetime_to_unix(uint64_t ft) {
  if (ft < 116444736000000000ull) return 0;
  return static_cast<int64_t>((ft - 116444736000000000ull) / 10000000ull);
}

struct DirInfo { uint64_t parent; uint16_t pseq; std::string name; bool in_use; uint16_t seq; };
struct FileCand { uint64_t rec; uint64_t parent; uint16_t pseq; DeletedFile f; };

class NtScanner {
 public:
  NtScanner(Ctx& c, NtGeo& g) : c_(c), g_(g) {}

  Status run() {
    uint64_t total = g_.mft_size / g_.rec_size;
    const uint64_t chunk_recs = std::max<uint64_t>(1, (1u << 20) / g_.rec_size);
    Bytes buf;
    for (uint64_t base = 0; base < total; base += chunk_recs) {
      if (c_.cancelled()) return Status::Make(Errc::Cancelled);
      c_.progress("scan", base, total);
      uint64_t cnt = std::min<uint64_t>(chunk_recs, total - base);
      Status s = read_mft(base * g_.rec_size, static_cast<size_t>(cnt * g_.rec_size), &buf);
      if (!s.ok()) { if (Ctx::fatal(s)) return s; c_.warn("MFT unreadable at record " + std::to_string(base)); continue; }
      for (uint64_t k = 0; k < cnt; k++) {
        uint64_t n = base + k;
        uint8_t* r = &buf[static_cast<size_t>(k * g_.rec_size)];
        if (!apply_fixups(r, static_cast<size_t>(g_.rec_size))) continue;
        c_.res.entries_seen++;
        parse_record(n, r);
        if (cands_.size() >= c_.opt.max_files) { c_.res.truncated = true; break; }
      }
    }
    c_.progress("scan", total, total);
    for (FileCand& fc : cands_) {
      resolve_path(&fc);
      c_.add(std::move(fc.f));
    }
    return Status::Ok();
  }

  // $MFT 自身のバイト列の [off, off+len) を読む (ランリスト経由)
  Status read_mft(uint64_t off, size_t len, Bytes* out) {
    out->assign(len, 0);
    size_t done = 0;
    while (done < len) {
      uint64_t pos = off + done, vcn = pos / g_.cbytes, in = pos % g_.cbytes;
      const NtRun* r = nullptr;
      for (const NtRun& x : g_.mft_runs) if (vcn >= x.vcn && vcn < x.vcn + x.len) { r = &x; break; }
      if (!r || r->sparse) return Status::Make(Errc::Corrupt);
      uint64_t avail = (r->vcn + r->len - vcn) * g_.cbytes - in;
      size_t n = static_cast<size_t>(std::min<uint64_t>(avail, len - done));
      Bytes tmp;
      Status s = c_.rd((r->lcn + (vcn - r->vcn)) * g_.cbytes + in, n, &tmp);
      if (!s.ok()) return s;
      std::memcpy(out->data() + done, tmp.data(), n);
      done += n;
    }
    return Status::Ok();
  }

 private:
  void parse_record(uint64_t n, const uint8_t* r) {
    uint16_t flags = L16(r + 22);
    bool in_use = flags & 1, is_dir = flags & 2;
    if (L64(r + 32) != 0) return;  // 拡張レコード
    size_t used = std::min<size_t>(L32(r + 24), static_cast<size_t>(g_.rec_size));
    size_t off = L16(r + 20);
    if (off < 48 || off >= used) return;
    // 名前 ($FILE_NAME) は Win32 > Win32+DOS > POSIX > DOS の順に選ぶ
    int best_ns = -1;
    std::string name;
    uint64_t parent = 0;
    uint16_t pseq = 0;
    bool have_data = false, attr_list = false;
    int64_t mtime = 0;
    const uint8_t* data = nullptr;
    uint32_t data_len = 0;
    for (int guard = 0; guard < 64; guard++) {
      uint32_t l = attr_len(r, off, used);
      if (!l) break;
      const uint8_t* a = r + off;
      uint32_t type = L32(a);
      bool nonres = a[8] != 0;
      uint8_t nlen = a[9];
      if (type == 0x10 && !nonres) {
        uint32_t vo = L16(a + 20);
        if (vo + 16 <= l) mtime = filetime_to_unix(L64(a + vo + 8));
      } else if (type == 0x30 && !nonres) {
        uint32_t vo = L16(a + 20), vl = L32(a + 16);
        if (vo + 66 <= l && vl >= 66 && vo + vl <= l) {
          const uint8_t* v = a + vo;
          int ns = v[65];
          int rank = ns == 1 ? 3 : ns == 3 ? 2 : ns == 0 ? 1 : 0;
          if (rank > best_ns) {
            unsigned nl = v[64];
            if (66 + nl * 2 <= vl) {
              std::vector<uint16_t> u(nl);
              for (unsigned i = 0; i < nl; i++) u[i] = L16(v + 66 + 2 * i);
              name = utf16_to_utf8(u.data(), nl);
              parent = L64(v) & 0xFFFFFFFFFFFFull;
              pseq = static_cast<uint16_t>(L64(v) >> 48);
              best_ns = rank;
              if (!mtime) mtime = filetime_to_unix(L64(v + 16));
            }
          }
        }
      } else if (type == 0x20) {
        attr_list = true;
      } else if (type == 0x80 && nlen == 0 && !have_data) {
        have_data = true;
        data = a;
        data_len = l;
      }
      off += l;
    }
    if (best_ns < 0 || name.empty()) return;
    if (is_dir) {
      dirs_[n] = {parent, pseq, name, in_use, L16(r + 16)};
      return;
    }
    if (in_use || n < 16) return;
    c_.res.entries_seen += 0;
    DeletedFile f;
    f.fs = "ntfs";
    f.name = name;
    f.mtime = mtime;
    f.node = n;
    f.layout_exact = true;
    if (!have_data) {
      if (attr_list) { f.level = RecoverLevel::None; f.notes.push_back("attribute-list"); }
      else return;  // データを持たない (空のファイル)
      cands_.push_back({n, parent, pseq, std::move(f)});
      return;
    }
    const uint8_t* a = data;
    bool nonres = a[8] != 0;
    uint16_t aflags = L16(a + 12);
    if (!nonres) {
      uint32_t vl = L32(a + 16), vo = L16(a + 20);
      if (vo > data_len || vl > data_len - vo) return;
      f.size = vl;
      if (!f.size && !c_.opt.include_empty) return;
      f.has_resident = true;
      f.resident.assign(a + vo, a + vo + vl);
      cands_.push_back({n, parent, pseq, std::move(f)});
      return;
    }
    if (data_len < 64) return;
    uint64_t start_vcn = L64(a + 16), last_vcn = L64(a + 24), alloc = L64(a + 40), real = L64(a + 48), init = L64(a + 56);
    uint32_t ro = L16(a + 32);
    f.size = real;
    if (!real && !c_.opt.include_empty) return;
    // 圧縮 (0x0001)。スパースのファイル (0x8000) は圧縮単位が入っていても圧縮ではない
    if (aflags & 0x0001) { f.level = RecoverLevel::None; f.notes.push_back("compressed"); }
    else if (aflags & 0x4000) { f.level = RecoverLevel::None; f.notes.push_back("encrypted"); }
    else if (start_vcn != 0) { f.level = RecoverLevel::None; f.notes.push_back("attribute-list"); }
    else {
      std::vector<NtRun> runs;
      if (ro >= data_len || !decode_runs(a + ro, data_len - ro, &runs, 0, g_.clusters)) { c_.res.skipped_invalid++; return; }
      uint64_t covered = 0;
      for (const NtRun& x : runs) covered += x.len;
      uint64_t need = (real + g_.cbytes - 1) / g_.cbytes;
      if (last_vcn + 1 < need || covered < need) {
        if (attr_list) { f.level = RecoverLevel::None; f.notes.push_back("attribute-list"); }
        else { c_.res.skipped_invalid++; return; }
      } else {
        (void)alloc;
        uint64_t data_len2 = std::min(init, real);
        uint64_t left = data_len2;
        for (const NtRun& x : runs) {
          if (!left) break;
          uint64_t bytes = std::min<uint64_t>(x.len * g_.cbytes, left);
          if (x.sparse) push_extent(&f.extents, 0, bytes, true);
          else push_extent(&f.extents, x.lcn * g_.cbytes, bytes, false);
          left -= bytes;
        }
        if (real > data_len2) push_extent(&f.extents, 0, real - data_len2, true);
      }
    }
    cands_.push_back({n, parent, pseq, std::move(f)});
  }

  void resolve_path(FileCand* fc) {
    std::string path = "/" + fc->f.name;
    uint64_t cur = fc->parent;
    uint16_t seq = fc->pseq;
    bool deleted_dir = false;
    for (int depth = 0; depth < 64; depth++) {
      if (cur == 5) break;
      auto it = dirs_.find(cur);
      if (it == dirs_.end() || (it->second.seq != seq && it->second.seq != static_cast<uint16_t>(seq + 1))) { path = "/?" + path; deleted_dir = true; break; }
      if (!it->second.in_use) deleted_dir = true;
      path = "/" + it->second.name + path;
      seq = it->second.pseq;
      cur = it->second.parent;
    }
    fc->f.path = path;
    fc->f.in_deleted_dir = deleted_dir;
  }

  Ctx& c_;
  NtGeo& g_;
  std::map<uint64_t, DirInfo> dirs_;
  std::vector<FileCand> cands_;
};

}  // namespace

Status scan_ntfs(Ctx& c) {
  Bytes b;
  Status s = c.rd(0, 512, &b);
  if (!s.ok()) return s;
  if (std::memcmp(&b[3], "NTFS    ", 8) != 0) return Status::Make(Errc::Corrupt);
  NtGeo g;
  g.bps = L16(&b[11]);
  g.spc = b[13];
  if (g.bps < 512 || g.spc == 0) return Status::Make(Errc::Corrupt);
  if (g.spc > 128) g.spc = 1u << (256 - g.spc);  // 2 のべき乗の指数表現 (負の値)
  g.cbytes = static_cast<uint64_t>(g.bps) * g.spc;
  uint64_t total_secs = L64(&b[40]);
  g.clusters = total_secs / g.spc;
  uint64_t mft_lcn = L64(&b[48]);
  int8_t cpr = static_cast<int8_t>(b[64]);
  g.rec_size = cpr > 0 ? static_cast<uint64_t>(cpr) * g.cbytes : (1ull << (-cpr));
  if (g.rec_size < 512 || g.rec_size > 65536 || g.rec_size % 512 || mft_lcn >= g.clusters || (total_secs * g.bps > c.dev.size())) return Status::Make(Errc::Corrupt);
  // $MFT (レコード 0) の $DATA のランリスト
  Bytes rec;
  s = c.rd(mft_lcn * g.cbytes, static_cast<size_t>(g.rec_size), &rec);
  if (!s.ok()) return s;
  if (!apply_fixups(rec.data(), rec.size())) return Status::Make(Errc::Corrupt);
  size_t used = std::min<size_t>(L32(&rec[24]), rec.size()), off = L16(&rec[20]);
  bool found = false;
  for (int guard = 0; guard < 64 && !found; guard++) {
    uint32_t l = attr_len(rec.data(), off, used);
    if (!l) break;
    const uint8_t* a = &rec[off];
    if (L32(a) == 0x80 && a[9] == 0 && a[8] != 0 && l >= 64) {
      uint32_t ro = L16(a + 32);
      g.mft_size = L64(a + 48);
      if (ro >= l || !decode_runs(a + ro, l - ro, &g.mft_runs, 0, g.clusters)) return Status::Make(Errc::Corrupt);
      found = true;
    }
    off += l;
  }
  if (!found || g.mft_size < g.rec_size || g.mft_size > (1ull << 36)) return Status::Make(Errc::Corrupt);
  NtScanner sc(c, g);
  return sc.run();
}

}  // namespace ud
}  // namespace usbmem
