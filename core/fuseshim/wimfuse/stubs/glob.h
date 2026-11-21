/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Android (bionic) の <glob.h> は API 28 以降。minSdk 26 のために、宣言だけの代用ヘッダと、常に「一致なし」を返す実装 (wimfuse_io.c) を使う。
 * wimlib は glob を wimlib_reference_resource_files (WIMLIB_REF_FLAG_GLOB_ENABLE。分割 WIM の参照) でしか使わず、wimfuse は呼ばない。 */
#ifndef WIMFUSE_STUB_GLOB_H
#define WIMFUSE_STUB_GLOB_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
  size_t gl_pathc;
  char **gl_pathv;
  size_t gl_offs;
} glob_t;
#define GLOB_ERR (1 << 0)
#define GLOB_NOSORT (1 << 5)
#define GLOB_NOSPACE 1
#define GLOB_ABORTED 2
#define GLOB_NOMATCH 3
int glob(const char *pattern, int flags, int (*errfunc)(const char *, int), glob_t *pglob);
void globfree(glob_t *pglob);
#ifdef __cplusplus
}
#endif
#endif
