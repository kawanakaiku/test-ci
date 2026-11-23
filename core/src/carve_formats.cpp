// SPDX-License-Identifier: Apache-2.0
// カービングの形式ごとの署名と構造の検査。各形式の公開仕様 (JPEG: ITU-T T.81 / JFIF、PNG: W3C PNG 仕様、GIF: GIF89a 仕様、BMP: Microsoft の仕様、
// PDF: ISO 32000、ZIP: PKWARE APPNOTE、MP4/MOV: ISO/IEC 14496-12 と QuickTime ファイル形式、MP3: ISO/IEC 11172-3 の MPEG オーディオのフレーム、
// RIFF: Microsoft の仕様、7z: 7-Zip の 7zFormat.txt、gzip: RFC 1952、SQLite: SQLite のファイル形式の文書、PE: Microsoft の PE 仕様) による。
// 長さが構造から確定できるものだけを対象にし、確定できないものは捨てる (誤検出を減らす)。
#include <zlib.h>

#include <algorithm>

#include "carve_internal.h"

namespace usbmem {
namespace carve {

// ------------------------------------------------------------------------------------------------ CarveReader
const uint8_t* CarveReader::at(uint64_t off, size_t n) {
  if (n == 0 || n > (64u << 10) || off > limit_ || n > limit_ - off) return nullptr;
  if (!(off >= wstart_ && off + n <= wend_)) {
    size_t want = static_cast<size_t>(std::min<uint64_t>(limit_ - off, 32u << 10));
    if (want < n) want = n;
    buf_.resize(want);
    IoOptions io;
    io.cancel = cancel_;
    reads++;
    st_ = dev_.read_at(off, buf_.data(), want, io);
    if (!st_.ok()) { io_error_ = true; wstart_ = wend_ = 0; return nullptr; }
    wstart_ = off;
    wend_ = off + want;
  }
  return buf_.data() + (off - wstart_);
}

bool CarveReader::stream(uint64_t off, uint64_t len, const std::function<bool(const uint8_t*, size_t)>& fn) {
  if (off > limit_ || len > limit_ - off) return false;
  std::vector<uint8_t> tmp(static_cast<size_t>(std::min<uint64_t>(len, 1u << 20)));
  uint64_t done = 0;
  while (done < len) {
    if (cancelled()) return false;
    size_t n = static_cast<size_t>(std::min<uint64_t>(len - done, tmp.size()));
    IoOptions io;
    io.cancel = cancel_;
    reads++;
    st_ = dev_.read_at(off + done, tmp.data(), n, io);
    if (!st_.ok()) { io_error_ = true; return false; }
    done += n;
    if (!fn(tmp.data(), n)) return true;
  }
  return true;
}

namespace {

// ------------------------------------------------------------------------------------------------ 共通の小道具
bool printable4(const uint8_t* p) {
  for (int i = 0; i < 4; i++) if (p[i] < 0x20 || p[i] > 0x7E) return false;
  return true;
}
bool is_alpha4(const uint8_t* p) {
  for (int i = 0; i < 4; i++) if (!((p[i] >= 'A' && p[i] <= 'Z') || (p[i] >= 'a' && p[i] <= 'z'))) return false;
  return true;
}
// [from, from+len) の中で最初の pat (n バイト) の位置。見つからなければ UINT64_MAX。
uint64_t find_pattern(CarveReader& r, uint64_t from, uint64_t len, const char* pat, size_t n) {
  uint64_t found = UINT64_MAX, base = from;
  std::vector<uint8_t> carry;
  r.stream(from, len, [&](const uint8_t* d, size_t k) {
    std::vector<uint8_t> w;
    w.reserve(carry.size() + k);
    w.insert(w.end(), carry.begin(), carry.end());
    w.insert(w.end(), d, d + k);
    const uint8_t* p = w.data();
    size_t sz = w.size();
    size_t i = 0;
    while (i + n <= sz) {
      const void* q = std::memchr(p + i, pat[0], sz - n + 1 - i);
      if (!q) break;
      i = static_cast<size_t>(static_cast<const uint8_t*>(q) - p);
      if (std::memcmp(p + i, pat, n) == 0) { found = base + i; return false; }
      i++;
    }
    size_t keep = std::min<size_t>(n - 1, sz);
    carry.assign(p + sz - keep, p + sz);
    base += sz - keep;
    return true;
  });
  return found;
}

// ------------------------------------------------------------------------------------------------ JPEG
bool jpeg_quick(const uint8_t* p, size_t) { return p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF && p[3] >= 0xC0 && p[3] != 0xFF && !(p[3] >= 0xD0 && p[3] <= 0xD9); }

bool jpeg_validate(CarveReader& r, uint64_t start, uint64_t max_len, Match* m) {
  uint64_t pos = start + 2;
  bool sof = false, sos = false;
  unsigned dqt = 0, dht = 0, w = 0, h = 0, comps = 0;
  for (int seg = 0; seg < 200000; seg++) {
    if (pos - start > max_len) return false;
    const uint8_t* p = r.at(pos, 2);
    if (!p || p[0] != 0xFF) return false;
    uint8_t mk = p[1];
    if (mk == 0xFF) { pos++; continue; }  // 詰め物
    if (mk == 0xD9) {                     // EOI
      if (!sos || !sof) return false;
      m->length = pos + 2 - start;
      m->ext = "jpg";
      m->detail = std::to_string(w) + "x" + std::to_string(h);
      m->confidence = dqt && dht ? 97 : 90;
      return true;
    }
    if (mk == 0x00 || mk == 0xD8 || mk == 0x01 || (mk >= 0xD0 && mk <= 0xD7)) return false;  // 位置が合わない / 入れ子の SOI
    const uint8_t* hd = r.at(pos, 4);
    if (!hd) return false;
    unsigned len = be16(hd + 2);
    if (len < 2) return false;
    if ((mk >= 0xC0 && mk <= 0xCF) && mk != 0xC4 && mk != 0xC8 && mk != 0xCC) {  // SOF
      const uint8_t* s = r.at(pos + 4, 6);
      if (!s) return false;
      unsigned prec = s[0];
      h = be16(s + 1);
      w = be16(s + 3);
      comps = s[5];
      if (len != 8 + 3 * comps || !comps || comps > 4 || !w || !h || !(prec == 8 || prec == 12 || prec == 16 || prec == 2 || prec == 4)) return false;
      sof = true;
    } else if (mk == 0xDB) dqt++;
    else if (mk == 0xC4) dht++;
    if (mk == 0xDA) {  // SOS: ヘッダの後はエントロピー符号化データ。次のマーカー (FF の後が 00 / RSTn 以外) まで
      if (!sof) return false;
      const uint8_t* s = r.at(pos + 4, 1);
      if (!s || s[0] == 0 || s[0] > 4 || len != 6 + 2u * s[0]) return false;
      sos = true;
      uint64_t scan = pos + 2 + len;
      uint64_t next = UINT64_MAX;
      bool prev_ff = false;
      uint64_t base = scan;
      uint64_t span = r.limit() > scan ? std::min<uint64_t>(r.limit() - scan, max_len) : 0;
      bool ok = r.stream(scan, span, [&](const uint8_t* d, size_t n) {
        size_t i = 0;
        if (prev_ff && n) {
          if (d[0] != 0x00 && !(d[0] >= 0xD0 && d[0] <= 0xD7)) { next = base - 1; return false; }
          prev_ff = false;
          i = 1;
        }
        while (i < n) {
          const void* q = std::memchr(d + i, 0xFF, n - i);
          if (!q) break;
          size_t k = static_cast<size_t>(static_cast<const uint8_t*>(q) - d);
          if (k + 1 >= n) { prev_ff = true; i = n; break; }
          uint8_t c = d[k + 1];
          if (c != 0x00 && !(c >= 0xD0 && c <= 0xD7) && c != 0xFF) { next = base + k; return false; }
          if (c == 0xFF) { i = k + 1; continue; }  // FF FF: 2 つ目が次の候補
          i = k + 2;
        }
        base += n;
        return true;
      });
      if (!ok || next == UINT64_MAX) return false;
      pos = next;
      continue;
    }
    pos += 2 + len;
  }
  return false;
}

// ------------------------------------------------------------------------------------------------ PNG
bool png_quick(const uint8_t* p, size_t) { return std::memcmp(p, "\x89PNG\r\n\x1a\n", 8) == 0; }

bool png_validate(CarveReader& r, uint64_t start, uint64_t max_len, Match* m) {
  uint64_t pos = start + 8;
  bool first = true, idat = false;
  unsigned w = 0, h = 0;
  for (int n = 0; n < 2000000; n++) {
    if (pos - start > max_len) return false;
    const uint8_t* hd = r.at(pos, 8);
    if (!hd) return false;
    uint32_t len = be32(hd);
    if (len > 0x7FFFFFFFu || !is_alpha4(hd + 4) || (hd[6] & 0x20)) return false;
    char type[4];
    std::memcpy(type, hd + 4, 4);
    if (first) {
      if (std::memcmp(type, "IHDR", 4) != 0 || len != 13) return false;
      const uint8_t* ih = r.at(pos + 8, 13);
      if (!ih) return false;
      w = be32(ih);
      h = be32(ih + 4);
      unsigned depth = ih[8], ct = ih[9];
      bool ok = w && h && w <= 0x7FFFFFFFu && h <= 0x7FFFFFFFu && ih[10] == 0 && ih[11] == 0 && ih[12] <= 1;
      switch (ct) {
        case 0: ok = ok && (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16); break;
        case 2: case 4: case 6: ok = ok && (depth == 8 || depth == 16); break;
        case 3: ok = ok && (depth == 1 || depth == 2 || depth == 4 || depth == 8); break;
        default: ok = false;
      }
      if (!ok) return false;
      first = false;
    } else if (!std::memcmp(type, "IHDR", 4)) {
      return false;
    }
    if (pos + 12 + len > r.limit()) return false;
    // CRC (チャンクの種別 + データ)
    uLong crc = ::crc32(0L, Z_NULL, 0);
    crc = ::crc32(crc, reinterpret_cast<const Bytef*>(type), 4);
    bool ok = r.stream(pos + 8, len, [&](const uint8_t* d, size_t k) { crc = ::crc32(crc, d, static_cast<uInt>(k)); return true; });
    if (!ok) return false;
    const uint8_t* cp = r.at(pos + 8 + len, 4);
    if (!cp || be32(cp) != static_cast<uint32_t>(crc)) return false;
    if (!std::memcmp(type, "IDAT", 4)) idat = true;
    pos += 12 + len;
    if (!std::memcmp(type, "IEND", 4)) {
      if (len != 0 || !idat) return false;
      m->length = pos - start;
      m->ext = "png";
      m->detail = std::to_string(w) + "x" + std::to_string(h);
      m->confidence = 98;
      return true;
    }
  }
  return false;
}

// ------------------------------------------------------------------------------------------------ GIF
bool gif_quick(const uint8_t* p, size_t) { return std::memcmp(p, "GIF87a", 6) == 0 || std::memcmp(p, "GIF89a", 6) == 0; }

// ブロック列 (長さ + データ ... 0) を読み飛ばして次の位置を返す。失敗は 0
uint64_t gif_skip_blocks(CarveReader& r, uint64_t pos, uint64_t stop) {
  for (int i = 0; i < 100000000; i++) {
    if (pos >= stop) return 0;
    const uint8_t* p = r.at(pos, 1);
    if (!p) return 0;
    uint64_t l = p[0];
    pos += 1 + l;
    if (l == 0) return pos;
  }
  return 0;
}

bool gif_validate(CarveReader& r, uint64_t start, uint64_t max_len, Match* m) {
  const uint8_t* h = r.at(start, 13);
  if (!h) return false;
  unsigned w = le16(h + 6), ht = le16(h + 8);
  if (!w || !ht) return false;
  uint64_t pos = start + 13;
  if (h[10] & 0x80) pos += 3ull << ((h[10] & 7) + 1);
  uint64_t stop = start + std::min<uint64_t>(max_len, r.limit() - start);
  int images = 0;
  for (int n = 0; n < 1000000; n++) {
    const uint8_t* p = r.at(pos, 1);
    if (!p || pos >= stop) return false;
    if (p[0] == 0x3B) {
      if (!images) return false;
      m->length = pos + 1 - start;
      m->ext = "gif";
      m->detail = std::to_string(w) + "x" + std::to_string(ht);
      m->confidence = 90;
      return true;
    }
    if (p[0] == 0x21) {
      if (!r.at(pos, 2)) return false;
      pos = gif_skip_blocks(r, pos + 2, stop);
      if (!pos) return false;
    } else if (p[0] == 0x2C) {
      const uint8_t* d = r.at(pos, 10);
      if (!d) return false;
      if (!le16(d + 5) || !le16(d + 7)) return false;
      pos += 10;
      if (d[9] & 0x80) pos += 3ull << ((d[9] & 7) + 1);
      const uint8_t* lz = r.at(pos, 1);
      if (!lz || lz[0] < 1 || lz[0] > 8) return false;
      pos = gif_skip_blocks(r, pos + 1, stop);
      if (!pos) return false;
      images++;
    } else {
      return false;
    }
  }
  return false;
}

// ------------------------------------------------------------------------------------------------ BMP
bool bmp_quick(const uint8_t* p, size_t) { return p[0] == 'B' && p[1] == 'M' && p[6] == 0 && p[7] == 0 && p[8] == 0 && p[9] == 0; }

bool bmp_validate(CarveReader& r, uint64_t start, uint64_t max_len, Match* m) {
  const uint8_t* h = r.at(start, 34);
  if (!h) return false;
  uint64_t fsize = le32(h + 2), off = le32(h + 10);
  uint32_t dib = le32(h + 14);
  if (fsize < 26 || fsize > max_len || start + fsize > r.limit()) return false;
  int64_t w, ht;
  unsigned planes, bpp, comp = 0;
  if (dib == 12) { w = le16(h + 18); ht = le16(h + 20); planes = le16(h + 22); bpp = le16(h + 24); }
  else if (dib == 40 || dib == 52 || dib == 56 || dib == 64 || dib == 108 || dib == 124) {
    w = static_cast<int32_t>(le32(h + 18));
    ht = static_cast<int32_t>(le32(h + 22));
    planes = le16(h + 26);
    bpp = le16(h + 28);
    comp = le32(h + 30);
  } else return false;
  if (ht < 0) ht = -ht;
  if (planes != 1 || w <= 0 || ht <= 0 || w > 65535 * 4 || ht > 65535 * 4) return false;
  if (!(bpp == 1 || bpp == 4 || bpp == 8 || bpp == 16 || bpp == 24 || bpp == 32)) return false;
  if (comp > 6 || off < 14 + dib || off >= fsize) return false;
  bool exact = false;
  if (comp == 0 || comp == 3) {
    uint64_t stride = ((static_cast<uint64_t>(w) * bpp + 31) / 32) * 4;
    uint64_t expect = off + stride * static_cast<uint64_t>(ht);
    if (fsize != expect) return false;
    exact = true;
  } else if (comp == 1 && bpp != 8) return false;
  else if (comp == 2 && bpp != 4) return false;
  m->length = fsize;
  m->ext = "bmp";
  m->detail = std::to_string(w) + "x" + std::to_string(ht);
  m->confidence = exact ? 90 : 60;
  return true;
}

// ------------------------------------------------------------------------------------------------ PDF
bool pdf_quick(const uint8_t* p, size_t) { return std::memcmp(p, "%PDF-", 5) == 0 && (p[5] == '1' || p[5] == '2') && p[6] == '.' && p[7] >= '0' && p[7] <= '9'; }

bool is_ws(uint8_t c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t' || c == '\f' || c == 0; }

// pos から "数字 数字 obj" が続くか (増分更新・リニア化 PDF の続き)
bool looks_like_obj(CarveReader& r, uint64_t pos) {
  const uint8_t* p = r.at(pos, 24);
  if (!p) {
    uint64_t left = r.limit() > pos ? r.limit() - pos : 0;
    if (left < 8) return false;
    p = r.at(pos, static_cast<size_t>(std::min<uint64_t>(left, 24)));
    if (!p) return false;
  }
  size_t i = 0, digits = 0;
  while (i < 24 && p[i] >= '0' && p[i] <= '9') { i++; digits++; }
  if (!digits || i >= 20 || p[i] != ' ') return false;
  i++;
  size_t d2 = 0;
  while (i < 24 && p[i] >= '0' && p[i] <= '9') { i++; d2++; }
  if (!d2 || i + 4 > 24 || p[i] != ' ') return false;
  return std::memcmp(p + i + 1, "obj", 3) == 0;
}

bool pdf_validate(CarveReader& r, uint64_t start, uint64_t max_len, Match* m) {
  uint64_t end_limit = std::min<uint64_t>(r.limit(), start + max_len);
  uint64_t search = start + 8;
  for (int round = 0; round < 10000; round++) {
    uint64_t e = find_pattern(r, search, end_limit - search, "%%EOF", 5);
    if (e == UINT64_MAX) return false;
    uint64_t after = e + 5;
    // 直後の改行
    uint64_t fin = after;
    const uint8_t* p = r.at(after, 2);
    if (p) { if (p[0] == '\r' && p[1] == '\n') fin = after + 2; else if (p[0] == '\n' || p[0] == '\r') fin = after + 1; }
    else if ((p = r.at(after, 1)) != nullptr && (p[0] == '\n' || p[0] == '\r')) fin = after + 1;
    // 増分更新が続くか
    uint64_t nxt = fin;
    while (nxt < end_limit && nxt - fin < 4) { const uint8_t* q = r.at(nxt, 1); if (!q || !is_ws(q[0])) break; nxt++; }
    if (looks_like_obj(r, fin) || looks_like_obj(r, nxt)) { search = after; continue; }
    // "startxref <n> %%EOF" と、その値が指す位置 (xref テーブルか xref ストリームのオブジェクト) を確かめる
    uint64_t back = std::min<uint64_t>(e - start, 80);
    const uint8_t* t = r.at(e - back, static_cast<size_t>(back));
    if (!t) return false;
    std::string tail(reinterpret_cast<const char*>(t), static_cast<size_t>(back));
    size_t sx = tail.rfind("startxref");
    if (sx == std::string::npos) return false;
    size_t i = sx + 9;
    while (i < tail.size() && is_ws(static_cast<uint8_t>(tail[i]))) i++;
    uint64_t v = 0;
    size_t nd = 0;
    while (i < tail.size() && tail[i] >= '0' && tail[i] <= '9' && nd < 15) { v = v * 10 + static_cast<uint64_t>(tail[i] - '0'); i++; nd++; }
    if (!nd || v >= e - start) return false;
    // startxref の値は xref テーブル ("xref") か xref ストリームのオブジェクト ("N G obj") を厳密に指す (署名だけの偽物が、遠くの本物の %%EOF に結びつくのを防ぐ)
    const uint8_t* x = r.at(start + v, 4);
    if (!x) return false;
    bool ok = std::memcmp(x, "xref", 4) == 0 || looks_like_obj(r, start + v);
    if (!ok) return false;
    m->length = fin - start;
    m->ext = "pdf";
    m->detail = std::string("PDF-") + static_cast<char>(r.at(start + 5, 1)[0]) + ".x";
    m->confidence = 90;
    return true;
  }
  return false;
}

// ------------------------------------------------------------------------------------------------ ZIP (docx / xlsx / pptx / jar / apk / odt / epub ...)
bool zip_quick(const uint8_t* p, size_t) { return p[0] == 'P' && p[1] == 'K' && p[2] == 3 && p[3] == 4; }

bool name_ok(const uint8_t* n, size_t len) {
  if (!len) return false;
  for (size_t i = 0; i < len; i++) if (n[i] == 0 || n[i] == 0x7F || (n[i] < 0x20)) return false;
  return true;
}

bool zip_validate(CarveReader& r, uint64_t start, uint64_t max_len, Match* m) {
  uint64_t stop = start + std::min<uint64_t>(max_len, r.limit() - start);
  uint64_t pos = start;
  std::vector<uint64_t> locals;
  std::string first_name;
  std::string first_data;
  for (int n = 0; n < 200000; n++) {
    const uint8_t* h = r.at(pos, 30);
    if (!h || std::memcmp(h, "PK\x03\x04", 4) != 0) break;
    unsigned flags = le16(h + 6), method = le16(h + 8);
    uint64_t csize = le32(h + 18), usize = le32(h + 22);
    unsigned nlen = le16(h + 26), elen = le16(h + 28);
    if (le16(h + 4) > 100 || method > 99) return false;
    const uint8_t* nm = r.at(pos + 30, nlen ? nlen : 1);
    if (!nm || !name_ok(nm, nlen)) return false;
    if (locals.empty()) first_name.assign(reinterpret_cast<const char*>(nm), nlen);
    bool zip64 = false;
    uint64_t data_start = pos + 30 + nlen + elen;
    if (elen) {
      std::vector<uint8_t> ex(elen);
      const uint8_t* e = r.at(pos + 30 + nlen, elen);
      if (!e) return false;
      std::memcpy(ex.data(), e, elen);
      for (size_t o = 0; o + 4 <= ex.size();) {
        unsigned id = le16(&ex[o]), sz = le16(&ex[o + 2]);
        if (o + 4 + sz > ex.size()) break;
        if (id == 1) {
          size_t q = o + 4;
          if (usize == 0xFFFFFFFFu && q + 8 <= o + 4 + sz) { usize = le64(&ex[q]); q += 8; zip64 = true; }
          if (csize == 0xFFFFFFFFu && q + 8 <= o + 4 + sz) { csize = le64(&ex[q]); zip64 = true; }
        }
        o += 4 + sz;
      }
    }
    locals.push_back(pos - start);
    if (flags & 8) {  // データ記述子: 圧縮サイズはデータの後ろ
      if (csize == 0 && method == 0) return false;
      uint64_t sig = data_start, found = UINT64_MAX;
      for (int tries = 0; tries < 100000 && sig < stop; tries++) {
        uint64_t s = find_pattern(r, sig, stop - sig, "PK\x07\x08", 4);
        if (s == UINT64_MAX) break;
        const uint8_t* d = r.at(s, 24 > stop - s ? static_cast<size_t>(stop - s) : 24);
        if (d && stop - s >= 16) {
          uint64_t cs = zip64 && stop - s >= 24 ? le64(d + 8) : le32(d + 8);
          uint64_t dlen = zip64 ? 24 : 16;
          if (cs == s - data_start) {
            const uint8_t* nx = r.at(s + dlen, 4);
            if (nx && (std::memcmp(nx, "PK\x03\x04", 4) == 0 || std::memcmp(nx, "PK\x01\x02", 4) == 0)) { found = s + dlen; break; }
          }
        }
        sig = s + 1;
      }
      if (found == UINT64_MAX) return false;
      pos = found;
    } else {
      pos = data_start + csize;
      if (pos > stop) return false;
    }
    if (locals.size() == 1 && first_name == "mimetype" && method == 0 && usize > 0 && usize < 128 && !(flags & 8)) {
      const uint8_t* d = r.at(data_start, static_cast<size_t>(usize));
      if (d) first_data.assign(reinterpret_cast<const char*>(d), static_cast<size_t>(usize));
    }
  }
  if (locals.empty()) return false;
  // 中央ディレクトリ
  uint64_t cd_start = pos;
  size_t idx = 0;
  bool has_word = false, has_xl = false, has_ppt = false, has_manifest = false, has_android = false, has_dex = false;
  std::vector<uint64_t> sorted = locals;
  std::sort(sorted.begin(), sorted.end());
  for (int n = 0; n < 200000; n++) {
    const uint8_t* h = r.at(pos, 46);
    if (!h || std::memcmp(h, "PK\x01\x02", 4) != 0) break;
    unsigned nlen = le16(h + 28), elen = le16(h + 30), clen = le16(h + 32);
    uint64_t loff = le32(h + 42);
    uint64_t csz = le32(h + 20);
    (void)csz;
    if (loff == 0xFFFFFFFFu && elen) {
      const uint8_t* e = r.at(pos + 46 + nlen, elen);
      if (e) {
        std::vector<uint8_t> ex(e, e + elen);
        uint64_t u = le32(h + 24);
        size_t q = 0;
        for (size_t o = 0; o + 4 <= ex.size();) {
          unsigned id = le16(&ex[o]), sz = le16(&ex[o + 2]);
          if (o + 4 + sz > ex.size()) break;
          if (id == 1) {
            q = o + 4;
            if (u == 0xFFFFFFFFu && q + 8 <= o + 4 + sz) q += 8;
            if (csz == 0xFFFFFFFFu && q + 8 <= o + 4 + sz) q += 8;
            if (q + 8 <= o + 4 + sz) loff = le64(&ex[q]);
          }
          o += 4 + sz;
        }
      }
    }
    if (!std::binary_search(sorted.begin(), sorted.end(), loff)) return false;
    const uint8_t* nm = r.at(pos + 46, nlen ? nlen : 1);
    if (!nm || !name_ok(nm, nlen)) return false;
    std::string name(reinterpret_cast<const char*>(nm), nlen);
    if (name.compare(0, 5, "word/") == 0) has_word = true;
    if (name.compare(0, 3, "xl/") == 0) has_xl = true;
    if (name.compare(0, 4, "ppt/") == 0) has_ppt = true;
    if (name == "META-INF/MANIFEST.MF") has_manifest = true;
    if (name == "AndroidManifest.xml") has_android = true;
    if (name == "classes.dex") has_dex = true;
    pos += 46 + nlen + elen + clen;
    idx++;
    if (pos > stop) return false;
  }
  if (idx == 0 || idx != locals.size()) return false;
  uint64_t cd_end = pos;
  // zip64 の終端レコード・ロケータ (あれば)
  const uint8_t* z = r.at(pos, 4);
  if (z && std::memcmp(z, "PK\x06\x06", 4) == 0) {
    const uint8_t* zh = r.at(pos, 12);
    if (!zh) return false;
    uint64_t sz = le64(zh + 4);
    if (sz > 4096) return false;
    pos += 12 + sz;
    const uint8_t* loc = r.at(pos, 4);
    if (!loc || std::memcmp(loc, "PK\x06\x07", 4) != 0) return false;
    pos += 20;
  }
  const uint8_t* e = r.at(pos, 22);
  if (!e || std::memcmp(e, "PK\x05\x06", 4) != 0) return false;
  unsigned total = le16(e + 10), clen = le16(e + 20);
  uint64_t cdsize = le32(e + 12), cdoff = le32(e + 16);
  if (total != 0xFFFF && total != idx) return false;
  if (cdsize != 0xFFFFFFFFu && cdsize != cd_end - cd_start) return false;
  if (cdoff != 0xFFFFFFFFu && cdoff != cd_start - start) return false;
  uint64_t end = pos + 22 + clen;
  if (end > stop) return false;
  m->length = end - start;
  m->confidence = 95;
  m->detail = std::to_string(idx) + " entries";
  if (has_word) m->ext = "docx";
  else if (has_xl) m->ext = "xlsx";
  else if (has_ppt) m->ext = "pptx";
  else if (has_android && has_dex) m->ext = "apk";
  else if (has_manifest) m->ext = "jar";
  else if (first_name == "mimetype" && first_data.compare(0, 39, "application/vnd.oasis.opendocument.text") == 0) m->ext = "odt";
  else if (first_name == "mimetype" && first_data.compare(0, 46, "application/vnd.oasis.opendocument.spreadsheet") == 0) m->ext = "ods";
  else if (first_name == "mimetype" && first_data.compare(0, 47, "application/vnd.oasis.opendocument.presentation") == 0) m->ext = "odp";
  else if (first_name == "mimetype" && first_data.compare(0, 20, "application/epub+zip") == 0) m->ext = "epub";
  else m->ext = "zip";
  return true;
}

// ------------------------------------------------------------------------------------------------ MP4 / MOV / 3GP / HEIC
bool mp4_quick(const uint8_t* p, size_t) {
  if (std::memcmp(p + 4, "ftyp", 4) != 0) return false;
  uint32_t sz = be32(p);
  return sz >= 16 && sz <= 4096 && (sz % 4) == 0;
}

bool mp4_known_top(const uint8_t* t) {
  static const char* kTop[] = {"ftyp", "moov", "mdat", "free", "skip", "wide", "uuid", "moof", "mfra", "sidx", "styp", "pnot", "pdin", "meta", "junk", "ssix", "prft", "emsg"};
  for (const char* k : kTop) if (std::memcmp(t, k, 4) == 0) return true;
  return false;
}

bool mp4_validate(CarveReader& r, uint64_t start, uint64_t max_len, Match* m) {
  uint64_t pos = start, stop = start + std::min<uint64_t>(max_len, r.limit() - start);
  bool moov = false, mdat = false, moof = false, first = true;
  std::string brand;
  uint64_t good_end = 0;
  for (int n = 0; n < 1000000; n++) {
    if (pos + 8 > stop) break;
    const uint8_t* h = r.at(pos, 16);
    if (!h) { h = r.at(pos, 8); if (!h) break; }
    uint64_t size = be32(h);
    const uint8_t* t = h + 4;
    if (!printable4(t) || !mp4_known_top(t)) break;
    uint64_t hdr = 8;
    if (size == 1) {
      const uint8_t* h2 = r.at(pos, 16);
      if (!h2) break;
      size = be64(h2 + 8);
      hdr = 16;
    } else if (size == 0) break;
    if (size < hdr || pos + size > stop) break;
    if (first) {
      if (std::memcmp(t, "ftyp", 4) != 0 || size < 16 || size > 4096) return false;
      const uint8_t* f = r.at(pos + 8, 4);
      if (!f || !printable4(f)) return false;
      brand.assign(reinterpret_cast<const char*>(f), 4);
      first = false;
    } else if (!std::memcmp(t, "ftyp", 4)) break;  // 次のファイル
    if (!std::memcmp(t, "moov", 4)) {
      const uint8_t* c = r.at(pos + hdr, 8);
      if (!c || std::memcmp(c + 4, "mvhd", 4) != 0) return false;
      moov = true;
    } else if (!std::memcmp(t, "mdat", 4)) mdat = true;
    else if (!std::memcmp(t, "moof", 4)) moof = true;
    pos += size;
    good_end = pos;
  }
  if (first || !good_end || !(moov || moof) || !(mdat || moof)) return false;
  m->length = good_end - start;
  m->confidence = 92;
  if (brand == "qt  ") m->ext = "mov";
  else if (brand == "M4A " || brand == "M4B ") m->ext = "m4a";
  else if (brand == "M4V " || brand == "M4VH" || brand == "M4VP") m->ext = "m4v";
  else if (brand.compare(0, 3, "3gp") == 0) m->ext = "3gp";
  else if (brand.compare(0, 3, "3g2") == 0) m->ext = "3g2";
  else if (brand == "heic" || brand == "heix" || brand == "mif1" || brand == "msf1" || brand == "hevc") m->ext = "heic";
  else if (brand == "avif") m->ext = "avif";
  else if (brand == "crx ") m->ext = "cr3";
  else m->ext = "mp4";
  m->detail = "brand " + brand;
  return true;
}

// ------------------------------------------------------------------------------------------------ MP3 (MPEG オーディオ)
struct Mpeg { int ver, layer; unsigned br, sr, len; };  // ver: 1 / 2 / 25

bool mpeg_header(const uint8_t* p, Mpeg* o) {
  if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return false;
  int vb = (p[1] >> 3) & 3, lb = (p[1] >> 1) & 3;
  if (vb == 1 || lb == 0) return false;
  int bi = p[2] >> 4, si = (p[2] >> 2) & 3, pad = (p[2] >> 1) & 1;
  if (bi == 0 || bi == 15 || si == 3) return false;
  static const unsigned v1[3][15] = {{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
                                     {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
                                     {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}};
  static const unsigned v2[3][15] = {{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
                                     {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
                                     {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}};
  static const unsigned sr1[3] = {44100, 48000, 32000};
  o->ver = vb == 3 ? 1 : vb == 2 ? 2 : 25;
  o->layer = 4 - lb;  // lb: 3 = layer I, 2 = II, 1 = III
  int li = o->layer - 1;
  o->br = (o->ver == 1 ? v1[li][bi] : v2[li][bi]) * 1000u;
  o->sr = o->ver == 1 ? sr1[si] : o->ver == 2 ? sr1[si] / 2 : sr1[si] / 4;
  if (o->layer == 1) o->len = (12 * o->br / o->sr + static_cast<unsigned>(pad)) * 4;
  else if (o->layer == 3 && o->ver != 1) o->len = 72 * o->br / o->sr + static_cast<unsigned>(pad);
  else o->len = 144 * o->br / o->sr + static_cast<unsigned>(pad);
  return o->len >= 24;
}

bool mp3_quick(const uint8_t* p, size_t) {
  if (std::memcmp(p, "ID3", 3) == 0) return (p[3] >= 2 && p[3] <= 4) && p[4] != 0xFF && p[6] < 0x80 && p[7] < 0x80 && p[8] < 0x80 && p[9] < 0x80;
  Mpeg h;
  return mpeg_header(p, &h) && h.layer >= 2;
}

bool mp3_validate(CarveReader& r, uint64_t start, uint64_t max_len, Match* m) {
  uint64_t pos = start, stop = start + std::min<uint64_t>(max_len, r.limit() - start);
  bool id3 = false;
  const uint8_t* p = r.at(pos, 10);
  if (!p) return false;
  if (std::memcmp(p, "ID3", 3) == 0) {
    id3 = true;
    uint64_t sz = (static_cast<uint64_t>(p[6]) << 21) | (p[7] << 14) | (p[8] << 7) | p[9];
    if ((p[5] & 0x0F) != 0 || sz > (64u << 20)) return false;
    pos += 10 + sz + ((p[5] & 0x10) ? 10 : 0);
    // タグの直後の詰め物 (ゼロ)
    for (int i = 0; i < 4096 && pos < stop; i++) { const uint8_t* z = r.at(pos, 1); if (!z || z[0] != 0) break; pos++; }
  }
  Mpeg first{};
  unsigned frames = 0;
  uint64_t last_end = pos;
  for (;;) {
    if (pos + 4 > stop) break;
    const uint8_t* h = r.at(pos, 4);
    Mpeg f;
    if (!h || !mpeg_header(h, &f)) break;
    if (frames == 0) { if (f.layer < 2) return false; first = f; }
    else if (f.ver != first.ver || f.layer != first.layer || f.sr != first.sr) break;
    if (pos + f.len > stop) break;
    pos += f.len;
    last_end = pos;
    frames++;
  }
  if (frames < (id3 ? 8u : 24u)) return false;
  uint64_t end = last_end;
  const uint8_t* t = r.at(end, 3);
  if (t && std::memcmp(t, "TAG", 3) == 0 && end + 128 <= stop) end += 128;
  m->length = end - start;
  m->ext = first.layer == 3 ? "mp3" : "mp2";
  m->detail = std::to_string(frames) + " frames, " + std::to_string(first.sr) + " Hz";
  m->confidence = id3 ? 90 : 75;
  return true;
}

// ------------------------------------------------------------------------------------------------ RIFF (WAV / AVI / WebP)
bool riff_quick(const uint8_t* p, size_t) {
  if (std::memcmp(p, "RIFF", 4) != 0) return false;
  return std::memcmp(p + 8, "WAVE", 4) == 0 || std::memcmp(p + 8, "AVI ", 4) == 0 || std::memcmp(p + 8, "WEBP", 4) == 0;
}

bool riff_validate(CarveReader& r, uint64_t start, uint64_t max_len, Match* m) {
  const uint8_t* h = r.at(start, 12);
  if (!h) return false;
  uint64_t total = static_cast<uint64_t>(le32(h + 4)) + 8;
  if (le32(h + 4) < 4 + 8 || le32(h + 4) == 0xFFFFFFFFu || total > max_len || start + total > r.limit()) return false;
  std::string form(reinterpret_cast<const char*>(h + 8), 4);
  uint64_t pos = start + 12, end = start + total;
  bool fmt = false, data = false, first = true, movi = false, hdrl = false;
  for (int n = 0; n < 1000000 && pos < end; n++) {
    const uint8_t* c = r.at(pos, 12 > end - pos ? static_cast<size_t>(end - pos) : 12);
    if (!c || end - pos < 8 || !printable4(c)) return false;
    uint64_t sz = le32(c + 4);
    if (pos + 8 + sz > end) return false;
    bool list = std::memcmp(c, "LIST", 4) == 0;
    if (form == "WAVE") {
      if (first && std::memcmp(c, "fmt ", 4) != 0) return false;
      if (!std::memcmp(c, "fmt ", 4)) {
        if (sz != 16 && sz != 18 && sz != 20 && sz != 40) return false;
        const uint8_t* f = r.at(pos + 8, 16);
        if (!f) return false;
        unsigned ch = le16(f + 2), rate = le32(f + 4), align = le16(f + 12), bits = le16(f + 14);
        if (!ch || ch > 32 || !rate || rate > 768000 || !align || bits > 64) return false;
        fmt = true;
      } else if (!std::memcmp(c, "data", 4)) data = true;
    } else if (form == "AVI ") {
      if (first && !(list && end - pos >= 12 && std::memcmp(c + 8, "hdrl", 4) == 0)) return false;
      if (list && std::memcmp(c + 8, "hdrl", 4) == 0) hdrl = true;
      if (list && std::memcmp(c + 8, "movi", 4) == 0) movi = true;
    } else {  // WEBP
      if (first && std::memcmp(c, "VP8 ", 4) != 0 && std::memcmp(c, "VP8L", 4) != 0 && std::memcmp(c, "VP8X", 4) != 0) return false;
    }
    first = false;
    pos += 8 + sz + (sz & 1);
  }
  if (pos != end && pos != end + 1) return false;  // 最後のチャンクが奇数長なら詰め物が 1 バイト
  if (form == "WAVE") { if (!fmt || !data) return false; m->ext = "wav"; }
  else if (form == "AVI ") { if (!hdrl || !movi) return false; m->ext = "avi"; }
  else m->ext = "webp";
  m->length = total;
  m->detail = form;
  m->confidence = 92;
  return true;
}

// ------------------------------------------------------------------------------------------------ 7z
bool sz_quick(const uint8_t* p, size_t) { return std::memcmp(p, "7z\xBC\xAF\x27\x1C", 6) == 0 && p[6] == 0 && p[7] <= 4; }

bool sz_validate(CarveReader& r, uint64_t start, uint64_t max_len, Match* m) {
  const uint8_t* h = r.at(start, 32);
  if (!h) return false;
  uint32_t scrc = le32(h + 8);
  if (static_cast<uint32_t>(::crc32(::crc32(0L, Z_NULL, 0), h + 12, 20)) != scrc) return false;
  uint64_t nho = le64(h + 12), nhs = le64(h + 20);
  uint32_t nhc = le32(h + 28);
  if (nhs == 0 || nhs > (64ull << 20) || nho > max_len || 32 + nho + nhs > max_len || start + 32 + nho + nhs > r.limit()) return false;
  uLong crc = ::crc32(0L, Z_NULL, 0);
  if (!r.stream(start + 32 + nho, nhs, [&](const uint8_t* d, size_t n) { crc = ::crc32(crc, d, static_cast<uInt>(n)); return true; })) return false;
  if (static_cast<uint32_t>(crc) != nhc) return false;
  m->length = 32 + nho + nhs;
  m->ext = "7z";
  m->detail = "";
  m->confidence = 98;
  return true;
}

// ------------------------------------------------------------------------------------------------ gzip
bool gz_quick(const uint8_t* p, size_t) { return p[0] == 0x1F && p[1] == 0x8B && p[2] == 8 && (p[3] & 0xE0) == 0 && (p[8] == 0 || p[8] == 2 || p[8] == 4) && (p[9] <= 13 || p[9] == 255); }

bool gz_validate(CarveReader& r, uint64_t start, uint64_t max_len, Match* m) {
  const uint8_t* h = r.at(start, 10);
  if (!h) return false;
  unsigned flg = h[3];
  uint64_t pos = start + 10;
  if (flg & 4) {
    const uint8_t* x = r.at(pos, 2);
    if (!x) return false;
    pos += 2 + le16(x);
  }
  for (int k = 0; k < 2; k++) {
    if (!(flg & (k == 0 ? 8 : 16))) continue;
    int i = 0;
    for (; i < 4096; i++) { const uint8_t* c = r.at(pos++, 1); if (!c) return false; if (c[0] == 0) break; }
    if (i >= 4096) return false;
  }
  if (flg & 2) pos += 2;
  if (pos >= start + max_len || pos >= r.limit()) return false;
  z_stream z;
  std::memset(&z, 0, sizeof z);
  if (inflateInit2(&z, -15) != Z_OK) return false;
  std::vector<uint8_t> out(64u << 10);
  uLong crc = ::crc32(0L, Z_NULL, 0);
  uint64_t produced = 0, consumed_total = 0;
  bool end_ok = false, bad = false;
  uint64_t span = std::min<uint64_t>(r.limit() - pos, start + max_len - pos);
  bool read_ok = r.stream(pos, span, [&](const uint8_t* d, size_t n) {
    z.next_in = const_cast<Bytef*>(d);
    z.avail_in = static_cast<uInt>(n);
    while (z.avail_in > 0) {
      z.next_out = out.data();
      z.avail_out = static_cast<uInt>(out.size());
      int rc = inflate(&z, Z_NO_FLUSH);
      size_t got = out.size() - z.avail_out;
      crc = ::crc32(crc, out.data(), static_cast<uInt>(got));
      produced += got;
      if (produced > (16ull << 30)) { bad = true; return false; }
      if (rc == Z_STREAM_END) { end_ok = true; consumed_total += n - z.avail_in; return false; }
      if (rc != Z_OK && rc != Z_BUF_ERROR) { bad = true; return false; }
      if (rc == Z_BUF_ERROR && z.avail_in > 0 && got == 0) { bad = true; return false; }
    }
    consumed_total += n;
    return true;
  });
  inflateEnd(&z);
  if (!read_ok || bad || !end_ok) return false;
  uint64_t tpos = pos + consumed_total;
  const uint8_t* t = r.at(tpos, 8);
  if (!t || tpos + 8 > start + max_len) return false;
  if (le32(t) != static_cast<uint32_t>(crc) || le32(t + 4) != static_cast<uint32_t>(produced)) return false;
  m->length = tpos + 8 - start;
  m->ext = "gz";
  m->detail = std::to_string(produced) + " bytes uncompressed";
  m->confidence = 98;
  return true;
}

// ------------------------------------------------------------------------------------------------ SQLite
bool sqlite_quick(const uint8_t* p, size_t) { return std::memcmp(p, "SQLite format 3", 16) == 0; }

bool sqlite_validate(CarveReader& r, uint64_t start, uint64_t max_len, Match* m) {
  const uint8_t* h = r.at(start, 100);
  if (!h) return false;
  uint32_t ps = be16(h + 16);
  if (ps == 1) ps = 65536;
  if (ps < 512 || ps > 65536 || (ps & (ps - 1))) return false;
  if (h[18] > 2 || h[18] == 0 || h[19] > 2 || h[19] == 0) return false;
  if (h[21] != 64 || h[22] != 32 || h[23] != 32) return false;
  uint32_t pages = be32(h + 28), counter = be32(h + 24), valid_for = be32(h + 92);
  if (!pages || counter != valid_for) return false;  // ページ数が信用できない (古い SQLite が書いたもの)
  uint32_t enc = be32(h + 56);
  if (enc < 1 || enc > 3) return false;
  const uint8_t* b = r.at(start + 100, 1);
  if (!b || !(b[0] == 2 || b[0] == 5 || b[0] == 10 || b[0] == 13)) return false;
  uint64_t len = static_cast<uint64_t>(pages) * ps;
  if (len > max_len || start + len > r.limit()) return false;
  m->length = len;
  m->ext = "sqlite";
  m->detail = std::to_string(pages) + " pages x " + std::to_string(ps);
  m->confidence = 93;
  return true;
}

// ------------------------------------------------------------------------------------------------ PE (EXE / DLL)
bool pe_quick(const uint8_t* p, size_t) { return p[0] == 'M' && p[1] == 'Z' && le32(p + 60) >= 64 && le32(p + 60) <= (1u << 20) && (le32(p + 60) & 3) == 0; }

bool pe_validate(CarveReader& r, uint64_t start, uint64_t max_len, Match* m) {
  const uint8_t* d = r.at(start, 64);
  if (!d) return false;
  uint64_t lfanew = le32(d + 60);
  const uint8_t* pe = r.at(start + lfanew, 24);
  if (!pe || std::memcmp(pe, "PE\0\0", 4) != 0) return false;
  unsigned machine = le16(pe + 4), nsec = le16(pe + 6), osz = le16(pe + 20), chars = le16(pe + 22);
  static const unsigned kMachines[] = {0x14c, 0x8664, 0x1c0, 0x1c4, 0xaa64, 0x200, 0x1f0, 0x166, 0xebc, 0x5032, 0x5064};
  bool mok = false;
  for (unsigned k : kMachines) mok = mok || k == machine;
  if (!mok || !nsec || nsec > 96 || osz < 96 || osz > 1024) return false;
  const uint8_t* opt = r.at(start + lfanew + 24, osz);
  if (!opt) return false;
  unsigned magic = le16(opt);
  bool p32p = magic == 0x20B;
  if (magic != 0x10B && !p32p) return false;
  uint32_t falign = le32(opt + 36), hdrs = le32(opt + 60);
  if (falign < 512 || falign > 65536 || (falign & (falign - 1))) return false;
  uint64_t end = hdrs;
  size_t dd = p32p ? 112 : 96;
  if (osz >= dd + 8 * 5) {
    uint32_t nrva = le32(opt + dd - 4);
    if (nrva >= 5) {
      uint64_t co = le32(opt + dd + 32), cs = le32(opt + dd + 36);
      if (co && cs) end = std::max<uint64_t>(end, co + cs);  // 署名 (WIN_CERTIFICATE) は本体の後ろ
    }
  }
  const uint8_t* sec = r.at(start + lfanew + 24 + osz, static_cast<size_t>(nsec) * 40);
  if (!sec) return false;
  uint64_t data_end = 0;
  for (unsigned i = 0; i < nsec; i++) {
    const uint8_t* s = sec + 40 * i;
    for (int k = 0; k < 8; k++) if (s[k] != 0 && (s[k] < 0x20 || s[k] > 0x7E)) return false;
    uint64_t rsz = le32(s + 16), rptr = le32(s + 20);
    if (rsz == 0) continue;
    if (rptr < hdrs || (rptr % 512) != 0) return false;
    data_end = std::max<uint64_t>(data_end, rptr + rsz);
  }
  if (!data_end) return false;
  end = std::max(end, data_end);
  if (end > max_len || start + end > r.limit() || end < 512) return false;
  m->length = end;
  m->ext = (chars & 0x2000) ? "dll" : (le16(opt + 68) == 1 ? "sys" : "exe");
  m->detail = p32p ? "PE32+" : "PE32";
  m->confidence = 85;
  return true;
}

}  // namespace

const std::vector<FormatDef>& formats() {
  static const std::vector<FormatDef> k = {
      {"jpeg", "jpg", "JPEG image", jpeg_quick, jpeg_validate, 125},
      {"png", "png", "PNG image", png_quick, png_validate, 45},
      {"gif", "gif", "GIF image", gif_quick, gif_validate, 20},
      {"bmp", "bmp", "BMP image", bmp_quick, bmp_validate, 30},
      {"pdf", "pdf", "PDF document", pdf_quick, pdf_validate, 64},
      {"zip", "zip,docx,xlsx,pptx,jar,apk,odt,ods,odp,epub", "ZIP archive and ZIP-based documents", zip_quick, zip_validate, 100},
      {"mp4", "mp4,mov,m4a,m4v,3gp,3g2,heic,avif,cr3", "MP4 / QuickTime / HEIF media", mp4_quick, mp4_validate, 32},
      {"mp3", "mp3,mp2", "MPEG audio", mp3_quick, mp3_validate, 512},
      {"riff", "wav,avi,webp", "WAV / AVI / WebP (RIFF)", riff_quick, riff_validate, 44},
      {"7z", "7z", "7-Zip archive", sz_quick, sz_validate, 32},
      {"gzip", "gz", "gzip compressed data", gz_quick, gz_validate, 20},
      {"sqlite", "sqlite", "SQLite database", sqlite_quick, sqlite_validate, 512},
      {"pe", "exe,dll,sys", "Windows executable (PE)", pe_quick, pe_validate, 512},
  };
  return k;
}

}  // namespace carve
}  // namespace usbmem
