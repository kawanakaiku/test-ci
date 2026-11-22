// SPDX-License-Identifier: Apache-2.0
// Zstandard (RFC 8878) の展開器。自前の実装: 形式の説明 (RFC 8878) だけから書いた。upstream の zstd のコードは見ていない・写していない。
// 1 回の呼び出しで、フレーム 1 つを呼び出し側の出力バッファへ展開する (出力バッファがそのまま窓の履歴になる)。辞書は未対応。
// 入力は信用しない: すべての長さ・距離・テーブルの検査をし、範囲外を読み書きしない。チェックサム (XXH64) の確認はしない (呼び出し側 = Btrfs / F2FS が自分のチェックサムを持つ)。
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

#include "usbmem/decomp.h"

namespace {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

constexpr u32 kMagic = 0xFD2FB528u;
constexpr u32 kSkipMagicLo = 0x184D2A50u, kSkipMagicMask = 0xFFFFFFF0u;
constexpr size_t kBlockMax = 128 * 1024;   // ブロックの大きさ / 1 ブロックが展開する長さの上限
constexpr unsigned kMaxHufBits = 12;
constexpr unsigned kMaxLL = 35, kMaxML = 52, kMaxOF = 31;

inline unsigned highbit(u32 v) { return 31u - static_cast<unsigned>(__builtin_clz(v)); }   // v != 0
// 長さ 0 のときはポインタに触らない (空の vector の data() は null のことがある)
inline void mcopy(void* d, const void* s, size_t n) { if (n) std::memcpy(d, s, n); }
inline void mfill(void* d, int v, size_t n) { if (n) std::memset(d, v, n); }

inline u32 le16(const u8* p) { return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8); }
inline u32 le24(const u8* p) { return le16(p) | (static_cast<u32>(p[2]) << 16); }
inline u32 le32(const u8* p) { return le24(p) | (static_cast<u32>(p[3]) << 24); }
inline u64 le64(const u8* p) { return le32(p) | (static_cast<u64>(le32(p + 4)) << 32); }

// 先頭から 64 ビット (リトルエンディアン) を読む。入力の終わりを越える分は 0
inline u64 load64(const u8* buf, size_t len, size_t at) {
  if (at >= len) return 0;
  if (len - at >= 8) { u64 v; mcopy(&v, buf + at, 8); return __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ ? v : __builtin_bswap64(v); }
  u64 v = 0;
  for (size_t i = 0; i < len - at; i++) v |= static_cast<u64>(buf[at + i]) << (8 * i);
  return v;
}

// 後ろから前へ読むビット列。最後のバイトの最上位の 1 ビットは終端の印で、その下から先頭へ向かって読む。
// avail = まだ読んでいないビット数。先頭を越えて読むと負になる (呼び出し側が「溢れ」として検査する)。越えた分のビットは 0。
struct RevBits {
  const u8* buf = nullptr;
  size_t len = 0;
  int64_t avail = 0;
  bool init(const u8* p, size_t n) {
    if (n == 0 || p[n - 1] == 0) return false;
    buf = p; len = n;
    avail = static_cast<int64_t>(n - 1) * 8 + highbit(p[n - 1]);
    return true;
  }
  // 次に読む nb (<= 56) ビット。最初に読むビットが最上位。先頭より前のビットは 0
  u64 peek(unsigned nb) const {
    if (nb == 0) return 0;
    const u64 mask = (u64(1) << nb) - 1;
    const int64_t start = avail - static_cast<int64_t>(nb);
    if (start >= 0) return (load64(buf, len, static_cast<size_t>(start >> 3)) >> (start & 7)) & mask;
    if (avail <= 0) return 0;
    return (load64(buf, len, 0) & ((u64(1) << avail) - 1)) << (-start);
  }
  u64 read(unsigned nb) { u64 v = peek(nb); avail -= nb; return v; }
};

// 前から読むビット列 (FSE の分布の記述用。下位ビットから)
struct FwdBits {
  const u8* buf; size_t len; size_t pos = 0;   // pos = 読んだビット数
  FwdBits(const u8* p, size_t n) : buf(p), len(n) {}
  u32 peek(unsigned nb) const { return static_cast<u32>((load64(buf, len, pos >> 3) >> (pos & 7)) & ((u64(1) << nb) - 1)); }
  u32 read(unsigned nb) { u32 v = peek(nb); pos += nb; return v; }
  bool over() const { return pos > len * 8; }
  size_t bytes() const { return (pos + 7) / 8; }
};

