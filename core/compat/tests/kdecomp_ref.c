// SPDX-License-Identifier: GPL-2.0-or-later
// 展開器の差分試験 (個人版だけ): カーネルの展開ライブラリ (usbmem_kdecomp。GPL-2.0 系) を「基準」として呼ぶ薄い C の層。test_decomp_kernel.cpp が使う。
// 自前の展開器 (core/src/decomp*.cpp。公開版にも入る) は、このファイルにもカーネルのコードにも依存しない。docs/decomp.md
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <linux/lz4.h>
#include <linux/lzo.h>
#include <linux/zstd.h>

/* 戻り値: 0 = 成功 (*dstlen に展開後の長さ)、負 = 失敗 */
int kref_lzo(const void *src, size_t clen, void *dst, size_t *dstlen) {
  size_t out = *dstlen;
  int rc = lzo1x_decompress_safe((const unsigned char *)src, clen, (unsigned char *)dst, &out);
  if (rc != LZO_E_OK) return -EIO;
  *dstlen = out;
  return 0;
}

int kref_lz4(const void *src, size_t clen, void *dst, size_t cap, size_t *dstlen) {
  if (clen > 0x7fffffff || cap > 0x7fffffff) return -EINVAL;
  int rc = LZ4_decompress_safe((const char *)src, (char *)dst, (int)clen, (int)cap);
  if (rc < 0) return -EIO;
  *dstlen = (size_t)rc;
  return 0;
}

int kref_zstd(const void *src, size_t clen, void *dst, size_t cap, size_t *dstlen) {
  size_t ws = zstd_dctx_workspace_bound();
  void *wk = malloc(ws);
  if (!wk) return -ENOMEM;
  zstd_dctx *dctx = zstd_init_dctx(wk, ws);
  int rc = -EIO;
  if (dctx) {
    size_t r = zstd_decompress_dctx(dctx, dst, cap, src, clen);
    if (!zstd_is_error(r)) { *dstlen = r; rc = 0; }
  }
  free(wk);
  return rc;
}
