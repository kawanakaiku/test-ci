// SPDX-License-Identifier: Apache-2.0
// kfsd (別プロセスの FS ドライバ。docs/kfsd.md) へつなぐ本体側の入口。jni_ntfs.cpp の mount_auto が使う。公開版・個人版とも同じ (Apache-2.0 のコードだけ)。
#pragma once
#include <memory>

#include "usbmem/block_device.h"
#include "usbmem/fs_volume.h"

// RPC (kfsd 経由) でマウントする設定か。Kotlin の Kfsd.configure が nativeSetMode で切り替える (個人版の既定は false = プロセス内、公開版は true)。
bool usbmem_kfsd_rpc_enabled();

// kfsd を起こして (Kotlin の KfsdNative.spawnChildFd → サービス → libkfsd.so)、dev をマウントする。失敗は nullptr と正の errno。
// kfsd が無い (プラグイン未導入・libkfsd.so が無い) ときは ENOPKG (65 = 「この FS には kfsd (プラグイン) が必要です」)。
std::unique_ptr<usbmem::FsVolume> usbmem_kfsd_mount(const usbmem::BlockDevicePtr& dev, bool rw, int* err, bool allow_noload, bool allow_dirty);
