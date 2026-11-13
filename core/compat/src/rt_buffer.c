// SPDX-License-Identifier: GPL-2.0-or-later
// buffer_head 系ファイルシステム (fs/ext2 など) のための VFS 周辺の実装 (docs/ext2-port.md)。
// 読み取りに使う: mpage_read_folio (get_block でファイルのブロックをディスクのブロックへ写してデバイスから読む)、iget_locked、bad inode、
// page_get_link / simple_get_link (シンボリックリンク)、rb_next / rb_prev。書き込み専用の API は「未実装として記録して -ENOSYS」。
#include <stdlib.h>
#include <compat/kernel.h>
#include <compat/usbmem_ntfs.h>
#include <linux/buffer_head.h>

struct user_namespace { int dummy; };
struct mnt_idmap { int dummy; };
struct user_namespace init_user_ns;
struct mnt_idmap nop_mnt_idmap;
const struct qstr dotdot_name = { .name = (const unsigned char *)"..", .len = 2 };
const struct qstr slash_name = { .name = (const unsigned char *)"/", .len = 1 };

/* ---- inode 取得: iget_locked(sb, ino) = iget5_locked で i_ino を比較する版 */
static int test_ino(struct inode *i, void *p) { return i->i_ino == (unsigned long)(uintptr_t)p; }
static int set_ino(struct inode *i, void *p) { i->i_ino = (unsigned long)(uintptr_t)p; return 0; }
struct inode *iget_locked(struct super_block *sb, unsigned long ino) {
  return iget5_locked(sb, ino, test_ino, set_ino, (void *)(uintptr_t)ino);
}
void make_bad_inode(struct inode *i) { i->i_flags |= 1u << 30; i->i_mode = 0100000 /* S_IFREG */; i->i_op = NULL; i->i_fop = NULL; }
bool is_bad_inode(struct inode *i) { return (i->i_flags >> 30) & 1; }
void iget_failed(struct inode *i) { make_bad_inode(i); unlock_new_inode(i); iput(i); }

/* ---- rbtree の走査 (rb_link_node + rb_insert_color は平衡化しない単純な二分木。順序走査は通常の親ポインタ辿り) */
#define RB_PARENT(n) ((struct rb_node *)((n)->__rb_parent_color & ~3UL))
struct rb_node *rb_next(const struct rb_node *n) {
  if (!n) return NULL;
  if (n->rb_right) { n = n->rb_right; while (n->rb_left) n = n->rb_left; return (struct rb_node *)n; }
  struct rb_node *p;
  while ((p = RB_PARENT(n)) && n == p->rb_right) n = p;
  return p;
}
struct rb_node *rb_prev(const struct rb_node *n) {
  if (!n) return NULL;
  if (n->rb_left) { n = n->rb_left; while (n->rb_right) n = n->rb_right; return (struct rb_node *)n; }
  struct rb_node *p;
  while ((p = RB_PARENT(n)) && n == p->rb_left) n = p;
  return p;
}

/* ---- mpage: ページキャッシュの 1 folio を、get_block が示すディスクのブロックから埋める。
 * 連続してマップされるブロックは 1 回のデバイス読み取りにまとめる (b_size に複数ブロックを返す get_block もある)。穴と EOF 以降はゼロ。 */
