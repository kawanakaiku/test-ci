/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * fs/fat の移植用に追加する互換 API (FAT のライブラリだけに -include で入る。他の FS のビルドには影響しない)。
 * 読み取り専用のマウントでは、書き込み系の関数は定義 (リンク) だけできればよく、呼ばれない。
 * 本当に動く必要があるのは: 起動時の初期化、ブート領域の解析、FAT の読み出し、ディレクトリの走査 (短い名前・長い名前)、ファイルの読み出し。
 */
#ifndef USBMEM_COMPAT_FAT_SHIM_H
#define USBMEM_COMPAT_FAT_SHIM_H
#include <compat/kernel.h>   /* 先に互換層の全体を取り込む。下の #undef / #define がそれを上書きする */

/* ---- stat.h */
#ifndef S_IRUGO
#define S_IRUGO (S_IRUSR | S_IRGRP | S_IROTH)
#define S_IWUGO (S_IWUSR | S_IWGRP | S_IWOTH)
#define S_IXUGO (S_IXUSR | S_IXGRP | S_IXOTH)
#endif

/* ---- printk (fat_msg が使う) */
#define printk_index_subsys_emit(...) ((void)0)
#define _printk(...) printk(__VA_ARGS__)

/* ---- 名前バッファ: FAT は PATH_MAX バイトのバッファを wchar_t * にも char * にも代入する */
#undef __getname
#define __getname() ((void *)malloc(4096))

/* ---- hex / ctype */
static inline char *hex_byte_pack(char *buf, u8 byte) {
  static const char a[] = "0123456789abcdef";
  *buf++ = a[byte >> 4]; *buf++ = a[byte & 15];
  return buf;
}
static inline int usbmem_hex_to_bin(unsigned char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  ch |= 0x20;
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  return -1;
}
static inline int hex2bin(u8 *dst, const char *src, size_t count) {
  while (count--) {
    int hi = usbmem_hex_to_bin((unsigned char)*src++), lo = usbmem_hex_to_bin((unsigned char)*src++);
    if (hi < 0 || lo < 0) return -EINVAL;
    *dst++ = (u8)((hi << 4) | lo);
  }
  return 0;
}
static inline int usbmem_isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
#define isalpha usbmem_isalpha

/* ---- ディレクトリ・VFS 補助 */
static inline bool dir_emit_dot(struct file *file, struct dir_context *ctx) { return dir_emit(ctx, ".", 1, file_inode(file)->i_ino, DT_DIR); }
static inline bool dir_emit_dotdot(struct file *file, struct dir_context *ctx) { return dir_emit(ctx, "..", 2, file_inode(file)->i_ino, DT_DIR); }
#define IS_DEADDIR(inode) 0
#define MAX_BUF_PER_PAGE (PAGE_SIZE / 512)
#define SB_NOATIME (1 << 10)
#define MSDOS_SUPER_MAGIC 0x4d44
#define ATTR_ATIME (1 << 4)
#define FS_XFLAG_IMMUTABLE 0x00000008
#define FS_XFLAG_CASENONPRESERVING 0x40000000

/* ---- inode の時刻: カーネルの inode_set_ctime() は設定後の timespec64 を返す (互換層のは void) */
static inline struct timespec64 usbmem_fat_set_ctime(struct inode *i, time64_t s, long ns) { i->i_ctime_.tv_sec = s; i->i_ctime_.tv_nsec = ns; return i->i_ctime_; }
#define inode_set_ctime(i, s, ns) usbmem_fat_set_ctime(i, s, ns)
#define inode_time_dirty_flag(inode) 0

/* ---- fatent.c */
#define DEFINE_SPINLOCK(x) spinlock_t x = { PTHREAD_MUTEX_INITIALIZER }
#define rounddown(x, y) ({ __typeof__(x) _x = (x); _x - (_x % (y)); })
#define need_resched() 0

/* ---- 書き込み・同期・direct IO 系 (読み取り専用では呼ばれない。型が通ればよい) */
#define TASK_UNINTERRUPTIBLE 2
#define set_current_state(s) ((void)0)
#define io_schedule_timeout(t) (0L)
#define compat_ptr_ioctl NULL
#define MODULE_ALIAS_NLS(x)
#define filemap_fdatawait_range(m, s, e) (0)
#define filemap_fdatawrite(m) (0)
#define sync_blockdev_nowait(b) (0)
#define sb_find_get_block_nonatomic(sb, blk) ((struct buffer_head *)NULL)
#define bdev_nr_sectors(b) (bdev_nr_bytes(b) >> 9)
#define scnprintf snprintf
#define current_umask() 022
#define iov_iter_rw(i) 0
#define blockdev_direct_IO(...) (-EINVAL)
int cont_write_begin(const void *iocb, struct address_space *mapping, loff_t pos, unsigned len, struct folio **foliop, void **fsdata, void *get_block, loff_t *bytes);

/* ---- mount オプションの定義 (解析はしない。fs_parse は -ENOPARAM) */
/* fs/fat は get_bh で bh の参照を増やし、brelse を複数回呼ぶ (fat_free_clusters など)。rt_bh.c の b_refs で数える */
#undef get_bh
#undef put_bh
#define get_bh(bh) do { (bh)->b_refs++; } while (0)
#define put_bh(bh) brelse(bh)
#define fsparam_bool(n, o) { n, 0, o }
#define fs_param_is_enum 0
#define fs_param_is_string 0

#endif
