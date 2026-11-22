// SPDX-License-Identifier: Apache-2.0
// 圧縮データの展開 (zlib: システムの libz / LZ4 ブロック / LZO1X)。API は usbmem/decomp.h。docs/decomp.md
// LZ4 ブロック: LZ4 block format (https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md) から書いた自前の展開器 (backup_lz4.cpp の LZ4 フレームも使う)。
// LZO1X: 公開されている LZO1X ストリームの記述 (命令の符号化・状態・距離の範囲・LZO-RLE の拡張) から書いた自前の展開器。minilzo・カーネルの lzo のコードは見ていない・写していない。
#include "usbmem/decomp.h"

#include <zlib.h>

#include <cerrno>
#include <cstdint>
#include <cstring>

namespace {

using u8 = uint8_t;

// ---- LZO1X ----
// 長さの可変長符号の続き: 0 のバイトごとに 255 を足し、最初の 0 でないバイトを足して終わる。
bool lzo_ext(const u8* in, size_t n, size_t* ip, size_t base, size_t* out) {
  size_t t = base;
  for (;;) {
    if (*ip >= n) return false;
    const u8 b = in[(*ip)++];
    if (b != 0) { t += b; break; }
    t += 255;
    if (t > (size_t(1) << 30)) return false;
  }
  *out = t;
  return true;
}

}  // namespace

