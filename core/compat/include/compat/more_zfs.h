/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * 圧縮ファイルシステム (squashfs / erofs) と、カーネルの展開ライブラリ (lib/{zlib_inflate,lz4,lzo,xz,zstd}) のための追加分。
 * 他のファイルシステムの追加分と重ならないよう、各定義は #ifndef で守る。docs/squashfs-port.md
 */
#ifndef USBMEM_COMPAT_MORE_ZFS_H
#define USBMEM_COMPAT_MORE_ZFS_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- ビット/バイトオーダ */
#ifndef __ffs
static inline unsigned long __ffs(unsigned long w) { return (unsigned long)__builtin_ctzl(w); }
#endif
#ifndef __fls
static inline unsigned long __fls(unsigned long w) { return (unsigned long)(63 - __builtin_clzl(w)); }
#endif
#ifndef swab16
#define swab16(x) __builtin_bswap16(x)
#define swab32(x) __builtin_bswap32(x)
#define swab64(x) __builtin_bswap64(x)
#endif
#ifndef get_unaligned_be16
#define get_unaligned_be16(p) ({ u16 _v; memcpy(&_v, (p), 2); __builtin_bswap16(_v); })
#endif
#ifndef get_unaligned_be32
#define get_unaligned_be32(p) ({ u32 _v; memcpy(&_v, (p), 4); __builtin_bswap32(_v); })
#endif
#ifndef get_unaligned_be64
#define get_unaligned_be64(p) ({ u64 _v; memcpy(&_v, (p), 8); __builtin_bswap64(_v); })
#endif
#ifndef put_unaligned_be16
#define put_unaligned_be16(v, p) do { u16 _v = __builtin_bswap16((u16)(v)); memcpy((p), &_v, 2); } while (0)
#endif
#ifndef put_unaligned_be32
#define put_unaligned_be32(v, p) do { u32 _v = __builtin_bswap32((u32)(v)); memcpy((p), &_v, 4); } while (0)
#endif
#ifndef put_unaligned_be64
#define put_unaligned_be64(v, p) do { u64 _v = __builtin_bswap64((u64)(v)); memcpy((p), &_v, 8); } while (0)
#endif


/* ---- errno / 定数 / マウントログ */
#ifndef ENOTSUPP
#define ENOTSUPP 524
#endif
#ifndef OFFSET_MAX
#define OFFSET_MAX INT64_MAX
#endif
#ifndef SQUASHFS_MAGIC
#define SQUASHFS_MAGIC 0x73717368
#endif
#ifndef EROFS_SUPER_MAGIC_V1
#define EROFS_SUPER_MAGIC_V1 0xE0F5E1E2
#endif
#ifndef errorf
#define errorf(fc, ...) ((void)(fc))   /* マウントのエラーメッセージは捨てる (戻り値の errno で伝える) */
#endif
#ifndef kcalloc
#define kcalloc(n, s, f) calloc((n), (s))
#endif
#define mapping_set_gfp_mask(m, g) ((void)0)
#ifndef generic_file_readonly_mmap_prepare
#define generic_file_readonly_mmap_prepare NULL
#endif


/* ---- NFS エクスポート (export_operations の fid)。使われはしないが、上流の export.c をコンパイルするために型だけ必要 */
#ifndef FILEID_INO32_GEN
#define FILEID_INVALID 0xff
#define FILEID_INO32_GEN 1
#define FILEID_INO32_GEN_PARENT 2
#define FILEID_INO64_GEN 0x81
#define FILEID_INO64_GEN_PARENT 0x82
struct fid {
  union {
    struct { u32 ino; u32 gen; u32 parent_ino; u32 parent_gen; } i32;
    struct { u64 ino; u32 gen; } __packed i64;
    u32 raw[6];
  };
};
#endif

/* ---- 先読み (読み取りは read_folio だけを使うので、先読みは起きない。コンパイルを通すための最小定義) */
#ifndef readahead_expand
static inline void readahead_expand(struct readahead_control *r, loff_t start, size_t len) { (void)r; (void)start; (void)len; }
static inline unsigned int __readahead_batch(struct readahead_control *r, struct page **pages, unsigned max) { (void)r; (void)pages; (void)max; return 0; }
static inline size_t readahead_batch_length(struct readahead_control *r) { (void)r; return 0; }
#endif

