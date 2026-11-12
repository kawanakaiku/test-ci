// SPDX-License-Identifier: Apache-2.0
// SHA-256 (FIPS 180-4)。確認トークンとテーブルバックアップの整合性検査に使う小さな自前実装。
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace usbmem {

class Sha256 {
 public:
  Sha256() { reset(); }
  void reset();
  void update(const void* data, size_t len);
  std::array<uint8_t, 32> finish();  // 以後は reset() するまで使えない

  static std::array<uint8_t, 32> hash(const void* data, size_t len) {
    Sha256 s;
    s.update(data, len);
    return s.finish();
  }

 private:
  void block(const uint8_t* p);
  uint32_t h_[8];
  uint8_t buf_[64];
  size_t buf_len_;
  uint64_t total_;
};

std::string hex_encode(const uint8_t* p, size_t n);
inline std::string hex_encode(const std::array<uint8_t, 32>& d) { return hex_encode(d.data(), d.size()); }

}  // namespace usbmem