// ---- FSE
struct FseEnt { u16 base; u8 nb; u8 sym; };
struct FseTable {
  FseEnt e[1u << 9];
  unsigned al = 0;
  bool valid = false;
};

// 正規化した頻度 cnt[0..nsym) (-1 = 確率が 1 未満) から復号表を作る
bool fse_build(FseTable* t, const int16_t* cnt, unsigned nsym, unsigned al) {
  const u32 size = 1u << al;
  u32 high = size - 1;
  u16 next[256];
  for (unsigned s = 0; s < nsym; s++) {
    if (cnt[s] == -1) { t->e[high].sym = static_cast<u8>(s); high--; next[s] = 1; }
    else next[s] = static_cast<u16>(cnt[s]);
  }
  const u32 step = (size >> 1) + (size >> 3) + 3, mask = size - 1;
  u32 pos = 0;
  for (unsigned s = 0; s < nsym; s++) {
    for (int i = 0; i < cnt[s]; i++) {
      t->e[pos].sym = static_cast<u8>(s);
      do pos = (pos + step) & mask; while (pos > high);
    }
  }
  if (pos != 0) return false;
  for (u32 u = 0; u < size; u++) {
    u32 ns = next[t->e[u].sym]++;
    if (ns == 0) return false;
    unsigned nb = al - highbit(ns);
    t->e[u].nb = static_cast<u8>(nb);
    t->e[u].base = static_cast<u16>((ns << nb) - size);
  }
  t->al = al;
  t->valid = true;
  return true;
}

// 分布の記述 (RFC 8878 4.1.1) を読んで復号表を作る。used = 読んだバイト数
bool fse_read(FseTable* t, const u8* p, size_t n, unsigned maxsym, unsigned maxal, size_t* used) {
  FwdBits br(p, n);
  const unsigned al = br.read(4) + 5;
  if (al > maxal) return false;
  int16_t cnt[256];
  int remaining = (1 << al) + 1;
  int threshold = 1 << al;
  unsigned nbits = al + 1;
  unsigned sym = 0;
  bool prev0 = false;
  while (remaining > 1 && sym <= maxsym) {
    if (prev0) {   // 確率 0 の直後は、0 が続く記号の数 (2 ビットずつ。3 なら続きを読む)
      for (;;) {
        unsigned r = br.read(2);
        if (sym + r > maxsym + 1u) return false;
        for (unsigned i = 0; i < r; i++) cnt[sym++] = 0;
        if (r != 3) break;
        if (br.over()) return false;
      }
      prev0 = false;
      if (sym > maxsym) break;
    }
    const int mx = 2 * threshold - 1 - remaining;
    const int v = static_cast<int>(br.peek(nbits));
    int count;
    if ((v & (threshold - 1)) < mx) { count = v & (threshold - 1); br.pos += nbits - 1; }
    else { count = v & (2 * threshold - 1); if (count >= threshold) count -= mx; br.pos += nbits; }
    count--;
    remaining -= count < 0 ? -count : count;
    if (remaining < 1) return false;
    cnt[sym++] = static_cast<int16_t>(count);
    prev0 = count == 0;
    while (remaining < threshold) { nbits--; threshold >>= 1; }
    if (br.over()) return false;
  }
  if (remaining != 1 || br.over()) return false;
  *used = br.bytes();
  return fse_build(t, cnt, sym, al);
}

// 既定の分布 (RFC 8878 3.1.1.3.2.2)
const int16_t kLLDefault[36] = {4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1};
const int16_t kMLDefault[53] = {1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1};
const int16_t kOFDefault[29] = {1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1};

struct DefaultTables {
  FseTable ll, ml, of;
  DefaultTables() {
    fse_build(&ll, kLLDefault, 36, 6);
    fse_build(&ml, kMLDefault, 53, 6);
    fse_build(&of, kOFDefault, 29, 5);
  }
};
const DefaultTables& defaults() { static const DefaultTables d; return d; }

// リテラル長・一致長の符号 → (基準値, 追加ビット数)
const u32 kLLBase[36] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
const u8 kLLBits[36] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
const u32 kMLBase[53] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34,
                         35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539};
const u8 kMLBits[53] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                        1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

// ---- Huffman
struct HufEnt { u8 sym; u8 nb; };
struct HufTable {
  HufEnt e[1u << kMaxHufBits];
  unsigned bits = 0;
  bool valid = false;
};

