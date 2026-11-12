// SPDX-License-Identifier: Apache-2.0
// ディスクイメージ形式の実装が共有する部品 (内部用)。
#pragma once
#include <cstring>
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "usbmem/disk_image.h"

namespace usbmem {
namespace imgint {

inline uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
inline uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
inline uint64_t be64(const uint8_t* p) { return (uint64_t(be32(p)) << 32) | be32(p + 4); }
inline uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
inline uint64_t le64(const uint8_t* p) { return uint64_t(le32(p)) | (uint64_t(le32(p + 4)) << 32); }

// 検証つきの安全な加算・乗算 (オーバーフローなら false)
inline bool add_ok(uint64_t a, uint64_t b, uint64_t* r) { return !__builtin_add_overflow(a, b, r); }
inline bool mul_ok(uint64_t a, uint64_t b, uint64_t* r) { return !__builtin_mul_overflow(a, b, r); }
inline bool is_pow2(uint64_t x) { return x != 0 && (x & (x - 1)) == 0; }

inline std::string hex(const uint8_t* p, size_t n) {
  static const char* d = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; i++) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
  return s;
}

// CRC-32C (Castagnoli)。VHDX のチェックサム用。
uint32_t crc32c(const void* data, size_t len, uint32_t seed = 0);

// zlib/deflate の展開。zlib_header=true なら zlib 形式、false なら生の deflate (QCOW2)。
// out は outlen バイトぶん確保済み。出力が outlen に満たなければ残りを 0 で埋める。入力不正は Corrupt。
Status inflate_to(const uint8_t* in, size_t inlen, uint8_t* out, size_t outlen, bool zlib_header);

// UTF-16 (le/be) → UTF-8。不正なサロゲートは U+FFFD。
std::string utf16_to_utf8(const uint8_t* p, size_t bytes, bool big_endian);
// '\' を '/' にする (Windows のパス表記の正規化)
std::string norm_slashes(std::string s);

// 先頭から len バイトを読み、dev の範囲外は 0 で埋める (バッキング/親のサイズが違うとき)。
Status read_zero_beyond(BlockDevice& dev, uint64_t off, void* buf, size_t len, const IoOptions& io);

// バイト数上限つき LRU キャッシュ (キー = ファイル内オフセットなど)。返したポインタは次の put まで有効。
class TableCache {
 public:
  explicit TableCache(size_t cap_bytes) : cap_(cap_bytes) {}
  const std::vector<uint8_t>* get(uint64_t key) {
    auto it = map_.find(key);
    if (it == map_.end()) return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second);
    return &it->second->second;
  }
  const std::vector<uint8_t>* put(uint64_t key, std::vector<uint8_t>&& v) {
    auto it = map_.find(key);
    if (it != map_.end()) { used_ -= it->second->second.size(); lru_.erase(it->second); map_.erase(it); }
    used_ += v.size();
    lru_.emplace_front(key, std::move(v));
    map_[key] = lru_.begin();
    while (used_ > cap_ && lru_.size() > 1) {
      auto& b = lru_.back();
      used_ -= b.second.size();
      map_.erase(b.first);
      lru_.pop_back();
    }
    return &lru_.front().second;
  }
  void clear() { lru_.clear(); map_.clear(); used_ = 0; }

 private:
  using List = std::list<std::pair<uint64_t, std::vector<uint8_t>>>;
  size_t cap_, used_ = 0;
  List lru_;
  std::unordered_map<uint64_t, List::iterator> map_;
};

// 読み取り専用イメージの共通基底。仮想ディスク上の範囲を「ユニット」(クラスタ/ブロック/グレイン) に分割して read_unit を呼ぶ。
// 全メソッドは 1 本のミューテックスで直列化する (キャッシュを共有するため)。
class ImageDeviceBase : public BlockDevice {
 public:
  ImageDeviceBase(BlockDevicePtr file, uint64_t vsize, uint32_t lbs, std::string desc)
      : file_(std::move(file)), vsize_(vsize), lbs_(lbs), desc_(std::move(desc)) {}
  Status read_at(uint64_t off, void* buf, size_t len, const IoOptions& opt = {}) final;
  Status write_at(uint64_t, const void*, size_t, const IoOptions& = {}) final { return Status::Make(Errc::ReadOnly); }
  Status flush() final { return Status::Ok(); }
  Status close() override;
  uint64_t size() const final { return vsize_; }
  uint32_t logical_block_size() const final { return lbs_; }
  bool is_read_only() const final { return true; }
  std::string describe() const final { return desc_; }

 protected:
  // off から続く、同じユニットに収まる最大バイト数 (> 0)
  virtual uint64_t unit_len(uint64_t off) const = 0;
  // [off, off+n) (n <= unit_len(off)) を out へ。mu_ を保持して呼ばれる
  virtual Status read_unit(uint64_t off, uint8_t* out, size_t n, const IoOptions& io) = 0;
  virtual void on_close() {}
  BlockDevicePtr file_;
  uint64_t vsize_;
  uint32_t lbs_;
  std::string desc_;
  bool closed_ = false;
  std::mutex mu_;
};

#define IMG_FAIL(info, code, msg) \
  do { if (info) (info)->detail = (msg); return Status::Make(code); } while (0)

// 形式ごとの実装 (depth = 現在のバッキング深さ)
Status open_qcow2_depth(BlockDevicePtr file, const ImageOpenOptions& o, uint32_t depth, BlockDevicePtr* out, ImageInfo* info);
Status open_vhd_depth(BlockDevicePtr file, const ImageOpenOptions& o, uint32_t depth, BlockDevicePtr* out, ImageInfo* info);
Status open_vhdx_depth(BlockDevicePtr file, const ImageOpenOptions& o, uint32_t depth, BlockDevicePtr* out, ImageInfo* info);
Status open_vmdk_depth(BlockDevicePtr file, const ImageOpenOptions& o, uint32_t depth, BlockDevicePtr* out, ImageInfo* info);
Status open_disk_image_depth(BlockDevicePtr file, const ImageOpenOptions& o, uint32_t depth, BlockDevicePtr* out, ImageInfo* info);

// バッキング/親を解決して開く (深さ制限つき)。format_hint が空なら opts.probe_backing_format に従って判別。
Status open_parent(const ImageOpenOptions& o, uint32_t depth, const ImageResolveRequest& req, BlockDevicePtr* out, ImageInfo* pinfo, ImageInfo* err_info);

}  // namespace imgint
}  // namespace usbmem
