// SPDX-License-Identifier: GPL-2.0-or-later
// カーネルと同じ意味の buffer_head (USBMEM_KBH: fs/ext4 + fs/jbd2 だけが使う)。docs/ext4-write.md。
//
// 既定の bh 実装 (rt_bh.c) は「呼び出しごとに別の bh」だが、jbd2 は bh の同一性 (b_private = journal_head、BH_JBD 等の状態ビット、
// b_count の参照数) に依存するので、ここではカーネルと同じく folio に bh の「輪」を付け、同じブロックには同じ bh を返す。
//
//  - 輪は folio->private に付き (FOLIO_KBH_RING)、folio の参照を 1 つ持つ。
//  - bd_mapping (メタデータ) の輪は、全部の bh が「遊んでいる」(参照 0・dirty でない・ロックされていない・journal_head が付いていない)
//    状態になったら外す (状態ビットは捨てる。BH_Verified などは再検査になるだけ)。folio は通常のページキャッシュとして残り、退避もできる。
//  - ファイルデータの folio の輪は ext4 の write_begin (create_empty_buffers) が付け、データの書き出し (usbmem_kbh_write_mapping) か
//    truncate (usbmem_kbh_release_folio) で外す。
//  - I/O は同期実行 (submit_bh は完了まで行って b_end_io を呼ぶ)。REQ_PREFLUSH は書く前の flush、REQ_FUA は書いた後の flush。
//    jbd2 のコミット順序 (ジャーナルのブロック → flush → コミットブロック (PREFLUSH|FUA)) はこれで守られる。
//  - BH_Dirty の bd_mapping の bh を書くのは、jbd2 のチェックポイント (write_dirty_buffer) と sync_blockdev (再生後) だけ。
//    folio の 512B dirty ビット (rt_pagecache.c の usbmem_bdev_writeback 用) は bd_mapping では立てない。
#include <stdio.h>
#include <stdlib.h>
#include <compat/kernel.h>
#include <compat/usbmem_ntfs.h>
#include <linux/buffer_head.h>

static int ring_is_bdev(const struct buffer_head *head) { return (head->kbh_flags_ & KBH_F_BDEV) != 0; }

static int bh_idle(const struct buffer_head *bh) {
  return atomic_read(&bh->b_count) == 0 && !bh->b_private &&
         !(bh->b_state & ((1UL << BH_Dirty) | (1UL << BH_Lock) | (1UL << BH_Async_Read) | (1UL << BH_Async_Write) | (1UL << BH_PrivateStart /* BH_JBD */)));
}
static int ring_idle(struct buffer_head *head) {
  struct buffer_head *bh = head;
  do { if (!bh_idle(bh)) return 0; bh = bh->b_this_page; } while (bh != head);
  return 1;
}
/* 輪を folio から外して解放する (呼び出し側が「外してよい」ことを確認済み) */
static void ring_free(struct folio *f) {
  struct buffer_head *head = f->private;
  if (!head || !((f->flags >> FOLIO_KBH_RING) & 1)) return;
  f->private = NULL;
  f->flags &= ~(1UL << FOLIO_KBH_RING);
  struct buffer_head *bh = head;
  do { struct buffer_head *n = bh->b_this_page; free(bh); bh = n; } while (bh != head);
  folio_put(f);   /* 輪の参照 */
}
static void maybe_free_bdev_ring(struct buffer_head *bh) {
  if (!(bh->kbh_flags_ & KBH_F_RING) || !(bh->kbh_flags_ & KBH_F_BDEV)) return;
  struct folio *f = bh->b_folio;
  if (f && f->private && ring_idle(f->private)) ring_free(f);
}

struct buffer_head *folio_buffers(struct folio *folio) {
  return ((folio->flags >> FOLIO_KBH_RING) & 1) ? (struct buffer_head *)folio->private : NULL;
}