static int fill_folio_from_disk(struct folio *folio, get_block_t *get_block) {
  struct inode *inode = folio->mapping->host;
  unsigned bits = inode->i_blkbits, bs = 1u << bits;
  loff_t isize = i_size_read(inode);
  sector_t blk = (sector_t)folio->index << (PAGE_SHIFT - bits);
  size_t pos = 0;
  int err = 0;
  char *data = folio->data;
  while (pos < PAGE_SIZE) {
    loff_t fpos = ((loff_t)folio->index << PAGE_SHIFT) + (loff_t)pos;
    if (fpos >= isize) { memset(data + pos, 0, PAGE_SIZE - pos); break; }
    struct buffer_head bh;
    memset(&bh, 0, sizeof bh);
    bh.b_size = PAGE_SIZE - pos;
    err = get_block(inode, blk, &bh, 0);
    if (err) break;
    /* マップされたときだけ b_size は「連続して対応が付いた長さ」。穴 (未マップ) は 1 ブロックずつ進める (カーネルの mpage と同じ) */
    size_t n = buffer_mapped(&bh) ? bh.b_size - bh.b_size % bs : bs;
    if (n == 0) n = bs;
    if (n > PAGE_SIZE - pos) n = PAGE_SIZE - pos;
    if (buffer_mapped(&bh)) {
      err = usbmem_bdev_read(inode->i_sb->s_bdev->impl, (u64)bh.b_blocknr * bs, data + pos, n);
      if (err) { err = -EIO; break; }
    } else {
      memset(data + pos, 0, n);
    }
    pos += n; blk += n / bs;
  }
  if (!err) folio_mark_uptodate(folio);
  return err;
}
int mpage_read_folio(struct folio *folio, get_block_t *get_block) {
  int err = fill_folio_from_disk(folio, get_block);
  folio_unlock(folio);
  return err;
}
void mpage_readahead(struct readahead_control *rac, get_block_t *get_block) { (void)rac; (void)get_block; }

/* ---- シンボリックリンク */
const char *simple_get_link(struct dentry *d, struct inode *i, struct delayed_call *done) { (void)d; (void)done; return i->i_link; }
static void put_link_folio(void *arg) { folio_put((struct folio *)arg); }
const char *page_get_link(struct dentry *d, struct inode *i, struct delayed_call *done) {
  (void)d;
  struct folio *f = read_mapping_folio(i->i_mapping, 0, NULL);
  if (IS_ERR(f)) return (const char *)f;
  done->fn = put_link_folio; done->arg = f;
  nd_terminate_link(f->data, i->i_size, PAGE_SIZE - 1);
  return f->data;
}

/* ---- 書き込み (buffer_head 系 FS: ext2 など)。docs/ext2-write.md
 * ファイルデータ・ディレクトリは inode ごとのページキャッシュ (folio 4096B) に置き、512B 単位の dirty ビットで追跡する。
 * 本物のカーネルの buffer_head 層 (folio ごとの bh リスト) は持たない: 「どのブロックがディスクのどこか」は get_block を呼べばいつでも分かるので、
 * 書き込み準備 (__block_write_begin) で確保・ゼロ埋め、writeback (mpage_writepages) で get_block(create=0) を引いて書く。
 * メタデータ (ビットマップ・inode テーブル・間接ブロック) は rt_bh.c の bh (bd_mapping 上) で、usbmem_bdev_writeback が書く。 */
#define NOSYS(name) usbmem_unimplemented(name)

int block_dirty_folio(struct address_space *m, struct folio *f) {
  bool was = (f->flags >> FOLIO_DIRTY) & 1;
  folio_mark_dirty(f);
  if (m && m->host) m->host->i_state |= I_DIRTY_PAGES;
  return !was;
}
void block_invalidate_folio(struct folio *f, size_t o, size_t l) { (void)f; (void)o; (void)l; }
bool block_is_partially_uptodate(struct folio *f, size_t from, size_t count) { (void)f; (void)from; (void)count; return false; }
int buffer_migrate_folio(struct address_space *m, struct folio *d, struct folio *s, int mode) { (void)m; (void)d; (void)s; (void)mode; NOSYS("buffer_migrate_folio"); return -ENOSYS; }
int simple_fsync(struct file *f, loff_t s, loff_t e, int ds) { (void)f; (void)s; (void)e; (void)ds; return 0; }   /* 永続化は usbmem_compat_sync で明示的に行う */
int generic_file_mmap_prepare(void *desc) { (void)desc; NOSYS("generic_file_mmap_prepare"); return -ENOSYS; }
unsigned long thp_get_unmapped_area(struct file *f, unsigned long a, unsigned long l, unsigned long p, unsigned long fl) { (void)f; (void)a; (void)l; (void)p; (void)fl; return 0; }
loff_t generic_llseek_cookie(struct file *f, loff_t o, int w, u64 *c) { (void)f; (void)o; (void)w; (void)c; NOSYS("generic_llseek_cookie"); return -ENOSYS; }

