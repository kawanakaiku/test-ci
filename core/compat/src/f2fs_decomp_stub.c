// SPDX-License-Identifier: Apache-2.0
// F2FS の圧縮クラスタの展開 (公開版): 公開版は GPL 系のカーネルの展開ライブラリ (usbmem_kdecomp) を含まないので、展開できない。
// 圧縮されたファイルの読み出しだけが -EOPNOTSUPP (95) で失敗する (マウント・圧縮されていないファイルの読み出しには影響しない)。個人版は f2fs_decomp.c。docs/editions.md
#include <errno.h>
#include <stddef.h>

/* alg: 0 = lzo, 1 = lz4, 2 = zstd, 3 = lzo-rle。f2fs_decomp.c と同じ形。 */
int usbmem_f2fs_decompress(int alg, const void *src, size_t clen, void *dst, size_t dstlen) {
  (void)alg; (void)src; (void)clen; (void)dst; (void)dstlen;
  return -EOPNOTSUPP;
}
