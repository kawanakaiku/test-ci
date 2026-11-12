/* SPDX-License-Identifier: GPL-2.0-or-later */
/* LinuxCompat 用の最小 <linux/crc32.h>: lib/xz が使う crc32_le() だけ (表を一度だけ作る素直な実装)。 */
#ifndef USBMEM_KLIB_CRC32_H
#define USBMEM_KLIB_CRC32_H
#include <compat/kernel.h>
#include <pthread.h>
static u32 usbmem_crc32_tab_[256];
static pthread_once_t usbmem_crc32_once_ = PTHREAD_ONCE_INIT;
static inline void usbmem_crc32_init_(void) {
  for (u32 i = 0; i < 256; i++) { u32 c = i; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); usbmem_crc32_tab_[i] = c; }
}
static inline u32 crc32_le(u32 crc, const unsigned char *p, size_t len) {
  pthread_once(&usbmem_crc32_once_, usbmem_crc32_init_);
  while (len--) crc = usbmem_crc32_tab_[(crc ^ *p++) & 0xff] ^ (crc >> 8);
  return crc;
}
#endif
