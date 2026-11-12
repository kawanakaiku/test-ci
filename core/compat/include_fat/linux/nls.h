/* SPDX-License-Identifier: GPL-2.0-or-later */
/* fs/fat 用の <linux/nls.h>: 上流 include/linux/nls.h のうち FAT と nls_cp*.c が使う部分。nls_table 本体は compat/fs.h、登録・検索は rt_pagecache.c。 */
#pragma once
#include <compat/kernel.h>

#define MAX_WCHAR_T 0xffff

static inline unsigned char nls_tolower(struct nls_table *t, unsigned char c) {
  unsigned char nc = t->charset2lower[c];
  return nc ? nc : c;
}
static inline unsigned char nls_toupper(struct nls_table *t, unsigned char c) {
  unsigned char nc = t->charset2upper[c];
  return nc ? nc : c;
}
static inline int nls_strnicmp(struct nls_table *t, const unsigned char *s1, const unsigned char *s2, int len) {
  while (len--) {
    if (nls_tolower(t, *s1++) != nls_tolower(t, *s2++)) return 1;
  }
  return 0;
}
