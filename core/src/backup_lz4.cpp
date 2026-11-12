// SPDX-License-Identifier: Apache-2.0
// LZ4 フレーム形式 (https://github.com/lz4/lz4/blob/dev/doc/lz4_Frame_format.md) の圧縮・展開。自前の実装で、外部ライブラリに依存しない。
// 書き出す形式は lz4 コマンド (`lz4 -d`) で展開できる。Partclone の画像は `partclone.* | lz4` と同じ並びで圧縮できる。
// 読み取りは壊れた入力に強くする (すべての長さ・オフセットを検査し、出力の上限を超えない)。
#include <algorithm>
#include <cstring>

#include "backup_internal.h"

namespace usbmem {
namespace {
using Bytes = std::vector<uint8_t>;

constexpr uint32_t kMagic = 0x184D2204u;
constexpr size_t kBlockMax = 1u << 20;  // 書くブロックの最大 (BD = 6 → 1 MiB)
constexpr size_t kMaxBlockRead = 4u << 20;

// ---- xxHash32
constexpr uint32_t P1 = 2654435761u, P2 = 2246822519u, P3 = 3266489917u, P4 = 668265263u, P5 = 374761393u;
inline uint32_t rotl(uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }
inline uint32_t rd32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24); }
struct Xxh32 {
  uint32_t v[4], total = 0, mem[4] = {0, 0, 0, 0};
  uint32_t seed;
  size_t memsize = 0;
  uint64_t len = 0;
  explicit Xxh32(uint32_t s = 0) : seed(s) { v[0] = s + P1 + P2; v[1] = s + P2; v[2] = s; v[3] = s - P1; }
  static uint32_t round(uint32_t acc, uint32_t in) { return rotl(acc + in * P2, 13) * P1; }
  void update(const uint8_t* p, size_t n) {
    len += n;
    uint8_t* m = reinterpret_cast<uint8_t*>(mem);
    if (memsize + n < 16) { std::memcpy(m + memsize, p, n); memsize += n; return; }
    if (memsize) {
      size_t fill = 16 - memsize;
      std::memcpy(m + memsize, p, fill);
      for (int i = 0; i < 4; i++) v[i] = round(v[i], rd32(m + 4 * i));
      p += fill; n -= fill; memsize = 0;
    }
    while (n >= 16) { for (int i = 0; i < 4; i++) v[i] = round(v[i], rd32(p + 4 * i)); p += 16; n -= 16; }
    if (n) { std::memcpy(m, p, n); memsize = n; }
  }
  uint32_t digest() const {
    uint32_t h = len >= 16 ? rotl(v[0], 1) + rotl(v[1], 7) + rotl(v[2], 12) + rotl(v[3], 18) : seed + P5;
    h += static_cast<uint32_t>(len);
    const uint8_t* p = reinterpret_cast<const uint8_t*>(mem);
    size_t n = memsize;
    while (n >= 4) { h = rotl(h + rd32(p) * P3, 17) * P4; p += 4; n -= 4; }
    while (n) { h = rotl(h + *p * P5, 11) * P1; p++; n--; }
    h ^= h >> 15; h *= P2; h ^= h >> 13; h *= P3; h ^= h >> 16;
    return h;
  }
};
uint32_t xxh32(const uint8_t* p, size_t n) { Xxh32 x(0); x.update(p, n); return x.digest(); }

// ---- ブロック圧縮 (貪欲法)
void emit_len(Bytes& o, size_t len) {
  while (len >= 255) { o.push_back(255); len -= 255; }
  o.push_back(static_cast<uint8_t>(len));
}
void compress_block(const uint8_t* src, size_t n, Bytes* out) {
  out->clear();
  const size_t kMfLimit = 12, kLastLiterals = 5;
  static thread_local std::vector<int32_t> table;
  constexpr int kHashLog = 14;
  table.assign(size_t(1) << kHashLog, -1);
  size_t anchor = 0, ip = 0;
  if (n >= kMfLimit + 1) {
    const size_t match_limit = n - kLastLiterals, mf_end = n - kMfLimit;
    while (ip < mf_end) {
      uint32_t seq = rd32(src + ip);
      uint32_t h = (seq * 2654435761u) >> (32 - kHashLog);
      int32_t ref = table[h];
      table[h] = static_cast<int32_t>(ip);
      if (ref < 0 || ip - static_cast<size_t>(ref) > 65535 || rd32(src + ref) != seq) { ip++; continue; }
      size_t mlen = 4;
      while (ip + mlen < match_limit && src[ref + mlen] == src[ip + mlen]) mlen++;
      size_t lit = ip - anchor;
      uint8_t token = static_cast<uint8_t>((lit >= 15 ? 15 : lit) << 4);
      size_t ml = mlen - 4;
      token |= static_cast<uint8_t>(ml >= 15 ? 15 : ml);
      out->push_back(token);
      if (lit >= 15) emit_len(*out, lit - 15);
      out->insert(out->end(), src + anchor, src + ip);
      size_t off = ip - static_cast<size_t>(ref);
      out->push_back(static_cast<uint8_t>(off));
      out->push_back(static_cast<uint8_t>(off >> 8));
      if (ml >= 15) emit_len(*out, ml - 15);
      ip += mlen;
      anchor = ip;
    }
  }
  size_t lit = n - anchor;
  out->push_back(static_cast<uint8_t>((lit >= 15 ? 15 : lit) << 4));
  if (lit >= 15) emit_len(*out, lit - 15);
  out->insert(out->end(), src + anchor, src + n);
}

