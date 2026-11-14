// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <cstdint>

namespace usbmem {
// CRC-32 (IEEE 802.3, 反射, 多項式 0xEDB88320)。GPT が使うもの。
uint32_t crc32(const void* data, size_t len, uint32_t seed = 0);
// CRC-32C (Castagnoli, 反射, 多項式 0x82F63B78) の生の更新: seed をそのまま使い、前後の反転はしない (Linux の crc32c() / Btrfs / ext4 の流儀。
// 標準の CRC-32C は crc32c_raw(0xFFFFFFFF, ...) の結果を ~ したもの)。CPU に CRC32C 命令 (x86 SSE4.2 / ARMv8 CRC32) があればそれを使い、なければ 8 バイト単位の表引き。
uint32_t crc32c_raw(uint32_t crc, const void* data, size_t len);
// 表引きの基準実装 (テスト用。命令版との一致確認に使う)
uint32_t crc32c_raw_sw(uint32_t crc, const void* data, size_t len);
// この CPU / ビルドで命令版を使っているか (診断用)
bool crc32c_hw_active();
}  // namespace usbmem
