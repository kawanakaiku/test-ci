// SPDX-License-Identifier: GPL-2.0-or-later
// LinuxCompat 実行時 (圧縮ファイルシステム向けの追加分): bio の追加 API / 待ち行列 / ページキャッシュの追加 API。
// squashfs (block.c, cache.c) と各展開ラッパが使う。docs/squashfs-port.md
#include <stdlib.h>
#include <compat/kernel.h>
#include <compat/usbmem_ntfs.h>

struct bio_set fs_bio_set;

/* ---- bio: kfree() で解放する単発 bio (ベクタ配列は bio の直後に置く) */
struct bio *bio_kmalloc(unsigned short nr_vecs, gfp_t gfp) {
  (void)gfp;
  struct bio *bio = calloc(1, sizeof *bio + (size_t)(nr_vecs ? nr_vecs : 1) * sizeof(struct bio_vec));
  return bio;
}
void bio_init_inline(struct bio *bio, struct block_device *bdev, unsigned short max_vecs, blk_opf_t opf) {
  memset(bio, 0, sizeof *bio);
  bio->bi_io_vec = (struct bio_vec *)(bio + 1);
  bio->bi_max_vecs = max_vecs ? max_vecs : 1;
  bio->bi_bdev = bdev; bio->bi_opf = opf; bio->remaining_ = 1;
}
void __bio_add_page(struct bio *bio, struct page *page, unsigned len, unsigned off) {
  bio->bi_io_vec[bio->bi_vcnt++] = (struct bio_vec){ page, len, off };
  bio->bi_iter.bi_size += len;
}
void bio_free_pages(struct bio *bio) {
  if (!bio) return;
  for (unsigned i = 0; i < bio->bi_vcnt; i++) put_page(bio->bi_io_vec[i].bv_page);
}

/* bio_alloc_clone / bio_trim: キャッシュ付き読み取り (squashfs の CONFIG_SQUASHFS_4K_DEVBLK_SIZE) 用。
 * 互換層の bio は bvec 配列を共有できないので、複製は配列を丸ごとコピーし、trim は先頭/末尾のバイトを実際に切り詰める。 */
struct bio *bio_alloc_clone(struct block_device *bdev, struct bio *src, gfp_t gfp, struct bio_set *bs) {
  (void)gfp; (void)bs;
  struct bio *b = bio_alloc(bdev, src->bi_vcnt ? src->bi_vcnt : 1, src->bi_opf, 0);
  if (!b) return NULL;
  memcpy(b->bi_io_vec, src->bi_io_vec, (size_t)src->bi_vcnt * sizeof(struct bio_vec));
  b->bi_vcnt = src->bi_vcnt; b->bi_iter = src->bi_iter;
  return b;
}
void bio_trim(struct bio *bio, sector_t offset, sector_t size) {
  u64 skip = (u64)offset << SECTOR_SHIFT, keep = (u64)size << SECTOR_SHIFT;
  unsigned out = 0;
  for (unsigned i = 0; i < bio->bi_vcnt && keep; i++) {
    struct bio_vec v = bio->bi_io_vec[i];
    if (skip >= v.bv_len) { skip -= v.bv_len; continue; }
    v.bv_offset += (unsigned)skip; v.bv_len -= (unsigned)skip; skip = 0;
    if (v.bv_len > keep) v.bv_len = (unsigned)keep;
    keep -= v.bv_len;
    bio->bi_io_vec[out++] = v;
  }
  bio->bi_vcnt = (unsigned short)out;
  bio->bi_iter.bi_size = (unsigned)(size << SECTOR_SHIFT);
  bio->bi_iter.bi_sector += offset;
}

/* ---- ページキャッシュ */
int filemap_add_folio(struct address_space *mapping, struct folio *folio, pgoff_t index, gfp_t gfp) {
  /* 他人の folio をキャッシュへ載せ替える API は互換層に無い。squashfs の「圧縮データの再利用キャッシュ」(4K_DEVBLK_SIZE) 専用で、使わない構成では呼ばれない。 */
  (void)mapping; (void)folio; (void)index; (void)gfp;
  return -ENOMEM;
}
struct page *find_get_page(struct address_space *mapping, pgoff_t index) {
  struct folio *f = filemap_get_folio(mapping, index);
  if (!f || IS_ERR(f)) return NULL;
  return &f->page;
}

/* ---- 待ち行列: 単一スレッド前提。待機中のエントリを大域リストで覚え、__wake_up がキーに合うものの ->func を呼ぶ。 */
static pthread_mutex_t g_wq_mu = PTHREAD_MUTEX_INITIALIZER;
static struct list_head g_wq = { &g_wq, &g_wq };

void init_wait_func(wait_queue_entry_t *wait, wait_queue_func_t func) {
  memset(wait, 0, sizeof *wait);
  wait->func = func; INIT_LIST_HEAD(&wait->entry);
}
void prepare_to_wait_exclusive(wait_queue_head_t *wq, wait_queue_entry_t *wait, int state) {
  (void)state;
  pthread_mutex_lock(&g_wq_mu);
  wait->head_ = wq; wait->woken_ = 0;
  if (wait->entry.next == &wait->entry) { wait->entry.next = g_wq.next; wait->entry.prev = &g_wq; g_wq.next->prev = &wait->entry; g_wq.next = &wait->entry; }
  pthread_mutex_unlock(&g_wq_mu);
}
void finish_wait(wait_queue_head_t *wq, wait_queue_entry_t *wait) {
  (void)wq;
  pthread_mutex_lock(&g_wq_mu);
  if (wait->entry.next != &wait->entry) { wait->entry.next->prev = wait->entry.prev; wait->entry.prev->next = wait->entry.next; wait->entry.next = wait->entry.prev = &wait->entry; }
  pthread_mutex_unlock(&g_wq_mu);
}
int default_wake_function(wait_queue_entry_t *wait, unsigned mode, int sync, void *key) {
  (void)mode; (void)sync; (void)key;
  wait->woken_ = 1;
  return 1;
}
void __wake_up(wait_queue_head_t *wq, unsigned mode, int nr_exclusive, void *key) {
  pthread_mutex_lock(&g_wq_mu);
  struct list_head *p = g_wq.next;
  while (p != &g_wq) {
    struct list_head *next = p->next;
    wait_queue_entry_t *w = container_of(p, wait_queue_entry_t, entry);
    if (w->head_ == wq && w->func && w->func(w, mode, 0, key) && nr_exclusive > 0 && --nr_exclusive == 0) break;
    p = next;
  }
  pthread_mutex_unlock(&g_wq_mu);
  wake_up_all(wq);   /* wait_event() で待つ側 (条件変数) も起こす */
}
