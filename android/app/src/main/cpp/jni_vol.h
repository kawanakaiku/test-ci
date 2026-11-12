// SPDX-License-Identifier: GPL-2.0-or-later
// マウント済みボリュームの JNI ハンドルの中身。jni_ntfs.cpp (マウント・一覧・読み書き) と jni_nested.cpp (ネストマウント) が共有する。
#pragma once
#include <memory>

#include "usbmem/fs_volume.h"
#include "usbmem/nested_mount.h"

// v は LockedFsVolume で包んでいる (ボリューム単位のネイティブ mutex。JNI 呼び出しと、子ボリュームからの読み出しが直列化される)。
// lineage = ネストの段数と祖先 (最上位のボリュームは nullptr = 段数 0)。
struct Vol {
  std::shared_ptr<usbmem::FsVolume> v;
  std::shared_ptr<const usbmem::NestedLineage> lineage;
  explicit Vol(std::unique_ptr<usbmem::FsVolume> p, std::shared_ptr<const usbmem::NestedLineage> l = nullptr)
      : v(usbmem::make_locked(std::move(p))), lineage(std::move(l)) {}
};
