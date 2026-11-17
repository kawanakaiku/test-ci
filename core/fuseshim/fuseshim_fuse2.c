/* SPDX-License-Identifier: GPL-2.0-or-later
 * fuseshim: libfuse 2.x 系 (高水準 API) の互換層。ntfs-3g (src/ntfs-3g.c) が使う関数だけを実装する (docs/fuse-layer.md)。
 *
 * 差し替える関数: fuse_opt_add_arg / fuse_opt_free_args / fuse_version / fuse_mount / fuse_unmount / fuse_new / fuse_destroy /
 *   fuse_loop / fuse_exit / fuse_get_session / fuse_set_signal_handlers / fuse_get_context、libfuse-lite 内の drop_privs / restore_privs。
 * /dev/fuse・fusermount・マウント・シグナルハンドラ・スレッドは一切使わない。
 *
 * ヘッダ (fuse.h など) はビルド時に取得する ntfs-3g のソースの include/fuse-lite を使う (libfuse 2.x の API のヘッダ。リポジトリには置かない)。
 * struct fuse_operations の並びは、ntfs-3g.c と本ファイルが同じヘッダでコンパイルされることで揃う。
 */
#define _GNU_SOURCE
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#ifndef FUSE_USE_VERSION
#define FUSE_USE_VERSION 26
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
  void *user_data;
  struct fuse_context ctx;
  int inited, exited;
};
struct fuse_chan {
  char *mountpoint;
};
struct fuse_session {
  int unused;
};

static pthread_mutex_t g_call = PTHREAD_MUTEX_INITIALIZER; /* 操作の呼び出しを直列化する (ntfs-3g は単一スレッド前提) */
static struct fuse *g_fuse;                                /* 現在のセッション (1 つだけ) */
static struct fuse_session g_session;
static fuseshim_loop_fn g_hook;
static void *g_hook_arg;

/* ---- 引数 (fuse_opt) ------------------------------------------------------------------------------------------ */
int fuse_opt_add_arg(struct fuse_args *args, const char *arg) {
  char **nv = realloc(args->argv, (size_t)(args->argc + 2) * sizeof(char *));
  char *copy = nv ? strdup(arg) : NULL;
  if (!nv || !copy) return -1;
  args->argv = nv;
  args->allocated = 1;
  args->argv[args->argc++] = copy;
  args->argv[args->argc] = NULL;
  return 0;
}

void fuse_opt_free_args(struct fuse_args *args) {
  if (args->argv && args->allocated) {
    for (int i = 0; i < args->argc; i++) free(args->argv[i]);
    free(args->argv);
  }
  args->argv = NULL;
  args->argc = 0;
  args->allocated = 0;
}

int fuse_version(void) { return FUSE_VERSION; }

/* libfuse-lite (fusermount.c) の特権の出し入れ。この層は特権を使わない。 */
int drop_privs(void) { return 0; }
int restore_privs(void) { return 0; }

/* ---- セッション ----------------------------------------------------------------------------------------------- */
struct fuse_chan *fuse_mount(const char *mountpoint, struct fuse_args *args) {
  (void)args; /* -o のオプションはカーネル向けなので見ない */
  if (!mountpoint) return NULL;
  struct fuse_chan *ch = calloc(1, sizeof *ch);
  if (!ch) return NULL;
  ch->mountpoint = strdup(mountpoint);
  return ch;
}

void fuse_unmount(const char *mountpoint, struct fuse_chan *ch) {
  (void)mountpoint;
  if (!ch) return;
  free(ch->mountpoint);
  free(ch);
}

struct fuse *fuse_new(struct fuse_chan *ch, struct fuse_args *args, const struct fuse_operations *op, size_t op_size,
                      void *user_data) {
  (void)ch;
  (void)args;
  struct fuse *f = calloc(1, sizeof *f);
  if (!f) return NULL;
  memcpy(&f->op, op, op_size < sizeof f->op ? op_size : sizeof f->op);
  f->user_data = user_data;
  f->ctx.fuse = f;
  f->ctx.uid = geteuid();
  f->ctx.gid = getegid();
  f->ctx.pid = getpid();
  f->ctx.umask = 0;
  f->ctx.private_data = user_data;
  g_fuse = f;
  return f;
}

void fuse_destroy(struct fuse *f) {
  if (!f) return;
  if (f->inited && f->op.destroy) f->op.destroy(f->ctx.private_data);
  if (g_fuse == f) g_fuse = NULL;
  free(f);
}

struct fuse_session *fuse_get_session(struct fuse *f) {
  (void)f;
  return &g_session;
}

int fuse_set_signal_handlers(struct fuse_session *se) {
  (void)se; /* アプリのシグナル処理を奪わない */
  return 0;
}

struct fuse_context *fuse_get_context(void) { return g_fuse ? &g_fuse->ctx : NULL; }

void fuse_exit(struct fuse *f) { f->exited = 1; }

void fuseshim_set_loop_hook(fuseshim_loop_fn fn, void *arg) {
  g_hook = fn;
  g_hook_arg = arg;
}

int fuse_loop(struct fuse *f) {
  struct fuse_conn_info conn;
  memset(&conn, 0, sizeof conn);
  conn.proto_major = 7;
  conn.proto_minor = 19;
  conn.max_write = 128 * 1024;
  conn.max_readahead = 128 * 1024;
  if (f->op.init) {
    void *p = f->op.init(&conn);
    if (p) f->ctx.private_data = p;
  }
  f->inited = 1;
  if (!g_hook) return -1; /* ホストがフックを入れていない = 要求を処理する人がいない */
  g_hook(g_hook_arg);
  return 0;
}

/* ---- ホストからの操作 ----------------------------------------------------------------------------------------- */
int fuseshim_ready(void) { return g_fuse && g_fuse->inited; }

#define LOCK() pthread_mutex_lock(&g_call)
#define UNLOCK() pthread_mutex_unlock(&g_call)
#define OPS() (g_fuse && g_fuse->inited ? &g_fuse->op : NULL)

int fuseshim_getattr(const char *path, struct fuseshim_attr *a) {
  struct stat st;
  memset(&st, 0, sizeof st);
  LOCK();
  const struct fuse_operations *op = OPS();
  int rc = (op && op->getattr) ? op->getattr(path, &st) : -ENOSYS;
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

static int rd_fill(void *buf, const char *name, const struct stat *st, off_t off) {
  (void)off;
  struct rd_ctx *r = buf;
  return r->cb(r->arg, name, st ? (uint64_t)st->st_ino : 0, st ? (uint32_t)st->st_mode : 0) ? 1 : 0; /* 非 0 = バッファ満杯 (止める) */
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
    if (!rc) rc = op->readdir(path, &r, rd_fill, 0, &fi);
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

/* ntfs-3g.c を -Ddaemon=fuseshim_daemon_stub でビルドする。daemon(3) は fork して親を exit させるので、同一プロセスでは致命的。
 * 呼ばれないように no_detach を渡すが、渡し忘れても何も起きないようにしておく。 */
int fuseshim_daemon_stub(int nochdir, int noclose) {
  (void)nochdir;
  (void)noclose;
  return 0;
}
