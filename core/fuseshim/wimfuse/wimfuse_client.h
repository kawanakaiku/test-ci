// SPDX-License-Identifier: GPL-2.0-or-later
// wimfuse の本体側 (アプリのプロセス): 別プロセスの libwimfuse.so と RPC (../rpc_volume.h + wimfuse_proto.h) で話し、WIM のイメージを FsVolume として見せる。
// ここには wimlib のコードは入らない (LGPL / GPL のコードは別プロセス側だけ)。WIM ファイルの中身は、子からのコールバックで src (BlockDevice) から読んで返す。
#pragma once
#include <memory>
#include <string>

#include "usbmem/block_device.h"
#include "usbmem/fs_volume.h"

namespace usbmem {
namespace wimfuse {

// fd = 子とつながった UNIX ソケット (所有権を渡す。成功しても失敗しても閉じる)。src = WIM ファイル (読み取り専用で使う)。
// WIM のヘッダとイメージ一覧を JSON で *out に入れる (wimfuse_main.cpp の wim_info_json)。戻り値 0 / 負の errno (*out = エラー文)。終わったら子に QUIT を送る。
int client_info(int fd, BlockDevicePtr src, std::string* out);

// image (1 始まり) を読み取り専用でマウントした FsVolume を返す。FsVolume を放すと fd を閉じ、子は EOF で終わる。
// 失敗時は nullptr、*err = 正の errno、*msg = 子のエラー文 (あれば)。
std::unique_ptr<FsVolume> client_mount(int fd, BlockDevicePtr src, int image, int* err, std::string* msg);

}  // namespace wimfuse
}  // namespace usbmem
