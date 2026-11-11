// SPDX-License-Identifier: Apache-2.0
// USB Mass Storage (Bulk-Only Transport + SCSI transparent command set) を BlockDevice として提供する。
// OS 依存の転送層は BulkTransport に分離する (Android: usbdevfs / テスト: ソフトウェア模擬デバイス)。
// 仕様: USB Mass Storage Class Bulk-Only Transport 1.0, SCSI SPC-4 / SBC-3 (INQUIRY, TEST UNIT READY, REQUEST SENSE,
// READ CAPACITY(10/16), MODE SENSE(6), READ/WRITE(10/16), SYNCHRONIZE CACHE(10))。
#pragma once
#include <cstdint>
#include <chrono>
#include <mutex>
#include <string>

#include "usbmem/block_device.h"

namespace usbmem {

enum class UsbDir { In, Out };

// 1 つの Mass Storage インタフェースの bulk エンドポイント対。実装はスレッドセーフでなくてよい (MscDevice が直列化する)。
class BulkTransport {
 public:
  /** 1 回の bulk 転送要求の最大バイト数 (実装が分割するときの単位)。方向別。実行時の調整用で、既定は実装依存。 */
  virtual void set_chunk(UsbDir dir, size_t bytes) { (void)dir; (void)bytes; }
  virtual ~BulkTransport() = default;
  // 転送できたバイト数を *done へ。短い転送は Ok のまま done < len。
  // STALL は Errc::Io + sys_errno=EPIPE、タイムアウトは Errc::Timeout、抜去は Errc::Disconnected。
  virtual Status bulk_out(const void* buf, size_t len, size_t* done, uint32_t timeout_ms) = 0;
  virtual Status bulk_in(void* buf, size_t len, size_t* done, uint32_t timeout_ms) = 0;
  virtual Status clear_halt(UsbDir ep) = 0;
  // クラス要求 Bulk-Only Mass Storage Reset (bRequest 0xFF)。
  virtual Status mass_storage_reset() = 0;
};

struct MscOptions {
  uint8_t lun = 0;
  bool read_only = true;                  // false でも書き込み禁止メディア (WP) なら読み取り専用になる
  uint32_t max_transfer = 64 * 1024;      // 1 コマンドあたりの最大バイト数 (ブロックサイズの倍数に丸める)。書き込みは常にこの値
  uint32_t max_read_transfer = 0;         // 読み取り 1 コマンドあたりの最大バイト数。0 = max_transfer。読み取りだけ大きくして速度を上げられる (書き込みは変えない)
  uint32_t io_timeout_ms = 10000;
  uint32_t ready_wait_ms = 10000;         // TEST UNIT READY を待つ上限
  uint32_t retry_delay_ms = 200;
};

// SCSI SENSE の要約 (診断・エラー対応表示用)
struct ScsiSense { uint8_t key = 0, asc = 0, ascq = 0; };

class MscDevice final : public BlockDevice {
 public:
  // transport の所有権を取る。INQUIRY → TEST UNIT READY → READ CAPACITY まで行い、失敗時は out を設定しない。
  static Status open(std::unique_ptr<BulkTransport> transport, const MscOptions& opt, BlockDevicePtr* out);

  Status read_at(uint64_t, void*, size_t, const IoOptions& = {}) override;
  Status write_at(uint64_t, const void*, size_t, const IoOptions& = {}) override;
  Status flush() override;
  Status close() override;
  uint64_t size() const override { return blocks_ * block_size_; }
  uint32_t logical_block_size() const override { return block_size_; }
  bool is_read_only() const override { return read_only_; }
  std::string describe() const override;

