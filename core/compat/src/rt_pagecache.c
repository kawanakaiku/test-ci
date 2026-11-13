// SPDX-License-Identifier: GPL-2.0-or-later
// LinuxCompat 実行時 (2/3): ページキャッシュ(folio)・block/bio・iomap(読み取り)・NLS。
// folio は常に PAGE_SIZE (4096) 固定。large folio は未対応。
#include <stdio.h>
#include <pthread.h>
#include <compat/kernel.h>
#include <compat/usbmem_ntfs.h>

#include <sys/mman.h>
#if defined(__SANITIZE_ADDRESS__)
#define USBMEM_ASAN_BUILD 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define USBMEM_ASAN_BUILD 1
#endif
#endif

#define NB 256
struct pcache { struct folio *b[NB]; pthread_mutex_t mu; size_t count; unsigned clock; };

/* 1 マッピングあたりのキャッシュ folio 数の上限 (既定 8192 = 32 MiB)。超えたら、キャッシュしか参照していない clean な folio を古い順に捨てる。
 * 上限が無いと、巨大ファイルを読むだけでメモリが際限なく増える (7.3 TiB の NTFS で $Bitmap だけで約 490 MB)。環境変数 USBMEM_PCACHE_MAX で変更 (テスト用: 小さくして退避を酷使する)。 */
static size_t pcache_max(void) {
  static size_t v;
  if (!v) { const char *e = getenv("USBMEM_PCACHE_MAX"); long n = e ? atol(e) : 8192; v = n < 64 ? 64 : (size_t)n; }
  return v;
}

/* folio のデータ領域 (4096B・4096 境界)。posix_memalign(4096, 4096) は、アロケータによっては 1 ページあたり約 8 KiB を使う (実機で RSS が 2 倍になった)。
 * 大きな領域 (1 MiB) から切り出して再利用する。ASan ビルドは境界検査のため通常の確保のまま。 */
