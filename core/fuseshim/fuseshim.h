/* SPDX-License-Identifier: GPL-2.0-or-later
 * fuseshim: プロセス内の libfuse 互換の層 (docs/fuse-layer.md)。
 *
 * FUSE のプログラム (ntfs-3g など) が呼ぶ libfuse の関数 (fuse_mount / fuse_new / fuse_loop ...) を差し替え、
 * /dev/fuse を使わずに、そのプログラムが登録した fuse_operations (getattr / readdir / read ...) を直接呼ぶ。
 * このヘッダは libfuse のヘッダに依存しない (ホスト側の C++ から使う C API)。実体は fuseshim_fuse2.c (libfuse 2.x 系の API)。
 *
 * 前提 (試作): 1 プロセスに 1 セッション。操作の呼び出しはグローバルのミューテックスで直列化する。
 */
#ifndef USBMEM_FUSESHIM_H
#define USBMEM_FUSESHIM_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- ブロックデバイスの差し替え (ntfs3g_io.c: libntfs-3g の unix_io.c の代わり) ---------------------------------
 * usbmem の BlockDevice を、libntfs-3g の ntfs_device_operations (open / pread / ioctl ...) に見せる。
 * fuseshim_ntfs3g_io_bind() で結び付けた 1 つのデバイスが、ntfs-3g が開くどのデバイス名にも対応する (名前は無視)。 */
struct fuseshim_blkdev {
  void *ctx;
  int (*read_at)(void *ctx, uint64_t off, void *buf, size_t len);        /* 0 / 負の errno */
  int (*write_at)(void *ctx, uint64_t off, const void *buf, size_t len); /* 0 / 負の errno */
  int (*flush)(void *ctx);                                               /* 0 / 負の errno */
  uint64_t size;       /* バイト */
  uint32_t block_size; /* 論理ブロックサイズ (BLKSSZGET の答え) */
  int read_only;
};
void fuseshim_ntfs3g_io_bind(const struct fuseshim_blkdev *dev);

/* ---- fuse_loop の差し替え ------------------------------------------------------------------------------------
 * ホストが fuse_loop() の中で実行する関数を登録する。fuse_loop() は init を呼んだ後 fn(arg) を呼び、fn が戻ると 0 を返す
 * (= ホストが「アンマウント」したことになる)。fn の間、ホストの別スレッドが下の fuseshim_* 操作を呼べる。 */
typedef void (*fuseshim_loop_fn)(void *arg);
void fuseshim_set_loop_hook(fuseshim_loop_fn fn, void *arg);

/* ---- 登録された fuse_operations を呼ぶ (すべて 0 以上 = 成功 / 負の errno) ------------------------------------- */
struct fuseshim_attr {
  uint64_t ino, size, blocks;
  uint32_t mode, nlink, uid, gid;
  int64_t mtime_sec, atime_sec, ctime_sec;
  uint32_t mtime_nsec;
};
struct fuseshim_statvfs {
  uint64_t bsize, frsize, blocks, bfree, bavail, files, ffree;
  uint32_t namemax;
};
/* 戻り値が 0 以外ならそこで列挙を止める。cb の中から fuseshim_* を呼んではならない (ロックを持っている)。 */
typedef int (*fuseshim_dirent_fn)(void *arg, const char *name, uint64_t ino, uint32_t mode);

int fuseshim_ready(void);   /* 1 = fuse_loop に入っている (操作を呼べる) */
int fuseshim_getattr(const char *path, struct fuseshim_attr *out);
int fuseshim_readdir(const char *path, fuseshim_dirent_fn cb, void *arg);
/* open + read (len に届くまで繰り返す) + release。戻り値は読めたバイト数 (EOF で短くなる) か負の errno。 */
int64_t fuseshim_read(const char *path, uint64_t off, void *buf, size_t len);
int fuseshim_readlink(const char *path, char *buf, size_t size); /* NUL 終端。0 / 負の errno */
int fuseshim_statfs(const char *path, struct fuseshim_statvfs *out);
int fuseshim_getxattr(const char *path, const char *name, char *value, size_t size); /* 長さ / 負の errno */

#ifdef __cplusplus
}
#endif
#endif
