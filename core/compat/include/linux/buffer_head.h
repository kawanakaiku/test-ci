/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * buffer_head 互換。2 つの実装がある (構造体は共通):
 *
 * (1) 既定 (fs/exfat・fat・udf・isofs・ext2 など。実装: src/rt_bh.c)
 *     ブロックデバイスのページキャッシュ (bd_mapping, 4096B 固定 folio) を共有し、struct buffer_head は
 *     「folio 内の [b_data, b_data + b_size) への参照」だけを持つ。同じブロックを複数の bh で読んでも同一メモリを指す (bh 自体は呼び出しごとに別)。
 *     b_count は「追加の参照数」(0 = 参照 1 つ)。
 *
 * (2) USBMEM_KBH (fs/ext4 + fs/jbd2 の翻訳単位だけ。実装: src/rt_kbh.c。docs/ext4-write.md)
 *     カーネルと同じ意味の buffer_head: 同じ (デバイス, ブロック, サイズ) には同じ bh が返る (folio に付いた bh の輪)、
 *     b_count はカーネルと同じ参照数、b_private (jbd2 の journal_head)、b_end_io、submit_bh、create_empty_buffers / folio_buffers など。
 *     jbd2 は bh の同一性と状態ビット (BH_JBD など) に依存するため、(1) の「呼び出しごとに別の bh」では動かない。
 *     他の FS に影響しないよう、関数名はマクロで usbmem_kbh_* に付け替える (同じプロセスに両方の実装が入る)。
 *
 * ブロックサイズは 512..4096 (PAGE_SIZE を超えない) 前提。
 */
#ifndef USBMEM_LINUX_BUFFER_HEAD_H
#define USBMEM_LINUX_BUFFER_HEAD_H
#include <compat/kernel.h>
#include <linux/blkdev.h>
#include <linux/pagemap.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef USBMEM_KBH
/* 上流 include/linux/buffer_head.h と同じ並び (BH_PrivateStart 以降は jbd2.h / ext4.h が使う) */
enum bh_state_bits { BH_Uptodate, BH_Dirty, BH_Lock, BH_Req, BH_Mapped, BH_New, BH_Async_Read, BH_Async_Write, BH_Delay, BH_Boundary,
  BH_Write_EIO, BH_Unwritten, BH_Quiet, BH_Meta, BH_Prio, BH_Defer_Completion, BH_PrivateStart };
#else
enum bh_state_bits { BH_Uptodate, BH_Dirty, BH_Lock, BH_Write_EIO, BH_Mapped, BH_New, BH_Boundary, BH_Delay, BH_Unwritten, BH_Req, BH_Verified, BH_Meta, BH_Prio, BH_Quiet,
  BH_PrivateStart = 16, BH_JBDPrivateStart = 22 /* 上流は jbd2 の状態ビットの後 */ };
#endif

struct buffer_head;
typedef void (bh_end_io_t)(struct buffer_head *bh, int uptodate);
struct buffer_head {
  unsigned long b_state;
  sector_t b_blocknr;      /* b_size 単位のブロック番号 */
  size_t b_size;
  char *b_data;
  struct folio *b_folio;   /* (1): 参照を 1 つ保持 (brelse で folio_put)。(2): bh の輪が folio の参照を 1 つ持つ */
  struct buffer_head *b_this_page;   /* (2): 同じ folio の bh の輪 */
  struct block_device *b_bdev;
  atomic_t b_count;        /* (1): 追加の参照数 (0 = 参照 1 つ。fs/fat と udf が get_bh で共有)。(2): カーネルと同じ参照数 */
  /* ---- (2) USBMEM_KBH だけが使う */
  void *b_private;         /* jbd2: journal_head */
  bh_end_io_t *b_end_io;
  struct list_head b_assoc_buffers;   /* jbd2: ログ用 bh のリスト */
  struct address_space *b_assoc_map;
  struct list_head kbh_dirty_;        /* デバイスの「書き戻し待ち (BH_Dirty)」リスト (bd_mapping 上の bh のみ) */
  unsigned kbh_flags_;                /* KBH_F_* */
};

