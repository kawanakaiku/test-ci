// SPDX-License-Identifier: Apache-2.0
// transform 実装内部の共有宣言 (公開 API ではない)。
#pragma once
#include <string>

#include "usbmem/transform.h"

namespace usbmem {
namespace tf {

// 各形式の probe / open。その形式でなければ probe は Unsupported (info 未変更)、open は Unsupported。
Status bitlocker_probe(BlockDevice& dev, VolumeInfo* info);
Status bitlocker_open(BlockDevicePtr base, const Credentials& cred, const OpenOptions& opt, BlockDevicePtr* out, VolumeInfo* info);
Status luks_probe(BlockDevice& dev, VolumeInfo* info);
Status luks_open(BlockDevicePtr base, const Credentials& cred, const OpenOptions& opt, BlockDevicePtr* out, VolumeInfo* info);

// 読み取り補助: 範囲検査つき。base が短ければ Corrupt ではなく OutOfRange をそのまま返す。
Status read_exact(BlockDevice& dev, uint64_t off, void* buf, size_t len);

inline uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
inline uint64_t le64(const uint8_t* p) { return uint64_t(le32(p)) | (uint64_t(le32(p + 4)) << 32); }
inline uint16_t be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
inline uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
inline uint64_t be64(const uint8_t* p) { return (uint64_t(be32(p)) << 32) | be32(p + 4); }
inline void put_le64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = uint8_t(v >> (8 * i)); }

}  // namespace tf
}  // namespace usbmem
