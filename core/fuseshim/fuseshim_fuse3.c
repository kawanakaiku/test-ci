/* SPDX-License-Identifier: GPL-2.0-or-later
 * fuseshim: libfuse 3.x 系 (高水準 API) の互換層。wimlib の wimmount (src/mount_image.c) が使う関数だけを実装する (docs/fuse-layer.md 6 章)。
 *
 * wimlib は fuse_main() (= fuse_main_real_versioned) に引数列と fuse_operations を渡して呼ぶだけで、マウント・ループ・終了を libfuse に任せる。
 * ここでは fuse_main_real_versioned が init を呼んだ後、ホストのフックに処理を渡し (fuseshim_set_loop_hook)、フックが戻ったら 0 を返す。
 * 公開 API (fuseshim_*) は fuseshim_fuse2.c と同じ (libfuse 2.x と 3.x で fuse_operations の引数が違う分だけ呼び方が違う)。
 * 1 つの実行体に入れるのは 2 または 3 のどちらか (fuseshim_* のシンボルが重なる)。
 *
 * ヘッダはシステムの libfuse3 (fuse3/fuse.h。LGPL-2.1 のヘッダだけ。libfuse3 のライブラリはリンクしない) を使う。
 */
#define _GNU_SOURCE
#ifndef FUSE_USE_VERSION
#define FUSE_USE_VERSION 30
#endif
#include <fuse.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "fuseshim.h"

struct fuse {
  struct fuse_operations op;
  struct fuse_context ctx;
  int inited, exited;
};

static pthread_mutex_t g_call = PTHREAD_MUTEX_INITIALIZER;
static struct fuse *g_fuse;
static fuseshim_loop_fn g_hook;
static void *g_hook_arg;

void fuseshim_set_loop_hook(fuseshim_loop_fn fn, void *arg) {
  g_hook = fn;
  g_hook_arg = arg;
}

struct fuse_context *fuse_get_context(void) { return g_fuse ? &g_fuse->ctx : NULL; }

void fuse_exit(struct fuse *f) { f->exited = 1; }

int fuse_main_real_versioned(int argc, char *argv[], const struct fuse_operations *op, size_t op_size, struct libfuse_version *version,
                             void *user_data) {
  (void)argc;
  (void)argv; /* マウントポイントや -o はカーネル向けなので見ない */
  (void)version;
  struct fuse *f = calloc(1, sizeof *f);
  if (!f) return 3;
  memcpy(&f->op, op, op_size < sizeof f->op ? op_size : sizeof f->op);
  f->ctx.fuse = f;
  f->ctx.uid = geteuid();
  f->ctx.gid = getegid();
  f->ctx.pid = getpid();
  f->ctx.umask = 0;
  f->ctx.private_data = user_data;
  g_fuse = f;

  struct fuse_conn_info conn;
  struct fuse_config cfg;
  memset(&conn, 0, sizeof conn);
  memset(&cfg, 0, sizeof cfg);
  conn.proto_major = 7;
  conn.proto_minor = 31;
  if (f->op.init) {
    void *p = f->op.init(&conn, &cfg);
    if (p) f->ctx.private_data = p;
  }
  f->inited = 1;
  int rc = 0;
  if (g_hook)
    g_hook(g_hook_arg);
  else
    rc = 3;
  if (f->op.destroy) f->op.destroy(f->ctx.private_data);
  g_fuse = NULL;
  free(f);
  return rc;
}

/* ---- ホストからの操作 (fuseshim_fuse2.c と同じ意味) -------------------------------------------------------- */
int fuseshim_ready(void) { return g_fuse && g_fuse->inited; }

#define LOCK() pthread_mutex_lock(&g_call)
#define UNLOCK() pthread_mutex_unlock(&g_call)
#define OPS() (g_fuse && g_fuse->inited ? &g_fuse->op : NULL)

