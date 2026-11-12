// SPDX-License-Identifier: Apache-2.0
// mkfs_*.cpp の内部共有部品 (公開しない)。
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "usbmem/mkfs.h"

namespace usbmem {
namespace mkfsimpl {

inline void p16(uint8_t* p, uint32_t v) { p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8); }
inline void p32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>(v >> (8 * i)); }
inline void p64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = static_cast<uint8_t>(v >> (8 * i)); }
inline uint32_t g32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24); }
inline bool pow2(uint64_t v) { return v && !(v & (v - 1)); }
inline uint32_t ilog2(uint64_t v) { uint32_t r = 0; while (v > 1) { v >>= 1; r++; } return r; }
inline uint64_t ceil_div(uint64_t a, uint64_t b) { return (a + b - 1) / b; }
inline uint64_t align_up(uint64_t a, uint64_t b) { return ceil_div(a, b) * b; }

// 組み立ての入力 (Mkfs::plan が検証したあとの値)
struct Params {
  MkfsOptions opt;
  uint64_t length = 0;       // 範囲の長さ (バイト)
  uint32_t sector_size = 512;
  uint64_t now = 0;          // UNIX 秒
};

// 組み立ての出力
struct Build {
  std::vector<MkfsStage> stages;
  std::vector<std::pair<std::string, std::string>> layout;
  std::vector<std::string> changes;
  std::vector<Diagnostic> problems;

  void error(const char* code, std::string msg) { problems.push_back({Severity::Error, code, std::move(msg)}); }
  void warn(const char* code, std::string msg) { problems.push_back({Severity::Warning, code, std::move(msg)}); }
  bool has_error() const {
    for (auto& d : problems) if (d.severity == Severity::Error) return true;
    return false;
  }
  MkfsStage& stage(const std::string& name) {
    for (auto& s : stages) if (s.name == name) return s;
    stages.push_back(MkfsStage{name, {}});
    return stages.back();
  }
  void data(const std::string& stage_name, uint64_t off, std::vector<uint8_t> bytes, const char* what) {
    MkfsRegion r;
    r.offset = off;
    r.length = bytes.size();
    r.data = std::move(bytes);
    r.what = what;
    stage(stage_name).regions.push_back(std::move(r));
  }
  void zero(const std::string& stage_name, uint64_t off, uint64_t len, const char* what) {
    if (!len) return;
    MkfsRegion r;
    r.offset = off;
    r.length = len;
    r.what = what;
    stage(stage_name).regions.push_back(std::move(r));
  }
};

void build_fat32(const Params& p, Build* b);
void build_exfat(const Params& p, Build* b);
void build_ext2(const Params& p, Build* b);

}  // namespace mkfsimpl
}  // namespace usbmem