int generic_block_bmap(struct address_space *m, sector_t block, get_block_t *get_block) {
  struct inode *inode = m->host;
  struct buffer_head bh; memset(&bh, 0, sizeof bh);
  bh.b_size = 1u << inode->i_blkbits;
  if (get_block(inode, block, &bh, 0)) return 0;
  return buffer_mapped(&bh) ? (int)bh.b_blocknr : 0;
}

/* 書き込みの準備 (Linux の __block_write_begin 相当)。[pos, pos+len) は folio 内に収まる。folio は lock 済み。
 *  1. folio が uptodate でなく、全面上書きでもないなら、まず現在の内容をディスクから読む (穴・EOF 以降はゼロ)。
 *  2. 範囲に触れる各ブロックを get_block(create=1) で確保。新規確保ブロックは、書かない部分をゼロにして**ブロック全体を dirty** にする
 *     (ディスクに残っていた古い内容が見えない・書き戻しで必ず上書きされる)。 */
int __block_write_begin(struct folio *folio, loff_t pos, unsigned len, get_block_t *get_block) {
  struct inode *inode = folio->mapping->host;
  unsigned bits = inode->i_blkbits, bs = 1u << bits;
  size_t from = (size_t)(pos & (PAGE_SIZE - 1)), to = from + len;
  if (to > PAGE_SIZE) return -EINVAL;
  if (!folio_test_uptodate(folio) && !(from == 0 && to == PAGE_SIZE)) {
    int e = fill_folio_from_disk(folio, get_block);
    if (e) return e;
  }
  if (!len) return 0;   /* 長さ 0 (FAT の拡張 truncate = fat_cont_expand_simple): ブロックの確保はしない。folio は上で読んでおく (write_end が uptodate にするため) */
  sector_t block = ((sector_t)folio->index << (PAGE_SHIFT - bits)) + (from >> bits);
  size_t b0 = from & ~(size_t)(bs - 1), b1 = (to + bs - 1) & ~(size_t)(bs - 1);
  char *data = folio->data;
  for (size_t off = b0; off < b1; off += bs, block++) {
    struct buffer_head bh; memset(&bh, 0, sizeof bh);
    bh.b_size = bs;
    int e = get_block(inode, block, &bh, 1);
    if (e) return e;
    if (buffer_new(&bh)) {
      if (from > off) memset(data + off, 0, from - off);
      if (to < off + bs) memset(data + to, 0, off + bs - to);
      usbmem_folio_dirty_range(folio, off, bs);
      inode->i_state |= I_DIRTY_PAGES;
    }
  }
  return 0;
}
int block_write_begin(struct address_space *m, loff_t pos, unsigned len, struct folio **foliop, get_block_t *get_block) {
  struct folio *f = filemap_grab_folio(m, (pgoff_t)(pos >> PAGE_SHIFT));
  if (IS_ERR(f)) return (int)PTR_ERR(f);
  int e = __block_write_begin(f, pos, len, get_block);
  if (e) { folio_unlock(f); folio_put(f); return e; }
  *foliop = f;
  return 0;
}
/* コピーした範囲を dirty にする。全面上書きで uptodate でなかった folio は、全部コピーできたときだけ uptodate にする。 */
int block_write_end(loff_t pos, unsigned len, unsigned copied, struct folio *folio) {
  size_t start = (size_t)(pos & (PAGE_SIZE - 1));
  if (copied < len && !folio_test_uptodate(folio)) {
    /* 全面上書きなのに途中までしかコピーできなかった (ホスト経路では起きない)。中身が不定なので書き戻さない。 */
    folio_clear_dirty(folio);
    return 0;
  }
  if (copied == len && start == 0 && len == PAGE_SIZE) folio_mark_uptodate(folio);
  else if (!folio_test_uptodate(folio)) folio_mark_uptodate(folio);   /* 部分書き込みは write_begin で全体を読んである */
  if (copied) {
    usbmem_folio_dirty_range(folio, start, copied);
    if (folio->mapping && folio->mapping->host) folio->mapping->host->i_state |= I_DIRTY_PAGES;
  }
  return (int)copied;
}
int generic_write_end(const struct kiocb *k, struct address_space *m, loff_t pos, unsigned len, unsigned copied, struct folio *folio, void *fs) {
  (void)k; (void)fs;
  struct inode *inode = m->host;
  copied = (unsigned)block_write_end(pos, len, copied, folio);
  int size_changed = 0;
  if (pos + (loff_t)copied > inode->i_size) { i_size_write(inode, pos + (loff_t)copied); size_changed = 1; }
  folio_unlock(folio); folio_put(folio);
  if (size_changed) mark_inode_dirty(inode);
  return (int)copied;
}