extern "C" {

int usbmem_zlib_decompress(const void* src, size_t srclen, void* dst, size_t dstcap, size_t* outlen) {
  *outlen = 0;
  z_stream s;
  std::memset(&s, 0, sizeof s);
  if (srclen > 0xFFFFFFFFu || dstcap > 0xFFFFFFFFu) return -EIO;
  if (inflateInit(&s) != Z_OK) return -ENOMEM;
  s.next_in = static_cast<Bytef*>(const_cast<void*>(src));
  s.avail_in = static_cast<uInt>(srclen);
  s.next_out = static_cast<Bytef*>(dst);
  s.avail_out = static_cast<uInt>(dstcap);
  const int r = inflate(&s, Z_SYNC_FLUSH);
  *outlen = static_cast<size_t>(s.total_out);
  int rc = -EIO;
  if (r == Z_STREAM_END) rc = 0;
  else if ((r == Z_OK || r == Z_BUF_ERROR) && s.avail_out == 0) rc = -ENOSPC;
  inflateEnd(&s);
  return rc;
}

int usbmem_lz4_block_decompress(const void* src, size_t srclen, void* dst, size_t prefix, size_t cap, size_t* outlen) {
  const u8* in = static_cast<const u8*>(src);
  u8* out = static_cast<u8*>(dst);
  const size_t n = srclen;
  size_t ip = 0, op = prefix;
  const size_t limit = prefix + cap;
  *outlen = 0;
  while (ip < n) {
    const u8 token = in[ip++];
    size_t lit = token >> 4;
    if (lit == 15) {
      u8 b;
      do {
        if (ip >= n) return -EIO;
        b = in[ip++];
        lit += b;
        if (lit > limit) return -EIO;
      } while (b == 255);
    }
    if (lit > n - ip) return -EIO;
    if (lit > limit - op) return -ENOSPC;
    if (lit) std::memcpy(out + op, in + ip, lit);
    ip += lit;
    op += lit;
    if (ip == n) break;   // 最後の系列はリテラルだけ
    if (n - ip < 2) return -EIO;
    const size_t off = in[ip] | (static_cast<size_t>(in[ip + 1]) << 8);
    ip += 2;
    if (off == 0 || off > op) return -EIO;
    size_t ml = token & 15u;
    if (ml == 15) {
      u8 b;
      do {
        if (ip >= n) return -EIO;
        b = in[ip++];
        ml += b;
        if (ml > limit) return -EIO;
      } while (b == 255);
    }
    ml += 4;
    if (ml > limit - op) return -ENOSPC;
    for (size_t i = 0; i < ml; i++, op++) out[op] = out[op - off];
  }
  *outlen = op - prefix;
  return 0;
}

int usbmem_lzo1x_decompress(const void* src, size_t srclen, void* dst, size_t* dstlen) {
  const u8* in = static_cast<const u8*>(src);
  u8* out = static_cast<u8*>(dst);
  const size_t n = srclen, cap = *dstlen;
  size_t ip = 0, op = 0;
  *dstlen = 0;
  int version = 0;   // 0 = LZO1X、1 = LZO-RLE (先頭が 17, 1)
  if (n >= 5 && in[0] == 17) {
    version = in[1];
    if (version > 1) return -EIO;
    ip = 2;
  }
  // 「直前の命令が写したリテラルの数」の状態: 0 = 無し / 1..3 = 短いリテラル / 4 = 長いリテラル列。次の 0..15 の命令の意味が変わる
  unsigned state = 0;
  int rc = 0;
  auto literals = [&](size_t cnt) -> bool {
    if (cnt > n - ip) { rc = -EIO; return false; }
    if (cnt > cap - op) { rc = -ENOSPC; return false; }
    if (cnt) std::memcpy(out + op, in + ip, cnt);
    ip += cnt;
    op += cnt;
    return true;
  };
  if (ip >= n) return -EIO;
  if (in[ip] > 17) {   // 先頭の 1 バイトだけの特別な符号: リテラル列
    const size_t t = in[ip++] - 17u;
    if (!literals(t)) { *dstlen = op; return rc; }
    state = t < 4 ? static_cast<unsigned>(t) : 4u;
  }
  for (;;) {
    if (ip >= n) { *dstlen = op; return -EIO; }   // 終端の印が無い
    const unsigned b = in[ip++];
    size_t mlen, dist;
    unsigned trail;   // 命令の後ろに続くリテラルの数 (0..3)
    if (b < 16) {
      if (state == 0) {   // 長いリテラル列
        size_t t = b;
        if (t == 0 && !lzo_ext(in, n, &ip, 15, &t)) { *dstlen = op; return -EIO; }
        if (!literals(t + 3)) { *dstlen = op; return rc; }
        state = 4;
        continue;
      }
      if (ip >= n) { *dstlen = op; return -EIO; }
      dist = (static_cast<size_t>(in[ip++]) << 2) + (b >> 2) + 1;
      if (state == 4) { dist += 2048; mlen = 3; } else mlen = 2;
      trail = b & 3;
    } else if (b < 32) {
      const unsigned h = (b >> 3) & 1;
      size_t l = b & 7;
      if (version == 1 && h && n - ip >= 3 && in[ip + 1] == 0xFF && (in[ip] & 0xFC) == 0xFC) {
        // LZO-RLE: 0 の連続。長さ = ((4 つめのバイト << 3) | 下位 3 ビット) + 4
        const size_t run = ((static_cast<size_t>(in[ip + 2]) << 3) | l) + 4;
        trail = in[ip] & 3u;
        ip += 3;
        if (run > cap - op) { *dstlen = op; return -ENOSPC; }
        std::memset(out + op, 0, run);
        op += run;
        if (!literals(trail)) { *dstlen = op; return rc; }
        state = trail;
        continue;
      }
      if (l == 0 && !lzo_ext(in, n, &ip, 7, &l)) { *dstlen = op; return -EIO; }
      mlen = l + 2;
      if (n - ip < 2) { *dstlen = op; return -EIO; }
      const unsigned w = in[ip] | (static_cast<unsigned>(in[ip + 1]) << 8);
      ip += 2;
      trail = w & 3;
      const size_t d = w >> 2;
      if (h == 0 && d == 0) {   // 距離 16384 の印 = ストリームの終わり
        *dstlen = op;
        return ip == n ? 0 : -EIO;
      }
      dist = 16384 + (static_cast<size_t>(h) << 14) + d;
    } else if (b < 64) {
      size_t l = b & 31;
      if (l == 0 && !lzo_ext(in, n, &ip, 31, &l)) { *dstlen = op; return -EIO; }
      mlen = l + 2;
      if (n - ip < 2) { *dstlen = op; return -EIO; }
      const unsigned w = in[ip] | (static_cast<unsigned>(in[ip + 1]) << 8);
      ip += 2;
      trail = w & 3;
      dist = (w >> 2) + 1;
    } else {
      if (ip >= n) { *dstlen = op; return -EIO; }
      mlen = b < 128 ? 3 + ((b >> 5) & 1u) : 5 + ((b >> 5) & 3u);
      dist = (static_cast<size_t>(in[ip++]) << 3) + ((b >> 2) & 7u) + 1;
      trail = b & 3;
    }
    if (dist > op) { *dstlen = op; return -EIO; }
    if (mlen > cap - op) { *dstlen = op; return -ENOSPC; }
    if (dist >= mlen) { std::memcpy(out + op, out + op - dist, mlen); op += mlen; }
    else for (size_t i = 0; i < mlen; i++, op++) out[op] = out[op - dist];
    if (!literals(trail)) { *dstlen = op; return rc; }
    state = trail;
  }
}

}  // extern "C"
