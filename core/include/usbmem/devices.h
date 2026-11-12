// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <mutex>
#include <shared_mutex>
#include <vector>

#include "usbmem/block_device.h"

namespace usbmem {

// POSIX fd 上の RAW イメージ / ブロックデバイス。pread/pwrite 使用。
class RawImageDevice final : public BlockDevice {
 public:
  // path を開く。read_only=false は明示的に要求された場合のみ。
  static Status open_path(const std::string& path, bool read_only, uint32_t block_size,
                          BlockDevicePtr* out);
  // Android: ContentResolver から得た fd(detachFd)。take_ownership なら close で閉じる。
  static Status open_fd(int fd, bool read_only, bool take_ownership, uint32_t block_size,
                        BlockDevicePtr* out);
  ~RawImageDevice() override;

  Status read_at(uint64_t, void*, size_t, const IoOptions& = {}) override;
  Status write_at(uint64_t, const void*, size_t, const IoOptions& = {}) override;
  Status flush() override;
  Status close() override;
  uint64_t size() const override { return size_; }
  uint32_t logical_block_size() const override { return block_size_; }
  bool is_read_only() const override { return read_only_; }
  std::string describe() const override;

 private:
  RawImageDevice(int fd, bool ro, bool own, uint64_t size, uint32_t bs)
      : fd_(fd), read_only_(ro), own_(own), size_(size), block_size_(bs) {}
  int fd_;
  const bool read_only_;
  const bool own_;
  const uint64_t size_;
  const uint32_t block_size_;
  mutable std::shared_mutex mu_;  // read=共有 / write,flush,close=排他(書き込み直列化)
};

// メモリ上のデバイス(テスト・小さな一時データ用)。
class MemoryDevice final : public BlockDevice {
 public:
  explicit MemoryDevice(std::vector<uint8_t> data, bool read_only = false, uint32_t block_size = 512)
      : data_(std::move(data)), read_only_(read_only), block_size_(block_size) {}
  Status read_at(uint64_t, void*, size_t, const IoOptions& = {}) override;
  Status write_at(uint64_t, const void*, size_t, const IoOptions& = {}) override;
  Status flush() override { return Status::Ok(); }
  Status close() override;
  uint64_t size() const override { return data_.size(); }
  uint32_t logical_block_size() const override { return block_size_; }
  bool is_read_only() const override { return read_only_; }
  std::string describe() const override { return "memory"; }
  std::vector<uint8_t>& bytes() { return data_; }  // テスト用の直接アクセス

 private:
  std::vector<uint8_t> data_;
  bool read_only_;
  uint32_t block_size_;
  bool closed_ = false;
  mutable std::mutex mu_;
};

// 親デバイスの一区間をデバイスとして見せる(パーティション・オフセット変換の基礎)。
class SubRangeDevice final : public BlockDevice {
 public:
  // parent 範囲外なら nullptr を返し status に OutOfRange。
  static Status create(BlockDevicePtr parent, uint64_t offset, uint64_t length, bool force_read_only,
                       BlockDevicePtr* out);
  Status read_at(uint64_t, void*, size_t, const IoOptions& = {}) override;
  Status write_at(uint64_t, const void*, size_t, const IoOptions& = {}) override;
  Status flush() override { return parent_->flush(); }
  Status close() override { return Status::Ok(); }  // 親は所有者が閉じる
  uint64_t size() const override { return length_; }
  uint32_t logical_block_size() const override { return parent_->logical_block_size(); }
  bool is_read_only() const override { return read_only_ || parent_->is_read_only(); }  // write_at と同じ判定 (ネストの子など、読み取り専用の親の上では書き込み可にならない)
  std::string describe() const override;

 private:
  SubRangeDevice(BlockDevicePtr p, uint64_t off, uint64_t len, bool ro)
      : parent_(std::move(p)), offset_(off), length_(len), read_only_(ro) {}
  BlockDevicePtr parent_;
  uint64_t offset_, length_;
  bool read_only_;
};

// 連続した読み取りを検出して、1 つの大きな読み取り (window バイト) にまとめるラッパー。
// USB のようにコマンド 1 回あたりの待ち時間が大きいデバイスで、4 KiB ずつの連続読みが律速になるのを避ける
// (7.3 TiB の NTFS で、空きクラスタ数のためのビットマップ全読み (約 12 万回の 4 KiB 読み) が 2 分 40 秒かかった)。
//  - 直前の読み取りの終端から続く読み取りが 2 回続いたら、窓を先読みする。ランダム読みは先読みせず、そのまま通す。
//  - 書き込みは窓を捨ててから下位へ通す (古い内容を返さない)。窓は 1 つだけで、メモリは window バイト固定。
class ReadAheadDevice final : public BlockDevice {
 public:
  static BlockDevicePtr wrap(BlockDevicePtr inner, size_t window = 256 * 1024);
  Status read_at(uint64_t, void*, size_t, const IoOptions& = {}) override;
  Status write_at(uint64_t, const void*, size_t, const IoOptions& = {}) override;
  Status flush() override { return inner_->flush(); }
  Status close() override { return Status::Ok(); }  // 下位は所有者が閉じる
  uint64_t size() const override { return inner_->size(); }
  uint32_t logical_block_size() const override { return inner_->logical_block_size(); }
  bool is_read_only() const override { return inner_->is_read_only(); }
  std::string describe() const override { return "readahead(" + inner_->describe() + ")"; }
  /** 下位への読み取り回数 (テスト・診断用) */
  uint64_t inner_reads() const;
  /** 下位への読み取りに使った時間 (ナノ秒の合計。診断用)。wrap() の戻り値が ReadAheadDevice でなければ false */
  static bool stats(const BlockDevicePtr& dev, uint64_t* reads, uint64_t* bytes, uint64_t* ns);

 private:
  ReadAheadDevice(BlockDevicePtr in, size_t window);
  BlockDevicePtr inner_;
  size_t window_;
  mutable std::mutex mu_;
  std::vector<uint8_t> win_;
  uint64_t win_off_ = 0, win_len_ = 0, last_end_ = UINT64_MAX;
  int streak_ = 0;
  uint64_t inner_reads_ = 0, inner_bytes_ = 0, inner_ns_ = 0;
  Status timed_read_(uint64_t off, void* buf, size_t len, const IoOptions& io);
};

}  // namespace usbmem
