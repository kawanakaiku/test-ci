// SPDX-License-Identifier: Apache-2.0
#include "usbmem/crc32.h"

#include <array>
#include <cstring>

#if defined(__GNUC__) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ && !defined(USBMEM_NO_HW_CRC)
#if defined(__x86_64__)
#define USBMEM_CRC_X86 1
#include <cpuid.h>
#include <nmmintrin.h>
#elif defined(__aarch64__) && defined(__linux__)
#define USBMEM_CRC_ARM 1
#include <arm_acle.h>
#include <sys/auxv.h>
#ifndef HWCAP_CRC32
#define HWCAP_CRC32 (1 << 7)
#endif
#endif
#endif
#ifndef USBMEM_CRC_X86
#define USBMEM_CRC_X86 0
#endif
#ifndef USBMEM_CRC_ARM
#define USBMEM_CRC_ARM 0
#endif

namespace usbmem {
namespace {
constexpr std::array<uint32_t, 256> make_table() {
  std::array<uint32_t, 256> t{};
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    t[i] = c;
  }
  return t;
}
constexpr auto kTable = make_table();
}  // namespace

uint32_t crc32(const void* data, size_t len, uint32_t seed) {
  const auto* p = static_cast<const uint8_t*>(data);
  uint32_t c = ~seed;
  for (size_t i = 0; i < len; i++) c = kTable[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return ~c;
}
}  // namespace usbmem

// ---- CRC-32C
namespace usbmem {
namespace {
struct Crc32cTables {
  uint32_t t[8][256];
  Crc32cTables() {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : (c >> 1);
      t[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; i++)
      for (int s = 1; s < 8; s++) t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFF];
  }
};
const Crc32cTables& c32c_tables() { static const Crc32cTables t; return t; }

#if USBMEM_CRC_X86
#define CRC_TARGET __attribute__((target("sse4.2")))
CRC_TARGET uint32_t crc32c_hw(uint32_t crc, const uint8_t* p, size_t n) {
  uint64_t c = crc;
  // 3 本の独立した流れに分けると命令の遅延が隠れるが、ここでは単純な 8 バイト逐次 (数 GB/s) で十分
  while (n >= 8) { uint64_t v; std::memcpy(&v, p, 8); c = _mm_crc32_u64(c, v); p += 8; n -= 8; }
  uint32_t c32 = uint32_t(c);
  while (n--) c32 = _mm_crc32_u8(c32, *p++);
  return c32;
}
bool crc_cpu_has() {
  unsigned a, b, c, d;
  if (!__get_cpuid(1, &a, &b, &c, &d)) return false;
  return (c & (1u << 20)) != 0;  // SSE4.2
}
#elif USBMEM_CRC_ARM
#if defined(__clang__)
#define CRC_TARGET __attribute__((target("crc")))
#else
#define CRC_TARGET __attribute__((target("+crc")))
#endif
CRC_TARGET uint32_t crc32c_hw(uint32_t crc, const uint8_t* p, size_t n) {
  while (n >= 8) { uint64_t v; std::memcpy(&v, p, 8); crc = __crc32cd(crc, v); p += 8; n -= 8; }
  while (n--) crc = __crc32cb(crc, *p++);
  return crc;
}
bool crc_cpu_has() { return (getauxval(AT_HWCAP) & HWCAP_CRC32) != 0; }
#endif

#if USBMEM_CRC_X86 || USBMEM_CRC_ARM
// 命令版が表引き版と一致するときだけ使う (初回のみ判定)
bool hw_ok() {
  static const bool ok = [] {
    if (!crc_cpu_has()) return false;
    uint8_t buf[257];
    uint32_t x = 0x2545F491u;
    for (auto& b : buf) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; b = uint8_t(x >> 11); }
    for (size_t len : {size_t(0), size_t(1), size_t(7), size_t(8), size_t(9), size_t(63), size_t(257)})
      for (size_t skew : {size_t(0), size_t(1), size_t(3)}) {
        if (skew + len > sizeof buf) continue;
        if (crc32c_hw(0xFFFFFFFFu, buf + skew, len) != crc32c_raw_sw(0xFFFFFFFFu, buf + skew, len)) return false;
        if (crc32c_hw(0x12345678u, buf + skew, len) != crc32c_raw_sw(0x12345678u, buf + skew, len)) return false;
      }
    return true;
  }();
  return ok;
}
#endif
}  // namespace

uint32_t crc32c_raw_sw(uint32_t crc, const void* data, size_t len) {
  const auto* p = static_cast<const uint8_t*>(data);
  const Crc32cTables& T = c32c_tables();
  while (len && (reinterpret_cast<uintptr_t>(p) & 3)) { crc = T.t[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8); len--; }
  while (len >= 8) {
    uint32_t a, b;
    std::memcpy(&a, p, 4);
    std::memcpy(&b, p + 4, 4);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    a = __builtin_bswap32(a); b = __builtin_bswap32(b);
#endif
    a ^= crc;
    crc = T.t[7][a & 0xFF] ^ T.t[6][(a >> 8) & 0xFF] ^ T.t[5][(a >> 16) & 0xFF] ^ T.t[4][a >> 24] ^
          T.t[3][b & 0xFF] ^ T.t[2][(b >> 8) & 0xFF] ^ T.t[1][(b >> 16) & 0xFF] ^ T.t[0][b >> 24];
    p += 8; len -= 8;
  }
  while (len--) crc = T.t[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  return crc;
}

bool crc32c_hw_active() {
#if USBMEM_CRC_X86 || USBMEM_CRC_ARM
  return hw_ok();
#else
  return false;
#endif
}

uint32_t crc32c_raw(uint32_t crc, const void* data, size_t len) {
#if USBMEM_CRC_X86 || USBMEM_CRC_ARM
  if (len >= 8 && hw_ok()) return crc32c_hw(crc, static_cast<const uint8_t*>(data), len);
#endif
  return crc32c_raw_sw(crc, data, len);
}
}  // namespace usbmem
