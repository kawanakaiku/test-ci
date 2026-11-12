// SPDX-License-Identifier: Apache-2.0
// AES (FIPS 197) と CBC / XTS (IEEE 1619) / CCM (SP 800-38C)。表引き方式 (ハードウェア命令なし)。
// S-box は GF(2^8) の逆元から起動時に生成する (表の転記ミスを避けるため)。
#include <mutex>

#include "usbmem/crypto.h"

namespace usbmem {
namespace crypto {

namespace {

struct Tables {
  uint8_t sbox[256], isbox[256];
  uint32_t te0[256], td0[256];
  Tables() {
    // 乗法の逆元 (generator 3 を使った標準的な生成)
    uint8_t p = 1, q = 1;
    do {
      p = uint8_t(p ^ (p << 1) ^ ((p & 0x80) ? 0x1b : 0));            // p *= 3
      q ^= uint8_t(q << 1); q ^= uint8_t(q << 2); q ^= uint8_t(q << 4);  // q /= 3
      if (q & 0x80) q ^= 0x09;
      uint8_t x = uint8_t(q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) ^ ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4)));
      sbox[p] = uint8_t(x ^ 0x63);
    } while (p != 1);
    sbox[0] = 0x63;
    for (int i = 0; i < 256; i++) isbox[sbox[i]] = uint8_t(i);
    for (int i = 0; i < 256; i++) {
      uint8_t s = sbox[i];
      uint8_t s2 = xt(s), s3 = uint8_t(s2 ^ s);
      te0[i] = (uint32_t(s2) << 24) | (uint32_t(s) << 16) | (uint32_t(s) << 8) | s3;
      uint8_t v = isbox[i];
      td0[i] = (uint32_t(mul(v, 14)) << 24) | (uint32_t(mul(v, 9)) << 16) | (uint32_t(mul(v, 13)) << 8) | mul(v, 11);
    }
  }
  static uint8_t xt(uint8_t a) { return uint8_t((a << 1) ^ ((a & 0x80) ? 0x1b : 0)); }
  static uint8_t mul(uint8_t a, int b) {
    uint8_t r = 0;
    while (b) { if (b & 1) r ^= a; a = xt(a); b >>= 1; }
    return r;
  }
};

const Tables& T() { static const Tables t; return t; }