/* ---- bio の追加分 (squashfs の block.c / 各展開ラッパが使う)。本体: src/rt_zfs.c */
#ifndef PAGE_SECTORS
#define PAGE_SECTORS (PAGE_SIZE >> 9)
#endif
struct bvec_iter_all { struct bio_vec bv; int idx; };
static inline struct bio_vec *bvec_init_iter_all(struct bvec_iter_all *it) { it->idx = 0; return &it->bv; }
static inline bool bio_next_segment(const struct bio *bio, struct bvec_iter_all *it) {
  if (it->idx >= bio->bi_vcnt) return false;
  it->bv = bio->bi_io_vec[it->idx++];
  return true;
}
static inline void *bvec_virt(const struct bio_vec *bv) { return (char *)bv->bv_page->folio_->data + bv->bv_offset; }
extern struct bio_set fs_bio_set;
struct bio *bio_kmalloc(unsigned short nr_vecs, gfp_t gfp);                 /* kfree() で解放する (bio_put ではない) */
void bio_init_inline(struct bio *bio, struct block_device *bdev, unsigned short max_vecs, blk_opf_t opf);
void __bio_add_page(struct bio *bio, struct page *page, unsigned len, unsigned off);
void bio_free_pages(struct bio *bio);
struct bio *bio_alloc_clone(struct block_device *bdev, struct bio *src, gfp_t gfp, struct bio_set *bs);
void bio_trim(struct bio *bio, sector_t offset, sector_t size);

/* ---- ページキャッシュの追加分 */
int filemap_add_folio(struct address_space *mapping, struct folio *folio, pgoff_t index, gfp_t gfp);
struct page *find_get_page(struct address_space *mapping, pgoff_t index);

/* ---- 待ち行列 (squashfs の cache.c)。単一スレッド前提の互換層なので、実際に待つことはほぼ無い。本体: src/rt_zfs.c */
typedef struct wait_queue_entry wait_queue_entry_t;
typedef int (*wait_queue_func_t)(wait_queue_entry_t *wait, unsigned mode, int flags, void *key);
struct wait_queue_entry { unsigned flags; void *private; wait_queue_func_t func; struct list_head entry; wait_queue_head_t *head_; int woken_; };
#ifndef TASK_NORMAL
#define TASK_NORMAL 3
#define TASK_UNINTERRUPTIBLE 2
#define TASK_KILLABLE 0x102
#endif
void init_wait_func(wait_queue_entry_t *wait, wait_queue_func_t func);
void prepare_to_wait_exclusive(wait_queue_head_t *wq, wait_queue_entry_t *wait, int state);
void finish_wait(wait_queue_head_t *wq, wait_queue_entry_t *wait);
int default_wake_function(wait_queue_entry_t *wait, unsigned mode, int sync, void *key);
void __wake_up(wait_queue_head_t *wq, unsigned mode, int nr_exclusive, void *key);
#ifndef wake_up
#define wake_up(wq) __wake_up((wq), TASK_NORMAL, 1, NULL)
#endif
#ifndef list_del_init_careful
static inline void list_del_init_careful(struct list_head *e) { list_del_init(e); }
#endif

/* kernel.h の put_unaligned* は do { } while (0) なので、式の位置 (return put_unaligned(...)) では使えない。上流の lib/lz4 が式として使うため、文式にする。 */
#undef put_unaligned
#undef put_unaligned_le16
#undef put_unaligned_le32
#undef put_unaligned_le64
#define put_unaligned(v, p) ({ __typeof__(*(p)) _pu = (v); memcpy((p), &_pu, sizeof _pu); (void)0; })
#define put_unaligned_le16(v, p) ({ u16 _pu = (v); memcpy((p), &_pu, 2); (void)0; })
#define put_unaligned_le32(v, p) ({ u32 _pu = (v); memcpy((p), &_pu, 4); (void)0; })
#define put_unaligned_le64(v, p) ({ u64 _pu = (v); memcpy((p), &_pu, 8); (void)0; })

#ifdef __cplusplus
}
#endif

#ifdef USBMEM_FS_EROFS
#include <compat/more_erofs.h>   /* erofs の翻訳単位だけ */
#endif
#endif