// 重み weights[0..n) (最後の記号の重みは含まない) から復号表を作る
bool huf_build(HufTable* t, u8* weights, unsigned n) {
  if (n > 255) return false;
  u32 sum = 0;
  for (unsigned i = 0; i < n; i++) {
    if (weights[i] > kMaxHufBits) return false;
    if (weights[i]) sum += 1u << (weights[i] - 1);
  }
  if (sum == 0) return false;
  const unsigned bits = highbit(sum) + 1;
  if (bits > kMaxHufBits) return false;
  const u32 rest = (1u << bits) - sum;
  if (rest == 0 || (rest & (rest - 1)) != 0) return false;
  weights[n++] = static_cast<u8>(highbit(rest) + 1);
  u32 start[kMaxHufBits + 2] = {0};
  u32 count[kMaxHufBits + 2] = {0};
  for (unsigned i = 0; i < n; i++) if (weights[i]) count[weights[i]]++;
  if (count[1] < 2 || (count[1] & 1)) return false;   // 最も長い符号は 2 つ 1 組 (完全な二分木)
  u32 pos = 0;
  for (unsigned w = 1; w <= bits; w++) { start[w] = pos; pos += count[w] << (w - 1); }
  if (pos != (1u << bits)) return false;
  for (unsigned s = 0; s < n; s++) {
    unsigned w = weights[s];
    if (!w) continue;
    u32 len = 1u << (w - 1);
    for (u32 i = 0; i < len; i++) { t->e[start[w] + i].sym = static_cast<u8>(s); t->e[start[w] + i].nb = static_cast<u8>(bits + 1 - w); }
    start[w] += len;
  }
  t->bits = bits;
  t->valid = true;
  return true;
}

// 木の記述 (FSE で圧縮された重み、または 4 ビットずつの直接表記) を読み、復号表を作る。used = 読んだバイト数
bool huf_read(HufTable* t, const u8* p, size_t n, size_t* used) {
  if (n < 1) return false;
  const unsigned hdr = p[0];
  u8 w[256];
  unsigned nw;
  if (hdr >= 128) {
    nw = hdr - 127;
    const size_t bytes = (nw + 1) / 2;
    if (1 + bytes > n) return false;
    for (unsigned i = 0; i < nw; i++) w[i] = (i & 1) ? (p[1 + i / 2] & 15) : (p[1 + i / 2] >> 4);
    *used = 1 + bytes;
  } else {
    if (hdr == 0 || 1 + static_cast<size_t>(hdr) > n) return false;
    FseTable ft;
    size_t u;
    if (!fse_read(&ft, p + 1, hdr, 12, 6, &u)) return false;
    RevBits rb;
    if (!rb.init(p + 1 + u, hdr - u)) return false;
    unsigned s1 = static_cast<unsigned>(rb.read(ft.al)), s2 = static_cast<unsigned>(rb.read(ft.al));
    if (rb.avail < 0) return false;
    nw = 0;
    for (;;) {
      if (nw + 2 > 255) return false;
      const FseEnt a = ft.e[s1];
      w[nw++] = a.sym;
      s1 = a.base + static_cast<unsigned>(rb.read(a.nb));
      if (rb.avail < 0) { w[nw++] = ft.e[s2].sym; break; }
      const FseEnt b = ft.e[s2];
      w[nw++] = b.sym;
      s2 = b.base + static_cast<unsigned>(rb.read(b.nb));
      if (rb.avail < 0) { w[nw++] = ft.e[s1].sym; break; }
    }
    *used = 1 + hdr;
  }
  return huf_build(t, w, nw);
}

// 1 本のビット列から count 個の記号を復号する。ビット列を過不足なく使い切ること (規格どおりの圧縮器は必ずそうなる) を確かめ、
// 読み過ぎ・読み残しは壊れたデータとして拒否する (upstream の展開器は最後の記号の読み過ぎを見逃すことがある。壊れた入力での差はそこだけ)。
bool huf_stream(const HufTable& t, const u8* p, size_t n, u8* out, size_t count) {
  RevBits rb;
  if (!rb.init(p, n)) return false;
  for (size_t i = 0; i < count; i++) {
    const HufEnt e = t.e[rb.peek(t.bits)];
    out[i] = e.sym;
    rb.avail -= e.nb;
    if (rb.avail < 0) return false;
  }
  return rb.avail == 0;
}

struct Frame {
  u8* out;
  size_t cap;
  size_t op = 0;
  HufTable huf;
  FseTable ll, of, ml;
  u32 rep[3] = {1, 4, 8};
  std::vector<u8> lits;
  bool truncated = false;   // 出力が cap に収まらなかった (入った分は書いた)
};