inline uint32_t rd(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
inline void wr(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
inline uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

}  // namespace

bool Aes::set_key(const uint8_t* key, size_t len) {
  rounds_ = 0;
  if (len != 16 && len != 24 && len != 32) return false;
  const Tables& t = T();
  const int nk = int(len / 4);
  const int nr = nk + 6;
  const int total = 4 * (nr + 1);
  for (int i = 0; i < nk; i++) ek_[i] = rd(key + 4 * i);
  uint32_t rcon = 1;
  for (int i = nk; i < total; i++) {
    uint32_t tmp = ek_[i - 1];
    if (i % nk == 0) {
      tmp = (uint32_t(t.sbox[(tmp >> 16) & 0xff]) << 24) | (uint32_t(t.sbox[(tmp >> 8) & 0xff]) << 16) | (uint32_t(t.sbox[tmp & 0xff]) << 8) | t.sbox[tmp >> 24];
      tmp ^= rcon << 24;
      rcon = Tables::xt(uint8_t(rcon));
    } else if (nk > 6 && i % nk == 4) {
      tmp = (uint32_t(t.sbox[tmp >> 24]) << 24) | (uint32_t(t.sbox[(tmp >> 16) & 0xff]) << 16) | (uint32_t(t.sbox[(tmp >> 8) & 0xff]) << 8) | t.sbox[tmp & 0xff];
    }
    ek_[i] = ek_[i - nk] ^ tmp;
  }
  // 逆暗号用: 最終ラウンド鍵を先頭に並べ替え、中間ラウンド鍵に InvMixColumns を適用する
  for (int r = 0; r <= nr; r++)
    for (int c = 0; c < 4; c++) dk_[4 * r + c] = ek_[4 * (nr - r) + c];
  for (int r = 1; r < nr; r++)
    for (int c = 0; c < 4; c++) {
      uint32_t w = dk_[4 * r + c];
      dk_[4 * r + c] = t.td0[t.sbox[w >> 24]] ^ ror(t.td0[t.sbox[(w >> 16) & 0xff]], 8) ^ ror(t.td0[t.sbox[(w >> 8) & 0xff]], 16) ^ ror(t.td0[t.sbox[w & 0xff]], 24);
    }
  rounds_ = nr;
  return true;
}

void Aes::encrypt_block(const uint8_t in[16], uint8_t out[16]) const {
  const Tables& t = T();
  const uint32_t* rk = ek_;
  uint32_t s0 = rd(in) ^ rk[0], s1 = rd(in + 4) ^ rk[1], s2 = rd(in + 8) ^ rk[2], s3 = rd(in + 12) ^ rk[3];
  for (int r = 1; r < rounds_; r++) {
    rk += 4;
    uint32_t t0 = t.te0[s0 >> 24] ^ ror(t.te0[(s1 >> 16) & 0xff], 8) ^ ror(t.te0[(s2 >> 8) & 0xff], 16) ^ ror(t.te0[s3 & 0xff], 24) ^ rk[0];
    uint32_t t1 = t.te0[s1 >> 24] ^ ror(t.te0[(s2 >> 16) & 0xff], 8) ^ ror(t.te0[(s3 >> 8) & 0xff], 16) ^ ror(t.te0[s0 & 0xff], 24) ^ rk[1];
    uint32_t t2 = t.te0[s2 >> 24] ^ ror(t.te0[(s3 >> 16) & 0xff], 8) ^ ror(t.te0[(s0 >> 8) & 0xff], 16) ^ ror(t.te0[s1 & 0xff], 24) ^ rk[2];
    uint32_t t3 = t.te0[s3 >> 24] ^ ror(t.te0[(s0 >> 16) & 0xff], 8) ^ ror(t.te0[(s1 >> 8) & 0xff], 16) ^ ror(t.te0[s2 & 0xff], 24) ^ rk[3];
    s0 = t0; s1 = t1; s2 = t2; s3 = t3;
  }
  rk += 4;
  const uint8_t* sb = t.sbox;
  wr(out, ((uint32_t(sb[s0 >> 24]) << 24) | (uint32_t(sb[(s1 >> 16) & 0xff]) << 16) | (uint32_t(sb[(s2 >> 8) & 0xff]) << 8) | sb[s3 & 0xff]) ^ rk[0]);
  wr(out + 4, ((uint32_t(sb[s1 >> 24]) << 24) | (uint32_t(sb[(s2 >> 16) & 0xff]) << 16) | (uint32_t(sb[(s3 >> 8) & 0xff]) << 8) | sb[s0 & 0xff]) ^ rk[1]);
  wr(out + 8, ((uint32_t(sb[s2 >> 24]) << 24) | (uint32_t(sb[(s3 >> 16) & 0xff]) << 16) | (uint32_t(sb[(s0 >> 8) & 0xff]) << 8) | sb[s1 & 0xff]) ^ rk[2]);
  wr(out + 12, ((uint32_t(sb[s3 >> 24]) << 24) | (uint32_t(sb[(s0 >> 16) & 0xff]) << 16) | (uint32_t(sb[(s1 >> 8) & 0xff]) << 8) | sb[s2 & 0xff]) ^ rk[3]);
}

void Aes::decrypt_block(const uint8_t in[16], uint8_t out[16]) const {
  const Tables& t = T();
  const uint32_t* rk = dk_;
  uint32_t s0 = rd(in) ^ rk[0], s1 = rd(in + 4) ^ rk[1], s2 = rd(in + 8) ^ rk[2], s3 = rd(in + 12) ^ rk[3];
  for (int r = 1; r < rounds_; r++) {
    rk += 4;
    uint32_t t0 = t.td0[s0 >> 24] ^ ror(t.td0[(s3 >> 16) & 0xff], 8) ^ ror(t.td0[(s2 >> 8) & 0xff], 16) ^ ror(t.td0[s1 & 0xff], 24) ^ rk[0];
    uint32_t t1 = t.td0[s1 >> 24] ^ ror(t.td0[(s0 >> 16) & 0xff], 8) ^ ror(t.td0[(s3 >> 8) & 0xff], 16) ^ ror(t.td0[s2 & 0xff], 24) ^ rk[1];
    uint32_t t2 = t.td0[s2 >> 24] ^ ror(t.td0[(s1 >> 16) & 0xff], 8) ^ ror(t.td0[(s0 >> 8) & 0xff], 16) ^ ror(t.td0[s3 & 0xff], 24) ^ rk[2];
    uint32_t t3 = t.td0[s3 >> 24] ^ ror(t.td0[(s2 >> 16) & 0xff], 8) ^ ror(t.td0[(s1 >> 8) & 0xff], 16) ^ ror(t.td0[s0 & 0xff], 24) ^ rk[3];
    s0 = t0; s1 = t1; s2 = t2; s3 = t3;
  }
  rk += 4;
  const uint8_t* ib = t.isbox;
  wr(out, ((uint32_t(ib[s0 >> 24]) << 24) | (uint32_t(ib[(s3 >> 16) & 0xff]) << 16) | (uint32_t(ib[(s2 >> 8) & 0xff]) << 8) | ib[s1 & 0xff]) ^ rk[0]);
  wr(out + 4, ((uint32_t(ib[s1 >> 24]) << 24) | (uint32_t(ib[(s0 >> 16) & 0xff]) << 16) | (uint32_t(ib[(s3 >> 8) & 0xff]) << 8) | ib[s2 & 0xff]) ^ rk[1]);
  wr(out + 8, ((uint32_t(ib[s2 >> 24]) << 24) | (uint32_t(ib[(s1 >> 16) & 0xff]) << 16) | (uint32_t(ib[(s0 >> 8) & 0xff]) << 8) | ib[s3 & 0xff]) ^ rk[2]);
  wr(out + 12, ((uint32_t(ib[s3 >> 24]) << 24) | (uint32_t(ib[(s2 >> 16) & 0xff]) << 16) | (uint32_t(ib[(s1 >> 8) & 0xff]) << 8) | ib[s0 & 0xff]) ^ rk[3]);
}

void aes_cbc_decrypt(const Aes& k, const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t len) {
  uint8_t prev[16], cur[16], tmp[16];
  std::memcpy(prev, iv, 16);
  for (size_t o = 0; o + 16 <= len; o += 16) {
    std::memcpy(cur, in + o, 16);  // in == out の場合に備えて暗号文を退避
    k.decrypt_block(cur, tmp);
    for (int i = 0; i < 16; i++) out[o + i] = uint8_t(tmp[i] ^ prev[i]);
    std::memcpy(prev, cur, 16);
  }
}

namespace {
inline void xts_mul_alpha(uint8_t t[16]) {
  uint8_t carry = uint8_t(t[15] >> 7);
  for (int i = 15; i > 0; i--) t[i] = uint8_t((t[i] << 1) | (t[i - 1] >> 7));
  t[0] = uint8_t((t[0] << 1) ^ (carry ? 0x87 : 0));
}
void xts_crypt(const Aes& k1, const Aes& k2, uint64_t tweak, const uint8_t* in, uint8_t* out, size_t len, bool dec) {
  uint8_t t[16] = {0}, x[16];
  for (int i = 0; i < 8; i++) t[i] = uint8_t(tweak >> (8 * i));
  k2.encrypt_block(t, t);
  for (size_t o = 0; o + 16 <= len; o += 16) {
    for (int i = 0; i < 16; i++) x[i] = uint8_t(in[o + i] ^ t[i]);
    if (dec) k1.decrypt_block(x, x); else k1.encrypt_block(x, x);
    for (int i = 0; i < 16; i++) out[o + i] = uint8_t(x[i] ^ t[i]);
    xts_mul_alpha(t);
  }
}
}  // namespace

void aes_xts_decrypt(const Aes& k1, const Aes& k2, uint64_t tweak, const uint8_t* in, uint8_t* out, size_t len) { xts_crypt(k1, k2, tweak, in, out, len, true); }
void aes_xts_encrypt(const Aes& k1, const Aes& k2, uint64_t tweak, const uint8_t* in, uint8_t* out, size_t len) { xts_crypt(k1, k2, tweak, in, out, len, false); }

bool aes_ccm_decrypt(const Aes& k, const uint8_t nonce[12], const uint8_t* ct, size_t len, const uint8_t tag[16], uint8_t* out) {
  if (len > 0xFFFFFF) return false;  // L = 3 バイトで表せる長さまで
  // CTR: A_i = flags(L-1 = 2) | nonce(12) | counter(3 バイト BE)
  uint8_t a[16], s[16];
  a[0] = 2;
  std::memcpy(a + 1, nonce, 12);
  auto ctr = [&](uint32_t i) { a[13] = uint8_t(i >> 16); a[14] = uint8_t(i >> 8); a[15] = uint8_t(i); };
  for (size_t o = 0, blk = 1; o < len; o += 16, blk++) {
    ctr(uint32_t(blk));
    k.encrypt_block(a, s);
    size_t n = len - o < 16 ? len - o : 16;
    for (size_t i = 0; i < n; i++) out[o + i] = uint8_t(ct[o + i] ^ s[i]);
  }
  // CBC-MAC (M = 16, L = 3, 追加データなし): flags = 8 * ((16 - 2) / 2) + (L - 1) = 58
  uint8_t b[16], mac[16];
  b[0] = 58;
  std::memcpy(b + 1, nonce, 12);
  b[13] = uint8_t(len >> 16); b[14] = uint8_t(len >> 8); b[15] = uint8_t(len);
  k.encrypt_block(b, mac);
  for (size_t o = 0; o < len; o += 16) {
    size_t n = len - o < 16 ? len - o : 16;
    for (size_t i = 0; i < 16; i++) b[i] = uint8_t(mac[i] ^ (i < n ? out[o + i] : 0));
    k.encrypt_block(b, mac);
  }
  ctr(0);
  k.encrypt_block(a, s);
  uint8_t diff = 0;
  for (int i = 0; i < 16; i++) diff |= uint8_t((mac[i] ^ s[i]) ^ tag[i]);
  if (diff != 0) { secure_zero(out, len); return false; }
  return true;
}

}  // namespace crypto
}  // namespace usbmem
