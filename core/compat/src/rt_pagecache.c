// SPDX-License-Identifier: GPL-2.0-or-later
// LinuxCompat 実行時 (2/3): ページキャッシュ(folio)・block/bio・iomap(読み取り)・NLS。
// folio は常に PAGE_SIZE (4096) 固定。large folio は未対応。
#include <compat/kernel.h>
#include <compat/usbmem_ntfs.h>

#define NB 256
struct pcache { struct folio *b[NB]; pthread_mutex_t mu; };

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
  if (posix_memalign(&f->data, PAGE_SIZE, PAGE_SIZE)) { free(f); return NULL; }
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
static void folio_free(struct folio *f) { pthread_mutex_destroy(&f->lock_.m); free(f->data); free(f); }
/* 参照規約は Linux と同じ: キャッシュが 1、get 系 API が呼び出し側に +1。最後の put で解放。 */
void folio_put(struct folio *f) { if (atomic_dec_return(&f->refcount) == 0) folio_free(f); }
void folio_lock(struct folio *f) { pthread_mutex_lock(&f->lock_.m); f->flags |= 1UL << FOLIO_LOCKED; }
void folio_unlock(struct folio *f) { f->flags &= ~(1UL << FOLIO_LOCKED); pthread_mutex_unlock(&f->lock_.m); }
int folio_test_locked(const struct folio *f) { return (f->flags >> FOLIO_LOCKED) & 1; }
int folio_test_uptodate(const struct folio *f) { return (f->flags >> FOLIO_UPTODATE) & 1; }
int folio_test_writeback(const struct folio *f) { return (f->flags >> FOLIO_WRITEBACK) & 1; }
void folio_mark_uptodate(struct folio *f) { f->flags |= 1UL << FOLIO_UPTODATE; }
void folio_clear_uptodate(struct folio *f) { f->flags &= ~(1UL << FOLIO_UPTODATE); }
void folio_mark_dirty(struct folio *f) { f->flags |= 1UL << FOLIO_DIRTY; }
void folio_clear_dirty(struct folio *f) { f->flags &= ~(1UL << FOLIO_DIRTY); }
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
size_t offset_in_folio(const struct folio *f, loff_t pos) { (void)f; return (size_t)pos & (PAGE_SIZE - 1); }
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

/* folio を取得 (無ければ作る)。返す folio は未 lock / 未 uptodate。 */
static struct folio *get_or_create(struct address_space *m, pgoff_t idx) {
  struct pcache *p = pc_get(m);
  pthread_mutex_lock(&p->mu);
  struct folio *f = pc_find_locked(p, idx);
  if (!f) {
    f = folio_new(m, idx);
    if (f) { atomic_set(&f->refcount, 2); /* キャッシュ + 呼び出し側 */ f->hash_next_ = p->b[idx % NB]; p->b[idx % NB] = f; }
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
void truncate_inode_pages(struct address_space *m, loff_t from) {
  struct pcache *p = m->pages; if (!p) return;
  pgoff_t first = (from + PAGE_SIZE - 1) >> PAGE_SHIFT;
  pthread_mutex_lock(&p->mu);
  for (int i = 0; i < NB; i++) {
    struct folio **pp = &p->b[i];
    while (*pp) {
      struct folio *f = *pp;
      if (f->index >= first) { *pp = f->hash_next_; folio_put(f); /* キャッシュ参照を外す。使用中なら最後の put で解放 */ } else pp = &f->hash_next_;
    }
  }
  pthread_mutex_unlock(&p->mu);
}
void truncate_inode_pages_final(struct address_space *m) {
  truncate_inode_pages(m, 0);
  if (m->pages) { struct pcache *pc = m->pages; pthread_mutex_destroy(&pc->mu); free(pc); m->pages = NULL; }  /* mapping 終了: キャッシュ管理体も解放 */
}
void truncate_pagecache(struct inode *i, loff_t n) { truncate_inode_pages(i->i_mapping, n); }
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
int blkdev_issue_flush(struct block_device *b) { return usbmem_bdev_flush(b->impl); }
int bdev_rw_virt(struct block_device *b, sector_t sec, void *buf, size_t len, blk_opf_t opf) {
  return (opf & REQ_OP_MASK) == REQ_OP_WRITE ? usbmem_bdev_write(b->impl, sec << SECTOR_SHIFT, buf, len)
                                              : usbmem_bdev_read(b->impl, sec << SECTOR_SHIFT, buf, len);
}
int blk_status_to_errno(int s) { return s; }

struct bio *bio_alloc(struct block_device *b, unsigned short nr, blk_opf_t opf, gfp_t g) {
  (void)g;
  struct bio *bio = calloc(1, sizeof *bio);
  if (!bio) return NULL;
  bio->bi_io_vec = calloc(nr ? nr : 1, sizeof(struct bio_vec));
  bio->bi_max_vecs = nr ? nr : 1; bio->bi_bdev = b; bio->bi_opf = opf;
  return bio;
}
struct bio *bio_alloc_bioset(struct block_device *b, unsigned short nr, blk_opf_t opf, gfp_t g, struct bio_set *s) { (void)s; return bio_alloc(b, nr, opf, g); }
void bio_init(struct bio *bio, struct block_device *b, struct bio_vec *vecs, unsigned short nr, blk_opf_t opf) {
  memset(bio, 0, sizeof *bio); bio->bi_bdev = b; bio->bi_io_vec = vecs; bio->bi_max_vecs = nr; bio->bi_opf = opf;
}
void bio_uninit(struct bio *bio) { (void)bio; }
void bio_put(struct bio *bio) { if (bio) { free(bio->bi_io_vec); free(bio); } }
void bio_chain(struct bio *a, struct bio *b) { (void)a; (void)b; usbmem_unimplemented("bio_chain"); }
int bio_add_page(struct bio *bio, struct page *p, unsigned len, unsigned off) {
  if (bio->bi_vcnt >= bio->bi_max_vecs) return 0;
  bio->bi_io_vec[bio->bi_vcnt++] = (struct bio_vec){ p, len, off };
  bio->bi_iter.bi_size += len; return (int)len;
}
bool bio_add_folio(struct bio *bio, struct folio *f, size_t len, size_t off) { return bio_add_page(bio, &f->page, (unsigned)len, (unsigned)off) != 0; }
unsigned short bio_max_segs(unsigned n) { return n > 256 ? 256 : (unsigned short)n; }
sector_t bio_end_sector(const struct bio *bio) { return bio->bi_iter.bi_sector + (bio->bi_iter.bi_size >> SECTOR_SHIFT); }
int bioset_init(struct bio_set *s, unsigned a, unsigned b, int c) { (void)s; (void)a; (void)b; (void)c; return 0; }
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
  if (bio->bi_end_io) bio->bi_end_io(bio);
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
const struct iomap_read_ops iomap_bio_read_ops = { iomap_bio_read_folio_range, NULL, NULL };

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

/* ---- NLS / UTF (UTF-16LE <-> UTF-8) */
static struct nls_table g_nls_default = { "utf8", NULL, NULL };
struct nls_table *load_nls(const char *cs) { (void)cs; return &g_nls_default; }
struct nls_table *load_nls_default(void) { return &g_nls_default; }
void unload_nls(struct nls_table *t) { (void)t; }
int utf16s_to_utf8s(const void *in, int inlen, int endian, u8 *out, int maxout) {
  (void)endian; const u16 *s = in; int o = 0;
  for (int i = 0; i < inlen; i++) {
    u32 c = s[i];
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