int decode_literals(Frame& f, const u8* p, size_t n, size_t* used) {
  if (n < 1) return -EIO;
  const unsigned type = p[0] & 3, sf = (p[0] >> 2) & 3;
  size_t regen, hdr;
  if (type <= 1) {
    if (sf == 0 || sf == 2) { regen = p[0] >> 3; hdr = 1; }
    else if (sf == 1) { if (n < 2) return -EIO; regen = le16(p) >> 4; hdr = 2; }
    else { if (n < 3) return -EIO; regen = le24(p) >> 4; hdr = 3; }
    if (regen > kBlockMax) return -EIO;
    f.lits.resize(regen);
    if (type == 0) {
      if (n - hdr < regen) return -EIO;
      mcopy(f.lits.data(), p + hdr, regen);
      *used = hdr + regen;
    } else {
      if (n - hdr < 1) return -EIO;
      mfill(f.lits.data(), p[hdr], regen);
      *used = hdr + 1;
    }
    return 0;
  }
  size_t csize;
  unsigned streams;
  if (sf <= 1) {
    if (n < 3) return -EIO;
    const u32 v = le24(p);
    regen = (v >> 4) & 0x3FF; csize = (v >> 14) & 0x3FF; hdr = 3; streams = sf == 0 ? 1 : 4;
  } else if (sf == 2) {
    if (n < 4) return -EIO;
    const u32 v = le32(p);
    regen = (v >> 4) & 0x3FFF; csize = (v >> 18) & 0x3FFF; hdr = 4; streams = 4;
  } else {
    if (n < 5) return -EIO;
    const u64 v = le32(p) | (static_cast<u64>(p[4]) << 32);
    regen = (v >> 4) & 0x3FFFF; csize = (v >> 22) & 0x3FFFF; hdr = 5; streams = 4;
  }
  if (regen > kBlockMax || csize > n - hdr) return -EIO;
  const u8* d = p + hdr;
  size_t dn = csize;
  if (type == 2) {
    size_t u;
    if (!huf_read(&f.huf, d, dn, &u)) return -EIO;
    d += u; dn -= u;
  } else if (!f.huf.valid) {
    return -EIO;
  }
  f.lits.resize(regen);
  if (streams == 1) {
    if (!huf_stream(f.huf, d, dn, f.lits.data(), regen)) return -EIO;
  } else {
    if (dn < 6) return -EIO;
    const size_t s1 = le16(d), s2 = le16(d + 2), s3 = le16(d + 4);
    d += 6; dn -= 6;
    if (s1 + s2 + s3 > dn) return -EIO;
    const size_t s4 = dn - s1 - s2 - s3;
    const size_t seg = (regen + 3) / 4;
    if (seg * 3 > regen) return -EIO;
    const size_t last = regen - seg * 3;
    if (!huf_stream(f.huf, d, s1, f.lits.data(), seg) || !huf_stream(f.huf, d + s1, s2, f.lits.data() + seg, seg) ||
        !huf_stream(f.huf, d + s1 + s2, s3, f.lits.data() + 2 * seg, seg) || !huf_stream(f.huf, d + s1 + s2 + s3, s4, f.lits.data() + 3 * seg, last))
      return -EIO;
  }
  *used = hdr + csize;
  return 0;
}

// 記号の符号化の種類 (0 既定 / 1 RLE / 2 FSE / 3 前のブロックの表を再利用) に従って表を用意する
int prepare_table(FseTable* t, unsigned mode, const FseTable& def, unsigned maxsym, unsigned maxal, const u8** p, size_t* n) {
  switch (mode) {
    case 0: *t = def; return 0;
    case 1: {
      if (*n < 1 || (*p)[0] > maxsym) return -EIO;
      t->e[0].sym = (*p)[0]; t->e[0].nb = 0; t->e[0].base = 0; t->al = 0; t->valid = true;
      *p += 1; *n -= 1;
      return 0;
    }
    case 2: {
      size_t u;
      if (!fse_read(t, *p, *n, maxsym, maxal, &u)) return -EIO;
      *p += u; *n -= u;
      return 0;
    }
    default: return t->valid ? 0 : -EIO;
  }
}

// 出力へ n バイト書く (cap を越える分は捨てて truncated にする)
inline size_t room(const Frame& f, size_t n) { return n < f.cap - f.op ? n : f.cap - f.op; }