static struct buffer_head *ring_create(struct folio *f, unsigned long size, unsigned long b_state, int bdev_ring) {
  unsigned n = (unsigned)(PAGE_SIZE / size);
  struct buffer_head *head = NULL, *tail = NULL;
  for (unsigned i = 0; i < n; i++) {
    struct buffer_head *bh = calloc(1, sizeof *bh);
    if (!bh) { /* 作りかけを捨てる */
      while (head) { struct buffer_head *x = head->b_this_page; free(head); head = x; }
      return NULL;
    }
    bh->b_size = size; bh->b_folio = f; bh->b_data = (char *)f->data + (size_t)i * size;
    bh->b_state = b_state;
    INIT_LIST_HEAD(&bh->b_assoc_buffers);
    bh->kbh_flags_ = KBH_F_RING | (bdev_ring ? KBH_F_BDEV : 0);
    if (!head) head = bh; else tail->b_this_page = bh;
    tail = bh;
  }
  tail->b_this_page = head;
  folio_get(f);
  f->private = head;
  f->flags |= 1UL << FOLIO_KBH_RING;
  return head;
}

/* ファイルデータの folio に bh の輪を付ける (カーネルの create_empty_buffers と同じ: 未マップ・b_state を初期値に) */
struct buffer_head *create_empty_buffers(struct folio *folio, unsigned long blocksize, unsigned long b_state) {
  struct buffer_head *head = folio_buffers(folio);
  if (head) return head;
  if (folio_test_uptodate(folio)) b_state |= 1UL << BH_Uptodate;
  head = ring_create(folio, blocksize, b_state, folio->mapping && folio->mapping->host_bdev_);
  if (!head) usbmem_printk("usbmem kbh: create_empty_buffers: out of memory\n");
  return head;
}

/* bd_mapping の folio に (size の) 輪を用意する。サイズ違いの輪が遊んでいれば作り直す (カーネルの set_blocksize が bdev のキャッシュを捨てるのと同じ) */
static struct buffer_head *bdev_ring_get(struct block_device *bdev, struct folio *f, unsigned size) {
  struct buffer_head *head = folio_buffers(f);
  if (head && head->b_size != size) {
    if (!ring_idle(head)) { usbmem_printk("usbmem kbh: block size change %zu -> %u on a busy buffer\n", head->b_size, size); return NULL; }
    ring_free(f);
    head = NULL;
  }
  if (!head) {
    head = ring_create(f, size, (1UL << BH_Mapped) | (folio_test_uptodate(f) ? (1UL << BH_Uptodate) : 0), 1);
    if (!head) return NULL;
    sector_t first = (sector_t)(((u64)f->index << PAGE_SHIFT) / size);
    struct buffer_head *bh = head; sector_t b = first;
    do { bh->b_blocknr = b++; bh->b_bdev = bdev; bh = bh->b_this_page; } while (bh != head);
  }
  return head;
}
static struct buffer_head *ring_nth(struct buffer_head *head, unsigned i) { while (i--) head = head->b_this_page; return head; }

static int blk_args_ok(struct block_device *bdev, sector_t block, unsigned size) {
  if (!bdev || !bdev->bd_mapping || size < 512 || size > PAGE_SIZE || (size & (size - 1))) return 0;
  u64 off = (u64)block * size;
  if (off / size != (u64)block || off + size > usbmem_bdev_size(bdev->impl)) return 0;
  return 1;
}

/* 既にキャッシュ (輪) にあれば参照を付けて返す。無ければ NULL (カーネルの __find_get_block)。 */
struct buffer_head *__find_get_block(struct block_device *bdev, sector_t block, unsigned size) {
  if (!blk_args_ok(bdev, block, size)) return NULL;
  u64 off = (u64)block * size;
  struct folio *f = __filemap_get_folio(bdev->bd_mapping, (pgoff_t)(off >> PAGE_SHIFT), 0, 0);
  if (IS_ERR(f)) return NULL;
  struct buffer_head *head = folio_buffers(f), *bh = NULL;
  if (head && head->b_size == size) { bh = ring_nth(head, (unsigned)((off & (PAGE_SIZE - 1)) / size)); get_bh(bh); }
  folio_put(f);
  return bh;
}

/* カーネルの __getblk: 読み込みはしない契約だが、ここでは輪を新しく作るとき folio 全体を読み込んでおく (uptodate になる。
 * 輪があるときは読まない: 輪の中に dirty な bh があれば、folio を読み直すとその内容を壊すため)。読めなかったら uptodate でない bh を返す。 */