int fuseshim_getattr(const char *path, struct fuseshim_attr *a) {
  struct stat st;
  memset(&st, 0, sizeof st);
  LOCK();
  const struct fuse_operations *op = OPS();
  int rc = (op && op->getattr) ? op->getattr(path, &st, NULL) : -ENOSYS;
  UNLOCK();
  if (rc) return rc;
  memset(a, 0, sizeof *a);
  a->ino = (uint64_t)st.st_ino;
  a->size = (uint64_t)st.st_size;
  a->blocks = (uint64_t)st.st_blocks;
  a->mode = (uint32_t)st.st_mode;
  a->nlink = (uint32_t)st.st_nlink;
  a->uid = (uint32_t)st.st_uid;
  a->gid = (uint32_t)st.st_gid;
  a->mtime_sec = (int64_t)st.st_mtim.tv_sec;
  a->mtime_nsec = (uint32_t)st.st_mtim.tv_nsec;
  a->atime_sec = (int64_t)st.st_atim.tv_sec;
  a->ctime_sec = (int64_t)st.st_ctim.tv_sec;
  return 0;
}

struct rd_ctx {
  fuseshim_dirent_fn cb;
  void *arg;
};

static int rd_fill(void *buf, const char *name, const struct stat *st, off_t off, enum fuse_fill_dir_flags flags) {
  (void)off;
  (void)flags;
  struct rd_ctx *r = buf;
  return r->cb(r->arg, name, st ? (uint64_t)st->st_ino : 0, st ? (uint32_t)st->st_mode : 0) ? 1 : 0;
}

int fuseshim_readdir(const char *path, fuseshim_dirent_fn cb, void *arg) {
  struct rd_ctx r = {cb, arg};
  struct fuse_file_info fi;
  memset(&fi, 0, sizeof fi);
  fi.flags = O_RDONLY;
  LOCK();
  const struct fuse_operations *op = OPS();
  int rc = 0, opened = 0;
  if (!op || !op->readdir) {
    rc = -ENOSYS;
  } else {
    if (op->opendir) {
      rc = op->opendir(path, &fi);
      opened = (rc == 0);
    }
    if (!rc) rc = op->readdir(path, &r, rd_fill, 0, &fi, 0);
    if (opened && op->releasedir) op->releasedir(path, &fi);
  }
  UNLOCK();
  return rc;
}

int64_t fuseshim_read(const char *path, uint64_t off, void *buf, size_t len) {
  struct fuse_file_info fi;
  memset(&fi, 0, sizeof fi);
  fi.flags = O_RDONLY;
  LOCK();
  const struct fuse_operations *op = OPS();
  if (!op || !op->read) {
    UNLOCK();
    return -ENOSYS;
  }
  int rc = op->open ? op->open(path, &fi) : 0;
  if (rc) {
    UNLOCK();
    return rc;
  }
  size_t done = 0;
  int err = 0;
  while (done < len) {
    int n = op->read(path, (char *)buf + done, len - done, (off_t)(off + done), &fi);
    if (n < 0) {
      err = n;
      break;
    }
    if (n == 0) break;
    done += (size_t)n;
  }
  if (op->release) op->release(path, &fi);
  UNLOCK();
  return (done == 0 && err) ? err : (int64_t)done;
}

int fuseshim_readlink(const char *path, char *buf, size_t size) {
  LOCK();
  const struct fuse_operations *op = OPS();
  int rc = (op && op->readlink) ? op->readlink(path, buf, size) : -ENOSYS;
  UNLOCK();
  if (size) buf[size - 1] = 0;
  return rc;
}

int fuseshim_statfs(const char *path, struct fuseshim_statvfs *out) {
  struct statvfs sv;
  memset(&sv, 0, sizeof sv);
  LOCK();
  const struct fuse_operations *op = OPS();
  int rc = (op && op->statfs) ? op->statfs(path, &sv) : -ENOSYS;
  UNLOCK();
  if (rc) return rc;
  out->bsize = sv.f_bsize;
  out->frsize = sv.f_frsize;
  out->blocks = sv.f_blocks;
  out->bfree = sv.f_bfree;
  out->bavail = sv.f_bavail;
  out->files = sv.f_files;
  out->ffree = sv.f_ffree;
  out->namemax = (uint32_t)sv.f_namemax;
  return 0;
}

int fuseshim_getxattr(const char *path, const char *name, char *value, size_t size) {
  LOCK();
  const struct fuse_operations *op = OPS();
  int rc = (op && op->getxattr) ? op->getxattr(path, name, value, size) : -ENOSYS;
  UNLOCK();
  return rc;
}