#ifndef USBMEM_KBH
/* ======================= (1) 既定の実装 (src/rt_bh.c) ======================= */
void mark_buffer_dirty(struct buffer_head *bh);
/* mapping_metadata_bhs (compat/fs.h): ext2/ext4 が inode ごとに持つ「メタデータ bh の追跡」。読み取り専用では何も追跡しない (書き込み側は未対応)。 */
static inline int mmb_sync(struct mapping_metadata_bhs *m) { (void)m; return 0; }
static inline void mmb_invalidate(struct mapping_metadata_bhs *m) { (void)m; }
static inline void mmb_mark_buffer_dirty(struct buffer_head *bh, struct mapping_metadata_bhs *m) { (void)m; mark_buffer_dirty(bh); }

struct buffer_head *__bread(struct block_device *bdev, sector_t block, unsigned size);
struct buffer_head *__getblk(struct block_device *bdev, sector_t block, unsigned size);
void __brelse(struct buffer_head *bh);
void brelse(struct buffer_head *bh);
void bforget(struct buffer_head *bh);
void mark_buffer_dirty(struct buffer_head *bh);
int sync_dirty_buffer(struct buffer_head *bh);

#define sb_bread(sb, block) __bread((sb)->s_bdev, (block), (sb)->s_blocksize)
#define sb_getblk(sb, block) __getblk((sb)->s_bdev, (block), (sb)->s_blocksize)
static inline void __breadahead(struct block_device *bdev, sector_t block, unsigned size) { (void)bdev; (void)block; (void)size; }
#define sb_breadahead(sb, block) __breadahead((sb)->s_bdev, (block), (sb)->s_blocksize)

static inline int buffer_uptodate(const struct buffer_head *bh) { return (bh->b_state >> BH_Uptodate) & 1; }
static inline void set_buffer_uptodate(struct buffer_head *bh) { bh->b_state |= 1UL << BH_Uptodate; }
static inline void clear_buffer_uptodate(struct buffer_head *bh) { bh->b_state &= ~(1UL << BH_Uptodate); }
static inline int buffer_dirty(const struct buffer_head *bh) { return (bh->b_state >> BH_Dirty) & 1; }
static inline int buffer_write_io_error(const struct buffer_head *bh) { return (bh->b_state >> BH_Write_EIO) & 1; }
static inline int buffer_mapped(const struct buffer_head *bh) { return (bh->b_state >> BH_Mapped) & 1; }
static inline int buffer_new(const struct buffer_head *bh) { return (bh->b_state >> BH_New) & 1; }
static inline void set_buffer_new(struct buffer_head *bh) { bh->b_state |= 1UL << BH_New; }
static inline void clear_buffer_new(struct buffer_head *bh) { bh->b_state &= ~(1UL << BH_New); }
static inline void set_buffer_boundary(struct buffer_head *bh) { bh->b_state |= 1UL << BH_Boundary; }
static inline void clear_buffer_write_io_error(struct buffer_head *bh) { bh->b_state &= ~(1UL << BH_Write_EIO); }
/* get_block コールバックが「ファイルのブロック → ディスクのブロック」の対応を返すのに使う (カーネルの map_bh と同じ) */
static inline void map_bh(struct buffer_head *bh, struct super_block *sb, sector_t block) {
  bh->b_state |= 1UL << BH_Mapped; bh->b_bdev = sb->s_bdev; bh->b_blocknr = block; bh->b_size = sb->s_blocksize;
}
/* メタデータ bh は常に uptodate (bd_mapping から読み込み済み) なので何もしない */
static inline int bh_read(struct buffer_head *bh, blk_opf_t flags) { (void)bh; (void)flags; return 0; }
/* 上流の BUFFER_FNS / TAS_BUFFER_FNS: b_state のビット操作の関数群 (ext4 は独自のビット (Verified/Uninit など) を足す) */
#define BUFFER_FNS(bit, name) \
  static inline void set_buffer_##name(struct buffer_head *bh) { bh->b_state |= 1UL << BH_##bit; } \
  static inline void clear_buffer_##name(struct buffer_head *bh) { bh->b_state &= ~(1UL << BH_##bit); } \
  static inline int buffer_##name(const struct buffer_head *bh) { return (bh->b_state >> BH_##bit) & 1; }
