// SPDX-License-Identifier: GPL-2.0-or-later
// ntfs-3g (無改変の src/ntfs-3g.c。main を ntfs3g_main に改名してビルド) を fuseshim の上で動かす。docs/fuse-layer.md
#pragma once
#include <memory>
#include <string>

#include "fuseshim_volume.h"
#include "usbmem/block_device.h"

namespace usbmem {

// BlockDevice を ntfs-3g に渡して読み取り専用でマウントする。失敗なら nullptr と、*err に理由 (ntfs-3g の終了コード付き)。
// 1 プロセスで 1 回だけ (ntfs-3g の main / libntfs-3g はグローバル状態を持ち、終了後の再実行は想定されていない。docs/fuse-layer.md 3 章)。
// 戻り値の stop() / デストラクタが「アンマウント」(fuse_loop を返す → ntfs_umount)。dev は stop() の後まで生きている必要がある (共有で保持する)。
std::unique_ptr<FuseShimSession> open_ntfs3g(BlockDevicePtr dev, const std::string& extra_opts, std::string* err);

}  // namespace usbmem
