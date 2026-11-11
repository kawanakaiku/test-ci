// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <cstdint>

namespace usbmem {
// CRC-32 (IEEE 802.3, 反射, 多項式 0xEDB88320)。GPT が使うもの。
uint32_t crc32(const void* data, size_t len, uint32_t seed = 0);
}  // namespace usbmem