struct buffer_head *bdev_getblk(struct block_device *bdev, sector_t block, unsigned size, gfp_t gfp) {
  (void)gfp;
  if (!blk_args_ok(bdev, block, size)) return NULL;
  u64 off = (u64)block * size;
  pgoff_t idx = (pgoff_t)(off >> PAGE_SHIFT);
  struct folio *f = __filemap_get_folio(bdev->bd_mapping, idx, 0, 0);
  if (!IS_ERR(f) && !folio_buffers(f) && !folio_test_uptodate(f)) { folio_put(f); f = ERR_PTR(-ENOENT); }
  if (IS_ERR(f)) {
    f = read_mapping_folio(bdev->bd_mapping, idx, NULL);
    if (IS_ERR(f)) f = __filemap_get_folio(bdev->bd_mapping, idx, FGP_CREAT, 0);   /* 読めない: 未 uptodate の bh を返す (読み込み側が -EIO にする) */
    if (IS_ERR(f)) return NULL;
  }
  struct buffer_head *head = bdev_ring_get(bdev, f, size), *bh = NULL;
  if (head) { bh = ring_nth(head, (unsigned)((off & (PAGE_SIZE - 1)) / size)); get_bh(bh); }
  folio_put(f);
  return bh;
}
struct buffer_head *__getblk(struct block_device *bdev, sector_t block, unsigned size) { return bdev_getblk(bdev, block, size, 0); }
struct buffer_head *__bread_gfp(struct block_device *bdev, sector_t block, unsigned size, gfp_t gfp) {
  struct buffer_head *bh = bdev_getblk(bdev, block, size, gfp);
  if (bh && bh_read(bh, 0) < 0) { brelse(bh); return NULL; }
  return bh;
}
struct buffer_head *__bread(struct block_device *bdev, sector_t block, unsigned size) { return __bread_gfp(bdev, block, size, 0); }

void put_bh(struct buffer_head *bh) {
  if (atomic_dec_return(&bh->b_count) < 0) { usbmem_printk("usbmem kbh: put_bh underflow (block %llu)\n", (unsigned long long)bh->b_blocknr); atomic_set(&bh->b_count, 0); }
  if (atomic_read(&bh->b_count) == 0) maybe_free_bdev_ring(bh);
}
void __brelse(struct buffer_head *bh) {
  if (!bh) return;
  if (atomic_read(&bh->b_count) <= 0) { usbmem_printk("usbmem kbh: brelse of free buffer (block %llu)\n", (unsigned long long)bh->b_blocknr); return; }
  put_bh(bh);
}
void brelse(struct buffer_head *bh) { __brelse(bh); }
/* bforget: dirty を捨てて手放す (解放したメタデータブロックの古い内容を後で書かない) */
void __bforget(struct buffer_head *bh) {
  if (!bh) return;
  clear_buffer_dirty(bh);
  __brelse(bh);
}
void bforget(struct buffer_head *bh) { __bforget(bh); }

/* bd_mapping の bh: BH_Dirty だけ (書くのはチェックポイント / sync_blockdev)。ファイルデータの bh: folio の 512B dirty ビットも立てる */
void mark_buffer_dirty(struct buffer_head *bh) {
  if (test_set_buffer_dirty(bh)) return;
  if ((bh->kbh_flags_ & KBH_F_RING) && !(bh->kbh_flags_ & KBH_F_BDEV) && bh->b_folio) {
    struct folio *f = bh->b_folio;
    usbmem_folio_dirty_range(f, bh_offset(bh), bh->b_size);
    if (f->mapping && f->mapping->host) f->mapping->host->i_state |= I_DIRTY_PAGES;
  }
}
void mark_buffer_dirty_inode(struct buffer_head *bh, struct inode *inode) { (void)inode; mark_buffer_dirty(bh); }
void mark_buffer_write_io_error(struct buffer_head *bh) { set_buffer_write_io_error(bh); }

void __lock_buffer(struct buffer_head *bh) {
  if (test_and_set_bit(BH_Lock, &bh->b_state)) {
    /* 単一スレッド: 既にロックされている = 呼び出し元の上位がロックしたまま (再入)。待っても解けない */
    usbmem_printk("usbmem kbh: lock_buffer on a locked buffer (block %llu)\n", (unsigned long long)bh->b_blocknr);
    abort();
  }
}
void unlock_buffer(struct buffer_head *bh) { clear_bit(BH_Lock, &bh->b_state); }