#define TAS_BUFFER_FNS(bit, name) \
  static inline int test_set_buffer_##name(struct buffer_head *bh) { int o = (bh->b_state >> BH_##bit) & 1; bh->b_state |= 1UL << BH_##bit; return o; } \
  static inline int test_clear_buffer_##name(struct buffer_head *bh) { int o = (bh->b_state >> BH_##bit) & 1; bh->b_state &= ~(1UL << BH_##bit); return o; }
BUFFER_FNS(Delay, delay)
BUFFER_FNS(Unwritten, unwritten)
BUFFER_FNS(Verified, verified)
BUFFER_FNS(Meta, meta)
BUFFER_FNS(Prio, prio)
static inline void mmb_init(struct mapping_metadata_bhs *m, struct address_space *mapping) { m->mapping = mapping; }
static inline void lock_buffer(struct buffer_head *bh) { bh->b_state |= 1UL << BH_Lock; }   /* 単一スレッドで使う (ext4 の BUG_ON(!buffer_locked) を満たすためのビットだけ) */
static inline void unlock_buffer(struct buffer_head *bh) { bh->b_state &= ~(1UL << BH_Lock); }
static inline int buffer_locked(const struct buffer_head *bh) { return (bh->b_state >> BH_Lock) & 1; }
static inline int trylock_buffer(struct buffer_head *bh) { if (buffer_locked(bh)) return 0; lock_buffer(bh); return 1; }

static inline void clean_bdev_aliases(struct block_device *bdev, sector_t block, sector_t len) { (void)bdev; (void)block; (void)len; }

#else
/* ======================= (2) USBMEM_KBH: カーネルと同じ意味の buffer_head (src/rt_kbh.c) ======================= */
#define BUFFER_FNS(bit, name) \
  static inline void set_buffer_##name(struct buffer_head *bh) { __atomic_fetch_or(&bh->b_state, 1UL << BH_##bit, __ATOMIC_SEQ_CST); } \
  static inline void clear_buffer_##name(struct buffer_head *bh) { __atomic_fetch_and(&bh->b_state, ~(1UL << BH_##bit), __ATOMIC_SEQ_CST); } \
  static inline int buffer_##name(const struct buffer_head *bh) { return (bh->b_state >> BH_##bit) & 1; }
#define TAS_BUFFER_FNS(bit, name) \
  static inline int test_set_buffer_##name(struct buffer_head *bh) { return (__atomic_fetch_or(&bh->b_state, 1UL << BH_##bit, __ATOMIC_SEQ_CST) >> BH_##bit) & 1; } \
  static inline int test_clear_buffer_##name(struct buffer_head *bh) { return (__atomic_fetch_and(&bh->b_state, ~(1UL << BH_##bit), __ATOMIC_SEQ_CST) >> BH_##bit) & 1; }
BUFFER_FNS(Uptodate, uptodate)
BUFFER_FNS(Dirty, dirty)
TAS_BUFFER_FNS(Dirty, dirty)
BUFFER_FNS(Lock, locked)
BUFFER_FNS(Req, req)
TAS_BUFFER_FNS(Req, req)
BUFFER_FNS(Mapped, mapped)
BUFFER_FNS(New, new)
BUFFER_FNS(Async_Read, async_read)
BUFFER_FNS(Async_Write, async_write)
BUFFER_FNS(Delay, delay)
BUFFER_FNS(Boundary, boundary)
BUFFER_FNS(Write_EIO, write_io_error)
BUFFER_FNS(Unwritten, unwritten)
BUFFER_FNS(Meta, meta)
BUFFER_FNS(Prio, prio)
BUFFER_FNS(Defer_Completion, defer_completion)

