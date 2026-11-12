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

/* readdir の "." と ".."。"." は dir 自身の ino、".." は呼び出し側 (ホスト入口) が file->private_data に入れた親の ino。 */
#ifndef USBMEM_DIR_EMIT_DOT
#define USBMEM_DIR_EMIT_DOT 1
static inline bool dir_emit_dot(struct file *file, struct dir_context *ctx) {
  return dir_emit(ctx, ".", 1, file->f_inode ? file->f_inode->i_ino : 0, DT_DIR);
}
static inline bool dir_emit_dotdot(struct file *file, struct dir_context *ctx) {
  return dir_emit(ctx, "..", 2, (u64)(uintptr_t)file->private_data, DT_DIR);
}
#endif
