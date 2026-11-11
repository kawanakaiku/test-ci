// SPDX-License-Identifier: Apache-2.0
// usbdevfs (Linux/Android の /dev/bus/usb 相当) の ioctl で BulkTransport を実装する。
// Android では UsbDeviceConnection.getFileDescriptor() の fd をそのまま渡せる (claimInterface は Kotlin 側で行う)。
// 実機の USB メモリでの動作は未検証 (Phase 3 の実機テストで確認する)。
#pragma once
#include <memory>

#include "usbmem/usb_msc.h"

namespace usbmem {

struct UsbEndpoints {
  uint8_t ep_in = 0;      // bEndpointAddress (bit7 = 1)
  uint8_t ep_out = 0;     // bEndpointAddress (bit7 = 0)
  uint8_t interface = 0;  // bInterfaceNumber (クラス要求 wIndex)
};

// fd の所有権は取らない (UsbDeviceConnection が所有)。fd はこのオブジェクトより長く生きている必要がある。
std::unique_ptr<BulkTransport> make_usbdevfs_transport(int fd, const UsbEndpoints& eps);

}  // namespace usbmem
