/* SPDX-License-Identifier: GPL-2.0-or-later
 * libntfs-3g の unix_io.c の代わり (docs/fuse-layer.md 2 章)。
 *
 * libntfs-3g は、デバイスへの I/O をすべて struct ntfs_device_operations (open / close / seek / read / write / pread / pwrite / sync /
 * stat / ioctl) 越しに行う。既定の実装 (unix_io.c) は /dev ノードやイメージを open(2) / pread(2) する。
 * ここでは同じ構造体 (シンボル名 ntfs_device_unix_io_ops = ntfs_device_default_io_ops) を、usbmem の BlockDevice を呼ぶ実装で定義する。
 * ビルドでは unix_io.c を外してこのファイルを入れるだけ (ntfs-3g のソースは無改変)。
 *
 * 結び付けるデバイスは fuseshim_ntfs3g_io_bind() で渡す 1 つ。ntfs-3g に渡すデバイス名は、存在するパスなら何でもよい (名前は使わない)。
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef HAVE_LINUX_FS_H
#include <linux/fs.h>
#endif

#include "types.h"
#include "device.h"
#include "logging.h"
#include "misc.h"

#include "fuseshim.h"

static struct fuseshim_blkdev g_dev;
static int g_bound;

struct io_priv {
  s64 pos; /* read / write / seek の現在位置 */
};
#define PRIV(dev) ((struct io_priv *)(dev)->d_private)

void fuseshim_ntfs3g_io_bind(const struct fuseshim_blkdev *dev) {
  if (dev) {
    g_dev = *dev;
    g_bound = 1;
  } else {
    memset(&g_dev, 0, sizeof g_dev);
    g_bound = 0;
  }
}

static int io_open(struct ntfs_device *dev, int flags) {
  if (NDevOpen(dev)) {
    errno = EBUSY;
    return -1;
  }
  if (!g_bound) {
    errno = ENODEV;
    return -1;
  }
  int want_rw = ((flags & O_RDWR) == O_RDWR);
  if (want_rw && g_dev.read_only) {
    errno = EROFS; /* libntfs-3g は NTFS_MNT_MAY_RDONLY なら読み取り専用でやり直す */
    return -1;
  }
  dev->d_private = ntfs_malloc(sizeof(struct io_priv));
  if (!dev->d_private) return -1;
  PRIV(dev)->pos = 0;
  if (!want_rw) NDevSetReadOnly(dev);
  NDevSetOpen(dev);
  return 0;
}

static int io_sync(struct ntfs_device *dev) {
  if (NDevReadOnly(dev)) return 0;
  int rc = g_dev.flush ? g_dev.flush(g_dev.ctx) : 0;
  if (rc < 0) {
    errno = -rc;
    return -1;
  }
  NDevClearDirty(dev);
  return 0;
}

static int io_close(struct ntfs_device *dev) {
  if (!NDevOpen(dev)) {
    errno = EBADF;
    return -1;
  }
  if (NDevDirty(dev) && io_sync(dev)) return -1;
  free(dev->d_private);
  dev->d_private = NULL;
  NDevClearOpen(dev);
  return 0;
}

static s64 io_seek(struct ntfs_device *dev, s64 offset, int whence) {
  s64 base;
  switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = PRIV(dev)->pos; break;
    case SEEK_END: base = (s64)g_dev.size; break;
    default: errno = EINVAL; return -1;
  }
  if (base + offset < 0) {
    errno = EINVAL;
    return -1;
  }
  PRIV(dev)->pos = base + offset;
  return PRIV(dev)->pos;
}

/* 範囲外は read(2) と同じく EOF (短い read / 0) にする。BlockDevice は範囲外を OutOfRange で断るので、ここで切り詰める。 */
static s64 io_pread(struct ntfs_device *dev, void *buf, s64 count, s64 offset) {
  (void)dev;
  if (offset < 0 || count < 0) {
    errno = EINVAL;
    return -1;
  }
  if ((u64)offset >= g_dev.size) return 0;
  if ((u64)count > g_dev.size - (u64)offset) count = (s64)(g_dev.size - (u64)offset);
  if (count == 0) return 0;
  int rc = g_dev.read_at(g_dev.ctx, (u64)offset, buf, (size_t)count);
  if (rc < 0) {
    errno = -rc;
    return -1;
  }
  return count;
}

static s64 io_pwrite(struct ntfs_device *dev, const void *buf, s64 count, s64 offset) {
  if (NDevReadOnly(dev) || g_dev.read_only || !g_dev.write_at) {
    errno = EROFS;
    return -1;
  }
  if (offset < 0 || count < 0 || (u64)offset > g_dev.size || (u64)count > g_dev.size - (u64)offset) {
    errno = ENOSPC;
    return -1;
  }
  NDevSetDirty(dev);
  int rc = g_dev.write_at(g_dev.ctx, (u64)offset, buf, (size_t)count);
  if (rc < 0) {
    errno = -rc;
    return -1;
  }
  return count;
}

static s64 io_read(struct ntfs_device *dev, void *buf, s64 count) {
  s64 n = io_pread(dev, buf, count, PRIV(dev)->pos);
  if (n > 0) PRIV(dev)->pos += n;
  return n;
}

static s64 io_write(struct ntfs_device *dev, const void *buf, s64 count) {
  s64 n = io_pwrite(dev, buf, count, PRIV(dev)->pos);
  if (n > 0) PRIV(dev)->pos += n;
  return n;
}

/* 通常ファイル (S_IFREG) として答える。ntfs_device_size_get は下の ioctl (BLKGETSIZE64) を先に試す。 */
static int io_stat(struct ntfs_device *dev, struct stat *st) {
  (void)dev;
  memset(st, 0, sizeof *st);
  st->st_mode = S_IFREG | 0600;
  st->st_size = (off_t)g_dev.size;
  st->st_blksize = (blksize_t)(g_dev.block_size ? g_dev.block_size : 512);
  st->st_blocks = (blkcnt_t)(g_dev.size / 512);
  st->st_nlink = 1;
  return 0;
}

static int io_ioctl(struct ntfs_device *dev, unsigned long request, void *argp) {
  (void)dev;
#ifdef BLKGETSIZE64
  if (request == BLKGETSIZE64) {
    *(u64 *)argp = g_dev.size;
    return 0;
  }
#endif
#ifdef BLKSSZGET
  if (request == BLKSSZGET) {
    *(int *)argp = (int)(g_dev.block_size ? g_dev.block_size : 512);
    return 0;
  }
#endif
  (void)argp;
  errno = ENOTTY; /* HDIO_GETGEO など。通常ファイルと同じ答え */
  return -1;
}

struct ntfs_device_operations ntfs_device_unix_io_ops = {
    .open = io_open,
    .close = io_close,
    .seek = io_seek,
    .read = io_read,
    .write = io_write,
    .pread = io_pread,
    .pwrite = io_pwrite,
    .sync = io_sync,
    .stat = io_stat,
    .ioctl = io_ioctl,
};