#ifndef USBMEM_ASAN_BUILD
static pthread_mutex_t g_slab_mu = PTHREAD_MUTEX_INITIALIZER;
static char *g_slab_cur, *g_slab_end;
static void *g_slab_free;   /* 解放済みページの単方向リスト (ページ先頭に次のポインタ) */
#define SLAB_BYTES (256 * PAGE_SIZE)
static void *data_alloc(void) {
  pthread_mutex_lock(&g_slab_mu);
  void *pg = g_slab_free;
  if (pg) g_slab_free = *(void **)pg;
  else {
    if (g_slab_cur == g_slab_end) {
      void *base = mmap(NULL, SLAB_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (base == MAP_FAILED) { pthread_mutex_unlock(&g_slab_mu); return NULL; }
      g_slab_cur = base; g_slab_end = g_slab_cur + SLAB_BYTES;
    }
    pg = g_slab_cur; g_slab_cur += PAGE_SIZE;
  }
  pthread_mutex_unlock(&g_slab_mu);
  return pg;
}
static void data_free(void *pg) { pthread_mutex_lock(&g_slab_mu); *(void **)pg = g_slab_free; g_slab_free = pg; pthread_mutex_unlock(&g_slab_mu); }
#else
static void *data_alloc(void) { void *p = NULL; return posix_memalign(&p, PAGE_SIZE, PAGE_SIZE) ? NULL : p; }
static void data_free(void *pg) { free(pg); }
#endif

static struct pcache *pc_get(struct address_space *m) {
  if (!m->pages) {
    struct pcache *p = calloc(1, sizeof *p);
    pthread_mutex_init(&p->mu, NULL);
    m->pages = p;
  }
  return m->pages;
}
static struct folio *folio_new(struct address_space *m, pgoff_t idx) {
  struct folio *f = calloc(1, sizeof *f);
  if (!f) return NULL;
  f->data = data_alloc();
  if (!f->data) { free(f); return NULL; }
  memset(f->data, 0, PAGE_SIZE);
  f->mapping = m; f->index = idx;
  atomic_set(&f->refcount, 1);
  pthread_mutex_init(&f->lock_.m, NULL);
  f->page.folio_ = f;
  return f;
}
static struct folio *pc_find_locked(struct pcache *p, pgoff_t idx) {
  for (struct folio *f = p->b[idx % NB]; f; f = f->hash_next_) if (f->index == idx) { atomic_inc(&f->refcount); return f; }  /* 呼び出し側へ +1 参照 */
  return NULL;
}

/* ---- folio 基本 */
void *folio_address(const struct folio *f) { return f->data; }
size_t folio_size(const struct folio *f) { (void)f; return PAGE_SIZE; }
unsigned folio_shift(const struct folio *f) { (void)f; return PAGE_SHIFT; }
long folio_nr_pages(const struct folio *f) { (void)f; return 1; }
loff_t folio_pos(const struct folio *f) { return (loff_t)f->index << PAGE_SHIFT; }
loff_t folio_next_pos(const struct folio *f) { return folio_pos(f) + PAGE_SIZE; }
bool folio_contains(const struct folio *f, pgoff_t i) { return f->index == i; }
struct page *folio_page(struct folio *f, unsigned long n) { (void)n; return &f->page; }
struct folio *page_folio(struct page *p) { return p->folio_; }
void folio_get(struct folio *f) { atomic_inc(&f->refcount); }
static void folio_free(struct folio *f) { pthread_mutex_destroy(&f->lock_.m); data_free(f->data); free(f); }
/* 参照規約は Linux と同じ: キャッシュが 1、get 系 API が呼び出し側に +1。最後の put で解放。 */
void folio_put(struct folio *f) { if (atomic_dec_return(&f->refcount) == 0) folio_free(f); }
void folio_lock(struct folio *f) { pthread_mutex_lock(&f->lock_.m); f->flags |= 1UL << FOLIO_LOCKED; }
void folio_unlock(struct folio *f) { f->flags &= ~(1UL << FOLIO_LOCKED); pthread_mutex_unlock(&f->lock_.m); }
int folio_test_locked(const struct folio *f) { return (f->flags >> FOLIO_LOCKED) & 1; }
int folio_test_uptodate(const struct folio *f) { return (f->flags >> FOLIO_UPTODATE) & 1; }
int folio_test_writeback(const struct folio *f) { return (f->flags >> FOLIO_WRITEBACK) & 1; }
void folio_mark_uptodate(struct folio *f) { f->flags |= 1UL << FOLIO_UPTODATE; }
void folio_clear_uptodate(struct folio *f) { f->flags &= ~(1UL << FOLIO_UPTODATE); }
void folio_mark_dirty(struct folio *f) { f->flags |= 1UL << FOLIO_DIRTY; f->dirty_blocks_ = 0xFF; }
void folio_clear_dirty(struct folio *f) { f->flags &= ~(1UL << FOLIO_DIRTY); f->dirty_blocks_ = 0; }
void usbmem_folio_dirty_range(struct folio *f, size_t off, size_t len) {
  if (off >= PAGE_SIZE || !len) return;
  size_t end = off + len; if (end > PAGE_SIZE) end = PAGE_SIZE;
  for (size_t b = off / 512; b * 512 < end; b++) f->dirty_blocks_ |= (unsigned char)(1u << b);
  f->flags |= 1UL << FOLIO_DIRTY;
}
/* [off, off+len) に完全に含まれる 512B ブロックの dirty を外す (bforget / 直接書き込み済みの bh 用)。部分的にしか含まれないブロックは dirty のまま残す (安全側)。 */
void usbmem_folio_clean_range(struct folio *f, size_t off, size_t len) {
  if (!((f->flags >> FOLIO_DIRTY) & 1) || off >= PAGE_SIZE || !len) return;
  if (!f->dirty_blocks_) f->dirty_blocks_ = 0xFF;   /* ビット未設定の dirty は全体扱い */
  size_t end = off + len; if (end > PAGE_SIZE) end = PAGE_SIZE;
  for (size_t b = (off + 511) / 512; (b + 1) * 512 <= end; b++) f->dirty_blocks_ &= (unsigned char)~(1u << b);
  if (!f->dirty_blocks_) f->flags &= ~(1UL << FOLIO_DIRTY);
}
void folio_start_writeback(struct folio *f) { f->flags |= 1UL << FOLIO_WRITEBACK; }
void folio_end_writeback(struct folio *f) { f->flags &= ~(1UL << FOLIO_WRITEBACK); }
void folio_wait_writeback(struct folio *f) { (void)f; }
void folio_redirty_for_writepage(struct writeback_control *w, struct folio *f) { (void)w; folio_mark_dirty(f); }
void folio_zero_segment(struct folio *f, size_t s, size_t e) { if (e > PAGE_SIZE) e = PAGE_SIZE; if (s < e) memset((char *)f->data + s, 0, e - s); }
void folio_zero_segments(struct folio *f, size_t s1, size_t e1, size_t s2, size_t e2) { folio_zero_segment(f, s1, e1); folio_zero_segment(f, s2, e2); }
void folio_fill_tail(struct folio *f, size_t off, const char *from, size_t len) {
  if (off + len > PAGE_SIZE) len = PAGE_SIZE - off;
  memcpy((char *)f->data + off, from, len); folio_zero_segment(f, off + len, PAGE_SIZE);
}
void *kmap_local_folio(struct folio *f, size_t off) { return (char *)f->data + off; }
void *kmap_local_page(struct page *p) { return p->folio_->data; }
void flush_dcache_folio(struct folio *f) { (void)f; }
void flush_dcache_page(struct page *p) { (void)p; }
void memcpy_from_folio(char *to, struct folio *f, size_t off, size_t len) { memcpy(to, (char *)f->data + off, len); }
void memcpy_to_folio(struct folio *f, size_t off, const char *from, size_t len) { memcpy((char *)f->data + off, from, len); }
void memcpy_from_page(char *to, struct page *p, size_t off, size_t len) { memcpy(to, (char *)p->folio_->data + off, len); }
void memcpy_to_page(struct page *p, size_t off, const char *from, size_t len) { memcpy((char *)p->folio_->data + off, from, len); }
void memzero_page(struct page *p, size_t off, size_t len) { memset((char *)p->folio_->data + off, 0, len); }
void zero_user_segment(struct page *p, unsigned s, unsigned e) { folio_zero_segment(p->folio_, s, e); }
void zero_user_segments(struct page *p, unsigned s1, unsigned e1, unsigned s2, unsigned e2) { folio_zero_segments(p->folio_, s1, e1, s2, e2); }
struct page *alloc_page(gfp_t g) { (void)g; struct folio *f = folio_new(NULL, 0); return f ? &f->page : NULL; }
void put_page(struct page *p) { if (p) folio_put(p->folio_); }  /* alloc_page は参照 1、キャッシュ上の page は get 系が +1 */
void lock_page(struct page *p) { folio_lock(p->folio_); }
void unlock_page(struct page *p) { folio_unlock(p->folio_); }
int PageUptodate(const struct page *p) { return folio_test_uptodate(p->folio_); }
int PageDirty(const struct page *p) { return (p->folio_->flags >> FOLIO_DIRTY) & 1; }
void SetPageUptodate(struct page *p) { folio_mark_uptodate(p->folio_); }

/* ---- address_space */
gfp_t mapping_gfp_mask(struct address_space *m) { (void)m; return 0; }
void mapping_set_error(struct address_space *m, int e) { (void)m; (void)e; }
void filemap_invalidate_lock(struct address_space *m) { pthread_rwlock_wrlock(&m->invalidate_lock.l); }
void filemap_invalidate_unlock(struct address_space *m) { pthread_rwlock_unlock(&m->invalidate_lock.l); }
void filemap_invalidate_lock_shared(struct address_space *m) { pthread_rwlock_rdlock(&m->invalidate_lock.l); }
void filemap_invalidate_unlock_shared(struct address_space *m) { pthread_rwlock_unlock(&m->invalidate_lock.l); }

/* 上限を超えていたら、キャッシュだけが参照する clean な folio を捨てる (p->mu を持って呼ぶ)。
 * 各ハッシュ鎖の末尾 (最も古く挿入されたもの) を優先する。refcount==1 は「キャッシュ以外に参照者がいない」ことを意味し、
 * 参照の取得 (pc_find_locked) も p->mu の下で行うので、ここで解放しても競合しない。dirty / 書き戻し中 / ロック中 / 読み込み中は捨てない。 */
static int pc_evictable(struct folio *f) {
  return atomic_read(&f->refcount) == 1 && !f->read_pending_ &&
         !(f->flags & ((1UL << FOLIO_DIRTY) | (1UL << FOLIO_WRITEBACK) | (1UL << FOLIO_LOCKED)));
}
static void pc_evict_locked(struct pcache *p, struct folio *keep) {
  size_t max = pcache_max();
  if (p->count <= max) return;
  size_t target = max - max / 16;   /* 少し余裕を持って、毎回ではなくまとめて捨てる */
  for (int pass = 0; pass < 4 * NB && p->count > target; pass++) {
    struct folio **victim = NULL;
    for (struct folio **pp = &p->b[p->clock++ % NB]; *pp; pp = &(*pp)->hash_next_)
      if (*pp != keep && pc_evictable(*pp)) victim = pp;   /* 鎖で最後に見つかった (=最も古い) もの */
    if (victim) { struct folio *f = *victim; *victim = f->hash_next_; p->count--; folio_free(f); }
  }
}

/* folio を取得 (無ければ作る)。返す folio は未 lock / 未 uptodate。 */
static struct folio *get_or_create(struct address_space *m, pgoff_t idx) {
  struct pcache *p = pc_get(m);
  pthread_mutex_lock(&p->mu);
  struct folio *f = pc_find_locked(p, idx);
  if (!f) {
    f = folio_new(m, idx);
    if (f) { atomic_set(&f->refcount, 2); /* キャッシュ + 呼び出し側 */ f->hash_next_ = p->b[idx % NB]; p->b[idx % NB] = f; p->count++; pc_evict_locked(p, f); }
  }
  pthread_mutex_unlock(&p->mu);
  return f;
}
struct folio *filemap_lock_folio(struct address_space *m, pgoff_t idx) {
  struct pcache *p = pc_get(m);
  pthread_mutex_lock(&p->mu); struct folio *f = pc_find_locked(p, idx); pthread_mutex_unlock(&p->mu);
  if (!f) return ERR_PTR(-ENOENT);
  folio_lock(f); return f;
}
struct folio *filemap_grab_folio(struct address_space *m, pgoff_t idx) {
  struct folio *f = get_or_create(m, idx);
  if (!f) return ERR_PTR(-ENOMEM);
  folio_lock(f); return f;
}
struct folio *__filemap_get_folio(struct address_space *m, pgoff_t idx, unsigned fgp, gfp_t g) {
  (void)g;
  struct folio *f = (fgp & FGP_CREAT) ? get_or_create(m, idx) : NULL;
  if (!f) { struct pcache *p = pc_get(m); pthread_mutex_lock(&p->mu); f = pc_find_locked(p, idx); pthread_mutex_unlock(&p->mu); }
  if (!f) return ERR_PTR(-ENOENT);
  if (fgp & FGP_LOCK) folio_lock(f);
  return f;
}
struct page *grab_cache_page_nowait(struct address_space *m, pgoff_t idx) {
  struct folio *f = get_or_create(m, idx); if (!f) return NULL; folio_lock(f); return &f->page;
}
/* ページを読む: 未 uptodate なら a_ops->read_folio を呼ぶ。返す folio は unlock 済み・uptodate。 */
struct folio *read_mapping_folio(struct address_space *m, pgoff_t idx, struct file *file) {
  struct folio *f = get_or_create(m, idx);
  if (!f) return ERR_PTR(-ENOMEM);
  if (folio_test_uptodate(f)) return f;
  folio_lock(f);
  if (folio_test_uptodate(f)) { folio_unlock(f); return f; }
  if (!m->a_ops || !m->a_ops->read_folio) { folio_unlock(f); folio_put(f); return ERR_PTR(-EIO); }
  int rc = ((int (*)(struct file *, struct folio *))m->a_ops->read_folio)(file, f);
  if (rc) { folio_put(f); return ERR_PTR(rc); }   /* read_folio は完了時に unlock する契約 */
  folio_lock(f); int ok = folio_test_uptodate(f); folio_unlock(f);
  if (!ok) { folio_put(f); return ERR_PTR(-EIO); }
  return f;
}
struct page *read_mapping_page(struct address_space *m, pgoff_t idx, struct file *file) {
  struct folio *f = read_mapping_folio(m, idx, file);
  return IS_ERR(f) ? (struct page *)f : &f->page;
}
/* USBMEM_KBH (ext4/jbd2 の rt_kbh.c) が folio に付けた bh の輪を外す。ext4 を含まないリンク (NTFS だけのテスト等) では未定義 = NULL */
extern void usbmem_kbh_release_folio(struct folio *f) __attribute__((weak));
void truncate_inode_pages(struct address_space *m, loff_t from) {
  struct pcache *p = m->pages; if (!p) return;
  pgoff_t first = (from + PAGE_SIZE - 1) >> PAGE_SHIFT;
  pthread_mutex_lock(&p->mu);
  for (int i = 0; i < NB; i++) {
    struct folio **pp = &p->b[i];
    while (*pp) {
      struct folio *f = *pp;
      if (f->index >= first) {
        *pp = f->hash_next_; p->count--;
        if (((f->flags >> FOLIO_KBH_RING) & 1) && usbmem_kbh_release_folio) {
          usbmem_kbh_release_folio(f);   /* 輪の持つ参照も外す (カーネルの invalidate_folio 相当) */
          /* カーネルは切り離した folio の mapping を NULL にする。ext4_write_begin は ENOSPC の失敗後 (ext4_truncate_failed_write が folio を捨てる) に
           * 同じ folio を持ったまま再試行するので、`folio->mapping != mapping` を見て取り直せるようにする (しないと、捨てた folio に輪を付け直して漏れる)。
           * KBH (ext4) の folio だけ: 他の FS は従来どおり。 */
          f->mapping = NULL;
        }
        folio_put(f); /* キャッシュ参照を外す。使用中なら最後の put で解放 */
      } else pp = &f->hash_next_;
    }
  }
  pthread_mutex_unlock(&p->mu);
}
static int cmp_folio_idx(const void *a, const void *b) { pgoff_t x = (*(struct folio *const *)a)->index, y = (*(struct folio *const *)b)->index; return x < y ? -1 : x > y; }
/* キャッシュ中の folio を index 昇順で列挙する (dirty_only なら dirty なものだけ)。各 folio に参照を 1 つ付けて返す (呼び出し側が folio_put と free)。 */
int usbmem_mapping_snapshot(struct address_space *m, int dirty_only, struct folio ***out, size_t *n) {
  struct pcache *p = m->pages; *out = NULL; *n = 0;
  if (!p) return 0;
  pthread_mutex_lock(&p->mu);
  size_t cap = 0;
  for (int i = 0; i < NB; i++) for (struct folio *f = p->b[i]; f; f = f->hash_next_) if (!dirty_only || ((f->flags >> FOLIO_DIRTY) & 1)) cap++;
  struct folio **v = cap ? malloc(cap * sizeof *v) : NULL;
  if (cap && !v) { pthread_mutex_unlock(&p->mu); return -ENOMEM; }
  size_t k = 0;
  for (int i = 0; i < NB; i++) for (struct folio *f = p->b[i]; f; f = f->hash_next_) if (!dirty_only || ((f->flags >> FOLIO_DIRTY) & 1)) { atomic_inc(&f->refcount); v[k++] = f; }
  pthread_mutex_unlock(&p->mu);
  if (k > 1) qsort(v, k, sizeof *v, cmp_folio_idx);
  *out = v; *n = k;
  return 0;
}
void truncate_inode_pages_final(struct address_space *m) {
  truncate_inode_pages(m, 0);
  if (m->pages) { struct pcache *pc = m->pages; pthread_mutex_destroy(&pc->mu); free(pc); m->pages = NULL; }  /* mapping 終了: キャッシュ管理体も解放 */
}
/* n が folio の途中なら、その folio の n 以降をゼロにする (Linux の truncate_inode_partial_folio と同じ。後で拡張したとき古いデータが穴に見えてしまうのを防ぐ)。dirty にはしない。 */
void truncate_pagecache(struct inode *i, loff_t n) {
  truncate_inode_pages(i->i_mapping, n);
  if (n > 0 && (n & (PAGE_SIZE - 1)) && i->i_mapping->pages) {
    struct pcache *p = i->i_mapping->pages;
    pthread_mutex_lock(&p->mu); struct folio *f = pc_find_locked(p, (pgoff_t)(n >> PAGE_SHIFT)); pthread_mutex_unlock(&p->mu);
    if (f) { folio_lock(f); folio_zero_segment(f, (size_t)(n & (PAGE_SIZE - 1)), PAGE_SIZE); folio_unlock(f); folio_put(f); }
  }
}
void truncate_setsize(struct inode *i, loff_t n) { i->i_size = n; truncate_pagecache(i, n); }
void truncate_pagecache_range(struct inode *i, loff_t a, loff_t b) { (void)a; (void)b; truncate_pagecache(i, a); }
unsigned long invalidate_mapping_pages(struct address_space *m, pgoff_t a, pgoff_t b) { (void)m; (void)a; (void)b; return 0; }
void unmap_mapping_range(struct address_space *m, loff_t a, loff_t b, int c) { (void)m; (void)a; (void)b; (void)c; }
void pagecache_isize_extended(struct inode *i, loff_t a, loff_t b) { (void)i; (void)a; (void)b; }
int filemap_write_and_wait(struct address_space *m) { (void)m; return 0; }
int filemap_write_and_wait_range(struct address_space *m, loff_t a, loff_t b) { (void)m; (void)a; (void)b; return 0; }
int file_write_and_wait_range(struct file *f, loff_t a, loff_t b) { (void)f; (void)a; (void)b; return 0; }
void file_ra_state_init(struct file_ra_state *r, struct address_space *m) { (void)m; memset(r, 0, sizeof *r); }
void page_cache_sync_readahead(struct address_space *m, struct file_ra_state *r, struct file *f, pgoff_t i, unsigned long n) { (void)m; (void)r; (void)f; (void)i; (void)n; }
vm_fault_t vmf_error(int e) { return e; }

/* ---- block / bio : 同期実行 */
unsigned bdev_logical_block_size(struct block_device *b) { return usbmem_bdev_lbs(b->impl); }
u64 bdev_nr_bytes(struct block_device *b) { return usbmem_bdev_size(b->impl); }
unsigned bdev_discard_granularity(struct block_device *b) { (void)b; return 0; }
unsigned bdev_max_discard_sectors(struct block_device *b) { (void)b; return 0; }
int bdev_freeze(struct block_device *b) { (void)b; return 0; }
int bdev_thaw(struct block_device *b) { (void)b; return 0; }
int sync_blockdev(struct block_device *b) { return usbmem_bdev_flush(b->impl); }
int sync_filesystem(struct super_block *s) { return s->s_bdev ? usbmem_bdev_flush(s->s_bdev->impl) : 0; }
void sync_inodes_sb(struct super_block *s) { (void)s; }
int blkdev_issue_flush(struct block_device *b) {
#ifdef USBMEM_MUTATE_NO_FLUSH   /* 突然変異テスト (ext4 の書き込み): flush を全部外す */
  (void)b; return 0;
#endif
  return usbmem_bdev_flush(b->impl);
}
int bdev_rw_virt(struct block_device *b, sector_t sec, void *buf, size_t len, blk_opf_t opf) {
  return (opf & REQ_OP_MASK) == REQ_OP_WRITE ? usbmem_bdev_write(b->impl, sec << SECTOR_SHIFT, buf, len)
                                              : usbmem_bdev_read(b->impl, sec << SECTOR_SHIFT, buf, len);
}
int blk_status_to_errno(int s) { return s; }

static struct bio *bio_alloc_pad(struct block_device *b, unsigned short nr, blk_opf_t opf, unsigned pad) {
  char *base = calloc(1, pad + sizeof(struct bio));
  if (!base) return NULL;
  struct bio *bio = (struct bio *)(base + pad);
  bio->bi_io_vec = calloc(nr ? nr : 1, sizeof(struct bio_vec));
  if (!bio->bi_io_vec) { free(base); return NULL; }
  bio->bi_max_vecs = nr ? nr : 1; bio->bi_bdev = b; bio->bi_opf = opf; bio->remaining_ = 1; bio->pad_ = pad;
  return bio;
}
struct bio *bio_alloc(struct block_device *b, unsigned short nr, blk_opf_t opf, gfp_t g) { (void)g; return bio_alloc_pad(b, nr, opf, 0); }
/* front_pad 付き bioset: fs/ntfs は bio を自前の ctx 構造体に埋め込む (container_of)。pad 分を前置確保して bio を後ろに置く。 */
struct bio *bio_alloc_bioset(struct block_device *b, unsigned short nr, blk_opf_t opf, gfp_t g, struct bio_set *s) { (void)g; return bio_alloc_pad(b, nr, opf, s ? s->front_pad : 0); }
void bio_init(struct bio *bio, struct block_device *b, struct bio_vec *vecs, unsigned short nr, blk_opf_t opf) {
  memset(bio, 0, sizeof *bio); bio->bi_bdev = b; bio->bi_io_vec = vecs; bio->bi_max_vecs = nr; bio->bi_opf = opf; bio->remaining_ = 1;
}
void bio_uninit(struct bio *bio) { (void)bio; }
void bio_put(struct bio *bio) { if (bio) { free(bio->bi_io_vec); free((char *)bio - bio->pad_); } }
/* chain: parent は child の完了も待つ。同期実行なので child 完了時に状態を引き継ぎ、残数 0 で parent の end_io を呼ぶ。 */
static void bio_endio(struct bio *bio);
static void bio_chain_endio(struct bio *child) {
  struct bio *parent = child->bi_private;
  if (child->bi_status && !parent->bi_status) parent->bi_status = child->bi_status;
  bio_put(child);
  bio_endio(parent);
}
static void bio_endio(struct bio *bio) {
  if (--bio->remaining_ > 0) return;
  if (bio->bi_end_io) bio->bi_end_io(bio);
}
void bio_chain(struct bio *child, struct bio *parent) {
  child->bi_private = parent; child->bi_end_io = bio_chain_endio; parent->remaining_++;
}
int bio_add_page(struct bio *bio, struct page *p, unsigned len, unsigned off) {
  if (bio->bi_vcnt >= bio->bi_max_vecs) return 0;
  bio->bi_io_vec[bio->bi_vcnt++] = (struct bio_vec){ p, len, off };
  bio->bi_iter.bi_size += len; return (int)len;
}
bool bio_add_folio(struct bio *bio, struct folio *f, size_t len, size_t off) { return bio_add_page(bio, &f->page, (unsigned)len, (unsigned)off) != 0; }
unsigned short bio_max_segs(unsigned n) { return n > 256 ? 256 : (unsigned short)n; }
sector_t bio_end_sector(const struct bio *bio) { return bio->bi_iter.bi_sector + (bio->bi_iter.bi_size >> SECTOR_SHIFT); }
int bioset_init(struct bio_set *s, unsigned a, unsigned front_pad, int c) { (void)a; (void)c; s->front_pad = front_pad; return 0; }
void bioset_exit(struct bio_set *s) { (void)s; }
void usbmem_fi_next(struct folio_iter *fi, struct bio *bio) {
  fi->idx_++;
  if (fi->idx_ >= bio->bi_vcnt) { fi->folio = NULL; return; }
  struct bio_vec *v = &bio->bi_io_vec[fi->idx_];
  fi->folio = v->bv_page->folio_; fi->offset = v->bv_offset; fi->length = v->bv_len;
}
static int bio_do_io(struct bio *bio) {
  u64 off = (u64)bio->bi_iter.bi_sector << SECTOR_SHIFT;
  int wr = (bio->bi_opf & REQ_OP_MASK) == REQ_OP_WRITE;
  for (unsigned i = 0; i < bio->bi_vcnt; i++) {
    struct bio_vec *v = &bio->bi_io_vec[i];
    char *buf = (char *)v->bv_page->folio_->data + v->bv_offset;
    int rc = wr ? usbmem_bdev_write(bio->bi_bdev->impl, off, buf, v->bv_len)
                : usbmem_bdev_read(bio->bi_bdev->impl, off, buf, v->bv_len);
    if (rc) return rc;
    off += v->bv_len;
  }
  return 0;
}
void submit_bio(struct bio *bio) {
  bio->bi_status = bio_do_io(bio);
  bio_endio(bio);
}
int submit_bio_wait(struct bio *bio) { bio->bi_status = bio_do_io(bio); return bio->bi_status; }

/* ---- iomap (読み取り専用段階) */
int iomap_iter_advance(struct iomap_iter *it, u64 count) {
  if (count > it->len) return -EIO;
  it->pos += (loff_t)count; it->len -= count; return 0;
}
int iomap_iter_continue(const struct iomap_iter *it, struct iomap *iomap, struct iomap *srcmap, int ret) {
  if (ret < 0) return ret;
  if (!it->len) return 0;
  if (it->pos == it->iter_start_pos) return -EIO;      /* 進捗なし = 無限ループ防止 */
  if (it->pos >= (loff_t)(iomap->offset + iomap->length)) { memset(iomap, 0, sizeof *iomap); memset(srcmap, 0, sizeof *srcmap); }
  return 1;
}
void iomap_finish_folio_read(struct folio *f, size_t off, size_t len, int err) {
  (void)off; f->read_pending_ -= (int)len; if (err) f->read_err_ = err;
}
int iomap_bio_read_folio_range(const struct iomap_iter *it, struct iomap_read_folio_ctx *ctx, size_t plen) {
  struct folio *f = ctx->cur_folio;
  if (ctx->read_ctx && ctx->ops->submit_read) ctx->ops->submit_read(it, ctx);  /* 1 range = 1 bio */
  struct bio *bio = bio_alloc(it->iomap.bdev, 1, REQ_OP_READ, 0);
  if (!bio) return -ENOMEM;
  loff_t disk = (loff_t)it->iomap.addr + (it->pos - it->iomap.offset);
  bio->bi_iter.bi_sector = (sector_t)(disk >> SECTOR_SHIFT);
  bio_add_folio(bio, f, plen, offset_in_folio(f, it->pos));
  f->read_pending_ += (int)plen;
  ctx->read_ctx = bio;
  return 0;
}
void iomap_bio_submit_read_endio(const struct iomap_iter *it, struct iomap_read_folio_ctx *ctx, bio_end_io_t end_io) {
  (void)it; struct bio *bio = ctx->read_ctx; if (!bio) return;
  ctx->read_ctx = NULL; bio->bi_end_io = end_io; submit_bio(bio);
}
/* 汎用の bio 読み取り (カーネルの iomap_bio_read_ops と同じ): 完了時に folio へ結果を通知して bio を解放する。erofs など、独自の submit_read を持たない FS が使う。 */
static void iomap_bio_default_end_io(struct bio *bio) {
  int error = blk_status_to_errno(bio->bi_status);
  struct folio_iter iter;
  bio_for_each_folio_all(iter, bio) iomap_finish_folio_read(iter.folio, iter.offset, iter.length, error);
  bio_put(bio);
}
static void iomap_bio_default_submit_read(const struct iomap_iter *it, struct iomap_read_folio_ctx *ctx) {
  iomap_bio_submit_read_endio(it, ctx, iomap_bio_default_end_io);
}
const struct iomap_read_ops iomap_bio_read_ops = { iomap_bio_read_folio_range, iomap_bio_default_submit_read, NULL };

void iomap_read_folio(iomap_iter_next_fn next, struct iomap_read_folio_ctx *ctx, void *private) {
  struct folio *f = ctx->cur_folio;
  struct inode *inode = f->mapping->host;
  struct iomap_iter it = { .inode = inode, .pos = folio_pos(f), .len = folio_size(f), .private = private };
  f->read_pending_ = 0; f->read_err_ = 0;
  loff_t isize = inode->i_size;
  int rc;
  for (;;) {
    rc = next(&it, &it.iomap, &it.srcmap);
    if (rc <= 0) break;
    it.iter_start_pos = it.pos;
    while (it.len) {
      u64 plen = iomap_length(&it);
      if (!plen) { rc = -EIO; goto out; }
      if (plen > it.len) plen = it.len;
      size_t foff = offset_in_folio(f, it.pos);
      if (foff + plen > PAGE_SIZE) plen = PAGE_SIZE - foff;
      if (it.pos >= isize) {                                 /* EOF 以降はゼロ */
        folio_zero_segment(f, foff, foff + plen);
      } else if (it.iomap.type == IOMAP_HOLE || it.iomap.type == IOMAP_UNWRITTEN || it.iomap.addr == IOMAP_NULL_ADDR) {
        folio_zero_segment(f, foff, foff + plen);
      } else if (it.iomap.type == IOMAP_INLINE) {
        size_t o = (size_t)(it.pos - it.iomap.offset);
        memcpy((char *)f->data + foff, (char *)it.iomap.inline_data + o, plen);
      } else {
        size_t n = plen;
        if (it.pos + (loff_t)n > isize) {                    /* 末尾を超える分はゼロ */
          n = (size_t)(isize - it.pos);
          folio_zero_segment(f, foff + n, foff + plen);
        }
        rc = ctx->ops->read_folio_range(&it, ctx, n);
        if (rc) goto out;
      }
      if (iomap_iter_advance(&it, plen)) { rc = -EIO; goto out; }
      if (it.pos >= (loff_t)(it.iomap.offset + it.iomap.length)) break;
    }
  }
out:
  if (ctx->read_ctx && ctx->ops->submit_read) ctx->ops->submit_read(&it, ctx);
  if (rc < 0 && !f->read_err_) f->read_err_ = rc;
  if (!f->read_err_ && f->read_pending_ == 0) folio_mark_uptodate(f);
  folio_unlock(f);
}
void iomap_readahead(iomap_iter_next_fn next, struct iomap_read_folio_ctx *ctx, void *private) { (void)next; (void)ctx; (void)private; usbmem_unimplemented("iomap_readahead"); }


/* ---- 書き込み (段階 1): dirty folio 管理と同期 writeback */
bool iomap_dirty_folio(struct address_space *m, struct folio *f) {
  bool was = (f->flags >> FOLIO_DIRTY) & 1;
  f->flags |= 1UL << FOLIO_DIRTY; f->dirty_blocks_ = 0xFF;
  if (m && m->host) m->host->i_state |= I_DIRTY_PAGES;
  return !was;
}
static int cmp_idx(const void *a, const void *b) { pgoff_t x = *(const pgoff_t *)a, y = *(const pgoff_t *)b; return x < y ? -1 : x > y; }
int usbmem_mapping_has_dirty(struct address_space *m) {
  struct pcache *p = m->pages; if (!p) return 0;
  int r = 0;
  pthread_mutex_lock(&p->mu);
  for (int i = 0; i < NB && !r; i++) for (struct folio *f = p->b[i]; f; f = f->hash_next_) if ((f->flags >> FOLIO_DIRTY) & 1) { r = 1; break; }
  pthread_mutex_unlock(&p->mu);
  return r;
}
/* Linux の writeback_iter と同じ契約: dirty folio を index 昇順で、lock 済み・dirty クリア済みで返す。
 * 呼び出し側は処理後に unlock する (ここで前回の folio の参照を外す)。最初の呼び出しで dirty 一覧を確定する。 */
struct folio *writeback_iter(struct address_space *m, struct writeback_control *wbc, struct folio *prev, int *err) {
  struct pcache *p = m->pages;
  if (!prev) {
    wbc->pos_ = 0; wbc->nsnap_ = 0; wbc->saved_err_ = 0; free(wbc->snap_); wbc->snap_ = NULL;
    *err = 0;
    if (!p) return NULL;
    size_t cap = 0, n = 0;
    pthread_mutex_lock(&p->mu);
    for (int i = 0; i < NB; i++) for (struct folio *f = p->b[i]; f; f = f->hash_next_) if ((f->flags >> FOLIO_DIRTY) & 1) cap++;
    wbc->snap_ = cap ? malloc(cap * sizeof(pgoff_t)) : NULL;
    if (wbc->snap_) for (int i = 0; i < NB; i++) for (struct folio *f = p->b[i]; f; f = f->hash_next_) if (((f->flags >> FOLIO_DIRTY) & 1) && n < cap) wbc->snap_[n++] = f->index;
    pthread_mutex_unlock(&p->mu);
    wbc->nsnap_ = n;
    if (n) qsort(wbc->snap_, n, sizeof(pgoff_t), cmp_idx);
  } else {
    if (*err && !wbc->saved_err_) wbc->saved_err_ = *err;
    folio_put(prev);
  }
  while (wbc->pos_ < wbc->nsnap_) {
    pgoff_t idx = wbc->snap_[wbc->pos_++];
    pthread_mutex_lock(&p->mu); struct folio *f = pc_find_locked(p, idx); pthread_mutex_unlock(&p->mu);
    if (!f) continue;
    folio_lock(f);
    if (!((f->flags >> FOLIO_DIRTY) & 1)) { folio_unlock(f); folio_put(f); continue; }
    f->flags &= ~(1UL << FOLIO_DIRTY);
    f->wb_blocks_ = f->dirty_blocks_ ? f->dirty_blocks_ : 0xFF; f->dirty_blocks_ = 0;   /* ビット未設定の dirty は全体扱い (安全側) */
    *err = 0;
    return f;
  }
  free(wbc->snap_); wbc->snap_ = NULL; wbc->nsnap_ = 0;
  *err = wbc->saved_err_;
  return NULL;
}

/* 同期 writeback: iomap の ioend は束ねず、1 範囲 = 1 bio をその場で書く。 */
int iomap_add_to_ioend(struct iomap_writepage_ctx *wpc, struct folio *f, loff_t pos, loff_t end_pos, unsigned len) {
  (void)end_pos;
  const struct iomap *im = &wpc->iomap;
  if (im->type == IOMAP_HOLE || im->type == IOMAP_UNWRITTEN) {
    /* 穴/未初期化の範囲: 読めばゼロなので、folio 上でもゼロなら書くものが無い (クラスタ < 4096 では 1 folio にデータと穴が混在し、
     * resident→non-resident 変換などで folio 全体が dirty になりうる)。ゼロ以外が載っているなら、捨てずにエラーにする。 */
    loff_t me = (loff_t)(im->offset + im->length);
    if (pos < (loff_t)im->offset || pos >= me) return -EIO;
    unsigned n = len; if (pos + (loff_t)n > me) n = (unsigned)(me - pos);
    size_t fo = offset_in_folio(f, pos); if (fo + n > PAGE_SIZE) n = (unsigned)(PAGE_SIZE - fo);
    const unsigned char *d = (const unsigned char *)f->data + fo;
    for (unsigned i = 0; i < n; i++) if (d[i]) return -EIO;
    return (int)n;
  }
  if (im->type != IOMAP_MAPPED || im->addr == IOMAP_NULL_ADDR || !im->bdev) return -EIO;
  loff_t map_end = (loff_t)(im->offset + im->length);
  if (pos < (loff_t)im->offset || pos >= map_end) return -EIO;
  if (pos + (loff_t)len > map_end) len = (unsigned)(map_end - pos);
  loff_t disk = (loff_t)im->addr + (pos - (loff_t)im->offset);
  size_t foff = offset_in_folio(f, pos);
  if (foff + len > PAGE_SIZE) len = (unsigned)(PAGE_SIZE - foff);
  struct bio *bio = bio_alloc(im->bdev, 1, REQ_OP_WRITE, 0);
  if (!bio) return -ENOMEM;
  bio->bi_iter.bi_sector = (sector_t)(disk >> SECTOR_SHIFT);
  bio_add_folio(bio, f, len, foff);
  int rc = submit_bio_wait(bio);
  bio_put(bio);
  return rc ? rc : (int)len;
}
int iomap_ioend_writeback_submit(struct iomap_writepage_ctx *wpc, int error) { (void)wpc; return error; }

/* 失敗した folio を dirty に戻す (dirty_blocks_ は呼び出し側が設定済みなので、全体 dirty にはしない) */
static void folio_mark_dirty_keep_(struct folio *f) { f->flags |= 1UL << FOLIO_DIRTY; }
int iomap_writepages(struct iomap_writepage_ctx *wpc) {
  struct inode *inode = wpc->inode;
  struct address_space *m = inode->i_mapping;
  unsigned bs = inode->i_sb->s_blocksize ? (unsigned)inode->i_sb->s_blocksize : 512;
  struct folio *f = NULL; int error = 0;
  while ((f = writeback_iter(m, wpc->wbc, f, &error))) {
    loff_t fpos = folio_pos(f), isize = inode->i_size;
    folio_start_writeback(f);
    error = 0;
    if (fpos < isize) {
      loff_t end_pos = fpos + PAGE_SIZE < isize ? fpos + PAGE_SIZE : isize;
      loff_t end_al = (end_pos + bs - 1) / bs * bs;
      /* dirty な 512B ブロックの連続区間ごとに、ブロック長 (bs) へ外側へ丸めて書き戻す。実カーネルの iomap と同様に、書いていない範囲
       * (穴・未初期化・別のエクステント) は要求しない。クラスタ < 4096 では 1 folio の中にそれらが混在する。 */
      loff_t done_to = fpos;   /* ここまでは処理済み (丸めで重なった区間を二重に書かない) */
      for (int b = 0; b < 8 && !error; b++) {
        if (!((f->wb_blocks_ >> b) & 1)) continue;
        int e = b; while (e + 1 < 8 && ((f->wb_blocks_ >> (e + 1)) & 1)) e++;
        loff_t a = fpos + (loff_t)b * 512, z = fpos + (loff_t)(e + 1) * 512;
        b = e;
        if (a >= end_pos) break;
        a = a / bs * bs; z = (z + bs - 1) / bs * bs;
        if (z > end_al) z = end_al;
        if (a < done_to) a = done_to;
        loff_t pos = a;
        while (pos < z) {
          ssize_t r = wpc->ops->writeback_range(wpc, f, (u64)pos, (unsigned)(z - pos), (u64)end_pos);
          if (r <= 0) { error = r < 0 ? (int)r : -EIO; break; }
          pos += r;
        }
        if (pos > done_to) done_to = pos;
      }
    }
    if (error) { f->dirty_blocks_ |= f->wb_blocks_; folio_mark_dirty_keep_(f); }   /* 失敗した folio は dirty のまま残す (データを失わない) */
    f->wb_blocks_ = 0;
    folio_end_writeback(f);
    folio_unlock(f);
  }
  return wpc->ops->writeback_submit(wpc, error);
}
int usbmem_mapping_writeback(struct address_space *m, int sync_all) {
  if (!m || !m->host || !m->a_ops || !m->a_ops->writepages || !usbmem_mapping_has_dirty(m)) return 0;
  struct writeback_control wbc = { .sync_mode = sync_all ? WB_SYNC_ALL : 0 };
  int rc = ((int (*)(struct address_space *, struct writeback_control *))m->a_ops->writepages)(m, &wbc);
  free(wbc.snap_);
  return rc;
}

/* ブロックデバイスのページキャッシュ (bd_mapping) の dirty 範囲を全てデバイスへ書く (buffer_head 系 FS のメタデータ)。
 * dirty は 512B 単位で追跡しているので、隣接する別の用途のブロック (ファイルデータなど) は上書きしない。
 * 失敗した folio は dirty のまま残し、最初のエラーを返す。flush は呼び出し側。 */
/* sel(off) が真の 512B ブロックだけを書く版 (exFAT: FAT・ビットマップとディレクトリエントリを別の段階で書くために使う)。選ばれなかったブロックは dirty のまま残す。 */
static int bdev_writeback_sel(struct block_device *bd, const struct usbmem_range *rs, int nr, int inside);
int usbmem_bdev_writeback_ranges(struct block_device *bd, const struct usbmem_range *rs, int nr, int inside) {
  return bdev_writeback_sel(bd, rs, nr, inside);
}
int usbmem_bdev_writeback(struct block_device *bd) { return bdev_writeback_sel(bd, NULL, 0, 0); }
int usbmem_bdev_dirty_ranges(struct block_device *bd, struct usbmem_range **out, size_t *np) {
  struct address_space *m = bd ? bd->bd_mapping : NULL;
  struct pcache *p = m ? m->pages : NULL;
  *out = NULL; *np = 0;
  if (!p) return 0;
  size_t cap = 0, n = 0; pgoff_t *snap = NULL;
  pthread_mutex_lock(&p->mu);
  for (int i = 0; i < NB; i++) for (struct folio *f = p->b[i]; f; f = f->hash_next_) if ((f->flags >> FOLIO_DIRTY) & 1) cap++;
  if (!cap) { pthread_mutex_unlock(&p->mu); return 0; }
  snap = malloc(cap * sizeof *snap);
  if (!snap) { pthread_mutex_unlock(&p->mu); return -ENOMEM; }
  for (int i = 0; i < NB; i++) for (struct folio *f = p->b[i]; f; f = f->hash_next_) if (((f->flags >> FOLIO_DIRTY) & 1) && n < cap) snap[n++] = f->index;
  pthread_mutex_unlock(&p->mu);
  qsort(snap, n, sizeof *snap, cmp_idx);
  struct usbmem_range *r = malloc((n * 8 + 1) * sizeof *r);
  if (!r) { free(snap); return -ENOMEM; }
  size_t cnt = 0;
  for (size_t k = 0; k < n; k++) {
    pthread_mutex_lock(&p->mu); struct folio *f = pc_find_locked(p, snap[k]); pthread_mutex_unlock(&p->mu);
    if (!f) continue;
    folio_lock(f);
    if ((f->flags >> FOLIO_DIRTY) & 1) {
      unsigned char bits = f->dirty_blocks_ ? f->dirty_blocks_ : 0xFF;
      u64 base = (u64)f->index << PAGE_SHIFT;
      for (int b = 0; b < 8; b++) if ((bits >> b) & 1) {
        u64 a = base + (u64)b * 512;
        if (cnt && r[cnt - 1].off + r[cnt - 1].len == a) r[cnt - 1].len += 512;
        else { r[cnt].off = a; r[cnt].len = 512; cnt++; }
      }
    }
    folio_unlock(f); folio_put(f);
  }
  free(snap);
  if (!cnt) { free(r); return 0; }
  *out = r; *np = cnt;
  return 0;
}
static int range_hit(const struct usbmem_range *rs, int nr, u64 a, u64 z) {   /* [a,z) が rs のどれかと重なるか */
  for (int i = 0; i < nr; i++) if (a < rs[i].off + rs[i].len && rs[i].off < z) return 1;
  return 0;
}
static int bdev_writeback_sel(struct block_device *bd, const struct usbmem_range *rs, int nr, int inside) {
  struct address_space *m = bd ? bd->bd_mapping : NULL;
  struct pcache *p = m ? m->pages : NULL;
  if (!p) return 0;
  size_t cap = 0, n = 0; pgoff_t *snap = NULL;
  pthread_mutex_lock(&p->mu);
  for (int i = 0; i < NB; i++) for (struct folio *f = p->b[i]; f; f = f->hash_next_) if ((f->flags >> FOLIO_DIRTY) & 1) cap++;
  if (!cap) { pthread_mutex_unlock(&p->mu); return 0; }
  snap = malloc(cap * sizeof *snap);
  if (!snap) { pthread_mutex_unlock(&p->mu); return -ENOMEM; }
  for (int i = 0; i < NB; i++) for (struct folio *f = p->b[i]; f; f = f->hash_next_) if (((f->flags >> FOLIO_DIRTY) & 1) && n < cap) snap[n++] = f->index;
  pthread_mutex_unlock(&p->mu);
  qsort(snap, n, sizeof *snap, cmp_idx);
  u64 dev_size = usbmem_bdev_size(bd->impl);
  int err = 0;
  for (size_t k = 0; k < n && !err; k++) {
    pthread_mutex_lock(&p->mu); struct folio *f = pc_find_locked(p, snap[k]); pthread_mutex_unlock(&p->mu);
    if (!f) continue;
    folio_lock(f);
    if ((f->flags >> FOLIO_DIRTY) & 1) {
      unsigned char bits = f->dirty_blocks_ ? f->dirty_blocks_ : 0xFF;
      u64 base = (u64)f->index << PAGE_SHIFT;
      unsigned char keep = 0;
      if (nr) for (int b = 0; b < 8; b++) if ((bits >> b) & 1) {
        int hit = range_hit(rs, nr, base + (u64)b * 512, base + (u64)b * 512 + 512);
        if (hit != inside) keep |= (unsigned char)(1u << b);   /* 選ばれなかったブロック */
      }
      bits &= (unsigned char)~keep;
      f->flags &= ~(1UL << FOLIO_DIRTY); f->dirty_blocks_ = 0;
      if (keep) { f->dirty_blocks_ = keep; f->flags |= 1UL << FOLIO_DIRTY; }
      for (int b = 0; b < 8 && !err; b++) {
        if (!((bits >> b) & 1)) continue;
        int e = b; while (e + 1 < 8 && ((bits >> (e + 1)) & 1)) e++;
        u64 a = base + (u64)b * 512, z = base + (u64)(e + 1) * 512;
        if (z > dev_size) z = dev_size;
        if (a < z) {
          int rc = usbmem_bdev_write(bd->impl, a, (char *)f->data + (a - base), (size_t)(z - a));
          if (rc) { err = rc; f->dirty_blocks_ |= (unsigned char)(bits & (0xFFu << b)); f->flags |= 1UL << FOLIO_DIRTY; }   /* 失敗した範囲以降は dirty に戻す */
        }
        b = e;
      }
    }
    folio_unlock(f); folio_put(f);
  }
  free(snap);
  return err;
}

/* ---- NLS / UTF (UTF-16LE <-> UTF-8) */
/* utf8 表: 上流 nls_utf8.c は大文字小文字の変換が恒等 (= FAT では大文字小文字を区別する) だが、ここでは ASCII の A-Z / a-z だけ変換する
 * (FAT の名前の照合は Windows と同じく大文字小文字を区別しない方がよい。UTF-8 の 2 バイト目以降 (0x80 以上) は変えないので、他の文字を取り違えない)。名前付きの表 (cp437 等) は register_nls で登録され、load_nls が名前で引く。未登録の名前は utf8 表 (従来どおり)。 */
static unsigned char g_nls_lower[256], g_nls_upper[256];
static int nls_utf8_uni2char(u16 uni, unsigned char *out, int boundlen) {
  if (boundlen <= 0) return -ENAMETOOLONG;
  u32 c = uni; int n = c < 0x80 ? 1 : c < 0x800 ? 2 : 3;
  if (c >= 0xD800 && c < 0xE000) { *out = '?'; return -EINVAL; }
  if (n > boundlen) return -ENAMETOOLONG;
  if (n == 1) out[0] = (unsigned char)c;
  else if (n == 2) { out[0] = 0xC0 | (c >> 6); out[1] = 0x80 | (c & 0x3F); }
  else { out[0] = 0xE0 | (c >> 12); out[1] = 0x80 | ((c >> 6) & 0x3F); out[2] = 0x80 | (c & 0x3F); }
  return n;
}
static int nls_utf8_char2uni(const unsigned char *raw, int boundlen, u16 *uni) {
  unicode_t u; int n = utf8_to_utf32(raw, boundlen, &u);
  if (n < 0 || u > 0xFFFF) { *uni = 0x003f; return -EINVAL; }
  *uni = (u16)u; return n;
}
static struct nls_table g_nls_default = { "utf8", nls_utf8_uni2char, (int (*)(const unsigned char *, int, void *))nls_utf8_char2uni, NULL, g_nls_lower, g_nls_upper, NULL, NULL };
static struct nls_table *g_nls_list;
static pthread_once_t g_nls_once = PTHREAD_ONCE_INIT;
static void nls_once(void) { for (int i = 0; i < 256; i++) { g_nls_lower[i] = (unsigned char)((i >= 'A' && i <= 'Z') ? i + 32 : i); g_nls_upper[i] = (unsigned char)((i >= 'a' && i <= 'z') ? i - 32 : i); } }
int register_nls(struct nls_table *t) { pthread_once(&g_nls_once, nls_once); t->next = g_nls_list; g_nls_list = t; return 0; }
int unregister_nls(struct nls_table *t) { for (struct nls_table **p = &g_nls_list; *p; p = &(*p)->next) if (*p == t) { *p = t->next; return 0; } return -EINVAL; }
struct nls_table *load_nls(const char *cs) {
  pthread_once(&g_nls_once, nls_once);
  for (struct nls_table *t = g_nls_list; cs && t; t = t->next) if (!strcmp(t->charset, cs)) return t;
  return &g_nls_default;
}
struct nls_table *load_nls_default(void) { pthread_once(&g_nls_once, nls_once); return &g_nls_default; }
void unload_nls(struct nls_table *t) { (void)t; }
int utf16s_to_utf8s(const void *in, int inlen, int endian, u8 *out, int maxout) {
  (void)endian; const u16 *s = in; int o = 0;
  for (int i = 0; i < inlen; i++) {
    u32 c = s[i];
    if (c == 0) break;   /* 上流 (lib/nls) と同じく NUL で終端する (fat は 255 文字ぶんのバッファを渡す) */
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < inlen && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00); i++; }
    else if (c >= 0xD800 && c < 0xE000) c = 0xFFFD;
    int n = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
    if (o + n > maxout) break;
    if (n == 1) out[o++] = (u8)c;
    else if (n == 2) { out[o++] = 0xC0 | (c >> 6); out[o++] = 0x80 | (c & 0x3F); }
    else if (n == 3) { out[o++] = 0xE0 | (c >> 12); out[o++] = 0x80 | ((c >> 6) & 0x3F); out[o++] = 0x80 | (c & 0x3F); }
    else { out[o++] = 0xF0 | (c >> 18); out[o++] = 0x80 | ((c >> 12) & 0x3F); out[o++] = 0x80 | ((c >> 6) & 0x3F); out[o++] = 0x80 | (c & 0x3F); }
  }
  return o;
}
int utf8s_to_utf16s(const u8 *in, int inlen, int endian, void *outv, int maxout) {
  (void)endian; u16 *out = outv; int o = 0;
  for (int i = 0; i < inlen;) {
    u32 c = in[i]; int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (!n || i + n > inlen) return -EINVAL;
    if (n == 1) c = in[i]; else {
      c &= (0xFF >> (n + 1));
      for (int k = 1; k < n; k++) { if ((in[i + k] & 0xC0) != 0x80) return -EINVAL; c = (c << 6) | (in[i + k] & 0x3F); }
    }
    /* 厳密検証: overlong / サロゲート / U+10FFFF 超を拒否 */
    if ((n == 2 && c < 0x80) || (n == 3 && c < 0x800) || (n == 4 && c < 0x10000) || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return -EINVAL;
    i += n;
    if (c >= 0x10000) { if (o + 2 > maxout) break; c -= 0x10000; out[o++] = 0xD800 + (c >> 10); out[o++] = 0xDC00 + (c & 0x3FF); }
    else { if (o + 1 > maxout) break; out[o++] = (u16)c; }
  }
  return o;
}

/* ---- ブロックデバイス自身のページキャッシュ (bd_mapping)。fs/ntfs の圧縮読み出しがデバイスのページを直接読む。 */
static int bdev_read_folio(struct file *file, struct folio *f) {
  (void)file;
  struct block_device *bd = f->mapping->host_bdev_;
  u64 off = (u64)f->index << PAGE_SHIFT, size = usbmem_bdev_size(bd->impl);
  memset(f->data, 0, PAGE_SIZE);
  int rc = 0;
  if (off < size) {
    size_t n = size - off < PAGE_SIZE ? (size_t)(size - off) : PAGE_SIZE;
    rc = usbmem_bdev_read(bd->impl, off, f->data, n);
  }
  if (!rc) f->flags |= 1UL << FOLIO_UPTODATE;
  folio_unlock(f);
  return rc;
}
static const struct address_space_operations g_bdev_aops = { .read_folio = (void *)bdev_read_folio };
void usbmem_bdev_mapping_init(struct block_device *bd) {
  bd->bd_data_.host_bdev_ = bd;
  bd->bd_data_.a_ops = &g_bdev_aops;
  pthread_rwlock_init(&bd->bd_data_.invalidate_lock.l, NULL);
  bd->bd_mapping = &bd->bd_data_;
}
void usbmem_bdev_mapping_free(struct block_device *bd) {
  truncate_inode_pages_final(&bd->bd_data_);
}