#define KBH_F_BDEV  1u   /* bd_mapping (メタデータ) の folio に付いた bh */
#define KBH_F_RING  2u   /* folio の輪に属する (alloc_buffer_head の単独 bh は 0) */
#define KBH_F_ONLIST 4u  /* kbh_dirty_ に入っている */

/* 関数名の付け替え: 既定の実装 (rt_bh.c) と同じ名前の関数を別名にする */
#define __bread usbmem_kbh___bread
#define __bread_gfp usbmem_kbh___bread_gfp
#define __getblk usbmem_kbh___getblk
#define __brelse usbmem_kbh___brelse
#define brelse usbmem_kbh_brelse
#define bforget usbmem_kbh_bforget
#define __bforget usbmem_kbh___bforget
#define mark_buffer_dirty usbmem_kbh_mark_buffer_dirty
#define mark_buffer_dirty_inode usbmem_kbh_mark_buffer_dirty_inode
#define sync_dirty_buffer usbmem_kbh_sync_dirty_buffer
#define __sync_dirty_buffer usbmem_kbh___sync_dirty_buffer
#define write_dirty_buffer usbmem_kbh_write_dirty_buffer
#define block_write_end usbmem_kbh_block_write_end
#define block_commit_write usbmem_kbh_block_commit_write
#define block_invalidate_folio usbmem_kbh_block_invalidate_folio
#define block_dirty_folio usbmem_kbh_block_dirty_folio
#define block_truncate_page usbmem_kbh_block_truncate_page
#define sync_blockdev usbmem_kbh_sync_blockdev
#define invalidate_bdev usbmem_kbh_invalidate_bdev
#undef block_read_full_folio
#define block_read_full_folio usbmem_kbh_block_read_full_folio
int block_read_full_folio(struct folio *folio, get_block_t *get_block);