/* 同期 I/O (上流 fs/buffer.c の __bh_submit と同じ契約: bio を作って end_io(bio) を呼ぶ。end_io は bio_endio_bh で bh を取り出す)。
 * bh->b_data をそのまま使う (jbd2 の影 bh は b_folio を持たない)。REQ_PREFLUSH = 書く前に flush、REQ_FUA = 書いた後に flush。 */
void bh_submit(struct buffer_head *bh, blk_opf_t opf, bio_end_io_t end_io) {
  struct usbmem_bdev *d = bh->b_bdev ? bh->b_bdev->impl : NULL;
  int rc = d ? 0 : -EIO;
  int wr = (opf & REQ_OP_MASK) == REQ_OP_WRITE;
  set_buffer_req(bh);
  if (wr && bh->b_bdev && bh->b_bdev->usbmem_ro_) { usbmem_printk("usbmem kbh: write to a read-only mount refused (block %llu)\n", (unsigned long long)bh->b_blocknr); rc = -EROFS; }
#ifdef USBMEM_MUTATE_NO_FLUSH   /* 突然変異テスト: ジャーナルのコミット順序の flush を外す (検出されるべき) */
  opf &= ~(blk_opf_t)(REQ_PREFLUSH | REQ_FUA);
#endif
  if (!rc && wr && (opf & REQ_PREFLUSH)) rc = usbmem_bdev_flush(d);
  if (!rc) {
    u64 off = (u64)bh->b_blocknr * bh->b_size;
#ifdef USBMEM_MUTATE_NO_SB_WRITE   /* 突然変異テスト: スーパーブロック (needs_recovery の印) を書かない */
    if (wr && off <= 1024 && off + bh->b_size > 1024) { rc = 0; goto io_done; }
#endif
    rc = wr ? usbmem_bdev_write(d, off, bh->b_data, bh->b_size) : usbmem_bdev_read(d, off, bh->b_data, bh->b_size);
  }
  if (!rc && wr && (opf & REQ_FUA)) rc = usbmem_bdev_flush(d);
#ifdef USBMEM_MUTATE_NO_SB_WRITE
io_done:
#endif
  struct bio *bio = bio_alloc(bh->b_bdev, 1, opf, 0);
  if (!bio) { usbmem_printk("usbmem kbh: bio_alloc failed\n"); abort(); }
  bio->bi_private = bh;
  bio->bi_status = rc ? 10 /* BLK_STS_IOERR */ : 0;
  bio->bi_end_io = end_io;
  end_io(bio);
}
bool bio_endio_bh(struct bio *bio, struct buffer_head **bhp) {
  bool ok = bio->bi_status == 0;
  *bhp = bio->bi_private;
  bio_put(bio);
  return ok;
}
void end_buffer_read_sync(struct buffer_head *bh, int uptodate) {
  if (uptodate) set_buffer_uptodate(bh); else clear_buffer_uptodate(bh);
  unlock_buffer(bh);
}
void bh_end_read(struct bio *bio) { struct buffer_head *bh; bool ok = bio_endio_bh(bio, &bh); end_buffer_read_sync(bh, ok); }
void bh_end_write(struct bio *bio) {
  struct buffer_head *bh; bool ok = bio_endio_bh(bio, &bh);
  if (ok) clear_buffer_write_io_error(bh); else mark_buffer_write_io_error(bh);
  unlock_buffer(bh);
}
int bh_uptodate_or_lock(struct buffer_head *bh) {
  if (!buffer_uptodate(bh)) {
    lock_buffer(bh);
    if (!buffer_uptodate(bh)) return 0;
    unlock_buffer(bh);
  }
  return 1;
}
int __bh_read(struct buffer_head *bh, blk_opf_t op_flags, bool wait) {
  bh_submit(bh, REQ_OP_READ | op_flags, bh_end_read);
  if (wait && !buffer_uptodate(bh)) return -EIO;
  return 0;
}
/* 戻り値: 1 = 既に uptodate / 0 = 読んだ / -EIO */
int bh_read(struct buffer_head *bh, blk_opf_t op_flags) {
  if (bh_uptodate_or_lock(bh)) return 1;
  return __bh_read(bh, op_flags, true);
}
void bh_read_nowait(struct buffer_head *bh, blk_opf_t op_flags) {
  if (!bh_uptodate_or_lock(bh)) __bh_read(bh, op_flags, false);
}
void write_dirty_buffer(struct buffer_head *bh, blk_opf_t op_flags) {
  lock_buffer(bh);
  if (!test_clear_buffer_dirty(bh)) { unlock_buffer(bh); return; }
  bh_submit(bh, REQ_OP_WRITE | op_flags, bh_end_write);
}
int __sync_dirty_buffer(struct buffer_head *bh, blk_opf_t op_flags) {
  lock_buffer(bh);
  if (test_clear_buffer_dirty(bh)) {
    if (!buffer_mapped(bh)) { unlock_buffer(bh); return -EIO; }
    bh_submit(bh, REQ_OP_WRITE | op_flags, bh_end_write);
    if (buffer_write_io_error(bh)) return -EIO;
  } else unlock_buffer(bh);
  return 0;
}
int sync_dirty_buffer(struct buffer_head *bh) { return __sync_dirty_buffer(bh, REQ_SYNC); }

