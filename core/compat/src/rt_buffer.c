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
int mpage_read_folio(struct folio *folio, get_block_t *get_block) {
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

/* ---- 書き込み専用 API: 読み取り専用マウントでは呼ばれない。呼ばれたら記録して失敗させる */
#define NOSYS(name) usbmem_unimplemented(name)
int block_dirty_folio(struct address_space *m, struct folio *f) { (void)m; (void)f; NOSYS("block_dirty_folio"); return 0; }
void block_invalidate_folio(struct folio *f, size_t o, size_t l) { (void)f; (void)o; (void)l; }
bool block_is_partially_uptodate(struct folio *f, size_t from, size_t count) { (void)f; (void)from; (void)count; return false; }
int buffer_migrate_folio(struct address_space *m, struct folio *d, struct folio *s, int mode) { (void)m; (void)d; (void)s; (void)mode; NOSYS("buffer_migrate_folio"); return -ENOSYS; }
int generic_block_bmap(struct address_space *m, sector_t b, get_block_t *g) { (void)m; (void)b; (void)g; NOSYS("generic_block_bmap"); return 0; }
int simple_fsync(struct file *f, loff_t s, loff_t e, int ds) { (void)f; (void)s; (void)e; (void)ds; return 0; }
int generic_file_mmap_prepare(void *desc) { (void)desc; NOSYS("generic_file_mmap_prepare"); return -ENOSYS; }
unsigned long thp_get_unmapped_area(struct file *f, unsigned long a, unsigned long l, unsigned long p, unsigned long fl) { (void)f; (void)a; (void)l; (void)p; (void)fl; return 0; }
int page_symlink(struct inode *i, const char *n, int l) { (void)i; (void)n; (void)l; NOSYS("page_symlink"); return -ENOSYS; }
loff_t generic_llseek_cookie(struct file *f, loff_t o, int w, u64 *c) { (void)f; (void)o; (void)w; (void)c; NOSYS("generic_llseek_cookie"); return -ENOSYS; }
ssize_t generic_file_write_iter(struct kiocb *k, struct iov_iter *i) { (void)k; (void)i; NOSYS("generic_file_write_iter"); return -ENOSYS; }
ssize_t generic_perform_write(struct kiocb *k, struct iov_iter *i) { (void)k; (void)i; NOSYS("generic_perform_write"); return -ENOSYS; }
int generic_write_end(const struct kiocb *k, struct address_space *m, loff_t p, unsigned l, unsigned c, struct folio *f, void *fs) { (void)k; (void)m; (void)p; (void)l; (void)c; (void)f; (void)fs; NOSYS("generic_write_end"); return -ENOSYS; }
int block_write_begin(struct address_space *m, loff_t p, unsigned l, struct folio **f, get_block_t *g) { (void)m; (void)p; (void)l; (void)f; (void)g; NOSYS("block_write_begin"); return -ENOSYS; }
int __block_write_begin(struct folio *f, loff_t p, unsigned l, get_block_t *g) { (void)f; (void)p; (void)l; (void)g; NOSYS("__block_write_begin"); return -ENOSYS; }
int block_write_end(loff_t p, unsigned l, unsigned c, struct folio *f) { (void)p; (void)l; (void)c; (void)f; NOSYS("block_write_end"); return -ENOSYS; }
int block_truncate_page(struct address_space *m, loff_t f, get_block_t *g) { (void)m; (void)f; (void)g; NOSYS("block_truncate_page"); return -ENOSYS; }
int mpage_writepages(struct address_space *m, struct writeback_control *w, get_block_t *g) { (void)m; (void)w; (void)g; NOSYS("mpage_writepages"); return -ENOSYS; }
