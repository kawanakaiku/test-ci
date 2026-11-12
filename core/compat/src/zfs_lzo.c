// SPDX-License-Identifier: GPL-2.0-or-later
// lib/lzo/lzo1x_decompress_safe.c を無改変で取り込むための薄いラッパ。
// ホストの <endian.h> は __BIG_ENDIAN と __LITTLE_ENDIAN を両方定義するが、上流の lzodefs.h は「両方定義されていればエラー」とするため、
// 取り込む直前に __BIG_ENDIAN だけを外す (対象は little-endian のみ。kernel.h が検査する)。
#include <compat/kernel.h>
#undef __BIG_ENDIAN
#include "lzo1x_decompress_safe.c"
