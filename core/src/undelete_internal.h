// SPDX-License-Identifier: Apache-2.0
// 削除ファイル列挙の内部部品 (FS ごとの解析が共有する文脈・バイト読み出し・文字コード・時刻変換)。
#pragma once
#include <cstring>
#include <string>
#include <vector>

#include "usbmem/backup.h"
#include "usbmem/undelete.h"

namespace usbmem {
namespace ud {

using Bytes = std::vector<uint8_t>;

inline uint16_t L16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t L32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24); }
inline uint64_t L64(const uint8_t* p) { return static_cast<uint64_t>(L32(p)) | (static_cast<uint64_t>(L32(p + 4)) << 32); }
inline uint32_t B32(const uint8_t* p) { return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3]; }
inline uint64_t B64(const uint8_t* p) { return (static_cast<uint64_t>(B32(p)) << 32) | B32(p + 4); }

// 解析の文脈。res->files へ追加する。
struct Ctx {
  BlockDevice& dev;
  const UndeleteOptions& opt;
  UndeleteResult& res;
  UsedMap used;
  bool used_ok = false;  // 割り当て情報が使える
  Ctx(BlockDevice& d, const UndeleteOptions& o, UndeleteResult& r) : dev(d), opt(o), res(r) {}

  bool cancelled() const { return opt.cancel && opt.cancel->cancelled(); }
  void progress(const char* phase, uint64_t done, uint64_t total) const { if (opt.progress) opt.progress(phase, done, total); }
  Status rd(uint64_t off, size_t len, Bytes* out) {
    out->resize(len);
    IoOptions io;
    io.cancel = opt.cancel;
    return dev.read_at(off, out->data(), len, io);
  }
  // 読み取りの失敗のうち、続けても意味がないもの (取り消し・切断)
  static bool fatal(const Status& s) { return s.code == Errc::Cancelled || s.code == Errc::Disconnected || s.code == Errc::Closed; }
  bool full() const { return res.files.size() >= opt.max_files; }
  void add(DeletedFile&& f) {
    if (full()) { res.truncated = true; return; }
    f.id = static_cast<uint32_t>(res.files.size());
    res.files.push_back(std::move(f));
  }
  void warn(const std::string& w) { if (res.warnings.size() < 64) res.warnings.push_back(w); }
};

// 文字コード / 時刻
void append_utf8(std::string* out, uint32_t cp);
std::string utf16_to_utf8(const uint16_t* u, size_t n);
int64_t dos_to_unix(uint16_t date, uint16_t time);  // FAT の日付・時刻 (UTC とみなす)。不正なら 0

// 割り当てビットマップの [first, first+n) の立っているビット数
uint64_t count_set_bits(const std::vector<uint8_t>& bits, uint64_t first, uint64_t n);

// 範囲 [off, off+len) を FS のバイト位置として extents の末尾へ足す (直前と連続なら延ばす)
void push_extent(std::vector<FileExtent>* v, uint64_t off, uint64_t len, bool hole);

// FS ごとの解析 (undelete_fat.cpp / undelete_exfat.cpp / undelete_ext.cpp / undelete_ntfs.cpp)
Status scan_fat(Ctx& c);
Status scan_exfat(Ctx& c);
Status scan_ext(Ctx& c);
Status scan_ntfs(Ctx& c);

}  // namespace ud
}  // namespace usbmem