/* 1 つの連続バッファへの書き込み (ホスト API 用)。Linux の generic_perform_write と同じく write_begin → コピー → write_end を folio ごとに繰り返す。 */
ssize_t generic_perform_write(struct kiocb *iocb, struct iov_iter *i) {
  struct address_space *m = iocb->ki_filp->f_mapping;
  const struct address_space_operations *ops = m->a_ops;
  typedef int (*wb_t)(const struct kiocb *, struct address_space *, loff_t, unsigned, struct folio **, void **);
  typedef int (*we_t)(const struct kiocb *, struct address_space *, loff_t, unsigned, unsigned, struct folio *, void *);
  if (!ops || !ops->write_begin || !ops->write_end) return -EOPNOTSUPP;
  loff_t pos = iocb->ki_pos; ssize_t written = 0; int status = 0;
  while (iov_iter_count(i)) {
    size_t offset = (size_t)(pos & (PAGE_SIZE - 1)), bytes = PAGE_SIZE - offset;
    if (bytes > iov_iter_count(i)) bytes = iov_iter_count(i);
    struct folio *folio = NULL; void *fsdata = NULL;
    status = ((wb_t)ops->write_begin)(iocb, m, pos, (unsigned)bytes, &folio, &fsdata);
    if (status < 0) break;
    size_t copied = copy_folio_from_iter_atomic(folio, offset, bytes, i);
    status = ((we_t)ops->write_end)(iocb, m, pos, (unsigned)bytes, (unsigned)copied, folio, fsdata);
    if (status < 0) break;
    if (status == 0) { status = -EIO; break; }
    pos += status; written += status;
  }
  iocb->ki_pos = pos;
  return written ? written : status;
}
ssize_t generic_file_write_iter(struct kiocb *iocb, struct iov_iter *from) {
  struct inode *inode = iocb->ki_filp->f_mapping->host;
  inode_lock(inode);
  ssize_t ret = generic_write_checks(iocb, from);
  if (ret > 0) {
    ret = file_modified(iocb->ki_filp);
    if (!ret) ret = generic_perform_write(iocb, from);
  }
  inode_unlock(inode);
  return ret;
}

/* cont_write_begin (fs/buffer.c): ファイルの終端 *bytes (割り当て済みの長さ。FAT の mmu_private) より先へ書くとき、間をゼロで埋めてから書く (FAT は穴を持てない)。
 * 間の埋め込みは a_ops->write_begin / write_end をそのまま呼ぶ (呼び出し側 FS が割り当て・i_size 更新を行う)。 */
