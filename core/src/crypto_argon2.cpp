// SPDX-License-Identifier: Apache-2.0
// BLAKE2b (RFC 7693) と Argon2 d/i/id (RFC 9106, version 0x13)。LUKS2 の keyslot KDF 用。
// 単一スレッドの参照実装相当 (lanes は順に処理する。結果は並列版と同じ)。メモリは memory_kib KiB を確保する (上限は呼び出し側が渡す)。
#include <algorithm>
#include <new>

#include "usbmem/crypto.h"

namespace usbmem {
namespace crypto {

namespace {

inline uint64_t rotr64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }
inline uint64_t ld64(const uint8_t* p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }
inline void st64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = uint8_t(v >> (8 * i)); }
inline void st32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = uint8_t(v >> (8 * i)); }

const uint64_t B2IV[8] = {0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
                          0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};
const uint8_t SIGMA[12][16] = {{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
                               {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4}, {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
                               {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13}, {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
                               {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11}, {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
                               {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5}, {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0},
                               {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3}};

struct B2 {
  uint64_t h[8];
  uint8_t buf[128];
  size_t buflen = 0;
  uint64_t t = 0;
  explicit B2(size_t outlen) {
    std::memcpy(h, B2IV, sizeof h);
    h[0] ^= 0x01010000ULL ^ uint64_t(outlen);  // fanout 1, depth 1, key なし
  }
  static void g(uint64_t* v, int a, int b, int c, int d, uint64_t x, uint64_t y) {
    v[a] = v[a] + v[b] + x; v[d] = rotr64(v[d] ^ v[a], 32);
    v[c] = v[c] + v[d];     v[b] = rotr64(v[b] ^ v[c], 24);
    v[a] = v[a] + v[b] + y; v[d] = rotr64(v[d] ^ v[a], 16);
    v[c] = v[c] + v[d];     v[b] = rotr64(v[b] ^ v[c], 63);
  }
  void compress(bool last) {
    uint64_t m[16], v[16];
    for (int i = 0; i < 16; i++) m[i] = ld64(buf + 8 * i);
    for (int i = 0; i < 8; i++) { v[i] = h[i]; v[i + 8] = B2IV[i]; }
    v[12] ^= t;  // 上位 64 ビットのカウンタは 0 (入力 2^64 バイト未満)
    if (last) v[14] = ~v[14];
    for (int r = 0; r < 12; r++) {
      const uint8_t* s = SIGMA[r];
      g(v, 0, 4, 8, 12, m[s[0]], m[s[1]]); g(v, 1, 5, 9, 13, m[s[2]], m[s[3]]);
      g(v, 2, 6, 10, 14, m[s[4]], m[s[5]]); g(v, 3, 7, 11, 15, m[s[6]], m[s[7]]);
      g(v, 0, 5, 10, 15, m[s[8]], m[s[9]]); g(v, 1, 6, 11, 12, m[s[10]], m[s[11]]);
      g(v, 2, 7, 8, 13, m[s[12]], m[s[13]]); g(v, 3, 4, 9, 14, m[s[14]], m[s[15]]);
    }
    for (int i = 0; i < 8; i++) h[i] ^= v[i] ^ v[i + 8];
  }
  void update(const uint8_t* p, size_t n) {
    while (n) {
      if (buflen == 128) { t += 128; compress(false); buflen = 0; }  // 最後のブロックは final で処理するため、満杯でも保留する
      size_t take = std::min(n, 128 - buflen);
      std::memcpy(buf + buflen, p, take);
      buflen += take; p += take; n -= take;
    }
  }
  void final(uint8_t* out, size_t outlen) {
    t += buflen;
    std::memset(buf + buflen, 0, 128 - buflen);
    compress(true);
    uint8_t full[64];
    for (int i = 0; i < 8; i++) st64(full + 8 * i, h[i]);
    std::memcpy(out, full, outlen);
    secure_zero(full, sizeof full);
  }
};

// H' (RFC 9106 3.3): 可変長ハッシュ
void blake2b_long(uint8_t* out, size_t outlen, const uint8_t* in, size_t inlen) {
  uint8_t lenb[4];
  st32(lenb, uint32_t(outlen));
  if (outlen <= 64) {
    B2 b(outlen);
    b.update(lenb, 4); b.update(in, inlen);
    b.final(out, outlen);
    return;
  }
  uint8_t v[64];
  {
    B2 b(64);
    b.update(lenb, 4); b.update(in, inlen);
    b.final(v, 64);
  }
  std::memcpy(out, v, 32);
  size_t pos = 32, remaining = outlen - 32;
  while (remaining > 64) {
    B2 b(64);
    b.update(v, 64);
    b.final(v, 64);
    std::memcpy(out + pos, v, 32);
    pos += 32; remaining -= 32;
  }
  B2 b(remaining);
  b.update(v, 64);
  b.final(out + pos, remaining);
  secure_zero(v, sizeof v);
}

typedef uint64_t Block[128];

inline uint64_t fBlaMka(uint64_t x, uint64_t y) { return x + y + 2 * uint64_t(uint32_t(x)) * uint64_t(uint32_t(y)); }
#define G_(a, b, c, d) \
  do { a = fBlaMka(a, b); d = rotr64(d ^ a, 32); c = fBlaMka(c, d); b = rotr64(b ^ c, 24); \
       a = fBlaMka(a, b); d = rotr64(d ^ a, 16); c = fBlaMka(c, d); b = rotr64(b ^ c, 63); } while (0)
#define ROUND_(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15) \
  do { G_(v0, v4, v8, v12); G_(v1, v5, v9, v13); G_(v2, v6, v10, v14); G_(v3, v7, v11, v15); \
       G_(v0, v5, v10, v15); G_(v1, v6, v11, v12); G_(v2, v7, v8, v13); G_(v3, v4, v9, v14); } while (0)

// G(X, Y): R = X xor Y; 行/列に P を適用; out = P(R) xor R (xor_into なら out ^= ...)
void compress_block(Block out, const Block x, const Block y, bool xor_into) {
  Block r, z;
  for (int i = 0; i < 128; i++) r[i] = x[i] ^ y[i];
  std::memcpy(z, r, sizeof z);
  for (int i = 0; i < 8; i++)
    ROUND_(z[16 * i + 0], z[16 * i + 1], z[16 * i + 2], z[16 * i + 3], z[16 * i + 4], z[16 * i + 5], z[16 * i + 6], z[16 * i + 7],
           z[16 * i + 8], z[16 * i + 9], z[16 * i + 10], z[16 * i + 11], z[16 * i + 12], z[16 * i + 13], z[16 * i + 14], z[16 * i + 15]);
  for (int i = 0; i < 8; i++)
    ROUND_(z[2 * i + 0], z[2 * i + 1], z[2 * i + 16], z[2 * i + 17], z[2 * i + 32], z[2 * i + 33], z[2 * i + 48], z[2 * i + 49],
           z[2 * i + 64], z[2 * i + 65], z[2 * i + 80], z[2 * i + 81], z[2 * i + 96], z[2 * i + 97], z[2 * i + 112], z[2 * i + 113]);
  if (xor_into) for (int i = 0; i < 128; i++) out[i] ^= z[i] ^ r[i];
  else for (int i = 0; i < 128; i++) out[i] = z[i] ^ r[i];
  secure_zero(r, sizeof r); secure_zero(z, sizeof z);
}

}  // namespace

void blake2b(const uint8_t* in, size_t inlen, uint8_t* out, size_t outlen) {
  B2 b(outlen);
  b.update(in, inlen);
  b.final(out, outlen);
}

bool argon2(Argon2Type type, const uint8_t* pw, size_t pwlen, const uint8_t* salt, size_t slen, uint32_t t_cost, uint32_t memory_kib,
            uint32_t lanes, uint8_t* out, size_t outlen, uint32_t max_memory_kib) {
  if (t_cost < 1 || lanes < 1 || lanes > 0xFFFFFF || outlen < 4 || outlen > 1024 || slen < 8 || memory_kib < 8 * lanes) return false;
  if (memory_kib > max_memory_kib) return false;
  const uint32_t m_prime = 4 * lanes * (memory_kib / (4 * lanes));
  const uint32_t lane_len = m_prime / lanes;
  const uint32_t seg_len = lane_len / 4;
  Block* mem = nullptr;
  try { mem = new Block[m_prime]; } catch (const std::bad_alloc&) { return false; }

  // H0
  uint8_t h0[72];
  {
    B2 b(64);
    uint8_t w[4];
    auto put32 = [&](uint32_t v) { st32(w, v); b.update(w, 4); };
    put32(lanes); put32(uint32_t(outlen)); put32(memory_kib); put32(t_cost); put32(0x13); put32(uint32_t(type));
    put32(uint32_t(pwlen)); if (pwlen) b.update(pw, pwlen);
    put32(uint32_t(slen)); b.update(salt, slen);
    put32(0);  // secret なし
    put32(0);  // associated data なし
    b.final(h0, 64);
  }
  for (uint32_t l = 0; l < lanes; l++) {
    uint8_t tmp[1024];
    st32(h0 + 64, 0); st32(h0 + 68, l);
    blake2b_long(tmp, 1024, h0, 72);
    for (int i = 0; i < 128; i++) mem[l * lane_len][i] = ld64(tmp + 8 * i);
    st32(h0 + 64, 1);
    blake2b_long(tmp, 1024, h0, 72);
    for (int i = 0; i < 128; i++) mem[l * lane_len + 1][i] = ld64(tmp + 8 * i);
    secure_zero(tmp, sizeof tmp);
  }
  secure_zero(h0, sizeof h0);

  Block addr, input, zero;
  std::memset(zero, 0, sizeof zero);
  for (uint32_t pass = 0; pass < t_cost; pass++) {
    for (uint32_t slice = 0; slice < 4; slice++) {
      const bool indep = type == Argon2Type::I || (type == Argon2Type::ID && pass == 0 && slice < 2);
      for (uint32_t lane = 0; lane < lanes; lane++) {
        uint32_t start = (pass == 0 && slice == 0) ? 2 : 0;
        if (indep) {
          std::memset(input, 0, sizeof input);
          input[0] = pass; input[1] = lane; input[2] = slice; input[3] = m_prime; input[4] = t_cost; input[5] = uint64_t(type);
          if (pass == 0 && slice == 0) {  // 最初の 2 ブロックは初期化済みなので、アドレスブロックを 1 つ進めておく
            input[6]++;
            compress_block(addr, zero, input, false);
            compress_block(addr, zero, addr, false);
          }
        }
        for (uint32_t index = start; index < seg_len; index++) {
          const uint32_t cur = lane * lane_len + slice * seg_len + index;
          const uint32_t prev = (slice * seg_len + index == 0) ? lane * lane_len + lane_len - 1 : cur - 1;
          uint64_t rnd;
          if (indep) {
            if (index % 128 == 0) {
              input[6]++;
              compress_block(addr, zero, input, false);
              compress_block(addr, zero, addr, false);
            }
            rnd = addr[index % 128];
          } else {
            rnd = mem[prev][0];
          }
          const uint32_t j1 = uint32_t(rnd), j2 = uint32_t(rnd >> 32);
          uint32_t ref_lane = (pass == 0 && slice == 0) ? lane : j2 % lanes;
          const bool same = ref_lane == lane;
          uint32_t area;
          if (pass == 0) {
            if (slice == 0) area = index - 1;
            else if (same) area = slice * seg_len + index - 1;
            else area = slice * seg_len + (index == 0 ? uint32_t(-1) : 0);
          } else {
            if (same) area = lane_len - seg_len + index - 1;
            else area = lane_len - seg_len + (index == 0 ? uint32_t(-1) : 0);
          }
          uint64_t rel = j1;
          rel = (rel * rel) >> 32;
          rel = area - 1 - ((uint64_t(area) * rel) >> 32);
          uint32_t startpos = 0;
          if (pass != 0) startpos = (slice == 3) ? 0 : (slice + 1) * seg_len;
          const uint32_t abs = uint32_t((startpos + rel) % lane_len);
          compress_block(mem[cur], mem[prev], mem[ref_lane * lane_len + abs], pass != 0);
        }
      }
    }
  }
  Block fin;
  std::memcpy(fin, mem[lane_len - 1], sizeof fin);
  for (uint32_t l = 1; l < lanes; l++)
    for (int i = 0; i < 128; i++) fin[i] ^= mem[l * lane_len + lane_len - 1][i];
  uint8_t fb[1024];
  for (int i = 0; i < 128; i++) st64(fb + 8 * i, fin[i]);
  blake2b_long(out, outlen, fb, 1024);
  secure_zero(fb, sizeof fb); secure_zero(fin, sizeof fin); secure_zero(addr, sizeof addr);
  secure_zero(mem, sizeof(Block) * m_prime);
  delete[] mem;
  return true;
}

}  // namespace crypto
}  // namespace usbmem
