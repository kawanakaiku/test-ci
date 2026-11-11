// SPDX-License-Identifier: Apache-2.0
// BlockDevice: 物理 USB / RAW イメージ / NBD / パーティション / 復号ボリュームを
// 同一インターフェースで扱うための抽象。ファイルシステム実装は必ずこの上に載せる。
//
// 契約:
//  - read_at / write_at は「全量成功」か「エラー」のどちらか。短い read/write は
//    実装側(下位 I/O)で継続して処理し、呼び出し側には見せない。
//  - 範囲外は OutOfRange。サイズ拡張はしない。
//  - 全メソッドはスレッドセーフ。書き込みは実装側で直列化される。
//  - 例外を投げない。
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "usbmem/status.h"

namespace usbmem {

// 協調的キャンセル。I/O 実装はチャンクごとに確認する。
class CancelToken {
 public:
  void cancel() { flag_.store(true, std::memory_order_relaxed); }
  bool cancelled() const { return flag_.load(std::memory_order_relaxed); }

 private:
  std::atomic<bool> flag_{false};
};

struct IoOptions {
  const CancelToken* cancel = nullptr;
  uint32_t timeout_ms = 0;  // 0 = 実装既定。実装が対応しない場合は無視される
};

class BlockDevice {
 public:
  virtual ~BlockDevice() = default;

  virtual Status read_at(uint64_t offset, void* buf, size_t len, const IoOptions& opt = {}) = 0;
  virtual Status write_at(uint64_t offset, const void* buf, size_t len, const IoOptions& opt = {}) = 0;
  virtual Status flush() = 0;
  // 以降 I/O は Closed を返す。複数回呼んでも安全。
  virtual Status close() = 0;

  virtual uint64_t size() const = 0;               // バイト
  virtual uint32_t logical_block_size() const = 0;  // バイト (512/4096 ...)
  virtual bool is_read_only() const = 0;
  virtual std::string describe() const = 0;  // ログ用。秘密情報を含めない
};

using BlockDevicePtr = std::shared_ptr<BlockDevice>;

// offset+len が [0, dev_size] に収まるか(オーバーフロー安全)。
inline bool range_ok(uint64_t dev_size, uint64_t offset, uint64_t len) {
  return offset <= dev_size && len <= dev_size - offset;
}

}  // namespace usbmem
