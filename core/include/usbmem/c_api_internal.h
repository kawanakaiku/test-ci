// SPDX-License-Identifier: Apache-2.0
// c_api の不透明ハンドルの中身。同一リポジトリ内の他バインディング (NTFS JNI など) が BlockDevice を共有するためのもの。
// C/Kotlin 側へは公開しない (c_api.h だけが公開面)。
#pragma once
#include <memory>

#include "usbmem/block_device.h"

namespace usbmem { struct NestedLineage; }  // compat/public/usbmem/nested_mount.h (ネストの段数と祖先。最上位のデバイスは nullptr)

struct usbmem_device {
  usbmem::BlockDevicePtr dev;
  std::shared_ptr<const usbmem::NestedLineage> lineage;  // ネストマウントで開いたデバイスだけ。この上に載るボリュームの系譜
};
