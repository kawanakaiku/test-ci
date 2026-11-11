/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * buffer_head 互換 (fs/exfat など、メタデータを sb_bread() で読む FS 用)。実装: src/rt_bh.c。
 * ブロックデバイスのページキャッシュ (bd_mapping, 4096B 固定 folio) を共有し、struct buffer_head は
 * 「folio 内の [b_data, b_data + b_size) への参照」だけを持つ。同じブロックを複数の bh で読んでも同一メモリを指す。
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

enum bh_state_bits { BH_Uptodate, BH_Dirty, BH_Lock, BH_Write_EIO };

struct buffer_head {
  unsigned long b_state;
  sector_t b_blocknr;      /* b_size 単位のブロック番号 */
  size_t b_size;
  char *b_data;
  struct folio *b_folio;   /* 参照を 1 つ保持 (brelse で folio_put) */
  struct block_device *b_bdev;
};

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
static inline void lock_buffer(struct buffer_head *bh) { (void)bh; }
static inline void unlock_buffer(struct buffer_head *bh) { (void)bh; }

static inline void clean_bdev_aliases(struct block_device *bdev, sector_t block, sector_t len) { (void)bdev; (void)block; (void)len; }

#ifdef __cplusplus
}
#endif
#endif