struct buffer_head *alloc_buffer_head(gfp_t gfp) {
  (void)gfp;
  struct buffer_head *bh = calloc(1, sizeof *bh);
  if (bh) INIT_LIST_HEAD(&bh->b_assoc_buffers);
  return bh;
}
void free_buffer_head(struct buffer_head *bh) {
  if (!bh) return;
  if (bh->kbh_flags_ & KBH_F_RING) { usbmem_printk("usbmem kbh: free_buffer_head on a ring buffer\n"); return; }
  free(bh);
}
void folio_set_bh(struct buffer_head *bh, struct folio *folio, unsigned long offset) {
  bh->b_folio = folio;
  bh->b_data = (char *)folio->data + offset;
}

/* カーネルの try_to_free_buffers: 全部遊んでいれば輪を外す */
bool try_to_free_buffers(struct folio *folio) {
  struct buffer_head *head = folio_buffers(folio);
  if (!head) return true;
  if (!ring_idle(head)) return false;
  ring_free(folio);
  return true;
}
/* truncate でキャッシュから外す folio (rt_pagecache.c の truncate_inode_pages)。dirty でも捨てる (カーネルの block_invalidate_folio と同じ)。
 * journal_head が付いた bh (data=journal のファイル) はここに来ない (書き込みで拒否する)。参照が残っていれば輪は外さない (漏れても壊さない)。 */
void usbmem_kbh_release_folio(struct folio *f) {
  struct buffer_head *head = folio_buffers(f);
  if (!head) return;
  struct buffer_head *bh = head;
  do { clear_buffer_dirty(bh); clear_buffer_mapped(bh); bh = bh->b_this_page; } while (bh != head);
  if (ring_idle(head)) ring_free(f);
  else usbmem_printk("usbmem kbh: truncating a folio whose buffers are still in use\n");
}
void block_invalidate_folio(struct folio *folio, size_t offset, size_t length) {
  struct buffer_head *head = folio_buffers(folio);
  if (!head) return;
  size_t stop = offset + length, cur = 0;
  struct buffer_head *bh = head;
  do {
    size_t next = cur + bh->b_size;
    if (cur >= offset && next <= stop) { clear_buffer_dirty(bh); clear_buffer_uptodate(bh); clear_buffer_mapped(bh); clear_buffer_new(bh); clear_buffer_req(bh); clear_buffer_delay(bh); clear_buffer_unwritten(bh); }
    cur = next; bh = bh->b_this_page;
  } while (bh != head);
  if (offset == 0 && length >= PAGE_SIZE) try_to_free_buffers(folio);
}
bool block_dirty_folio(struct address_space *mapping, struct folio *folio) {
  struct buffer_head *head = folio_buffers(folio);
  if (head) { struct buffer_head *bh = head; do { mark_buffer_dirty(bh); bh = bh->b_this_page; } while (bh != head); }
  bool was = (folio->flags >> FOLIO_DIRTY) & 1;
  folio->flags |= 1UL << FOLIO_DIRTY;
  if (!head) folio->dirty_blocks_ = 0xFF;
  if (mapping && mapping->host) mapping->host->i_state |= I_DIRTY_PAGES;
  return !was;
}