struct buffer_head *__bread_gfp(struct block_device *bdev, sector_t block, unsigned size, gfp_t gfp);
struct buffer_head *__bread(struct block_device *bdev, sector_t block, unsigned size);
struct buffer_head *__getblk(struct block_device *bdev, sector_t block, unsigned size);
struct buffer_head *bdev_getblk(struct block_device *bdev, sector_t block, unsigned size, gfp_t gfp);
struct buffer_head *__find_get_block(struct block_device *bdev, sector_t block, unsigned size);
#define __find_get_block_nonatomic(bdev, block, size) __find_get_block((bdev), (block), (size))
void __brelse(struct buffer_head *bh);
void brelse(struct buffer_head *bh);
void __bforget(struct buffer_head *bh);
void bforget(struct buffer_head *bh);
static inline void get_bh(struct buffer_head *bh) { atomic_inc(&bh->b_count); }
void put_bh(struct buffer_head *bh);
void mark_buffer_dirty(struct buffer_head *bh);
void mark_buffer_dirty_inode(struct buffer_head *bh, struct inode *inode);
void mark_buffer_write_io_error(struct buffer_head *bh);
int __sync_dirty_buffer(struct buffer_head *bh, blk_opf_t op);
int sync_dirty_buffer(struct buffer_head *bh);
void write_dirty_buffer(struct buffer_head *bh, blk_opf_t op);
void bh_submit(struct buffer_head *bh, blk_opf_t opf, bio_end_io_t end_io);
bool bio_endio_bh(struct bio *bio, struct buffer_head **bhp);
void bh_end_read(struct bio *bio);
void bh_end_write(struct bio *bio);
void end_buffer_read_sync(struct buffer_head *bh, int uptodate);
int __bh_read(struct buffer_head *bh, blk_opf_t op_flags, bool wait);
int bh_read(struct buffer_head *bh, blk_opf_t op_flags);
void bh_read_nowait(struct buffer_head *bh, blk_opf_t op_flags);
int bh_uptodate_or_lock(struct buffer_head *bh);
static inline void bh_readahead(struct buffer_head *bh, blk_opf_t op_flags) { (void)bh; (void)op_flags; }
void __lock_buffer(struct buffer_head *bh);
static inline void lock_buffer(struct buffer_head *bh) { __lock_buffer(bh); }
void unlock_buffer(struct buffer_head *bh);
static inline int trylock_buffer(struct buffer_head *bh) { return !test_and_set_bit(BH_Lock, &bh->b_state); }
static inline void wait_on_buffer(struct buffer_head *bh) { (void)bh; }   /* I/O は同期実行 (submit_bh が完了まで行う) */
struct buffer_head *alloc_buffer_head(gfp_t gfp);
void free_buffer_head(struct buffer_head *bh);
void folio_set_bh(struct buffer_head *bh, struct folio *folio, unsigned long offset);
struct buffer_head *folio_buffers(struct folio *folio);
struct buffer_head *create_empty_buffers(struct folio *folio, unsigned long blocksize, unsigned long b_state);
bool try_to_free_buffers(struct folio *folio);
void folio_zero_new_buffers(struct folio *folio, size_t from, size_t to);
int block_write_end(loff_t pos, unsigned len, unsigned copied, struct folio *folio);
void block_commit_write(struct folio *folio, size_t from, size_t to);
void block_invalidate_folio(struct folio *folio, size_t offset, size_t length);
bool block_dirty_folio(struct address_space *mapping, struct folio *folio);
int sync_blockdev(struct block_device *bdev);
void invalidate_bdev(struct block_device *bdev);
static inline unsigned long bh_offset(const struct buffer_head *bh) { return (unsigned long)(bh->b_data - (char *)bh->b_folio->data); }
static inline void touch_buffer(struct buffer_head *bh) { (void)bh; }
static inline void map_bh(struct buffer_head *bh, struct super_block *sb, sector_t block) {
  set_buffer_mapped(bh); bh->b_bdev = sb->s_bdev; bh->b_blocknr = block; bh->b_size = sb->s_blocksize;
}
static inline void clean_bdev_aliases(struct block_device *bdev, sector_t block, sector_t len) { (void)bdev; (void)block; (void)len; }
static inline void clean_bdev_bh_alias(struct buffer_head *bh) { (void)bh; }
static inline struct buffer_head *sb_bread(struct super_block *sb, sector_t block) { return __bread(sb->s_bdev, block, (unsigned)sb->s_blocksize); }
static inline struct buffer_head *sb_bread_unmovable(struct super_block *sb, sector_t block) { return __bread(sb->s_bdev, block, (unsigned)sb->s_blocksize); }
static inline struct buffer_head *sb_getblk(struct super_block *sb, sector_t block) { return __getblk(sb->s_bdev, block, (unsigned)sb->s_blocksize); }
static inline struct buffer_head *sb_find_get_block(struct super_block *sb, sector_t block) { return __find_get_block(sb->s_bdev, block, (unsigned)sb->s_blocksize); }
static inline struct buffer_head *sb_find_get_block_nonatomic(struct super_block *sb, sector_t block) { return __find_get_block(sb->s_bdev, block, (unsigned)sb->s_blocksize); }
static inline void sb_breadahead(struct super_block *sb, sector_t block) { (void)sb; (void)block; }
static inline void sb_breadahead_unmovable(struct super_block *sb, sector_t block) { (void)sb; (void)block; }
static inline void mmb_init(struct mapping_metadata_bhs *m, struct address_space *mapping) { m->mapping = mapping; }
static inline int mmb_sync(struct mapping_metadata_bhs *m) { (void)m; return 0; }
static inline void mmb_invalidate(struct mapping_metadata_bhs *m) { (void)m; }
static inline void mmb_mark_buffer_dirty(struct buffer_head *bh, struct mapping_metadata_bhs *m) { (void)m; mark_buffer_dirty(bh); }
/* ext4 のデータ書き出し (compat 側の実装。docs/ext4-write.md): folio の bh の輪のうち dirty なものを b_blocknr へ書く */
int usbmem_kbh_write_mapping(struct address_space *mapping, loff_t start, loff_t end);
int usbmem_kbh_mapping_has_dirty(struct address_space *mapping);
void usbmem_kbh_release_folio(struct folio *folio);   /* rt_pagecache.c: folio 解放前に bh の輪を外す */
#endif

#ifdef __cplusplus
}
#endif
#endif