// ---- ブロック展開 (境界を検査)。dst[0, prefix) は直前までの履歴 (連鎖ブロック用)。出力は dst[prefix, prefix+*out_len)。
bool decompress_block(const uint8_t* in, size_t n, uint8_t* dst, size_t prefix, size_t cap, size_t* out_len) {
  size_t ip = 0, op = prefix;
  const size_t limit = prefix + cap;
  while (ip < n) {
    uint8_t token = in[ip++];
    size_t lit = token >> 4;
    if (lit == 15) {
      uint8_t b;
      do {
        if (ip >= n) return false;
        b = in[ip++];
        lit += b;
        if (lit > limit) return false;
      } while (b == 255);
    }
    if (lit > n - ip || lit > limit - op) return false;
    std::memcpy(dst + op, in + ip, lit);
    ip += lit; op += lit;
    if (ip == n) break;  // 最後の系列はリテラルのみ
    if (n - ip < 2) return false;
    size_t off = in[ip] | (static_cast<size_t>(in[ip + 1]) << 8);
    ip += 2;
    if (off == 0 || off > op) return false;
    size_t ml = token & 15;
    if (ml == 15) {
      uint8_t b;
      do {
        if (ip >= n) return false;
        b = in[ip++];
        ml += b;
        if (ml > limit) return false;
      } while (b == 255);
    }
    ml += 4;
    if (ml > limit - op) return false;
    for (size_t i = 0; i < ml; i++, op++) dst[op] = dst[op - off];
  }
  *out_len = op - prefix;
  return true;
}

// ================================================================================================ 書き出し
class Lz4Sink final : public ByteSink {
 public:
  explicit Lz4Sink(ByteSink* in) : in_(in) {}
  Status write(const void* d, size_t n) override {
    if (!started_) { Status s = start(); if (!s.ok()) return s; }
    const uint8_t* p = static_cast<const uint8_t*>(d);
    while (n) {
      size_t take = std::min(n, kBlockMax - buf_.size());
      buf_.insert(buf_.end(), p, p + take);
      p += take; n -= take;
      if (buf_.size() == kBlockMax) { Status s = flush_block(); if (!s.ok()) return s; }
    }
    return Status::Ok();
  }
  Status finish() override {
    if (!started_) { Status s = start(); if (!s.ok()) return s; }
    if (!buf_.empty()) { Status s = flush_block(); if (!s.ok()) return s; }
    uint8_t z[4] = {0, 0, 0, 0};
    Status s = in_->write(z, 4);  // 終端マーク (内容チェックサムなし)
    if (!s.ok()) return s;
    return in_->finish();
  }
 private:
  Status start() {
    started_ = true;
    uint8_t h[7];
    h[0] = kMagic & 0xFF; h[1] = (kMagic >> 8) & 0xFF; h[2] = (kMagic >> 16) & 0xFF; h[3] = kMagic >> 24;
    h[4] = 0x60;  // version 01, ブロック独立, チェックサムなし
    h[5] = 0x60;  // 最大ブロック 1 MiB
    h[6] = static_cast<uint8_t>((xxh32(h + 4, 2) >> 8) & 0xFF);
    return in_->write(h, 7);
  }
  Status flush_block() {
    compress_block(buf_.data(), buf_.size(), &c_);
    uint8_t hdr[4];
    uint32_t sz;
    const Bytes* body;
    if (c_.size() < buf_.size()) { sz = static_cast<uint32_t>(c_.size()); body = &c_; }
    else { sz = static_cast<uint32_t>(buf_.size()) | 0x80000000u; body = &buf_; }
    for (int i = 0; i < 4; i++) hdr[i] = static_cast<uint8_t>(sz >> (8 * i));
    Status s = in_->write(hdr, 4);
    if (s.ok()) s = in_->write(body->data(), body->size());
    buf_.clear();
    return s;
  }
  ByteSink* in_;
  bool started_ = false;
  Bytes buf_, c_;
};

