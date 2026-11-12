// SPDX-License-Identifier: GPL-2.0-or-later
// erofs (Linux fs/erofs, 無改変) のホスト向け入口と、互換層の追加分 (idr / sysfs の空実装 など)。読み取り専用。
// 入口の構成は rt_vfs_squashfs.c と同じ: lookup が返した inode を「ピン」(参照を保持) して ino → inode の表を持つ (呼び出し側が任意の nid を渡しても
// ディスク上の任意の位置を inode として読まない)。i_ino は erofs の nid (64 ビットでは nid そのもの)。
#include <stdlib.h>
#include <compat/kernel.h>
#include <compat/usbmem_ntfs.h>

#include "usbmem/erofs_c.h"
#include "usbmem/ntfs_c.h"   /* usbmem_bdev_* の宣言 */

int usbmem_module_init_erofs(void);   /* super.c の module_init (USBMEM_MODULE_INIT_NAME)。共有ヘッダ usbmem_ntfs.h は触らずここで宣言 */

/* ---- 互換層の追加分 (more_erofs.h の本体) */
char *kstrndup(const char *s, size_t max, gfp_t gfp) {
  (void)gfp;
  size_t n = strnlen(s, max);
  char *p = malloc(n + 1);
  if (!p) return NULL;
  memcpy(p, s, n); p[n] = 0;
  return p;
}
ssize_t filemap_read(struct kiocb *iocb, struct iov_iter *to, ssize_t already) { (void)iocb; (void)to; (void)already; return -EOPNOTSUPP; }
int page_ref_count(const struct page *p) { return atomic_read(&p->folio_->refcount); }
void __free_page(struct page *p) { put_page(p); }
unsigned long alloc_pages_bulk(gfp_t gfp, unsigned long nr, struct page **pages) {
  unsigned long n = 0;
  for (unsigned long i = 0; i < nr; i++) {
    if (!pages[i]) { pages[i] = alloc_page(gfp); if (!pages[i]) break; }
    n++;
  }
  return n;
}

/* crc32c (スーパーブロックのチェックサム検証用)。ext4 側 (rt_ext4.c) にも同じ関数があるので weak にして、両方リンクしても衝突しないようにする。 */
__attribute__((weak)) u32 crc32c(u32 crc, const void *p, size_t len) {
  static uint32_t tab[256]; static int ready;
  if (!ready) {
    for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1; tab[i] = c; }
    ready = 1;
  }
  const unsigned char *b = p;
  while (len--) crc = tab[(crc ^ *b++) & 0xff] ^ (crc >> 8);
  return crc;
}

/* idr: 添字 = id の単純な配列 (追加デバイスは拒否されるので、ほぼ使われない) */
void idr_init(struct idr *i) { i->slots_ = NULL; i->n_ = 0; }
void idr_destroy(struct idr *i) { free(i->slots_); i->slots_ = NULL; i->n_ = 0; }
int idr_alloc(struct idr *i, void *ptr, int start, int end, gfp_t gfp) {
  (void)gfp;
  if (start < 0) start = 0;
  if (end <= 0) end = 1 << 16;
  int id = start;
  while (id < (int)i->n_ && i->slots_[id]) id++;
  if (id >= end) return -ENOSPC;
  if (id >= (int)i->n_) {
    void **ns = realloc(i->slots_, ((size_t)id + 1) * sizeof(void *));
    if (!ns) return -ENOMEM;
    for (unsigned k = i->n_; k <= (unsigned)id; k++) ns[k] = NULL;
    i->slots_ = ns; i->n_ = (unsigned)id + 1;
  }
  i->slots_[id] = ptr;
  return id;
}
void *idr_find(const struct idr *i, unsigned long id) { return id < i->n_ ? i->slots_[id] : NULL; }
int idr_for_each(const struct idr *i, int (*fn)(int id, void *p, void *data), void *data) {
  for (unsigned k = 0; k < i->n_; k++) if (i->slots_[k]) { int rc = fn((int)k, i->slots_[k], data); if (rc) return rc; }
  return 0;
}

/* sysfs / ファイルバック / page cache share は無効: 上流が参照するシンボルだけ空で用意する */
int erofs_register_sysfs(struct super_block *sb) { (void)sb; return 0; }
void erofs_unregister_sysfs(struct super_block *sb) { (void)sb; }
int erofs_init_sysfs(void) { return 0; }
void erofs_exit_sysfs(void) {}
const struct iomap_read_ops erofs_fileio_iomap_read_ops = { 0 };
const struct file_operations erofs_ishare_fops = { 0 };

