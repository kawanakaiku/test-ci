// SPDX-License-Identifier: Apache-2.0
// SHA-1 / SHA-256 / SHA-512 / HMAC / PBKDF2 (FIPS 180-4, RFC 2104, RFC 8018)
#include <algorithm>
#include <atomic>

#include "crypto_internal.h"

namespace usbmem {
namespace crypto {

void secure_zero(void* p, size_t n) {
  volatile uint8_t* v = static_cast<volatile uint8_t*>(p);
  while (n--) *v++ = 0;
#if defined(__GNUC__)
  __asm__ __volatile__("" : : "r"(p) : "memory");
#endif
}

namespace {

inline uint32_t rol32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
inline uint32_t ror32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
inline uint64_t ror64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }
inline uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
inline uint64_t be64(const uint8_t* p) { return (uint64_t(be32(p)) << 32) | be32(p + 4); }
inline void put_be32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
inline void put_be64(uint8_t* p, uint64_t v) { put_be32(p, uint32_t(v >> 32)); put_be32(p + 4, uint32_t(v)); }

const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL, 0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL, 0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL, 0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL};

}  // namespace

size_t hash_size(Hash h) { return h == Hash::Sha1 ? 20 : h == Hash::Sha256 ? 32 : 64; }
size_t hash_block_size(Hash h) { return h == Hash::Sha512 ? 128 : 64; }

HashCtx::HashCtx(Hash h) : kind_(h) {
  std::memset(st64_, 0, sizeof st64_);
  std::memset(st32_, 0, sizeof st32_);
  std::memset(buf_, 0, sizeof buf_);
  if (h == Hash::Sha1) {
    st32_[0] = 0x67452301; st32_[1] = 0xefcdab89; st32_[2] = 0x98badcfe; st32_[3] = 0x10325476; st32_[4] = 0xc3d2e1f0;
  } else if (h == Hash::Sha256) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(st32_, iv, sizeof iv);
  } else {
    static const uint64_t iv[8] = {0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
                                   0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};
    std::memcpy(st64_, iv, sizeof iv);
  }
}

#if USBMEM_HW_ANY
namespace {
std::atomic<bool> g_sha_hw_enabled{true};
thread_local int tl_sha_mode = -1;  // 自己テスト中だけ、このスレッドで強制 (0 = 表引き / 1 = 命令)
bool sha_hw_selftest();
bool sha_hw_usable() {
  static const bool ok = sha256_hw_cpu_has() && sha_hw_selftest();
  return ok;
}
inline bool use_sha_hw() {
  if (tl_sha_mode >= 0) return tl_sha_mode == 1;
  return g_sha_hw_enabled.load(std::memory_order_relaxed) && sha_hw_usable();
}
}  // namespace
bool sha256_hw_supported() { return sha_hw_usable(); }
void sha256_set_hw_enabled(bool on) { g_sha_hw_enabled.store(on, std::memory_order_relaxed); }
#else
bool sha256_hw_supported() { return false; }
void sha256_set_hw_enabled(bool) {}
#endif