/* カーネルの folio_zero_new_buffers: 書き込みに失敗したとき、新規確保ブロックのうち [from, to) と重なる部分をゼロにして dirty に */
void folio_zero_new_buffers(struct folio *folio, size_t from, size_t to) {
  struct buffer_head *head = folio_buffers(folio);
  if (!head) return;
  size_t block_start = 0;
  struct buffer_head *bh = head;
  do {
    size_t block_end = block_start + bh->b_size;
    if (buffer_new(bh)) {
      if (block_end > from && block_start < to) {
        if (!folio_test_uptodate(folio)) {
          size_t s = max(from, block_start), e = min(to, block_end);
          memset((char *)folio->data + s, 0, e - s);
          set_buffer_uptodate(bh);
        }
        clear_buffer_new(bh);
        mark_buffer_dirty(bh);
      }
    }
    block_start = block_end; bh = bh->b_this_page;
  } while (bh != head);
}
static void block_commit_range(struct folio *folio, size_t from, size_t to) {
  struct buffer_head *head = folio_buffers(folio);
  if (!head) {   /* 輪が無い (ext4 では起きないはず): 既定の範囲 dirty */
    if (to > from) usbmem_folio_dirty_range(folio, from, to - from);
    return;
  }
  int partial = 0;
  size_t block_start = 0;
  struct buffer_head *bh = head;
  do {
    size_t block_end = block_start + bh->b_size;
    if (block_end <= from || block_start >= to) { if (!buffer_uptodate(bh)) partial = 1; }
    else { set_buffer_uptodate(bh); mark_buffer_dirty(bh); }
    if (buffer_new(bh)) clear_buffer_new(bh);
    block_start = block_end; bh = bh->b_this_page;
  } while (bh != head);
  if (!partial) folio_mark_uptodate(folio);
}
int block_write_end(loff_t pos, unsigned len, unsigned copied, struct folio *folio) {
  size_t start = (size_t)(pos - folio_pos(folio));
  if (copied < len) {
    if (!folio_test_uptodate(folio)) copied = 0;
    folio_zero_new_buffers(folio, start + copied, start + len);
  }
  block_commit_range(folio, start, start + copied);
  return (int)copied;
}
void block_commit_write(struct folio *folio, size_t from, size_t to) { block_commit_range(folio, from, to); }

/* カーネルの block_read_full_folio: uptodate でない bh だけを get_block で写して読む (dirty な bh を読み直して壊さない) */
int block_read_full_folio(struct folio *folio, get_block_t *get_block) {
  struct inode *inode = folio->mapping->host;
  unsigned bs = 1u << inode->i_blkbits;
  struct buffer_head *head = folio_buffers(folio);
  if (!head) head = create_empty_buffers(folio, bs, 0);
  if (!head) { folio_unlock(folio); return -ENOMEM; }
  sector_t iblock = (sector_t)folio->index << (PAGE_SHIFT - inode->i_blkbits);
  loff_t isize = i_size_read(inode);
  int err = 0, fully = 1;
  struct buffer_head *bh = head;
  size_t off = 0;
  do {
    if (!buffer_uptodate(bh)) {
      loff_t fpos = folio_pos(folio) + (loff_t)off;
      if (!buffer_mapped(bh) && fpos < isize) {
        bh->b_size = bs;
        int e = get_block(inode, iblock, bh, 0);
        if (e) { err = e; fully = 0; goto next; }
        bh->b_size = bs;
      }
      if (!buffer_mapped(bh) || buffer_unwritten(bh) || fpos >= isize) {
        memset(bh->b_data, 0, bs);
        if (fpos >= isize || !buffer_mapped(bh) || buffer_unwritten(bh)) set_buffer_uptodate(bh);
      } else {
          lock_buffer(bh);
        if (__bh_read(bh, 0, true)) { err = -EIO; fully = 0; }
      }
    }
  next:
    off += bs; iblock++; bh = bh->b_this_page;
  } while (bh != head);
  if (fully && !err) folio_mark_uptodate(folio);
  folio_unlock(folio);
  return err;
}