/* ---- ホスト入口 */
struct pin { uint64_t ino; struct inode *inode; struct pin *next; };
struct usbmem_erofs { struct super_block *sb; uint64_t root_ino; struct pin *tab[1024]; };

static struct inode *pin_find(usbmem_erofs *v, uint64_t ino) {
  for (struct pin *p = v->tab[ino & 1023]; p; p = p->next) if (p->ino == ino) return p->inode;
  return NULL;
}
/* i の参照 (呼び出し側が持つ 1 つ) をピンへ移す。すでに同じ ino がピン済みなら、その参照を返して i を手放す。 */
static struct inode *pin_add(usbmem_erofs *v, struct inode *i) {
  struct inode *old = pin_find(v, i->i_ino);
  if (old) { iput(i); return old; }
  struct pin *p = malloc(sizeof *p);
  if (!p) { iput(i); return NULL; }
  p->ino = i->i_ino; p->inode = i; p->next = v->tab[i->i_ino & 1023]; v->tab[i->i_ino & 1023] = p;
  return i;
}

int usbmem_erofs_mount(struct usbmem_bdev *bdev, int ro, usbmem_erofs **out) {
  if (!ro) return -EROFS;   /* erofs は読み取り専用の形式 */
  struct block_device *bd = calloc(1, sizeof *bd);
  if (!bd) return -ENOMEM;
  bd->impl = bdev;
  usbmem_bdev_mapping_init(bd);
  struct super_block *sb = NULL;
  int rc = usbmem_compat_mount_fs(usbmem_module_init_erofs, bd, 1, &sb);
  if (rc) { usbmem_bdev_mapping_free(bd); free(bd); return rc; }
  usbmem_erofs *v = calloc(1, sizeof *v);
  if (!v) { usbmem_compat_umount(sb); usbmem_bdev_mapping_free(bd); free(bd); return -ENOMEM; }
  v->sb = sb;
  struct inode *root = sb->s_root->d_inode;
  v->root_ino = root->i_ino;
  ihold(root);
  if (!pin_add(v, root)) { usbmem_erofs_unmount(v); return -ENOMEM; }
  *out = v;
  return 0;
}

void usbmem_erofs_unmount(usbmem_erofs *v) {
  if (!v) return;
  for (int b = 0; b < 1024; b++) {
    struct pin *p = v->tab[b];
    while (p) { struct pin *n = p->next; iput(p->inode); free(p); p = n; }
    v->tab[b] = NULL;
  }
  struct block_device *bd = v->sb->s_bdev;
  usbmem_compat_umount(v->sb);
  usbmem_bdev_mapping_free(bd);
  free(bd);
  free(v);
}

uint64_t usbmem_erofs_root_ino(usbmem_erofs *v) { return v->root_ino; }

/* dir の直下の name を探し、見つかった inode をピンして返す (参照はピンが持つ)。 */
static int lookup_pin(usbmem_erofs *v, struct inode *dir, const char *name, size_t len, struct inode **out) {
  if (!S_ISDIR(dir->i_mode) || !dir->i_op || !dir->i_op->lookup) return -ENOTDIR;
  struct dentry *d = usbmem_d_alloc(v->sb, NULL, name, (unsigned)len);
  if (!d) return -ENOMEM;
  struct dentry *r = ((struct dentry *(*)(struct inode *, struct dentry *, unsigned))dir->i_op->lookup)(dir, d, 0);
  int rc = 0;
  if (IS_ERR(r)) rc = (int)PTR_ERR(r);
  else {
    struct dentry *use = r ? r : d;
    if (!use->d_inode) rc = -ENOENT;
    else { ihold(use->d_inode); struct inode *p = pin_add(v, use->d_inode); if (p) *out = p; else rc = -ENOMEM; }
    if (r && r != d) usbmem_d_free(r);
  }
  usbmem_d_free(d);
  return rc;
}

int usbmem_erofs_lookup(usbmem_erofs *v, uint64_t dir_ino, const char *name, size_t len, uint64_t *ino) {
  struct inode *dir = pin_find(v, dir_ino);
  if (!dir) return -ENOENT;
  struct inode *r = NULL;
  int rc = lookup_pin(v, dir, name, len, &r);
  if (!rc) *ino = r->i_ino;
  return rc;
}

int usbmem_erofs_stat_ino(usbmem_erofs *v, uint64_t ino, usbmem_erofs_stat *st) {
  struct inode *i = pin_find(v, ino);
  if (!i) return -ENOENT;
  st->ino = i->i_ino; st->size = (uint64_t)i->i_size; st->mode = i->i_mode; st->nlink = i->i_nlink;
  st->mtime_sec = i->i_mtime_.tv_sec; st->atime_sec = i->i_atime_.tv_sec; st->ctime_sec = i->i_ctime_.tv_sec;
  return 0;
}

