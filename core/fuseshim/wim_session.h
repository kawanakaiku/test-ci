// SPDX-License-Identifier: GPL-2.0-or-later
// wimlib の wimmount (src/mount_image.c の wimlib_mount_image。無改変の libwim) を fuseshim (libfuse 3.x 系) の上で動かす。docs/fuse-layer.md 6 章
#pragma once
#include <memory>
#include <string>

#include "fuseshim_volume.h"

namespace usbmem {

// WIM ファイルの image 番目 (1 始まり) を読み取り専用でマウントする。WIM は実在のパスで開く (wimlib は open(2) / pread(2) で読む。docs 6 章の「足りないもの」)。
// 1 プロセスで 1 回の想定。stop() / デストラクタがアンマウント。
std::unique_ptr<FuseShimSession> open_wim(const std::string& wim_path, int image, std::string* err);

}  // namespace usbmem
