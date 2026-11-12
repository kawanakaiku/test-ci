// SPDX-License-Identifier: Apache-2.0
// バックアップの内部部品 (Partclone イメージの逐次読み取り、ハッシュを取る入出力の層)。
#pragma once
#include <cstring>

#include "usbmem/backup.h"
#include "usbmem/crc32.h"
#include "usbmem/sha256.h"

namespace usbmem {
namespace backup_detail {

using Bytes = std::vector<uint8_t>;

constexpr size_t kHeaderBytes = 0x6e;          // image_head + fs_info + options + CRC
constexpr uint8_t kCsmNone = 0, kCsmCrc32 = 0x20;
constexpr uint8_t kBitmapBit = 1;
constexpr uint64_t kMaxChunkBytes = 64ull << 20;

// Partclone の CRC は「初期値 0xFFFFFFFF・最後の反転なし」。標準の crc32() を連ねて、最後に反転して得る。
struct PcCrc {
  uint32_t acc = 0;  // 標準 CRC-32 の値 (seed として渡す形)
  void reset() { acc = 0; }
  void update(const void* p, size_t n) { acc = crc32(p, n, acc); }
  uint32_t value() const { return ~acc; }
  void set_from_value(uint32_t v) { acc = ~v; }
};
inline uint32_t pc_crc(const void* p, size_t n) { PcCrc c; c.update(p, n); return c.value(); }

inline void put_le(Bytes& b, size_t o, uint64_t v, int n) { for (int i = 0; i < n; i++) b[o + i] = static_cast<uint8_t>(v >> (8 * i)); }
inline uint64_t get_le(const uint8_t* p, int n) { uint64_t v = 0; for (int i = 0; i < n; i++) v |= static_cast<uint64_t>(p[i]) << (8 * i); return v; }

// 読んだバイトの SHA-256 と大きさを数える層
class HashingSource final : public ByteSource {
 public:
  explicit HashingSource(ByteSource& s) : s_(s) {}
  Status read(void* buf, size_t len, size_t* got) override {
    Status st = s_.read(buf, len, got);
    if (st.ok()) { h_.update(buf, *got); bytes_ += *got; }
    return st;
  }
  std::string digest() { return hex_encode(h_.finish()); }
  uint64_t bytes() const { return bytes_; }
 private:
  ByteSource& s_;
  Sha256 h_;
  uint64_t bytes_ = 0;
};

// 書いたバイトの SHA-256 と大きさを数える層
class HashingSink final : public ByteSink {
 public:
  explicit HashingSink(ByteSink& s) : s_(s) {}
  Status write(const void* d, size_t n) override {
    Status st = s_.write(d, n);
    if (st.ok()) { h_.update(d, n); bytes_ += n; }
    return st;
  }
  Status finish() override { return s_.finish(); }
  std::string digest() { return hex_encode(h_.finish()); }
  uint64_t bytes() const { return bytes_; }
 private:
  ByteSink& s_;
  Sha256 h_;
  uint64_t bytes_ = 0;
};

inline Status read_exact(ByteSource& s, void* buf, size_t len, bool* eof_at_start = nullptr) {
  uint8_t* p = static_cast<uint8_t*>(buf);
  size_t done = 0;
  while (done < len) {
    size_t got = 0;
    Status st = s.read(p + done, len - done, &got);
    if (!st.ok()) return st;
    if (got == 0) {
      if (eof_at_start && done == 0) *eof_at_start = true;
      return Status::Make(Errc::Corrupt);  // 途中で切れている
    }
    done += got;
  }
  return Status::Ok();
}

// ヘッダ + ビットマップを組み立てる (書き出し側)。
Bytes make_image_head(const UsedMap& m, uint32_t blocks_per_checksum);
uint32_t default_blocks_per_checksum(uint32_t block_size);

// 画像の使用中ブロックを、チェックサムの区間 (最大 blocks_per_checksum 個) ごとに読む。CRC は読んだ区間ごとに検証する。
class BlockStream {
 public:
  BlockStream(ByteSource& src, const ImageInfo& info, const std::vector<uint8_t>& bits) : src_(src), info_(info), bits_(bits) {
    per_chunk_ = info.checksum_mode == kCsmCrc32 && info.blocks_per_checksum ? info.blocks_per_checksum : default_blocks_per_checksum(info.block_size);
  }
  // blocks: 区間内の各使用中ブロックの番号。data: その内容 (連結)。crc: 区間の CRC (チェックサムなしなら 0)。
  // 区間がもうないとき *done = true。破損は Corrupt (code に "checksum-mismatch" / "truncated")。
  FsStatus next(std::vector<uint64_t>* blocks, Bytes* data, uint32_t* crc, bool* done);
  uint64_t chunk_index() const { return chunk_; }
  // 次の区間に入る使用中ブロックの番号を bits から選ぶ (読み取りはしない)。*cursor は走査位置。
  static void select(const std::vector<uint8_t>& bits, uint64_t total, uint64_t per_chunk, uint64_t* cursor, std::vector<uint64_t>* blocks);
  uint64_t per_chunk() const { return per_chunk_; }
  // すべて読み終わったあとに、画像の末尾に余分なデータがないことを確かめる。
  FsStatus expect_end();
 private:
  ByteSource& src_;
  const ImageInfo& info_;
  const std::vector<uint8_t>& bits_;
  uint64_t per_chunk_;
  uint64_t cursor_ = 0, produced_ = 0, chunk_ = 0;
  PcCrc running_;
  bool running_init_ = false;
};

}  // namespace backup_detail
}  // namespace usbmem
