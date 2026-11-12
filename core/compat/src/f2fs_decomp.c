// SPDX-License-Identifier: GPL-2.0-or-later
// F2FS の圧縮クラスタの展開 (lzo / lzo-rle / lz4 / zstd)。カーネルの展開ライブラリ (usbmem_kdecomp) を呼ぶだけの薄いラッパ。
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <linux/lz4.h>
#include <linux/lzo.h>
#include <linux/zstd.h>

/* alg: 0 = lzo, 1 = lz4, 2 = zstd, 3 = lzo-rle (展開は lzo と同じ)。成功したら展開後のバイト数、失敗は負の errno。 */
int usbmem_f2fs_decompress(int alg, const void *src, size_t clen, void *dst, size_t dstlen) {
  if (clen == 0 || dstlen == 0) return -EINVAL;
  switch (alg) {
    case 0:
    case 3: {
      size_t out = dstlen;
      int rc = lzo1x_decompress_safe((const unsigned char *)src, clen, (unsigned char *)dst, &out);
      if (rc != LZO_E_OK) return -EIO;
      return (int)out;
    }
    case 1: {
      if (clen > 0x7fffffff || dstlen > 0x7fffffff) return -EINVAL;
      int rc = LZ4_decompress_safe((const char *)src, (char *)dst, (int)clen, (int)dstlen);
      if (rc < 0) return -EIO;
      return rc;
    }
    case 2: {
      size_t ws = zstd_dctx_workspace_bound();
      void *wk = malloc(ws);
      if (!wk) return -ENOMEM;
      zstd_dctx *dctx = zstd_init_dctx(wk, ws);
      int rc;
      if (!dctx) rc = -EIO;
      else {
        size_t r = zstd_decompress_dctx(dctx, dst, dstlen, src, clen);
        rc = zstd_is_error(r) ? -EIO : (int)r;
      }
      free(wk);
      return rc;
    }
    default:
      return -EOPNOTSUPP;
  }
}
