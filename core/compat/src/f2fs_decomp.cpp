// SPDX-License-Identifier: Apache-2.0
// F2FS の圧縮クラスタの展開 (lzo / lzo-rle / lz4 / zstd)。core の展開器 (usbmem/decomp.h。docs/decomp.md) を呼ぶだけの薄いラッパ。
#include <cerrno>
#include <climits>
#include <cstddef>

#include "usbmem/decomp.h"

// alg: 0 = lzo, 1 = lz4, 2 = zstd, 3 = lzo-rle (展開は lzo と同じ: 先頭の版の印で見分ける)。成功したら展開後のバイト数、失敗は負の errno。
extern "C" int usbmem_f2fs_decompress(int alg, const void* src, size_t clen, void* dst, size_t dstlen) {
  if (clen == 0 || dstlen == 0 || dstlen > static_cast<size_t>(INT_MAX)) return -EINVAL;
  size_t out = 0;
  int rc;
  switch (alg) {
    case 0:
    case 3:
      out = dstlen;
      rc = usbmem_lzo1x_decompress(src, clen, dst, &out);
      break;
    case 1:
      rc = usbmem_lz4_block_decompress(src, clen, dst, 0, dstlen, &out);
      break;
    case 2:
      rc = usbmem_zstd_decompress(src, clen, dst, dstlen, &out);
      break;
    default:
      return -EOPNOTSUPP;
  }
  return rc == 0 ? static_cast<int>(out) : -EIO;
}
