// SPDX-License-Identifier: Apache-2.0
// 圧縮データの展開 (Btrfs の圧縮 extent / F2FS の圧縮クラスタ / LZ4 バックアップが使う)。docs/decomp.md
//   zlib: システムの libz (zlib License)。LZ4 ブロック・LZO1X・Zstandard: 公開されている形式の説明 (LZ4 block format / LZO ストリームの説明 / RFC 8878) から書いた自前の展開器。
//   どれも「展開だけ」(圧縮はしない)。入力は信用せず、すべての長さ・距離・出力位置を検査する (壊れた入力で範囲外を読み書きしない)。
// 戻り値: 0 = 成功 (*outlen に展開後のバイト数)、-EIO = 入力が壊れている / 未対応の形式、-ENOSPC = 出力が dst に収まらない、-ENOMEM。
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// zlib ストリーム (zlib ヘッダ付き) を展開する。dst が足りないときは入った分だけ書き (*outlen)、-ENOSPC を返す。
int usbmem_zlib_decompress(const void* src, size_t srclen, void* dst, size_t dstcap, size_t* outlen);

// LZO1X の 1 ブロック (終端マーカーまで)。*dstlen は入力で dst の容量、出力で展開後の長さ。LZO-RLE (先頭 2 バイトが 17, 1) も展開できる。
int usbmem_lzo1x_decompress(const void* src, size_t srclen, void* dst, size_t* dstlen);

// Zstandard のフレーム 1 つ (先頭のスキップフレームは読み飛ばす。フレームの後ろは読まない)。辞書は未対応。
int usbmem_zstd_decompress(const void* src, size_t srclen, void* dst, size_t dstcap, size_t* outlen);

// LZ4 ブロック (フレームなし)。dst[0, prefix) は直前までの履歴 (連鎖ブロック用。無ければ 0)。出力は dst[prefix, prefix + *outlen)。cap は prefix を除いた出力の容量。
int usbmem_lz4_block_decompress(const void* src, size_t srclen, void* dst, size_t prefix, size_t cap, size_t* outlen);

#ifdef __cplusplus
}
#endif
