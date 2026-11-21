/* SPDX-License-Identifier: GPL-2.0-or-later
 * wimfuse の子プロセス (libwimfuse.so) で、wimlib (無改変の libwim.a) の WIM ファイルの入出力を、本体への RPC (コールバック) に差し替える。
 *
 * wimlib は WIM を open(2) → fstat → realpath → pread / read / lseek → close で読む (src/wim.c の begin_read、src/file_io.c)。
 * (64 ビット ABI の arm64-v8a / x86_64 専用: LP64 では pread / lseek / fstat が 64 ビットオフセットなので *64 の別名は不要)
 * リンク時の差し替え (-Wl,--wrap=open など) で、特別な名前 WIMFUSE_SRC_PATH の open だけを「仮想 fd」にし、その fd への pread / read / lseek / fstat を
 * wimfuse_src_read / wimfuse_src_size (wimfuse_main.cpp。RPC のコールバック) に結ぶ。ほかのパス・fd は本物にそのまま渡す (fd 番号は /dev/null を
 * 開いて確保するので、他の fd と衝突しない)。docs/fuse-layer.md 10 章
 *
 * あわせて、bionic に無い POSIX メッセージキュー (mq_*) の「常に失敗する」実装 (stubs/mqueue.h の理由) を置く。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "wimfuse_io.h"

/* C++ (wimfuse_main.cpp) の実装。size = 負の errno / バイト数。read = 負の errno / 読めたバイト数 */
extern int64_t wimfuse_src_size(void);
extern int64_t wimfuse_src_read(void *buf, size_t len, uint64_t off);

#define MAXV 8
static struct {
  int fd;       /* -1 = 空き */
  int64_t pos;  /* read / lseek の位置 */
} g_v[MAXV] = {{-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}};
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static int vslot(int fd) {  /* 仮想 fd の添字 / -1 */
  int r = -1;
  pthread_mutex_lock(&g_mu);
  for (int i = 0; i < MAXV; i++)
    if (g_v[i].fd == fd) { r = i; break; }
  pthread_mutex_unlock(&g_mu);
  return r;
}

extern int __real_open(const char *path, int flags, ...);
extern int __real_close(int fd);
extern ssize_t __real_pread(int fd, void *buf, size_t n, off_t off);
extern ssize_t __real_read(int fd, void *buf, size_t n);
extern off_t __real_lseek(int fd, off_t off, int whence);
extern int __real_fstat(int fd, struct stat *st);
extern char *__real_realpath(const char *path, char *resolved);

int __wrap_open(const char *path, int flags, ...) {
  if (path && strcmp(path, WIMFUSE_SRC_PATH) == 0) {
    int fd = __real_open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < MAXV; i++)
      if (g_v[i].fd < 0) { g_v[i].fd = fd; g_v[i].pos = 0; pthread_mutex_unlock(&g_mu); return fd; }
    pthread_mutex_unlock(&g_mu);
    __real_close(fd);
    errno = EMFILE;
    return -1;
  }
  mode_t mode = 0;
  if (flags & (O_CREAT | O_TMPFILE)) {
    va_list ap;
    va_start(ap, flags);
    mode = (mode_t)va_arg(ap, int);
    va_end(ap);
  }
  return __real_open(path, flags, mode);
}

int __wrap_close(int fd) {
  int s = vslot(fd);
  if (s >= 0) {
    pthread_mutex_lock(&g_mu);
    g_v[s].fd = -1;
    pthread_mutex_unlock(&g_mu);
  }
  return __real_close(fd);
}

static ssize_t vpread(void *buf, size_t n, int64_t off) {
  int64_t r = wimfuse_src_read(buf, n, (uint64_t)off);
  if (r < 0) { errno = (int)-r; return -1; }
  return (ssize_t)r;
}

ssize_t __wrap_pread(int fd, void *buf, size_t n, off_t off) {
  if (vslot(fd) < 0) return __real_pread(fd, buf, n, off);
  return vpread(buf, n, off);
}

ssize_t __wrap_read(int fd, void *buf, size_t n) {
  int s = vslot(fd);
  if (s < 0) return __real_read(fd, buf, n);
  ssize_t r = vpread(buf, n, g_v[s].pos);
  if (r > 0) g_v[s].pos += r;
  return r;
}

static int64_t vlseek(int s, int64_t off, int whence) {
  int64_t base;
  if (whence == SEEK_SET) base = 0;
  else if (whence == SEEK_CUR) base = g_v[s].pos;
  else if (whence == SEEK_END) {
    int64_t sz = wimfuse_src_size();
    if (sz < 0) { errno = (int)-sz; return -1; }
    base = sz;
  } else { errno = EINVAL; return -1; }
  if (base + off < 0) { errno = EINVAL; return -1; }
  g_v[s].pos = base + off;
  return g_v[s].pos;
}
off_t __wrap_lseek(int fd, off_t off, int whence) {
  int s = vslot(fd);
  return s < 0 ? __real_lseek(fd, off, whence) : (off_t)vlseek(s, off, whence);
}

int __wrap_fstat(int fd, struct stat *st) {
  if (vslot(fd) < 0) return __real_fstat(fd, st);
  int64_t sz = wimfuse_src_size();
  if (sz < 0) { errno = (int)-sz; return -1; }
  memset(st, 0, sizeof *st);
  st->st_mode = S_IFREG | 0444;
  st->st_size = (off_t)sz;
  st->st_nlink = 1;
  st->st_blksize = 4096;
  return 0;
}

/* wimlib は begin_read で WIM の絶対パスを realpath(path, NULL) で求める (存在しないパスは失敗) */
char *__wrap_realpath(const char *path, char *resolved) {
  if (path && strcmp(path, WIMFUSE_SRC_PATH) == 0) {
    if (resolved) { strcpy(resolved, path); return resolved; }
    return strdup(path);
  }
  return __real_realpath(path, resolved);
}

/* ---- POSIX メッセージキュー (bionic に無い)。読み取り専用のマウントでは呼ばれない (アンマウント要求・commit の進捗だけ) ---- */
#include <mqueue.h>
mqd_t mq_open(const char *name, int oflag, ...) { (void)name; (void)oflag; errno = ENOSYS; return -1; }
int mq_close(mqd_t mq) { (void)mq; errno = ENOSYS; return -1; }
int mq_unlink(const char *name) { (void)name; errno = ENOSYS; return -1; }
int mq_send(mqd_t mq, const char *msg, size_t len, unsigned prio) { (void)mq; (void)msg; (void)len; (void)prio; errno = ENOSYS; return -1; }
ssize_t mq_receive(mqd_t mq, char *msg, size_t len, unsigned *prio) { (void)mq; (void)msg; (void)len; (void)prio; errno = ENOSYS; return -1; }
