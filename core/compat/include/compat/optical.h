/* SPDX-License-Identifier: GPL-2.0-or-later */
/* 光学メディア系 (fs/isofs, fs/udf) だけが使う互換定義。ビルドで `-include compat/optical.h` として強制的に読み込む。
 * 共有ヘッダを書き換えずに済ませるため、他の FS の追加と衝突しないよう、すべて #ifndef で守ってある。 */
#pragma once
#include <ctype.h>
#include <compat/kernel.h>
#include <linux/fs.h>

#ifndef ISOFS_SUPER_MAGIC
#define ISOFS_SUPER_MAGIC 0x9660
#endif
#ifndef UDF_SUPER_MAGIC
#define UDF_SUPER_MAGIC 0x15013346
#endif
#ifndef S_IRUGO
#define S_IRUGO (S_IRUSR | S_IRGRP | S_IROTH)
#endif
#ifndef S_IXUGO
#define S_IXUGO (S_IXUSR | S_IXGRP | S_IXOTH)
#endif
#ifndef S_IWUGO
#define S_IWUGO (S_IWUSR | S_IWGRP | S_IWOTH)
#endif

/* 読み取り専用の汎用 fops / ページ経由のシンボリックリンク ops (実体は src/rt_vfs_isofs.c。ホスト入口は read_mapping_folio / get_link を直接呼ぶので中身は使われない) */
#define generic_ro_fops usbmem_optical_generic_ro_fops
#define page_symlink_inode_operations usbmem_optical_page_symlink_iops
extern const struct file_operations usbmem_optical_generic_ro_fops;
extern const struct inode_operations usbmem_optical_page_symlink_iops;

/* 共有の put_bh は何もしない (ext4 は get_bh/put_bh を brelse と併用するため)。isofs の rock.c は put_bh だけで bh を手放す (参照カウントなしの互換 bh では解放が必要)。 */
#undef put_bh
#define put_bh(bh) brelse(bh)
#undef get_bh
#define get_bh(bh) do { (bh)->b_count++; } while (0)   /* udf は bh を複数の extent_position で共有する (brelse は最後の 1 つで解放) */

#ifndef get_unaligned_be16
#define get_unaligned_be16(p) ({ u16 _v; memcpy(&_v, (p), 2); __builtin_bswap16(_v); })
#endif
#ifndef get_unaligned_be32
#define get_unaligned_be32(p) ({ u32 _v; memcpy(&_v, (p), 4); __builtin_bswap32(_v); })
#endif
#ifndef get_unaligned_be64
#define get_unaligned_be64(p) ({ u64 _v; memcpy(&_v, (p), 8); __builtin_bswap64(_v); })
#endif
#ifndef QSTR_INIT
#define QSTR_INIT(n, l) { .name = (const unsigned char *)(n), .len = (l) }
#endif
#ifndef QSTR_LEN
#define QSTR_LEN(n, l) ((struct qstr)QSTR_INIT(n, l))
#endif
#ifndef FS_XFLAG_CASENONPRESERVING
#define FS_XFLAG_CASENONPRESERVING 0x40000000u
#endif
#ifndef FILEID_INVALID
#define FILEID_INVALID 0xff
#endif
#ifndef MKDEV
#define MKDEV(ma, mi) ((dev_t)(((ma) << 20) | (mi)))
#endif
#ifndef UTF16_BIG_ENDIAN
#define UTF16_BIG_ENDIAN 1
#endif

/* readdir の "." と ".."。"." は dir 自身の ino、".." は呼び出し側 (ホスト入口) が file->f_pos に入れた親の ino (udf は private_data を自前で使うので別の欄を借りる。readdir 中 f_pos は参照されない)。 */
#ifndef USBMEM_DIR_EMIT_DOT
#define USBMEM_DIR_EMIT_DOT 1
static inline bool dir_emit_dot(struct file *file, struct dir_context *ctx) {
  return dir_emit(ctx, ".", 1, file->f_inode ? file->f_inode->i_ino : 0, DT_DIR);
}
static inline bool dir_emit_dotdot(struct file *file, struct dir_context *ctx) {
  return dir_emit(ctx, "..", 2, (u64)file->f_pos, DT_DIR);
}
#endif

/* udf (balloc.c): リトルエンディアンのビットマップ探索と inode のバイト数 (ブロック数の概算。読み取り専用では参照されない) */
#ifndef find_next_bit_le
static inline unsigned long find_next_bit_le(const void *addr, unsigned long size, unsigned long offset) {
  const unsigned char *p = (const unsigned char *)addr;
  for (unsigned long i = offset; i < size; i++) if (p[i >> 3] & (1u << (i & 7))) return i;
  return size;
}
#endif
#ifndef inode_add_bytes
static inline void inode_add_bytes(struct inode *inode, loff_t bytes) { inode->i_blocks += bytes >> 9; }
static inline void inode_sub_bytes(struct inode *inode, loff_t bytes) { inode->i_blocks -= bytes >> 9; }
#endif

