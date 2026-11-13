// SPDX-License-Identifier: Apache-2.0
#include "usbmem/usbdevfs_transport.h"

#include <errno.h>
#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>

#include <algorithm>

namespace usbmem {
namespace {
// usbdevfs の USBDEVFS_BULK は 1 回の長さに実装上の上限がある (古いカーネルは 16KiB)。
// 16KiB は 64/512/1024 の全 max packet size の倍数なので、チャンク分割しても IN の babble 判定を壊さない。
constexpr size_t kChunk = 16 * 1024;   // 既定。set_chunk() で実行時に変えられる (速度計測の結果で調整する)

Status map_errno(int e) {
  switch (e) {
    case EPIPE: return Status::Make(Errc::Io, EPIPE);            // STALL (呼び出し側が CLEAR_HALT する)
    case ETIMEDOUT: return Status::Make(Errc::Timeout);
    case ENODEV: case ESHUTDOWN: case ENOENT: case EPROTO:       // 抜去・バス切断。EPROTO はケーブル断で頻出
      return e == EPROTO ? Status::Make(Errc::Io, EPROTO) : Status::Make(Errc::Disconnected);
    default: return Status::Make(Errc::Io, e);
  }
}

class UsbdevfsTransport final : public BulkTransport {
 public:
  UsbdevfsTransport(int fd, const UsbEndpoints& e) : fd_(fd), eps_(e) {}

  Status bulk_out(const void* buf, size_t len, size_t* done, uint32_t timeout_ms) override {
    return xfer(eps_.ep_out, const_cast<void*>(buf), len, done, timeout_ms, chunk_out_);
  }
  Status bulk_in(void* buf, size_t len, size_t* done, uint32_t timeout_ms) override {
    return xfer(eps_.ep_in, buf, len, done, timeout_ms, chunk_in_);
  }
  void set_chunk(UsbDir dir, size_t bytes) override {   // 16KiB の倍数 (max packet size の倍数)。方向別 (読み取りだけ大きくできる)
    size_t c = std::max<size_t>(kChunk, bytes - bytes % kChunk);
    (dir == UsbDir::In ? chunk_in_ : chunk_out_) = c;
  }
  Status clear_halt(UsbDir ep) override {
    unsigned int addr = ep == UsbDir::In ? eps_.ep_in : eps_.ep_out;
    if (::ioctl(fd_, USBDEVFS_CLEAR_HALT, &addr) != 0) return map_errno(errno);
    return Status::Ok();
  }
  Status mass_storage_reset() override {
    struct usbdevfs_ctrltransfer c = {};
    c.bRequestType = 0x21;  // host→device, class, interface
    c.bRequest = 0xFF;      // Bulk-Only Mass Storage Reset
    c.wValue = 0;
    c.wIndex = eps_.interface;
    c.wLength = 0;
    c.timeout = 5000;
    c.data = nullptr;
    if (::ioctl(fd_, USBDEVFS_CONTROL, &c) < 0) return map_errno(errno);
    return Status::Ok();
  }

  Status get_max_lun(uint8_t* max_lun) override {
    struct usbdevfs_ctrltransfer c = {};
    uint8_t v = 0;
    c.bRequestType = 0xA1;  // device→host, class, interface
    c.bRequest = 0xFE;      // GET MAX LUN
    c.wValue = 0;
    c.wIndex = eps_.interface;
    c.wLength = 1;
    c.timeout = 3000;       // 応答しない機器で長く待たない
    c.data = &v;
    int r = ::ioctl(fd_, USBDEVFS_CONTROL, &c);
    if (r < 0) return map_errno(errno);   // STALL は EPIPE = Io/EPIPE (制御パイプの STALL は自動で解除される。LUN 1 つの機器の正常な応答)
    if (r != 1) return Status::Make(Errc::Io, EIO);
    *max_lun = v;
    return Status::Ok();
  }

 private:
  Status xfer(uint8_t ep, void* buf, size_t len, size_t* done, uint32_t timeout_ms, size_t& chunk) {
    *done = 0;
    auto* p = static_cast<uint8_t*>(buf);
    while (*done < len) {
      size_t n = std::min(len - *done, chunk);
      struct usbdevfs_bulktransfer b = {};
      b.ep = ep;
      b.len = static_cast<unsigned int>(n);
      b.timeout = timeout_ms ? timeout_ms : 10000;  // 0 = 無期限になるので避ける
      b.data = p + *done;
      int r = ::ioctl(fd_, USBDEVFS_BULK, &b);
      if (r < 0) {
        if (errno == EINTR) continue;
        // カーネルが 1 回の要求サイズを受け付けない (古いカーネルの usbfs は 16 KiB 超の BULK を EINVAL で拒否する。J9260 / カーネル 4.14 で確認)。
        // EINVAL は転送の開始前に返るので、データ・BOT の状態は壊れない。以後この方向は既定の 16 KiB に戻して、同じ箇所をやり直す。
        if (errno == EINVAL && n > kChunk) { chunk = kChunk; continue; }
        // 途中まで転送済みでも STALL/タイムアウトはエラーとして返す (BOT 側が回復手順を取る)
        return map_errno(errno);
      }
      *done += static_cast<size_t>(r);
      if (static_cast<size_t>(r) < n) break;  // 短いパケット = 転送終了
    }
    return Status::Ok();
  }
  int fd_;
  UsbEndpoints eps_;
  size_t chunk_in_ = kChunk, chunk_out_ = kChunk;
};
}  // namespace

std::unique_ptr<BulkTransport> make_usbdevfs_transport(int fd, const UsbEndpoints& eps) {
  return std::unique_ptr<BulkTransport>(new (std::nothrow) UsbdevfsTransport(fd, eps));
}

}  // namespace usbmem
