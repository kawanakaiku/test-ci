// SPDX-License-Identifier: Apache-2.0
// カービングの内部部品: 読み取り窓 (CarveReader) と形式の定義 (FormatDef)。
#pragma once
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "usbmem/block_device.h"
#include "usbmem/carve.h"

namespace usbmem {
namespace carve {

inline uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t le32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24); }
inline uint64_t le64(const uint8_t* p) { return static_cast<uint64_t>(le32(p)) | (static_cast<uint64_t>(le32(p + 4)) << 32); }
inline uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
inline uint32_t be32(const uint8_t* p) { return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3]; }
inline uint64_t be64(const uint8_t* p) { return (static_cast<uint64_t>(be32(p)) << 32) | be32(p + 4); }

// デバイスの範囲 [0, limit) を読む。少量の先読み付き。読み取りエラーや範囲外は nullptr を返す (io_error は I/O の失敗だけ立つ)。
class CarveReader {
 public:
  CarveReader(BlockDevice& dev, uint64_t limit, const CancelToken* cancel) : dev_(dev), limit_(limit), cancel_(cancel) {}
  // [off, off+n) を指すポインタ (次の at / stream までのみ有効)。n は 64 KiB まで。
  const uint8_t* at(uint64_t off, size_t n);
  // [off, off+len) を 1 MiB 以下の塊で fn へ渡す。fn が false を返すと止める (その時点までの結果は成功)。読めなければ false。
  bool stream(uint64_t off, uint64_t len, const std::function<bool(const uint8_t*, size_t)>& fn);
  uint64_t limit() const { return limit_; }
  void set_limit(uint64_t l) { limit_ = l; wstart_ = wend_ = 0; }
  void reset_error() { io_error_ = false; }
  bool cancelled() const { return cancel_ && cancel_->cancelled(); }
  bool io_error() const { return io_error_; }
  Status last_status() const { return st_; }
  uint64_t reads = 0;

 private:
  BlockDevice& dev_;
  uint64_t limit_;
  const CancelToken* cancel_;
  std::vector<uint8_t> buf_;
  uint64_t wstart_ = 0, wend_ = 0;
  bool io_error_ = false;
  Status st_;
};

struct Match {
  uint64_t length = 0;
  std::string ext;
  std::string detail;
  int confidence = 0;
};

// start から始まるファイルが形式に合うか。reader の limit までを範囲とする (max_len: これを超える長さは受け付けない)。
using Validator = bool (*)(CarveReader& r, uint64_t start, uint64_t max_len, Match* m);
// 先頭 n (>= 16) バイトの素早い判定 (署名)
using QuickCheck = bool (*)(const uint8_t* p, size_t n);

struct FormatDef {
  const char* id;
  const char* exts;
  const char* desc;
  QuickCheck quick;
  Validator validate;
  uint64_t min_len;   // これより短い一致は構造の偶然の一致とみなす
};

const std::vector<FormatDef>& formats();

}  // namespace carve
}  // namespace usbmem
