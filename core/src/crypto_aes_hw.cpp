// SPDX-License-Identifier: Apache-2.0
// AES のハードウェア命令版: x86 AES-NI / AArch64 ARMv8 Crypto Extensions。
// 関数単位の target 属性で命令を有効にする (ビルド全体は -maes / -march=...+crypto を要しない)。実行する前に CPU の対応を実行時に調べ、
// さらに表引き版 (crypto_aes.cpp) と全モードで一致することを自己テストしてから使う (一致しなければ使わない)。
#include <cstring>

#include "crypto_internal.h"

#if USBMEM_HW_ANY

#if USBMEM_HW_X86
#include <cpuid.h>
#include <immintrin.h>
#define HW_TARGET __attribute__((target("aes,sse2,ssse3")))
#else
#include <arm_neon.h>
#include <sys/auxv.h>
#ifndef HWCAP_AES
#define HWCAP_AES (1 << 3)
#endif
#if defined(__clang__)
#define HW_TARGET __attribute__((target("aes")))
#else
#define HW_TARGET __attribute__((target("+crypto")))
#endif
#endif

namespace usbmem {
namespace crypto {

namespace {

#if USBMEM_HW_X86
typedef __m128i V;
HW_TARGET inline V v_ld(const uint8_t* p) { return _mm_loadu_si128(reinterpret_cast<const __m128i*>(p)); }
HW_TARGET inline void v_st(uint8_t* p, V v) { _mm_storeu_si128(reinterpret_cast<__m128i*>(p), v); }
HW_TARGET inline V v_xor(V a, V b) { return _mm_xor_si128(a, b); }
// 暗号: rk = ラウンド鍵 (nr + 1 本)
HW_TARGET inline V enc_rounds(const uint8_t* rk, int nr, V b) {
  b = v_xor(b, v_ld(rk));
  for (int i = 1; i < nr; i++) b = _mm_aesenc_si128(b, v_ld(rk + 16 * i));
  return _mm_aesenclast_si128(b, v_ld(rk + 16 * nr));
}
// 復号: rk = 等価逆暗号の鍵 (hdk_)
HW_TARGET inline V dec_rounds(const uint8_t* rk, int nr, V b) {
  b = v_xor(b, v_ld(rk));
  for (int i = 1; i < nr; i++) b = _mm_aesdec_si128(b, v_ld(rk + 16 * i));
  return _mm_aesdeclast_si128(b, v_ld(rk + 16 * nr));
}
// 4 ブロック並列 (命令の遅延を隠す)
HW_TARGET inline void enc_rounds4(const uint8_t* rk, int nr, V b[4]) {
  V k = v_ld(rk);
  for (int j = 0; j < 4; j++) b[j] = v_xor(b[j], k);
  for (int i = 1; i < nr; i++) {
    k = v_ld(rk + 16 * i);
    for (int j = 0; j < 4; j++) b[j] = _mm_aesenc_si128(b[j], k);
  }
  k = v_ld(rk + 16 * nr);
  for (int j = 0; j < 4; j++) b[j] = _mm_aesenclast_si128(b[j], k);
}
HW_TARGET inline void dec_rounds4(const uint8_t* rk, int nr, V b[4]) {
  V k = v_ld(rk);
  for (int j = 0; j < 4; j++) b[j] = v_xor(b[j], k);
  for (int i = 1; i < nr; i++) {
    k = v_ld(rk + 16 * i);
    for (int j = 0; j < 4; j++) b[j] = _mm_aesdec_si128(b[j], k);
  }
  k = v_ld(rk + 16 * nr);
  for (int j = 0; j < 4; j++) b[j] = _mm_aesdeclast_si128(b[j], k);
}
#else  // ARM
typedef uint8x16_t V;
HW_TARGET inline V v_ld(const uint8_t* p) { return vld1q_u8(p); }
HW_TARGET inline void v_st(uint8_t* p, V v) { vst1q_u8(p, v); }
HW_TARGET inline V v_xor(V a, V b) { return veorq_u8(a, b); }
// 暗号: aese は AddRoundKey → SubBytes/ShiftRows、aesmc は MixColumns。最後のラウンドは MixColumns なしで、末尾の鍵を XOR する
HW_TARGET inline V enc_rounds(const uint8_t* rk, int nr, V b) {
  for (int i = 0; i < nr - 1; i++) b = vaesmcq_u8(vaeseq_u8(b, v_ld(rk + 16 * i)));
  b = vaeseq_u8(b, v_ld(rk + 16 * (nr - 1)));
  return v_xor(b, v_ld(rk + 16 * nr));
}
// 復号: 等価逆暗号の鍵 (hdk_) を先頭から。aesd は AddRoundKey → InvShiftRows/InvSubBytes、aesimc は InvMixColumns
HW_TARGET inline V dec_rounds(const uint8_t* rk, int nr, V b) {
  for (int i = 0; i < nr - 1; i++) b = vaesimcq_u8(vaesdq_u8(b, v_ld(rk + 16 * i)));
  b = vaesdq_u8(b, v_ld(rk + 16 * (nr - 1)));
  return v_xor(b, v_ld(rk + 16 * nr));
}
HW_TARGET inline void enc_rounds4(const uint8_t* rk, int nr, V b[4]) {
  for (int i = 0; i < nr - 1; i++) {
    V k = v_ld(rk + 16 * i);
    for (int j = 0; j < 4; j++) b[j] = vaesmcq_u8(vaeseq_u8(b[j], k));
  }
  V k = v_ld(rk + 16 * (nr - 1));
  for (int j = 0; j < 4; j++) b[j] = vaeseq_u8(b[j], k);
  k = v_ld(rk + 16 * nr);
  for (int j = 0; j < 4; j++) b[j] = v_xor(b[j], k);
}
HW_TARGET inline void dec_rounds4(const uint8_t* rk, int nr, V b[4]) {
  for (int i = 0; i < nr - 1; i++) {
    V k = v_ld(rk + 16 * i);
    for (int j = 0; j < 4; j++) b[j] = vaesimcq_u8(vaesdq_u8(b[j], k));
  }
  V k = v_ld(rk + 16 * (nr - 1));
  for (int j = 0; j < 4; j++) b[j] = vaesdq_u8(b[j], k);
  k = v_ld(rk + 16 * nr);
  for (int j = 0; j < 4; j++) b[j] = v_xor(b[j], k);
}
#endif

// XTS の tweak (128 ビット LE) に α (x) を掛ける
inline void xts_next(uint64_t* lo, uint64_t* hi) {
  const uint64_t carry = *hi >> 63;
  *hi = (*hi << 1) | (*lo >> 63);
  *lo = (*lo << 1) ^ (carry ? 0x87 : 0);
}

}  // namespace

bool AesHw::cpu_has() {
#if USBMEM_HW_X86
  unsigned a, b, c, d;
  if (!__get_cpuid(1, &a, &b, &c, &d)) return false;
  return (c & (1u << 25)) != 0 && (c & (1u << 9)) != 0 && (d & (1u << 26)) != 0;  // AES / SSSE3 / SSE2
#else
  return (getauxval(AT_HWCAP) & HWCAP_AES) != 0;
#endif
}

HW_TARGET void AesHw::enc_block(const Aes& k, const uint8_t in[16], uint8_t out[16]) { v_st(out, enc_rounds(k.hek_, k.rounds_, v_ld(in))); }
HW_TARGET void AesHw::dec_block(const Aes& k, const uint8_t in[16], uint8_t out[16]) { v_st(out, dec_rounds(k.hdk_, k.rounds_, v_ld(in))); }

HW_TARGET void AesHw::cbc_dec(const Aes& k, const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t len) {
  const int nr = k.rounds_;
  V prev = v_ld(iv);
  size_t o = 0;
  for (; o + 64 <= len; o += 64) {
    V c[4], b[4];
    for (int j = 0; j < 4; j++) c[j] = b[j] = v_ld(in + o + 16 * size_t(j));  // in == out の場合に備えて暗号文を先に読む
    dec_rounds4(k.hdk_, nr, b);
    v_st(out + o, v_xor(b[0], prev));
    for (int j = 1; j < 4; j++) v_st(out + o + 16 * size_t(j), v_xor(b[j], c[j - 1]));
    prev = c[3];
  }
  for (; o + 16 <= len; o += 16) {
    V c = v_ld(in + o);
    v_st(out + o, v_xor(dec_rounds(k.hdk_, nr, c), prev));
    prev = c;
  }
}

HW_TARGET void AesHw::xts(const Aes& k1, const Aes& k2, uint64_t tweak, const uint8_t* in, uint8_t* out, size_t len, bool dec) {
  uint8_t t0[16] = {0};
  for (int i = 0; i < 8; i++) t0[i] = uint8_t(tweak >> (8 * i));
  v_st(t0, enc_rounds(k2.hek_, k2.rounds_, v_ld(t0)));
  uint64_t lo, hi;
  std::memcpy(&lo, t0, 8);
  std::memcpy(&hi, t0 + 8, 8);
  const int nr = k1.rounds_;
  const uint8_t* rk = dec ? k1.hdk_ : k1.hek_;
  size_t o = 0;
  for (; o + 64 <= len; o += 64) {
    uint8_t tw[64];
    for (int j = 0; j < 4; j++) {
      std::memcpy(tw + 16 * j, &lo, 8);
      std::memcpy(tw + 16 * j + 8, &hi, 8);
      xts_next(&lo, &hi);
    }
    V t[4], b[4];
    for (int j = 0; j < 4; j++) { t[j] = v_ld(tw + 16 * j); b[j] = v_xor(v_ld(in + o + 16 * size_t(j)), t[j]); }
    if (dec) dec_rounds4(rk, nr, b); else enc_rounds4(rk, nr, b);
    for (int j = 0; j < 4; j++) v_st(out + o + 16 * size_t(j), v_xor(b[j], t[j]));
  }
  for (; o + 16 <= len; o += 16) {
    uint8_t tw[16];
    std::memcpy(tw, &lo, 8);
    std::memcpy(tw + 8, &hi, 8);
    xts_next(&lo, &hi);
    V t = v_ld(tw);
    V b = v_xor(v_ld(in + o), t);
    b = dec ? dec_rounds(rk, nr, b) : enc_rounds(rk, nr, b);
    v_st(out + o, v_xor(b, t));
  }
}

// 表引き版 (set_key_sw) とハードウェア版 (強制) を、鍵長 3 種 × {ブロック暗号・復号, CBC 復号, XTS 暗復号} で突き合わせる。
// 既知値 (FIPS 197 付録 C) も確認する。1 つでも違えばハードウェア版は使わない。
bool AesHw::selftest() {
  // FIPS 197 C.1: AES-128 鍵 000102..0f, 平文 00112233..ff, 暗号文 69c4e0d86a7b0430d8cdb78070b4c55a
  static const uint8_t kPt[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
  static const uint8_t kCt[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30, 0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};
  uint8_t key[64];
  uint32_t x = 0x12345678u;
  auto rnd = [&]() { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return uint8_t(x >> 8); };
  for (int i = 0; i < 64; i++) key[i] = uint8_t(i);
  {
    Aes a;
    a.set_key_impl(key, 16, Aes::kForce);
    uint8_t o[16], p[16];
    a.encrypt_block(kPt, o);
    if (std::memcmp(o, kCt, 16) != 0) return false;
    a.decrypt_block(kCt, p);
    if (std::memcmp(p, kPt, 16) != 0) return false;
  }
  const size_t kLen = 16 * 11;  // 4 ブロック単位 + 端数の両方を通す
  uint8_t data[16 * 11], o1[16 * 11], o2[16 * 11], iv[16];
  for (size_t kl : {size_t(16), size_t(24), size_t(32)}) {
    for (size_t i = 0; i < 64; i++) key[i] = rnd();
    for (size_t i = 0; i < kLen; i++) data[i] = rnd();
    for (int i = 0; i < 16; i++) iv[i] = rnd();
    Aes hw, sw, hw2, sw2;
    if (!hw.set_key_impl(key, kl, Aes::kForce) || !sw.set_key_impl(key, kl, Aes::kSwOnly)) return false;
    if (!hw2.set_key_impl(key + 32, kl, Aes::kForce) || !sw2.set_key_impl(key + 32, kl, Aes::kSwOnly)) return false;
    if (!hw.hw() || sw.hw()) return false;
    for (size_t o = 0; o + 16 <= kLen; o += 16) {  // ブロック単位
      hw.encrypt_block(data + o, o1); sw.encrypt_block(data + o, o2);
      if (std::memcmp(o1, o2, 16) != 0) return false;
      hw.decrypt_block(data + o, o1); sw.decrypt_block(data + o, o2);
      if (std::memcmp(o1, o2, 16) != 0) return false;
    }
    aes_cbc_decrypt(hw, iv, data, o1, kLen); aes_cbc_decrypt(sw, iv, data, o2, kLen);
    if (std::memcmp(o1, o2, kLen) != 0) return false;
    std::memcpy(o1, data, kLen);  // in == out
    aes_cbc_decrypt(hw, iv, o1, o1, kLen);
    if (std::memcmp(o1, o2, kLen) != 0) return false;
    for (int dec = 0; dec < 2; dec++) {
      const uint64_t tw = 0x0123456789abcdefull * (kl + 1);
      if (dec) { aes_xts_decrypt(hw, hw2, tw, data, o1, kLen); aes_xts_decrypt(sw, sw2, tw, data, o2, kLen); }
      else { aes_xts_encrypt(hw, hw2, tw, data, o1, kLen); aes_xts_encrypt(sw, sw2, tw, data, o2, kLen); }
      if (std::memcmp(o1, o2, kLen) != 0) return false;
    }
  }
  return true;
}

}  // namespace crypto
}  // namespace usbmem

#endif  // USBMEM_HW_ANY