static int cont_expand_zero(const void *iocb, struct address_space *mapping, loff_t pos, loff_t *bytes) {
  struct inode *inode = mapping->host;
  unsigned blocksize = i_blocksize(inode);
  const struct address_space_operations *aops = mapping->a_ops;
  typedef int (*wb_t)(const struct kiocb *, struct address_space *, loff_t, unsigned, struct folio **, void **);
  typedef int (*we_t)(const struct kiocb *, struct address_space *, loff_t, unsigned, unsigned, struct folio *, void *);
  pgoff_t index = (pgoff_t)(pos >> PAGE_SHIFT), curidx;
  loff_t curpos;
  unsigned offset = (unsigned)(pos & (PAGE_SIZE - 1)), zerofrom, len;
  struct folio *folio; void *fsdata; int err = 0;
  while (index > (curidx = (pgoff_t)((curpos = *bytes) >> PAGE_SHIFT))) {
    zerofrom = (unsigned)(curpos & (PAGE_SIZE - 1));
    if (zerofrom & (blocksize - 1)) { *bytes |= (blocksize - 1); (*bytes)++; }
    len = (unsigned)PAGE_SIZE - zerofrom;
    err = ((wb_t)aops->write_begin)(iocb, mapping, curpos, len, &folio, &fsdata);
    if (err) return err;
    folio_zero_segment(folio, zerofrom, zerofrom + len);
    err = ((we_t)aops->write_end)(iocb, mapping, curpos, len, len, folio, fsdata);
    if (err < 0) return err;
    err = 0;
  }
  if (index == curidx) {   /* pos を含むページ */
    zerofrom = (unsigned)(curpos & (PAGE_SIZE - 1));
    if (offset <= zerofrom) return 0;   /* 拡張しない (最後のブロックは書き込みで埋まる) */
    if (zerofrom & (blocksize - 1)) { *bytes |= (blocksize - 1); (*bytes)++; }
    len = offset - zerofrom;
    err = ((wb_t)aops->write_begin)(iocb, mapping, curpos, len, &folio, &fsdata);
    if (err) return err;
    folio_zero_segment(folio, zerofrom, zerofrom + len);
    err = ((we_t)aops->write_end)(iocb, mapping, curpos, len, len, folio, fsdata);
    if (err < 0) return err;
    err = 0;
  }
  return err;
}
int cont_write_begin(const void *iocb, struct address_space *mapping, loff_t pos, unsigned len, struct folio **foliop, void **fsdata, void *get_block, loff_t *bytes) {
  struct inode *inode = mapping->host;
  unsigned blocksize = i_blocksize(inode);
  (void)fsdata;
  int err = cont_expand_zero(iocb, mapping, pos, bytes);
  if (err) return err;
  unsigned zerofrom = (unsigned)(*bytes & (PAGE_SIZE - 1));
  if (pos + len > *bytes && (zerofrom & (blocksize - 1))) { *bytes |= (blocksize - 1); (*bytes)++; }
  return block_write_begin(mapping, pos, len, foliop, (get_block_t *)get_block);
}

/* truncate / 拡張で、新しいサイズが含まれるブロックの末尾 (サイズ以降) をゼロにする (そのブロックが穴ならすることはない)。呼び出しは i_size を変える前。 */
int block_truncate_page(struct address_space *m, loff_t from, get_block_t *get_block) {
  struct inode *inode = m->host;
  unsigned bits = inode->i_blkbits, bs = 1u << bits;
  unsigned offset = (unsigned)(from & (bs - 1));
  if (!offset) return 0;
  struct buffer_head bh; memset(&bh, 0, sizeof bh);
  bh.b_size = bs;
  int e = get_block(inode, (sector_t)(from >> bits), &bh, 0);
  if (e) return e;
  if (!buffer_mapped(&bh)) return 0;
  struct folio *f = filemap_grab_folio(m, (pgoff_t)(from >> PAGE_SHIFT));
  if (IS_ERR(f)) return (int)PTR_ERR(f);
  if (!folio_test_uptodate(f)) { e = fill_folio_from_disk(f, get_block); if (e) { folio_unlock(f); folio_put(f); return e; } }
  size_t fo = (size_t)(from & (PAGE_SIZE - 1));
  folio_zero_segment(f, fo, fo + (bs - offset));
  usbmem_folio_dirty_range(f, fo - offset, bs);
  inode->i_state |= I_DIRTY_PAGES;
  folio_unlock(f); folio_put(f);
  return 0;
}

/* writeback: dirty な 512B 区間をブロック境界へ外側に丸め、get_block(create=0) でディスク位置を引いてデバイスへ書く。
 * 隣り合う連続ブロックは 1 回の書き込みにまとめる。EOF 以降 (ブロック境界に切り上げた後) は書かない。穴に dirty データがあれば捨てずにエラー。 */
