// SPDX-License-Identifier: GPL-2.0-or-later
// kfsd の「FS の種類の判別 + マウント」(カーネル由来のドライバを呼ぶ)。android/app/src/main/cpp/jni_ntfs.cpp の mount_auto と同じ振り分けの規則。
// ホストのテストは、プロセス内のマウント (比較の基準) にも同じ関数を使う。
#pragma once
#include <memory>
#include <string>

#include "usbmem/block_device.h"
#include "usbmem/fs_volume.h"
#include "usbmem/kfsd_server.h"

namespace usbmem {
namespace kfsd {

// dev 全体のファイルシステムを判別してマウントする。失敗は nullptr と正の errno。*fs_type に判別結果の名前を入れる (任意)。
// 判別できたがこのビルドに入っていない FS は EPROTONOSUPPORT (93)、判別できなかったものは (NTFS が入っていれば NTFS として試す。無ければ) EINVAL。
std::unique_ptr<FsVolume> mount_auto(const BlockDevicePtr& dev, bool rw, int* err, bool allow_noload, bool allow_dirty, std::string* fs_type = nullptr);

// serve() に渡す MountFn (MountParams から mount_auto を呼ぶ。FAT のタイムゾーンもここで設定する)
MountFn mount_fn();

}  // namespace kfsd
}  // namespace usbmem