/* ---- ファイルデータの書き出し (ext4: ordered モードのデータ。jbd2 のコミット前と、ホスト API の書き込み直後に呼ぶ)
 * dirty な folio の輪のうち BH_Dirty の bh を b_blocknr へ書く。未マップ・遅延確保・未初期化 (unwritten) の bh に dirty なデータが載っていれば、
 * 書き先が確定していない = 書けないので -EIO (dirty のまま残す)。flush は呼び出し側 (jbd2 はコミットブロックの PREFLUSH)。 */
int usbmem_kbh_mapping_has_dirty(struct address_space *m) { return usbmem_mapping_has_dirty(m); }
int usbmem_kbh_write_mapping(struct address_space *m, loff_t start, loff_t end) {
  struct folio **v; size_t n;
  int err = usbmem_mapping_snapshot(m, 1, &v, &n);
  if (err) return err;
  for (size_t k = 0; k < n; k++) {
    struct folio *f = v[k];
    loff_t fpos = folio_pos(f);
    if (fpos + (loff_t)PAGE_SIZE <= start || (end >= 0 && fpos > end)) { folio_put(f); continue; }
    int locked = folio_trylock(f);   /* 失敗 = 同じスレッドの上位がロック中 (コミットが待ちの中で走った)。同期実行なので中身は安定している */
    struct buffer_head *head = folio_buffers(f);
    int ferr = 0;
    if (!head) {
      if (f->dirty_blocks_) { usbmem_printk("usbmem kbh: dirty data folio without buffers (ino %lu idx %lu)\n", m->host ? m->host->i_ino : 0, (unsigned long)f->index); ferr = -EIO; }
    } else {
      struct buffer_head *bh = head;
      do {
        if (buffer_dirty(bh)) {
          if (!buffer_mapped(bh) || buffer_delay(bh) || buffer_unwritten(bh) || !bh->b_bdev) {
            usbmem_printk("usbmem kbh: dirty buffer without a disk location (ino %lu idx %lu state %lx)\n", m->host ? m->host->i_ino : 0, (unsigned long)f->index, bh->b_state);
            ferr = -EIO;
          } else {
            int rc = usbmem_bdev_write(bh->b_bdev->impl, (u64)bh->b_blocknr * bh->b_size, bh->b_data, bh->b_size);
            if (rc) { set_buffer_write_io_error(bh); ferr = -EIO; }
            else clear_buffer_dirty(bh);
          }
        }
        bh = bh->b_this_page;
      } while (bh != head);
    }
    if (!ferr) {
      folio_clear_dirty(f);
      if (head && ring_idle(head)) ring_free(f);
    } else if (!err) err = ferr;
    if (locked) folio_unlock(f);
    folio_put(f);
  }
  free(v);
  if (!err && m->host && !usbmem_mapping_has_dirty(m)) m->host->i_state &= ~(unsigned long)I_DIRTY_PAGES;
  return err;
}

/* ---- デバイス全体 */
/* bd_mapping の BH_Dirty な bh を全部書いて flush (jbd2 の再生の最後・ext4 のアンマウント)。 */
int sync_blockdev(struct block_device *bdev) {
  if (!bdev || !bdev->bd_mapping) return 0;
  struct folio **v; size_t n;
  int err = usbmem_mapping_snapshot(bdev->bd_mapping, 0, &v, &n), wrote = 0;
  if (err) return err;
  for (size_t k = 0; k < n; k++) {
    struct folio *f = v[k];
    struct buffer_head *head = folio_buffers(f);
    if (head) {
      get_bh(head);   /* 輪を処理中に外させない */
      struct buffer_head *bh = head;
      do {
        if (buffer_dirty(bh)) {
          lock_buffer(bh);
          if (test_clear_buffer_dirty(bh)) {
            wrote = 1;
            bh_submit(bh, REQ_OP_WRITE | REQ_SYNC, bh_end_write);
            if (buffer_write_io_error(bh) && !err) err = -EIO;
          } else unlock_buffer(bh);
        }
        bh = bh->b_this_page;
      } while (bh != head);
      put_bh(head);
    }
    folio_put(f);
  }
  free(v);
#ifdef USBMEM_MUTATE_NO_FLUSH
  wrote = 0;
#endif
  int fr = wrote ? usbmem_bdev_flush(bdev->impl) : 0;   /* 書いたときだけ flush (読み取り専用のマウントでは何もしない) */
  return err ? err : fr;
}
void invalidate_bdev(struct block_device *bdev) { (void)bdev; }
