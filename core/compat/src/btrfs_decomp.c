// SPDX-License-Identifier: GPL-2.0-or-later
// Btrfs の圧縮 extent (zlib / lzo / zstd) の展開。カーネルの lib/{zlib_inflate,lzo,zstd} (third_party/kernel-lib、無改変) を呼ぶだけの薄いラッパ。
// 枠組み (LZO の sector 区切り) は btrfs_volume.cpp が解釈する。ここは「1 つの圧縮ストリームを展開する」だけ。
#include <compat/kernel.h>
#undef __BIG_ENDIAN
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <linux/lzo.h>
#include <linux/zlib.h>
#include <linux/zstd.h>

// zlib ストリーム (ヘッダ付き) を dst へ展開する。0 = 完了 (*outlen に書いた長さ)、負 = -errno。dst が足りないときは入った分だけ返し -ENOSPC。
int usbmem_btrfs_zlib_inflate(const void* src, size_t srclen, void* dst, size_t dstcap, size_t* outlen) {
  z_stream s;
  memset(&s, 0, sizeof s);
  s.workspace = malloc((size_t)zlib_inflate_workspacesize());
  if (!s.workspace) return -ENOMEM;
  int rc = -EIO;
  if (zlib_inflateInit(&s) == Z_OK) {
    s.next_in = (const Byte*)src;
    s.avail_in = (uInt)srclen;
    s.next_out = (Byte*)dst;
    s.avail_out = (uInt)dstcap;
    int r = zlib_inflate(&s, Z_SYNC_FLUSH);
    *outlen = (size_t)s.total_out;
    if (r == Z_STREAM_END) rc = 0;
    else if ((r == Z_OK || r == Z_BUF_ERROR) && s.avail_out == 0) rc = -ENOSPC;
    zlib_inflateEnd(&s);
  }
  free(s.workspace);
  return rc;
}

// LZO1X の 1 セグメントを展開する。*dstlen は入力で容量、出力で展開後の長さ。0 / 負の -errno
int usbmem_btrfs_lzo_segment(const void* src, size_t srclen, void* dst, size_t* dstlen) {
  size_t out = *dstlen;
  int r = lzo1x_decompress_safe((const unsigned char*)src, srclen, (unsigned char*)dst, &out);
  if (r != 0 /* LZO_E_OK */) return -EIO;
  *dstlen = out;
  return 0;
}

// zstd フレーム 1 つを展開する (フレームの後ろの 0 埋めは読まない)。0 / 負の -errno (-ENOSPC = dst 不足)
int usbmem_btrfs_zstd_inflate(const void* src, size_t srclen, void* dst, size_t dstcap, size_t* outlen) {
  size_t wsz = zstd_dstream_workspace_bound(1u << 20);
  void* w = malloc(wsz);
  if (!w) return -ENOMEM;
  int rc = -EIO;
  zstd_dstream* ds = zstd_init_dstream(1u << 20, w, wsz);
  if (ds) {
    zstd_in_buffer in = {src, srclen, 0};
    zstd_out_buffer out = {dst, dstcap, 0};
    for (;;) {
      size_t before_in = in.pos, before_out = out.pos;
      size_t r = zstd_decompress_stream(ds, &out, &in);
      if (zstd_is_error(r)) break;
      if (r == 0) { rc = 0; break; }
      if (out.pos == out.size) { rc = -ENOSPC; break; }
      if (in.pos == before_in && out.pos == before_out) break;   // 進まない (入力が足りない)
    }
    *outlen = out.pos;
  }
  free(w);
  return rc;
}