namespace {
void sha256_block_sw(uint32_t* st, const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = be32(p + 4 * i);
    for (int i = 16; i < 64; i++) {
      uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
    for (int i = 0; i < 64; i++) {
      uint32_t S1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
      uint32_t ch = (e & f) ^ (~e & g);
      uint32_t t1 = h + S1 + ch + K256[i] + w[i];
      uint32_t S0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
      uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t t2 = S0 + mj;
      h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}
}  // namespace

// 圧縮関数を直接回す (BitLocker の鍵伸長のように、短い入力を大量にハッシュする所のオーバーヘッド削減用)。st = 内部状態 8 ワード、blocks = 64 バイト × n。
void sha256_compress(uint32_t st[8], const uint8_t* blocks, size_t n) {
#if USBMEM_HW_ANY
  if (use_sha_hw()) { sha256_hw_blocks(st, blocks, n, K256); return; }
#endif
  for (size_t i = 0; i < n; i++) sha256_block_sw(st, blocks + 64 * i);
}

void HashCtx::block(const uint8_t* p) {
#if USBMEM_HW_ANY
  if (kind_ == Hash::Sha256 && use_sha_hw()) { sha256_hw_blocks(st32_, p, 1, K256); return; }
#endif
  if (kind_ == Hash::Sha1) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = be32(p + 4 * i);
    for (int i = 16; i < 80; i++) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = st32_[0], b = st32_[1], c = st32_[2], d = st32_[3], e = st32_[4];
    for (int i = 0; i < 80; i++) {
      uint32_t f, k;
      if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
      else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
      else { f = b ^ c ^ d; k = 0xca62c1d6; }
      uint32_t t = rol32(a, 5) + f + e + k + w[i];
      e = d; d = c; c = rol32(b, 30); b = a; a = t;
    }
    st32_[0] += a; st32_[1] += b; st32_[2] += c; st32_[3] += d; st32_[4] += e;
  } else if (kind_ == Hash::Sha256) {
    sha256_block_sw(st32_, p);
  } else {
    uint64_t w[80];
    for (int i = 0; i < 16; i++) w[i] = be64(p + 8 * i);
    for (int i = 16; i < 80; i++) {
      uint64_t s0 = ror64(w[i - 15], 1) ^ ror64(w[i - 15], 8) ^ (w[i - 15] >> 7);
      uint64_t s1 = ror64(w[i - 2], 19) ^ ror64(w[i - 2], 61) ^ (w[i - 2] >> 6);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint64_t a = st64_[0], b = st64_[1], c = st64_[2], d = st64_[3], e = st64_[4], f = st64_[5], g = st64_[6], h = st64_[7];
    for (int i = 0; i < 80; i++) {
      uint64_t S1 = ror64(e, 14) ^ ror64(e, 18) ^ ror64(e, 41);
      uint64_t ch = (e & f) ^ (~e & g);
      uint64_t t1 = h + S1 + ch + K512[i] + w[i];
      uint64_t S0 = ror64(a, 28) ^ ror64(a, 34) ^ ror64(a, 39);
      uint64_t mj = (a & b) ^ (a & c) ^ (b & c);
      uint64_t t2 = S0 + mj;
      h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    st64_[0] += a; st64_[1] += b; st64_[2] += c; st64_[3] += d; st64_[4] += e; st64_[5] += f; st64_[6] += g; st64_[7] += h;
  }
}

#if USBMEM_HW_ANY
namespace {
// 命令版と表引き版が、長さの違う入力 (1 / 2 ブロック・端数) で同じダイジェストを出すか。"abc" の既知値も確認する。
bool sha_hw_selftest() {
  static const uint8_t kAbc[32] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
                                   0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
  uint8_t msg[300];
  uint32_t x = 0xC0FFEEu;
  for (auto& b : msg) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; b = uint8_t(x >> 9); }
  bool ok = true;
  tl_sha_mode = 1;
  {
    uint8_t d[32];
    crypto::hash(Hash::Sha256, "abc", 3, d);
    ok = std::memcmp(d, kAbc, 32) == 0;
  }
  for (size_t len : {size_t(0), size_t(1), size_t(55), size_t(56), size_t(63), size_t(64), size_t(65), size_t(119), size_t(128), size_t(300)}) {
    if (!ok) break;
    uint8_t a[32], b[32];
    tl_sha_mode = 1; crypto::hash(Hash::Sha256, msg, len, a);
    tl_sha_mode = 0; crypto::hash(Hash::Sha256, msg, len, b);
    ok = std::memcmp(a, b, 32) == 0;
  }
  tl_sha_mode = -1;
  return ok;
}
}  // namespace
#endif

void HashCtx::update(const void* data, size_t n) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  const size_t bs = hash_block_size(kind_);
  total_ += n;
  if (buf_len_) {
    size_t take = std::min(n, bs - buf_len_);
    std::memcpy(buf_ + buf_len_, p, take);
    buf_len_ += take; p += take; n -= take;
    if (buf_len_ == bs) { block(buf_); buf_len_ = 0; }
  }
  while (n >= bs) { block(p); p += bs; n -= bs; }
  if (n) { std::memcpy(buf_, p, n); buf_len_ = n; }
}

void HashCtx::final(uint8_t* out) {
  const size_t bs = hash_block_size(kind_);
  const size_t lenbytes = bs == 128 ? 16 : 8;
  uint64_t bits = total_ * 8;
  uint8_t pad[256] = {0x80};
  size_t padlen = (buf_len_ < bs - lenbytes) ? (bs - lenbytes - buf_len_) : (2 * bs - lenbytes - buf_len_);
  uint8_t lenbuf[16] = {0};
  put_be64(lenbuf + 8, bits);  // 長さは常に末尾 8 バイト (SHA-512 の上位 64 ビットは 0)
  // total_ をここで変えない: update を使わず直接ブロック処理する
  uint8_t tail[256];
  std::memcpy(tail, buf_, buf_len_);
  std::memcpy(tail + buf_len_, pad, padlen);
  std::memcpy(tail + buf_len_ + padlen, lenbuf + (16 - lenbytes), lenbytes);
  size_t tl = buf_len_ + padlen + lenbytes;
  for (size_t o = 0; o < tl; o += bs) block(tail + o);
  if (kind_ == Hash::Sha512) { for (int i = 0; i < 8; i++) put_be64(out + 8 * i, st64_[i]); }
  else { int words = kind_ == Hash::Sha1 ? 5 : 8; for (int i = 0; i < words; i++) put_be32(out + 4 * i, st32_[i]); }
  secure_zero(tail, sizeof tail);
}

void hash(Hash h, const void* data, size_t n, uint8_t* out) {
  HashCtx c(h);
  c.update(data, n);
  c.final(out);
}

namespace {
// 事前計算した HMAC (PBKDF2 の反復で内側/外側の初期状態を使い回す)
struct HmacKey {
  HashCtx inner, outer;
  explicit HmacKey(Hash h, const uint8_t* key, size_t klen) : inner(h), outer(h) {
    const size_t bs = hash_block_size(h);
    uint8_t k[128] = {0};
    if (klen > bs) hash(h, key, klen, k); else if (klen) std::memcpy(k, key, klen);
    uint8_t ip[128], op[128];
    for (size_t i = 0; i < bs; i++) { ip[i] = k[i] ^ 0x36; op[i] = k[i] ^ 0x5c; }
    inner.update(ip, bs); outer.update(op, bs);
    secure_zero(k, sizeof k); secure_zero(ip, sizeof ip); secure_zero(op, sizeof op);
  }
  void mac(const uint8_t* d1, size_t n1, const uint8_t* d2, size_t n2, uint8_t* out) const {
    HashCtx i = inner;
    if (n1) i.update(d1, n1);
    if (n2) i.update(d2, n2);
    uint8_t ih[64];
    i.final(ih);
    HashCtx o = outer;
    o.update(ih, hash_size(i.kind()));
    o.final(out);
    secure_zero(ih, sizeof ih);
  }
};
}  // namespace

void hmac(Hash h, const uint8_t* key, size_t klen, const uint8_t* data, size_t dlen, uint8_t* out) {
  HmacKey k(h, key, klen);
  k.mac(data, dlen, nullptr, 0, out);
}

bool pbkdf2(Hash h, const uint8_t* pw, size_t pwlen, const uint8_t* salt, size_t slen, uint32_t iterations, uint8_t* out, size_t outlen) {
  if (iterations == 0 || outlen == 0) return false;
  const size_t hl = hash_size(h);
  HmacKey key(h, pw, pwlen);
  std::vector<uint8_t> sblock(slen + 4);
  if (slen) std::memcpy(sblock.data(), salt, slen);
  uint8_t u[64], t[64];
  for (uint32_t block = 1; outlen > 0; block++) {
    put_be32(sblock.data() + slen, block);
    key.mac(sblock.data(), sblock.size(), nullptr, 0, u);
    std::memcpy(t, u, hl);
    for (uint32_t i = 1; i < iterations; i++) {
      key.mac(u, hl, nullptr, 0, u);
      for (size_t j = 0; j < hl; j++) t[j] ^= u[j];
    }
    size_t take = std::min(outlen, hl);
    std::memcpy(out, t, take);
    out += take; outlen -= take;
  }
  secure_zero(u, sizeof u); secure_zero(t, sizeof t);
  return true;
}

}  // namespace crypto
}  // namespace usbmem