  /** ディスクの回転を止める (start=false) / 再開する (start=true)。SCSI START STOP UNIT (LOEJ=0: メディアは取り出さない)。
   *  メディアの内容は変えないので、読み取り専用でも送れる (専用のこの関数の中だけで許可リストを一時的に緩める)。
   *  書き込み可で止める前には SYNCHRONIZE CACHE を送る。未対応のデバイスは Unsupported。止めた後は、再開 (start=true) するまで READ は失敗しうる。 */
  Status start_stop(bool start);
  /** アイドル自動スピンダウン用。最後の I/O から idle_ms 以上たっていて、まだ停止していなければ止める (I/O 中は待たずに 0 を返す)。
   *  戻り値: 1 = 止めた / 0 = 何もしなかった (まだ使用中・すでに停止中・非対応と判明済み・I/O 実行中) / <0 = -Errc (未対応は Unsupported。以後は試さない)。
   *  止めた後の READ/WRITE/flush は、先に自動で START を送ってから実行する (呼び出し側は何もしなくてよい)。 */
  int idle_stop(uint64_t idle_ms);
  bool spun_down() const;

  ScsiSense last_sense() const { return last_sense_; }
  /** 送った SCSI オペコードの集計 (例: "12x1 1ax1 25x1 28x4237")。読み取り専用で WRITE 系が 0 であることの実機での証拠。 */
  std::string op_stats() const;
  /** **読み取り**の転送サイズの調整 (速度計測・チューニング用。書き込みの設定は変えない)。0 = 変更しない。
   *  read_chunk = 1 回の USB 転送要求 (IN, 16KiB の倍数)、read_max_transfer = 1 SCSI READ コマンドあたり (ブロック長の倍数に丸める)。 */
  void tune(size_t read_chunk, uint32_t read_max_transfer);
  /** 診断用: 直近の open() 中に送った SCSI コマンドと結果の記録 (失敗時の原因調査用)。 */
  static std::string last_open_trace();
  uint32_t op_count(uint8_t opcode) const { return op_counts_[opcode]; }
  bool write_protected() const { return write_protected_; }
  const std::string& vendor() const { return vendor_; }
  const std::string& product() const { return product_; }

 private:
  MscDevice(std::unique_ptr<BulkTransport> t, const MscOptions& o) : t_(std::move(t)), opt_(o) {}
  struct Cmd;
  Status exec(const uint8_t* cdb, uint8_t cdb_len, UsbDir dir, void* data, size_t len, size_t* transferred, uint32_t timeout_ms, bool* check_condition);
  Status exec_checked(const uint8_t* cdb, uint8_t cdb_len, UsbDir dir, void* data, size_t len, uint32_t timeout_ms, size_t min_len = SIZE_MAX);
  Status request_sense(ScsiSense* s);
  Status reset_recovery();
  Status rw_blocks(bool write, uint64_t lba, uint32_t nblocks, void* buf, const IoOptions& io);
  Status read_locked(uint64_t off, void* buf, size_t len, const IoOptions& io);
  Status write_locked(uint64_t off, const void* buf, size_t len, const IoOptions& io);
  Status initialize();
  Status map_sense(const ScsiSense& s);

  std::unique_ptr<BulkTransport> t_;
  MscOptions opt_;
  mutable std::mutex mu_;
  uint32_t tag_ = 1;
  uint64_t blocks_ = 0;
  uint32_t block_size_ = 0;
  bool read_only_ = true, write_protected_ = false, dead_ = false, closed_ = false;
  bool spun_down_ = false, stop_unsupported_ = false;
  std::chrono::steady_clock::time_point last_io_ = std::chrono::steady_clock::now();
  Status start_stop_locked(bool start);
  Status resume_if_stopped_locked();   // I/O の前に呼ぶ: 停止中なら START を送る。最終 I/O 時刻も更新
  bool allow_start_stop_ = false;   // start_stop() の実行中だけ true (読み取り専用の許可リストに START STOP UNIT を通す)
  ScsiSense last_sense_;
  uint32_t op_counts_[256] = {0};
  std::string trace_;
  char phase_ = '-';
  Status exec_impl(const uint8_t* cdb, uint8_t cdb_len, UsbDir dir, void* data, size_t len, size_t* transferred, uint32_t timeout_ms, bool* check_condition);
  std::string vendor_, product_;
};

}  // namespace usbmem
