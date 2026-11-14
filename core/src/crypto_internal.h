// SPDX-License-Identifier: Apache-2.0
// 暗号実装の内部宣言 (公開 API ではない)。AES のハードウェア命令版 (crypto_aes_hw.cpp)。
#pragma once
#include "usbmem/crypto.h"

// ハードウェア AES を試すビルドか。x86 (AES-NI) と AArch64 Linux/Android (ARMv8 Crypto Extensions)。リトルエンディアン + GCC/Clang のみ。
#if defined(__GNUC__) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ && !defined(USBMEM_NO_HW_AES)
#if defined(__x86_64__) || defined(__i386__)
#define USBMEM_HW_X86 1
#elif defined(__aarch64__) && defined(__linux__)
#define USBMEM_HW_ARM 1
#endif
#endif
#ifndef USBMEM_HW_X86
#define USBMEM_HW_X86 0
#endif
#ifndef USBMEM_HW_ARM
#define USBMEM_HW_ARM 0
#endif
#define USBMEM_HW_ANY (USBMEM_HW_X86 || USBMEM_HW_ARM)

namespace usbmem {
namespace crypto {

#if USBMEM_HW_ANY
struct AesHw {
  static bool cpu_has();       // CPUID / HWCAP に AES 命令があるか
  static bool selftest();      // 表引き版と全モードで一致するか (鍵長 128/192/256 × ECB / CBC / XTS、乱数データ)
  static void enc_block(const Aes& k, const uint8_t in[16], uint8_t out[16]);
  static void dec_block(const Aes& k, const uint8_t in[16], uint8_t out[16]);
  static void cbc_dec(const Aes& k, const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t len);
  static void xts(const Aes& k1, const Aes& k2, uint64_t tweak, const uint8_t* in, uint8_t* out, size_t len, bool dec);
};

// SHA-256 の圧縮関数 (x86 SHA-NI / ARMv8 SHA2)。K = SHA-256 のラウンド定数 64 個。使うかどうかの判定 (自己テスト) は crypto_hash.cpp。
bool sha256_hw_cpu_has();
void sha256_hw_blocks(uint32_t st[8], const uint8_t* p, size_t nblocks, const uint32_t* K);
#endif

}  // namespace crypto
}  // namespace usbmem
