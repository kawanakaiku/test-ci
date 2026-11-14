// SPDX-License-Identifier: Apache-2.0
// SHA-256 の圧縮関数のハードウェア命令版 (x86 SHA-NI / AArch64 ARMv8 SHA2)。BitLocker のパスワード鍵伸長 (SHA-256 × 0x100000 回) と
// LUKS の PBKDF2-SHA256 の主要コスト。CPU の対応を実行時に調べ、表の実装との一致を自己テストしてから使う (crypto_hash.cpp)。
#include "crypto_internal.h"

#if USBMEM_HW_ANY

#if USBMEM_HW_X86
#include <cpuid.h>
#include <immintrin.h>
#define SHA_TARGET __attribute__((target("sha,sse4.1,ssse3")))
#else
#include <arm_neon.h>
#include <sys/auxv.h>
#ifndef HWCAP_SHA2
#define HWCAP_SHA2 (1 << 6)
#endif
#if defined(__clang__)
#define SHA_TARGET __attribute__((target("sha2")))
#else
#define SHA_TARGET __attribute__((target("+crypto")))
#endif
#endif

namespace usbmem {
namespace crypto {

bool sha256_hw_cpu_has() {
#if USBMEM_HW_X86
  unsigned a, b, c, d;
  if (!__get_cpuid(1, &a, &b, &c, &d)) return false;
  const bool ssse3 = (c & (1u << 9)) != 0, sse41 = (c & (1u << 19)) != 0;
  if (!__get_cpuid_count(7, 0, &a, &b, &c, &d)) return false;
  return ssse3 && sse41 && (b & (1u << 29)) != 0;  // SHA
#else
  return (getauxval(AT_HWCAP) & HWCAP_SHA2) != 0;
#endif
}

#if USBMEM_HW_X86
SHA_TARGET void sha256_hw_blocks(uint32_t st[8], const uint8_t* p, size_t nblocks, const uint32_t* K) {
  const __m128i mask = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);
  __m128i tmp = _mm_loadu_si128(reinterpret_cast<const __m128i*>(st));          // DCBA
  __m128i s1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(st + 4));       // HGFE
  tmp = _mm_shuffle_epi32(tmp, 0xB1);                                           // CDAB
  s1 = _mm_shuffle_epi32(s1, 0x1B);                                             // EFGH
  __m128i s0 = _mm_alignr_epi8(tmp, s1, 8);                                     // ABEF
  s1 = _mm_blend_epi16(s1, tmp, 0xF0);                                          // CDGH
  while (nblocks--) {
    const __m128i save0 = s0, save1 = s1;
    __m128i w[4];
#pragma GCC unroll 16
    for (int t = 0; t < 16; t++) {
      if (t < 4) {
        w[t] = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 16 * t)), mask);
      } else {
        // W[t] = msg2(msg1(W[t-4], W[t-3]) + alignr(W[t-1], W[t-2], 4), W[t-1])
        __m128i m = _mm_sha256msg1_epu32(w[t & 3], w[(t + 1) & 3]);
        m = _mm_add_epi32(m, _mm_alignr_epi8(w[(t + 3) & 3], w[(t + 2) & 3], 4));
        w[t & 3] = _mm_sha256msg2_epu32(m, w[(t + 3) & 3]);
      }
      __m128i msg = _mm_add_epi32(w[t & 3], _mm_loadu_si128(reinterpret_cast<const __m128i*>(K + 4 * t)));
      s1 = _mm_sha256rnds2_epu32(s1, s0, msg);
      msg = _mm_shuffle_epi32(msg, 0x0E);
      s0 = _mm_sha256rnds2_epu32(s0, s1, msg);
    }
    s0 = _mm_add_epi32(s0, save0);
    s1 = _mm_add_epi32(s1, save1);
    p += 64;
  }
  tmp = _mm_shuffle_epi32(s0, 0x1B);        // FEBA
  s1 = _mm_shuffle_epi32(s1, 0xB1);         // DCHG
  s0 = _mm_blend_epi16(tmp, s1, 0xF0);      // DCBA
  s1 = _mm_alignr_epi8(s1, tmp, 8);         // HGFE
  _mm_storeu_si128(reinterpret_cast<__m128i*>(st), s0);
  _mm_storeu_si128(reinterpret_cast<__m128i*>(st + 4), s1);
}
#else
SHA_TARGET void sha256_hw_blocks(uint32_t st[8], const uint8_t* p, size_t nblocks, const uint32_t* K) {
  uint32x4_t s0 = vld1q_u32(st), s1 = vld1q_u32(st + 4);  // ABCD / EFGH
  while (nblocks--) {
    const uint32x4_t save0 = s0, save1 = s1;
    uint32x4_t w[4];
#pragma GCC unroll 16
    for (int t = 0; t < 16; t++) {
      if (t < 4) w[t] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p + 16 * t)));
      else w[t & 3] = vsha256su1q_u32(vsha256su0q_u32(w[t & 3], w[(t + 1) & 3]), w[(t + 2) & 3], w[(t + 3) & 3]);
      const uint32x4_t msg = vaddq_u32(w[t & 3], vld1q_u32(K + 4 * t));
      const uint32x4_t prev = s0;
      s0 = vsha256hq_u32(s0, s1, msg);
      s1 = vsha256h2q_u32(s1, prev, msg);
    }
    s0 = vaddq_u32(s0, save0);
    s1 = vaddq_u32(s1, save1);
    p += 64;
  }
  vst1q_u32(st, s0);
  vst1q_u32(st + 4, s1);
}
#endif

}  // namespace crypto
}  // namespace usbmem

#endif  // USBMEM_HW_ANY
