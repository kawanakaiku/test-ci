// SPDX-License-Identifier: Apache-2.0
// fscheck_*.cpp の内部共有部品 (公開しない)。
#pragma once
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "usbmem/fscheck.h"

namespace usbmem {
namespace fsck {

inline uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t le64(const uint8_t* p) { return le32(p) | (static_cast<uint64_t>(le32(p + 4)) << 32); }
inline void put16(uint8_t* p, uint16_t v) { p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8); }
inline void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>(v >> (8 * i)); }
inline void put64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = static_cast<uint8_t>(v >> (8 * i)); }
inline uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
inline uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
inline bool is_pow2(uint64_t v) { return v && !(v & (v - 1)); }
inline bool all_zero(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (p[i]) return false;
  return true;
}
inline uint32_t ilog2(uint64_t v) { uint32_t r = 0; while (v > 1) { v >>= 1; r++; } return r; }
// a * b が 64 ビットに収まるか
inline bool mul_ok(uint64_t a, uint64_t b, uint64_t* r) { return !__builtin_mul_overflow(a, b, r); }
inline bool add_ok(uint64_t a, uint64_t b, uint64_t* r) { return !__builtin_add_overflow(a, b, r); }

// 検査範囲の読み取り専用ビュー。64 KiB 単位のキャッシュ付き。範囲外の読み取りは OutOfRange。
// I/O エラーは最初の 1 つを io_error() に残す (呼び出し側は read の戻り値でも気づける)。
class Vol {
 public:
  Vol(BlockDevice& d, uint64_t off, uint64_t len) : dev_(d), off_(off), len_(len) {}
  uint64_t size() const { return len_; }
  Status read(uint64_t pos, void* buf, size_t n);
  Status io_error() const { return io_; }
  bool in_range(uint64_t pos, uint64_t n) const { return range_ok(len_, pos, n); }

 private:
  static constexpr uint64_t kChunk = 64 * 1024;
  static constexpr size_t kSlots = 256;
  Status chunk(uint64_t idx, const uint8_t** out);
  BlockDevice& dev_;
  uint64_t off_, len_;
  Status io_{};
  struct Slot { uint64_t idx = UINT64_MAX; std::vector<uint8_t> data; };
  std::vector<Slot> slots_;
};

// 1 ビット / 要素のビットマップ。メモリ上限のため確保失敗を false で返す。
class Bits {
 public:
  bool init(uint64_t n) {
    n_ = n;
    uint64_t words = (n + 63) / 64;
    if (words > (UINT64_MAX >> 4)) return false;
    try { w_.assign(static_cast<size_t>(words), 0); } catch (...) { return false; }
    return true;
  }
  uint64_t size() const { return n_; }
  bool get(uint64_t i) const { return i < n_ && ((w_[i >> 6] >> (i & 63)) & 1); }
  void set(uint64_t i) { if (i < n_) w_[i >> 6] |= (1ull << (i & 63)); }
  void clear(uint64_t i) { if (i < n_) w_[i >> 6] &= ~(1ull << (i & 63)); }
  uint64_t count() const {
    uint64_t c = 0;
    for (uint64_t x : w_) c += static_cast<uint64_t>(__builtin_popcountll(x));
    return c;
  }
  static uint64_t bytes_for(uint64_t n) { return (n + 63) / 64 * 8; }

 private:
  uint64_t n_ = 0;
  std::vector<uint64_t> w_;
};

// 検査の実行状態: 報告の蓄積・キャンセル・進捗。
class Ctx {
 public:
  Ctx(Vol& v, const FsCheckOptions& o, FsCheckReport& r) : vol(v), opt(o), rep(r) {}
  Vol& vol;
  const FsCheckOptions& opt;
  FsCheckReport& rep;
  uint64_t objects = 0;     // 走査した対象の数 (max_objects 用)

  void add(Severity s, const char* code, std::string msg, uint64_t offset = UINT64_MAX, std::string object = {});
  void error(const char* code, std::string msg, uint64_t offset = UINT64_MAX, std::string object = {}) { add(Severity::Error, code, std::move(msg), offset, std::move(object)); }
  void warn(const char* code, std::string msg, uint64_t offset = UINT64_MAX, std::string object = {}) { add(Severity::Warning, code, std::move(msg), offset, std::move(object)); }
  void info(const char* code, std::string msg, uint64_t offset = UINT64_MAX, std::string object = {}) { add(Severity::Info, code, std::move(msg), offset, std::move(object)); }
  // キャンセルされたか。立っていれば incomplete にして true
  bool stop();
  // 対象を 1 つ数える。上限を超えたら incomplete にして false
  bool count_object();
  void progress(const char* phase, uint64_t done, uint64_t total) { if (opt.progress) opt.progress(phase, done, total); }
  bool cancelled = false;
  bool over_budget(uint64_t bytes_needed) const { return bytes_needed > opt.memory_limit; }
  void incomplete(const char* code, const char* why);
};

std::string num(uint64_t v);
std::string hex(uint64_t v);

// 各 FS の検査 (読み取り専用)。I/O エラーは Status で返す。
Status check_fat(Ctx& c, FsType fs);
Status check_exfat(Ctx& c);
Status check_ext(Ctx& c, FsType fs);
Status check_ntfs(Ctx& c);

}  // namespace fsck
}  // namespace usbmem
