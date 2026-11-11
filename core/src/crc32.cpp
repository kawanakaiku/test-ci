// SPDX-License-Identifier: Apache-2.0
#include "usbmem/crc32.h"

#include <array>

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