int decode_sequences(Frame& f, const u8* p, size_t n) {
  size_t litpos = 0;
  const size_t litlen = f.lits.size();
  if (n < 1) return -EIO;
  size_t nseq;
  if (p[0] == 0) { nseq = 0; p++; n--; }
  else if (p[0] < 128) { nseq = p[0]; p++; n--; }
  else if (p[0] < 255) { if (n < 2) return -EIO; nseq = ((static_cast<size_t>(p[0]) - 128) << 8) + p[1]; p += 2; n -= 2; }
  else { if (n < 3) return -EIO; nseq = static_cast<size_t>(p[1]) + (static_cast<size_t>(p[2]) << 8) + 0x7F00; p += 3; n -= 3; }
  if (nseq == 0) {
    if (n != 0) return -EIO;
  } else {
    if (n < 1) return -EIO;
    const unsigned mm = p[0];
    if (mm & 3) return -EIO;
    p++; n--;
    const DefaultTables& d = defaults();
    int rc;
    if ((rc = prepare_table(&f.ll, mm >> 6, d.ll, kMaxLL, 9, &p, &n)) != 0) return rc;
    if ((rc = prepare_table(&f.of, (mm >> 4) & 3, d.of, kMaxOF, 8, &p, &n)) != 0) return rc;
    if ((rc = prepare_table(&f.ml, (mm >> 2) & 3, d.ml, kMaxML, 9, &p, &n)) != 0) return rc;
    RevBits rb;
    if (!rb.init(p, n)) return -EIO;
    unsigned sll = static_cast<unsigned>(rb.read(f.ll.al));
    unsigned sof = static_cast<unsigned>(rb.read(f.of.al));
    unsigned sml = static_cast<unsigned>(rb.read(f.ml.al));
    if (rb.avail < 0) return -EIO;
    const size_t block_start = f.op;
    for (size_t i = 0; i < nseq; i++) {
      const FseEnt el = f.ll.e[sll], eo = f.of.e[sof], em = f.ml.e[sml];
      const unsigned ofc = eo.sym;
      const u64 ofv = (u64(1) << ofc) + rb.read(ofc);
      const u64 ml = kMLBase[em.sym] + rb.read(kMLBits[em.sym]);
      const u64 ll = kLLBase[el.sym] + rb.read(kLLBits[el.sym]);
      if (i + 1 != nseq) {
        sll = el.base + static_cast<unsigned>(rb.read(el.nb));
        sml = em.base + static_cast<unsigned>(rb.read(em.nb));
        sof = eo.base + static_cast<unsigned>(rb.read(eo.nb));
      }
      if (rb.avail < 0) return -EIO;
      // 距離: 3 以下は直近の距離の再利用 (リテラル長が 0 のときは 1 つずれる)
      u64 off;
      if (ofv > 3) {
        off = ofv - 3;
        f.rep[2] = f.rep[1]; f.rep[1] = f.rep[0]; f.rep[0] = static_cast<u32>(off);
      } else {
        // ofv 1..3 → rep[0..2]。リテラル長が 0 のときは 1 つずれて rep[1], rep[2], (rep[0] - 1)
        const unsigned idx = static_cast<unsigned>(ofv) - (ll == 0 ? 0u : 1u);
        if (idx == 3) {
          if (f.rep[0] <= 1) return -EIO;
          off = f.rep[0] - 1u;
          f.rep[2] = f.rep[1]; f.rep[1] = f.rep[0]; f.rep[0] = static_cast<u32>(off);
        } else {
          off = f.rep[idx];
          if (idx == 1) { f.rep[1] = f.rep[0]; f.rep[0] = static_cast<u32>(off); }
          else if (idx == 2) { f.rep[2] = f.rep[1]; f.rep[1] = f.rep[0]; f.rep[0] = static_cast<u32>(off); }
        }
      }
      // リテラルのコピー
      if (ll > litlen - litpos) return -EIO;
      if (f.op + ll - block_start > kBlockMax || ml > kBlockMax) return -EIO;
      size_t w = room(f, static_cast<size_t>(ll));
      mcopy(f.out + f.op, f.lits.data() + litpos, w);
      f.op += w; litpos += static_cast<size_t>(ll);
      if (w < ll) { f.truncated = true; return 0; }
      // 一致のコピー (重なってよい)
      if (off == 0 || off > f.op) return -EIO;
      w = room(f, static_cast<size_t>(ml));
      const u8* s = f.out + f.op - off;
      u8* o = f.out + f.op;
      if (off >= w) mcopy(o, s, w);
      else for (size_t k = 0; k < w; k++) o[k] = s[k];
      f.op += w;
      if (w < ml) { f.truncated = true; return 0; }
    }
    if (rb.avail != 0) return -EIO;
  }
  const size_t rest = litlen - litpos;
  const size_t w = room(f, rest);
  mcopy(f.out + f.op, f.lits.data() + litpos, w);
  f.op += w;
  if (w < rest) f.truncated = true;
  return 0;
}