int mpage_writepages(struct address_space *m, struct writeback_control *wbc, get_block_t *get_block) {
  struct inode *inode = m->host;
  unsigned bits = inode->i_blkbits, bs = 1u << bits;
  struct usbmem_bdev *bdev = inode->i_sb->s_bdev->impl;
  struct folio *f = NULL; int error = 0;
  while ((f = writeback_iter(m, wbc, f, &error))) {
    loff_t fpos = folio_pos(f), isize = inode->i_size;
    folio_start_writeback(f);
    error = 0;
    if (fpos < isize) {
      loff_t end_al = (isize + bs - 1) / bs * bs;
      if (end_al > fpos + (loff_t)PAGE_SIZE) end_al = fpos + (loff_t)PAGE_SIZE;
      loff_t done_to = fpos;
      for (int b = 0; b < 8 && !error; b++) {
        if (!((f->wb_blocks_ >> b) & 1)) continue;
        int e = b; while (e + 1 < 8 && ((f->wb_blocks_ >> (e + 1)) & 1)) e++;
        loff_t a = fpos + (loff_t)b * 512, z = fpos + (loff_t)(e + 1) * 512;
        b = e;
        a = a / bs * bs; z = (z + bs - 1) / bs * bs;
        if (z > end_al) z = end_al;
        if (a < done_to) a = done_to;
        loff_t pos = a;
        while (pos < z && !error) {
          struct buffer_head bh; memset(&bh, 0, sizeof bh);
          bh.b_size = (size_t)(z - pos);
          int ge = get_block(inode, (sector_t)(pos >> bits), &bh, 0);
          if (ge) { error = ge; break; }
          size_t n = buffer_mapped(&bh) ? bh.b_size - bh.b_size % bs : bs;
          if (n == 0) n = bs;
          if (n > (size_t)(z - pos)) n = (size_t)(z - pos);
          const char *src = (const char *)f->data + (size_t)(pos - fpos);
          if (buffer_mapped(&bh)) {
            if (usbmem_bdev_write(bdev, (u64)bh.b_blocknr * bs, src, n)) error = -EIO;
          } else {
            for (size_t k = 0; k < n; k++) if (src[k]) { error = -EIO; break; }   /* 穴に載っている非ゼロ = 書き先が無い */
          }
          pos += (loff_t)n;
        }
        if (pos > done_to) done_to = pos;
      }
    }
    if (error) { f->dirty_blocks_ |= f->wb_blocks_; f->flags |= 1UL << FOLIO_DIRTY; }   /* 失敗した folio は dirty のまま残す */
    f->wb_blocks_ = 0;
    folio_end_writeback(f);
    folio_unlock(f);
  }
  return error;
}

/* 長いシンボリックリンク: リンク先の文字列 (NUL を除く) をページキャッシュの先頭ページに書く。i_size = 長さ。 */
int page_symlink(struct inode *inode, const char *symname, int len) {
  struct address_space *m = inode->i_mapping;
  const struct address_space_operations *ops = m->a_ops;
  typedef int (*wb_t)(const struct kiocb *, struct address_space *, loff_t, unsigned, struct folio **, void **);
  typedef int (*we_t)(const struct kiocb *, struct address_space *, loff_t, unsigned, unsigned, struct folio *, void *);
  if (len < 1 || len > (int)PAGE_SIZE || !ops || !ops->write_begin || !ops->write_end) return -EINVAL;
  struct folio *f = NULL; void *fsdata = NULL;
  int err = ((wb_t)ops->write_begin)(NULL, m, 0, (unsigned)(len - 1), &f, &fsdata);
  if (err < 0) return err;
  memcpy(f->data, symname, (size_t)(len - 1));
  err = ((we_t)ops->write_end)(NULL, m, 0, (unsigned)(len - 1), (unsigned)(len - 1), f, fsdata);
  if (err < 0) return err;
  mark_inode_dirty(inode);
  return 0;
}