// ================================================================================================ 読み取り
class Lz4Source final : public ByteSource {
 public:
  explicit Lz4Source(ByteSource* in) : in_(in) {}
  Status read(void* buf, size_t len, size_t* got) override {
    uint8_t* o = static_cast<uint8_t*>(buf);
    size_t done = 0;
    while (done < len) {
      if (cur_ >= cur_end_) {
        if (eof_) break;
        Status s = next_block();
        if (!s.ok()) return s;
        continue;
      }
      size_t take = std::min(len - done, cur_end_ - cur_);
      std::memcpy(o + done, out_.data() + cur_, take);
      cur_ += take; done += take;
    }
    *got = done;
    return Status::Ok();
  }
 private:
  Status corrupt() { return Status::Make(Errc::Corrupt); }
  Status rd(void* p, size_t n, bool allow_eof_at_start, bool* was_eof) {
    size_t got = 0;
    uint8_t* b = static_cast<uint8_t*>(p);
    size_t done = 0;
    while (done < n) {
      Status s = in_->read(b + done, n - done, &got);
      if (!s.ok()) return s;
      if (got == 0) {
        if (allow_eof_at_start && done == 0) { *was_eof = true; return Status::Ok(); }
        return corrupt();
      }
      done += got;
    }
    return Status::Ok();
  }
  Status header() {
    uint8_t m[4];
    bool e = false;
    Status s = rd(m, 4, false, &e);
    if (!s.ok()) return s;
    if (rd32(m) != kMagic) return Status::Make(Errc::Corrupt);
    uint8_t d[2];
    s = rd(d, 2, false, &e);
    if (!s.ok()) return s;
    uint8_t flg = d[0], bd = d[1];
    if ((flg >> 6) != 1 || (flg & 0x02) || (bd & 0x8F)) return corrupt();
    if (flg & 0x01) return Status::Make(Errc::Unsupported);  // 辞書 ID
    indep_ = (flg & 0x20) != 0;
    block_ck_ = (flg & 0x10) != 0;
    content_ck_ = (flg & 0x04) != 0;
    Bytes desc = {flg, bd};
    if (flg & 0x08) { uint8_t cs[8]; s = rd(cs, 8, false, &e); if (!s.ok()) return s; desc.insert(desc.end(), cs, cs + 8); }
    uint8_t hc;
    s = rd(&hc, 1, false, &e);
    if (!s.ok()) return s;
    if (hc != ((xxh32(desc.data(), desc.size()) >> 8) & 0xFF)) return corrupt();
    int id = (bd >> 4) & 7;
    if (id < 4) return corrupt();
    block_max_ = size_t(1) << (8 + 2 * id);  // 64 KiB, 256 KiB, 1 MiB, 4 MiB
    if (block_max_ > kMaxBlockRead) return corrupt();
    return Status::Ok();
  }
  Status next_block() {
    if (!started_) { Status s = header(); if (!s.ok()) return s; started_ = true; }
    uint8_t h[4];
    bool e = false;
    Status s = rd(h, 4, false, &e);
    if (!s.ok()) return s;
    uint32_t sz = rd32(h);
    if (sz == 0) {  // 終端
      if (content_ck_) { uint8_t c[4]; s = rd(c, 4, false, &e); if (!s.ok()) return s; if (rd32(c) != xx_.digest()) return corrupt(); }
      eof_ = true;
      cur_ = cur_end_ = 0;
      return Status::Ok();
    }
    bool raw = (sz & 0x80000000u) != 0;
    sz &= 0x7FFFFFFFu;
    if (sz > block_max_) return corrupt();
    comp_.resize(sz);
    s = rd(comp_.data(), sz, false, &e);
    if (!s.ok()) return s;
    if (block_ck_) { uint8_t c[4]; s = rd(c, 4, false, &e); if (!s.ok()) return s; if (rd32(c) != xxh32(comp_.data(), comp_.size())) return corrupt(); }
    // 履歴 (連鎖ブロックのとき直前の 64 KiB) を前に置いて展開する
    size_t prefix = 0;
    if (!indep_ && !hist_.empty()) prefix = hist_.size();
    out_.resize(prefix + block_max_);
    if (prefix) std::memcpy(out_.data(), hist_.data(), prefix);
    size_t produced = 0;
    if (raw) {
      if (sz > block_max_) return corrupt();
      std::memcpy(out_.data() + prefix, comp_.data(), sz);
      produced = sz;
    } else if (!decompress_block(comp_.data(), comp_.size(), out_.data(), prefix, block_max_, &produced)) {
      return corrupt();
    }
    if (content_ck_) xx_.update(out_.data() + prefix, produced);
    if (!indep_) {  // 次のブロックのために末尾 64 KiB を残す
      size_t total = prefix + produced;
      size_t keep = std::min<size_t>(total, 65536);
      hist_.assign(out_.begin() + static_cast<std::ptrdiff_t>(total - keep), out_.begin() + static_cast<std::ptrdiff_t>(total));
    }
    cur_ = prefix;
    cur_end_ = prefix + produced;
    return Status::Ok();
  }
  ByteSource* in_;
  bool started_ = false, eof_ = false, indep_ = true, block_ck_ = false, content_ck_ = false;
  size_t block_max_ = 0, cur_ = 0, cur_end_ = 0;
  Bytes comp_, out_, hist_;
  Xxh32 xx_{0};
};
}  // namespace

std::unique_ptr<ByteSink> make_lz4_sink(ByteSink* inner) { return std::unique_ptr<ByteSink>(new Lz4Sink(inner)); }
std::unique_ptr<ByteSource> make_lz4_source(ByteSource* inner) { return std::unique_ptr<ByteSource>(new Lz4Source(inner)); }
bool looks_like_lz4(const uint8_t* h) { return h[0] == 0x04 && h[1] == 0x22 && h[2] == 0x4D && h[3] == 0x18; }

}  // namespace usbmem
