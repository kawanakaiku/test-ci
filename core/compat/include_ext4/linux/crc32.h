/* SPDX-License-Identifier: GPL-2.0-or-later */
/* fs/jbd2 用の crc32 シム (ext4/jbd2 のライブラリにだけ入る)。crc32_be は JBD2_FEATURE_COMPAT_CHECKSUM (v1) で使う。実装: src/rt_jbd2.c */
#pragma once
#include <compat/kernel.h>
u32 usbmem_crc32_be(u32 crc, const void *p, size_t len);
#define crc32_be(crc, p, len) usbmem_crc32_be((crc), (p), (len))
