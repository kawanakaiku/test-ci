/* SPDX-License-Identifier: GPL-2.0-or-later */
/* fs.h / vfs_api.h に入らなかった補助定義 (ops 構造体、型付き alloc マクロなど)。 */
#ifndef USBMEM_COMPAT_MISC_H
#define USBMEM_COMPAT_MISC_H
#include <compat/vfs_api.h>
#include <compat/ops.h>

#ifdef __cplusplus
extern "C" {
#endif
struct file_system_type {
  const char *name; unsigned fs_flags; void *init_fs_context; const struct fs_parameter_spec *parameters;
  void (*kill_sb)(struct super_block *); void *owner;
};
struct fstrim_range { u64 start, len, minlen; };
struct vm_fault { struct vm_area_struct *vma; pgoff_t pgoff; struct page *page; };
struct vm_area_desc { struct file *file; unsigned long flags, start, end; pgoff_t pgoff; const struct vm_operations_struct *vm_ops; };
struct readahead_control { struct file *file; struct address_space *mapping; };
struct fs_parameter_spec { const char *name; int type; int opt; unsigned flags; const void *data; };
struct fid; struct pipe_inode_info;

#define DEFINE_MUTEX(name) struct mutex name = { PTHREAD_MUTEX_INITIALIZER }
/* 型付き alloc (新カーネル API): 第 1 引数は型または *ptr 式 */
#define kmalloc_obj(v, ...) ((__typeof__(v) *)usbmem_alloc(sizeof(v), 0))
#define kzalloc_obj(v, ...) ((__typeof__(v) *)usbmem_alloc(sizeof(v), __GFP_ZERO))
#define kvzalloc_obj(v, ...) ((__typeof__(v) *)usbmem_alloc(sizeof(v), __GFP_ZERO))
#define kmalloc_objs(v, n, ...) ((__typeof__(v) *)usbmem_alloc_array((size_t)(n), sizeof(v), 0))
#define kzalloc_objs(v, n, ...) ((__typeof__(v) *)usbmem_alloc_array((size_t)(n), sizeof(v), __GFP_ZERO))
#define kvmalloc_objs(v, n, ...) ((__typeof__(v) *)usbmem_alloc_array((size_t)(n), sizeof(v), 0))
#define kvzalloc_objs(v, n, ...) ((__typeof__(v) *)usbmem_alloc_array((size_t)(n), sizeof(v), __GFP_ZERO))
/* __free(fn): スコープ終了時に解放 (cleanup 属性) */
#define __free(fn) __attribute__((cleanup(fn##_cleanup)))
static inline void kfree_cleanup(void *p) { free(*(void **)p); }
static inline void kvfree_cleanup(void *p) { free(*(void **)p); }

/* inode 状態ヘルパ */
#define IS_RDONLY(i) sb_rdonly((i)->i_sb)
#define IS_APPEND(i) ((i)->i_flags & S_APPEND)
#define IS_IMMUTABLE(i) ((i)->i_flags & S_IMMUTABLE)
#define current_user_ns() ((struct user_namespace *)0)
void truncate_setsize(struct inode *, loff_t);
struct folio *filemap_lock_folio(struct address_space *, pgoff_t);
void iomap_finish_folio_read(struct folio *, size_t off, size_t len, int err);
struct folio *writeback_iter(struct address_space *, struct writeback_control *, struct folio *, int *);
#define O_NOATIME 01000000
#define MODULE_AUTHOR(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_LICENSE(x)
#define MODULE_ALIAS(x)
#define MODULE_ALIAS_FS(x)
#ifndef USBMEM_MODULE_INIT_NAME
#define USBMEM_MODULE_INIT_NAME usbmem_module_init_ntfs   /* FS ごとに CMake が -D で指定する (exfat: usbmem_module_init_exfat) */
#endif
#ifdef USBMEM_JBD2_MODULE   /* fs/jbd2 (ext4 と同じライブラリ): 別名 */
#define module_init(f) int usbmem_module_init_jbd2(void) { return f(); }
#else
#define module_init(f) int USBMEM_MODULE_INIT_NAME(void) { return f(); }
#endif
#define module_exit(f)
#ifndef ENOPARAM
#define ENOPARAM 519
#endif

/* iattr */
#define ATTR_MODE (1 << 0)
#define ATTR_UID (1 << 1)
#define ATTR_GID (1 << 2)
#define ATTR_SIZE (1 << 3)
#define ATTR_MTIME (1 << 5)
#define ATTR_CTIME (1 << 6)
#define ATTR_KILL_SUID (1 << 11)
#define ATTR_KILL_SGID (1 << 12)
#define ATTR_FILE (1 << 13)

/* fs_parser (mount option 定義用。値の意味は super.c 側) */
#define fsparam_flag(n, o) { n, 0, o }
#define fsparam_u32(n, o) { n, 0, o }
#define fsparam_u32oct(n, o) { n, 0, o }
#define fsparam_s32(n, o) { n, 0, o }
#define fsparam_u64(n, o) { n, 0, o }
#define fsparam_string(n, o) { n, 0, o }
#define fsparam_enum(n, o, t) { n, 0, o }
#ifdef __cplusplus
}
#endif
#endif
