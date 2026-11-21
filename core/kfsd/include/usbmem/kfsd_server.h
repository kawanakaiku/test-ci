// SPDX-License-Identifier: Apache-2.0
// kfsd のサーバ側の共通部分 (プロトコルの受け付けと FsVolume への振り分け)。ドライバ (カーネル由来のコード) は含まない。
// kfsd の実行ファイル (GPL-2.0-or-later) は、これに MountFn (ドライバを選んでマウントする関数) を渡して動かす。
// テストでは、メモリ上の FsVolume を返す MountFn で、ドライバなしにプロトコルの両端を試す (core/kfsd/tests/test_kfsd_proto.cpp)。
#pragma once
#include <functional>
#include <memory>
#include <string>

#include "usbmem/block_device.h"
#include "usbmem/fs_volume.h"
#include "usbmem/kfsd_proto.h"

namespace usbmem {
namespace kfsd {

struct MountParams {
  uint32_t flags = 0;          // MountFlags
  int tz_minutes_west = 0;
  bool rw() const { return flags & kMountRw; }
  bool allow_noload() const { return flags & kMountAllowNoload; }
  bool allow_dirty() const { return flags & kMountAllowDirty; }
};

struct MountResult {
  std::unique_ptr<FsVolume> vol;   // 失敗は null
  int err = 0;                     // 失敗時の正の errno
  std::string fs_type;             // probe の結果の名前 (任意)
};

// dev は本体のブロックデバイス (callback 越し)。read_only / size / block_size は本体のものと同じ。
using MountFn = std::function<MountResult(BlockDevicePtr dev, const MountParams& p)>;

// 戻り値 = プロセスの終了コード: 0 = kClose で正常に終了 / 1 = 本体が消えた (EOF・通信エラー) / 2 = プロトコル違反 / 3 = マウントに失敗 (本体へは errno を返した)。
int serve(int fd, const MountFn& mount);

}  // namespace kfsd
}  // namespace usbmem
