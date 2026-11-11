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

enum bh_state_bits { BH_Uptodate, BH_Dirty, BH_Lock, BH_Write_EIO, BH_Mapped, BH_New, BH_Boundary };

struct buffer_head {
  unsigned long b_state;
  sector_t b_blocknr;      /* b_size 単位のブロック番号 */
  size_t b_size;
  char *b_data;
  struct folio *b_folio;   /* 参照を 1 つ保持 (brelse で folio_put) */
  struct block_device *b_bdev;
};

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
static inline void mmb_init(struct mapping_metadata_bhs *m, struct address_space *mapping) { m->mapping = mapping; }
static inline void lock_buffer(struct buffer_head *bh) { (void)bh; }
static inline void unlock_buffer(struct buffer_head *bh) { (void)bh; }

static inline void clean_bdev_aliases(struct block_device *bdev, sector_t block, sector_t len) { (void)bdev; (void)block; (void)len; }

#ifdef __cplusplus
}
#endif
#endif