int decode_frame(const u8* in, size_t n, u8* out, size_t cap, size_t* outlen) {
  *outlen = 0;
  size_t ip = 0;
  // 先頭のスキップフレームは読み飛ばす
  while (n - ip >= 8 && (le32(in + ip) & kSkipMagicMask) == kSkipMagicLo) {
    const u32 sz = le32(in + ip + 4);
    if (sz > n - ip - 8) return -EIO;
    ip += 8 + sz;
  }
  if (n - ip < 5 || le32(in + ip) != kMagic) return -EIO;
  ip += 4;
  const unsigned fhd = in[ip++];
  const unsigned fcs_flag = fhd >> 6, single = (fhd >> 5) & 1, has_ck = (fhd >> 2) & 1, did_flag = fhd & 3;
  if (fhd & 0x08) return -EIO;   // 予約ビット
  if (!single) { if (n - ip < 1) return -EIO; ip++; }   // ウィンドウ記述子 (出力バッファが履歴なので使わない)
  static const unsigned kDidBytes[4] = {0, 1, 2, 4};
  if (n - ip < kDidBytes[did_flag]) return -EIO;
  u32 did = 0;
  for (unsigned i = 0; i < kDidBytes[did_flag]; i++) did |= static_cast<u32>(in[ip + i]) << (8 * i);
  ip += kDidBytes[did_flag];
  if (did != 0) return -EIO;   // 辞書は未対応
  unsigned fcs_bytes = fcs_flag == 0 ? (single ? 1u : 0u) : (1u << fcs_flag);
  bool has_fcs = fcs_bytes != 0;
  u64 fcs = 0;
  if (n - ip < fcs_bytes) return -EIO;
  for (unsigned i = 0; i < fcs_bytes; i++) fcs |= static_cast<u64>(in[ip + i]) << (8 * i);
  if (fcs_flag == 1) fcs += 256;
  ip += fcs_bytes;

  Frame* f = new (std::nothrow) Frame();
  if (!f) return -ENOMEM;
  struct Guard { Frame* p; ~Guard() { delete p; } } guard{f};
  f->out = out; f->cap = cap;
  for (bool last = false; !last;) {
    if (n - ip < 3) return -EIO;
    const u32 bh = le24(in + ip);
    ip += 3;
    last = bh & 1;
    const unsigned type = (bh >> 1) & 3;
    const size_t bsz = bh >> 3;
    if (type == 3 || bsz > kBlockMax) return -EIO;
    if (type == 0) {
      if (bsz > n - ip) return -EIO;
      size_t w = room(*f, bsz);
      mcopy(out + f->op, in + ip, w);
      f->op += w;
      if (w < bsz) f->truncated = true;
      ip += bsz;
    } else if (type == 1) {
      if (n - ip < 1) return -EIO;
      size_t w = room(*f, bsz);
      mfill(out + f->op, in[ip], w);
      f->op += w;
      if (w < bsz) f->truncated = true;
      ip += 1;
    } else {
      if (bsz > n - ip) return -EIO;
      size_t used;
      int rc = decode_literals(*f, in + ip, bsz, &used);
      if (rc) return rc;
      rc = decode_sequences(*f, in + ip + used, bsz - used);
      if (rc) return rc;
      ip += bsz;
    }
    if (f->truncated) { *outlen = f->op; return -ENOSPC; }
  }
  if (has_ck) { if (n - ip < 4) return -EIO; ip += 4; }
  if (has_fcs && fcs != f->op) return -EIO;
  *outlen = f->op;
  return 0;
}

}  // namespace

extern "C" int usbmem_zstd_decompress(const void* src, size_t srclen, void* dst, size_t dstcap, size_t* outlen) {
  try {
    return decode_frame(static_cast<const u8*>(src), srclen, static_cast<u8*>(dst), dstcap, outlen);
  } catch (const std::bad_alloc&) {
    *outlen = 0;
    return -ENOMEM;
  }
}