struct rd_ctx { struct dir_context c; usbmem_erofs *v; struct inode *dir; usbmem_erofs_dir_cb cb; void *user; int err; };
static bool rd_actor(struct dir_context *c, const char *name, int len, loff_t pos, u64 ino, unsigned type) {
  (void)pos; (void)ino;
  struct rd_ctx *r = (struct rd_ctx *)c;
  if (len == 1 && name[0] == '.') return true;
  if (len == 2 && name[0] == '.' && name[1] == '.') return true;
  /* 名前で lookup して実 inode をピンする (一覧の各項目を stat できるように) */
  struct inode *i = NULL;
  int rc = lookup_pin(r->v, r->dir, name, (size_t)len, &i);
  if (rc) { r->err = rc; return false; }
  if (r->cb(r->user, name, len, i->i_ino, type)) return false;
  return true;
}
int usbmem_erofs_readdir(usbmem_erofs *v, uint64_t dir_ino, usbmem_erofs_dir_cb cb, void *user) {
  struct inode *dir = pin_find(v, dir_ino);
  if (!dir) return -ENOENT;
  if (!S_ISDIR(dir->i_mode) || !dir->i_fop || !dir->i_fop->iterate_shared) return -ENOTDIR;
  struct file f; memset(&f, 0, sizeof f);
  f.f_inode = dir; f.f_mapping = dir->i_mapping; f.f_mode = FMODE_READ;
  int rc = 0;
  if (dir->i_fop->open) rc = ((int (*)(struct inode *, struct file *))dir->i_fop->open)(dir, &f);
  if (!rc) {
    struct rd_ctx r = { { rd_actor, 0, NULL }, v, dir, cb, user, 0 };
    down_read(&dir->i_rwsem);
    rc = ((int (*)(struct file *, struct dir_context *))dir->i_fop->iterate_shared)(&f, &r.c);
    up_read(&dir->i_rwsem);
    if (!rc) rc = r.err;
    if (dir->i_fop->release) ((int (*)(struct inode *, struct file *))dir->i_fop->release)(dir, &f);
  }
  return rc;
}

int64_t usbmem_erofs_read(usbmem_erofs *v, uint64_t ino, uint64_t off, void *buf, size_t len) {
  struct inode *i = pin_find(v, ino);
  if (!i) return -ENOENT;
  if (S_ISDIR(i->i_mode)) return -EISDIR;
  if (!S_ISREG(i->i_mode) && !S_ISLNK(i->i_mode)) return -EINVAL;   /* 特殊ファイルの読み取りは意味がない */
  if (S_ISLNK(i->i_mode)) {
    /* シンボリックリンク: リンク先の文字列を返す (get_link: 短いものは inode 内、長いものはページキャッシュ経由) */
    if (!i->i_op || !i->i_op->get_link) return -EINVAL;
    struct dentry dummy; memset(&dummy, 0, sizeof dummy);
    struct delayed_call done = { NULL, NULL };
    const char *t = ((const char *(*)(struct dentry *, struct inode *, struct delayed_call *))i->i_op->get_link)(&dummy, i, &done);
    int64_t ret;
    if (IS_ERR(t)) ret = PTR_ERR(t);
    else if (!t) ret = -EIO;
    else {
      size_t tl = strnlen(t, (size_t)i->i_size < PAGE_SIZE ? (size_t)i->i_size : PAGE_SIZE);
      if (off >= tl) ret = 0;
      else { size_t n = tl - (size_t)off; if (n > len) n = len; memcpy(buf, t + off, n); ret = (int64_t)n; }
    }
    if (done.fn) done.fn(done.arg);
    return ret;
  }
  uint64_t size = (uint64_t)i->i_size;
  if (off >= size) return 0;
  if (len > size - off) len = (size_t)(size - off);
  size_t done = 0;
  while (done < len) {
    uint64_t pos = off + done;
    struct folio *f = read_mapping_folio(i->i_mapping, (pgoff_t)(pos >> PAGE_SHIFT), NULL);
    if (IS_ERR(f)) return done ? (int64_t)done : PTR_ERR(f);
    size_t po = (size_t)(pos & (PAGE_SIZE - 1)), n = PAGE_SIZE - po;
    if (n > len - done) n = len - done;
    memcpy((char *)buf + done, (char *)f->data + po, n);
    folio_put(f);
    done += n;
  }
  return (int64_t)done;
}
