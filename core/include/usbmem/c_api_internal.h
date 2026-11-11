// SPDX-License-Identifier: Apache-2.0
// c_api の不透明ハンドルの中身。同一リポジトリ内の他バインディング (NTFS JNI など) が BlockDevice を共有するためのもの。
// C/Kotlin 側へは公開しない (c_api.h だけが公開面)。
#pragma once
#include "usbmem/block_device.h"

struct usbmem_device {
  usbmem::BlockDevicePtr dev;
};
