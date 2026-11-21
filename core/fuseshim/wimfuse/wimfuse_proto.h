// SPDX-License-Identifier: GPL-2.0-or-later
// wimfuse (別プロセスの wimlib) と本体の RPC の拡張の操作コード。基本の操作 (LOOKUP / STAT / READDIR / READ / READLINK) は ../rpc_volume.h。docs/fuse-layer.md 10 章
#pragma once
#include <cstdint>

namespace usbmem {
namespace wimfuse {

// 本体 → 子の要求 (rpc_serve の extra)
constexpr uint32_t kOpInfo = 0x101;   // WIM のヘッダとイメージ一覧。応答の本体 = JSON (UTF-8)。rc = 0 / 負の errno (本体 = エラー文)
constexpr uint32_t kOpMount = 0x102;  // a = イメージ番号 (1 始まり)。読み取り専用でマウントし、以降は FsVolume の操作に答える。rc = ルートの ino (> 0) / 負の errno (本体 = エラー文)
constexpr uint32_t kOpPing = 0x103;   // 何もしない (起動の確認)。rc = 子の pid

// 子 → 本体のコールバック (RpcVolume::CallbackFn が処理する。WIM ファイル = 本体の BlockDevice の中身を読ませる)
constexpr uint32_t kCbSrcSize = 0x110;  // rc = WIM ファイルのバイト数
constexpr uint32_t kCbSrcRead = 0x111;  // b = オフセット、c = 長さ。rc = 読めたバイト数 (EOF で短くなる)、本体 = そのバイト

}  // namespace wimfuse
}  // namespace usbmem