/* ---- fs/udf のための補助 (読み取りでは使われない書き込み側の参照も、コンパイルを通すために宣言だけ要る) */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#ifndef MAX_WCHAR_T
#define MAX_WCHAR_T 0xffff
#endif
#ifndef hex_asc_upper_hi
static const char usbmem_optical_hex_upper[] = "0123456789ABCDEF";
#define hex_asc_upper_hi(x) usbmem_optical_hex_upper[((x) & 0xf0) >> 4]
#define hex_asc_upper_lo(x) usbmem_optical_hex_upper[(x) & 0x0f]
#endif
#ifndef imajor
#define imajor(inode) ((unsigned)((inode)->i_rdev >> 20))
#define iminor(inode) ((unsigned)((inode)->i_rdev & 0xfffff))
#endif
#ifndef pr_notice
#define pr_notice(...) ((void)0)
#endif
#ifndef mutex_destroy
#define mutex_destroy(x) ((void)0)
#endif
#ifndef bh_readahead_batch
#define bh_readahead_batch(n, bhs, op) ((void)0)
#endif
#ifndef errorf
#define errorf(fc, ...) (fprintf(stderr, "udf: " __VA_ARGS__), fputc('\n', stderr))
#endif
#ifndef kvzalloc_flex
#define kvzalloc_flex(v, member, n, ...) kzalloc(sizeof(v) + (size_t)(n) * sizeof(((__typeof__(v) *)0)->member[0]), GFP_KERNEL)
#endif
#ifndef ktime_get_real_ts64
#define ktime_get_real_ts64(ts) ktime_get_coarse_real_ts64(ts)
#endif
#ifndef from_kuid
#define from_kuid(ns, u) ((uid_t)(u).val)
#define from_kgid(ns, g) ((gid_t)(g).val)
#endif
#ifndef overflowuid
#define overflowuid 65534
#define overflowgid 65534
#endif
#ifndef memcpy_from_file_folio
#define memcpy_from_file_folio(to, folio, pos, len) memcpy((to), (char *)(folio)->data + ((pos) & (PAGE_SIZE - 1)), (len))
#endif
#ifndef kstrtoint
static inline int kstrtoint(const char *s, unsigned int base, int *res) {
  /* 上流は kernel の errno を使わない (compat/kernel.h が errno を #undef する) ので、strtol の errno に頼らず自前で桁あふれを見る */
  long long v = 0; int neg = 0;
  if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
  if (base == 0) { base = 10; if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; } else if (s[0] == '0' && s[1]) { base = 8; s++; } }
  if (!*s) return -EINVAL;
  for (; *s; s++) {
    int d = (*s >= '0' && *s <= '9') ? *s - '0' : (*s >= 'a' && *s <= 'f') ? *s - 'a' + 10 : (*s >= 'A' && *s <= 'F') ? *s - 'A' + 10 : -1;
    if (*s == '\n' && !s[1]) break;
    if (d < 0 || (unsigned)d >= base) return -EINVAL;
    v = v * base + d;
    if (v > 2147483648LL) return -ERANGE;
  }
  if (neg) v = -v;
  if (v > 2147483647LL || v < -2147483648LL) return -ERANGE;
  *res = (int)v; return 0;
}
#endif
/* UTF-32 → UTF-8 (nls_base.c の utf32_to_utf8 と同じ: 範囲外・サロゲートは -EINVAL、バッファ不足は -ENAMETOOLONG)。共有側の同名関数と衝突しないよう別名にする。 */
static inline int usbmem_optical_utf32_to_utf8(u32 u, u8 *s, int maxout) {
  int n = u < 0x80 ? 1 : u < 0x800 ? 2 : u < 0x10000 ? 3 : 4;
  if (u > 0x10FFFF || (u >= 0xD800 && u <= 0xDFFF)) return -EINVAL;
  if (maxout < n) return -ENAMETOOLONG;
  if (n == 1) s[0] = (u8)u;
  else if (n == 2) { s[0] = 0xC0 | (u >> 6); s[1] = 0x80 | (u & 0x3F); }
  else if (n == 3) { s[0] = 0xE0 | (u >> 12); s[1] = 0x80 | ((u >> 6) & 0x3F); s[2] = 0x80 | (u & 0x3F); }
  else { s[0] = 0xF0 | (u >> 18); s[1] = 0x80 | ((u >> 12) & 0x3F); s[2] = 0x80 | ((u >> 6) & 0x3F); s[3] = 0x80 | (u & 0x3F); }
  return n;
}
#define utf32_to_utf8 usbmem_optical_utf32_to_utf8

/* exportfs の UDF 用ファイルハンドル (namei.c の fh_to_dentry などは読み取りのホスト入口では使わないが、コンパイルには型が要る) */
#ifndef FILEID_UDF_WITHOUT_PARENT
#define FILEID_UDF_WITHOUT_PARENT 0x51
#define FILEID_UDF_WITH_PARENT 0x52
struct fid {
  union {
    struct { u32 block; u16 partref; u16 parent_partref; u32 generation; u32 parent_block; u32 parent_generation; } udf;
    __u32 raw[0];
  };
};
#endif

/* 書き込み経路 (読み取り専用では呼ばれない) */
#ifndef __mpage_writepages
#define __mpage_writepages(...) (-EROFS)
#define blockdev_direct_IO(...) (-EOPNOTSUPP)
#define iov_iter_rw(i) 0
#define filemap_fdatawrite(m) 0
#endif
