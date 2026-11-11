// SPDX-License-Identifier: GPL-2.0-or-later
// ホスト向け VFS 風入口 (exFAT・読み取り専用)。Linux の VFS が行う「lookup → i_op->lookup / i_fop->iterate_shared / read_mapping_folio」を直接呼ぶ。
// NTFS (rt_vfs.c) との違い: exFAT の inode 番号は iunique() で実行時に採番され、ino から inode を引く iget(sb, ino) が無い。
// そこで lookup / readdir が返した inode を「ピン」(参照を保持) して ino → inode の表を持つ。unmount で全て解放する。
#include <stdlib.h>
#include <compat/kernel.h>
#include <compat/usbmem_ntfs.h>

#include "usbmem/exfat_c.h"
#include "usbmem/ntfs_c.h"   /* usbmem_bdev_* の宣言 */

#define EXFAT_ROOT_INO 1

struct pin { uint64_t ino; struct inode *inode; struct pin *next; };
struct usbmem_exfat { struct super_block *sb; struct pin *tab[1024]; };

static struct inode *pin_find(usbmem_exfat *v, uint64_t ino) {
  for (struct pin *p = v->tab[ino & 1023]; p; p = p->next) if (p->ino == ino) return p->inode;
  return NULL;
}
/* i の参照 (呼び出し側が持つ 1 つ) をピンへ移す。すでに同じ ino がピン済みなら、その参照を返して i を手放す。 */
static struct inode *pin_add(usbmem_exfat *v, struct inode *i) {
  struct inode *old = pin_find(v, i->i_ino);
  if (old) { iput(i); return old; }
  struct pin *p = malloc(sizeof *p);
  if (!p) { iput(i); return NULL; }
  p->ino = i->i_ino; p->inode = i; p->next = v->tab[i->i_ino & 1023]; v->tab[i->i_ino & 1023] = p;
  return i;
}

int usbmem_exfat_mount(struct usbmem_bdev *bdev, int ro, usbmem_exfat **out) {
  if (!ro) return -EROFS;   /* 書き込みは未対応 */
  struct block_device *bd = calloc(1, sizeof *bd);
  if (!bd) return -ENOMEM;
  bd->impl = bdev;
  usbmem_bdev_mapping_init(bd);
  struct super_block *sb = NULL;
  int rc = usbmem_compat_mount_fs(usbmem_module_init_exfat, bd, 1, &sb);
  if (rc) { usbmem_bdev_mapping_free(bd); free(bd); return rc; }
  usbmem_exfat *v = calloc(1, sizeof *v);
  if (!v) { usbmem_compat_umount(sb); usbmem_bdev_mapping_free(bd); free(bd); return -ENOMEM; }
  v->sb = sb;
  struct inode *root = sb->s_root->d_inode;
  ihold(root);
  if (!pin_add(v, root)) { usbmem_exfat_unmount(v); return -ENOMEM; }
  *out = v;
  return 0;
}

void usbmem_exfat_unmount(usbmem_exfat *v) {
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

/* dir の直下の name を探し、見つかった inode をピンして返す (参照はピンが持つ)。 */
static int lookup_pin(usbmem_exfat *v, struct inode *dir, const char *name, size_t len, struct inode **out) {
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

int usbmem_exfat_lookup(usbmem_exfat *v, uint64_t dir_ino, const char *name, size_t len, uint64_t *ino) {
  struct inode *dir = pin_find(v, dir_ino);
  if (!dir) return -ENOENT;
  struct inode *r = NULL;
  int rc = lookup_pin(v, dir, name, len, &r);
  if (!rc) *ino = r->i_ino;
  return rc;
}

int usbmem_exfat_stat_ino(usbmem_exfat *v, uint64_t ino, usbmem_exfat_stat *st) {
  struct inode *i = pin_find(v, ino);
  if (!i) return -ENOENT;
  st->ino = i->i_ino; st->size = (uint64_t)i->i_size; st->mode = i->i_mode; st->nlink = i->i_nlink;
  st->mtime_sec = i->i_mtime_.tv_sec; st->atime_sec = i->i_atime_.tv_sec; st->ctime_sec = i->i_ctime_.tv_sec;
  return 0;
}

struct rd_ctx { struct dir_context c; usbmem_exfat *v; struct inode *dir; usbmem_exfat_dir_cb cb; void *user; int err; };
static bool rd_actor(struct dir_context *c, const char *name, int len, loff_t pos, u64 ino, unsigned type) {
  (void)pos; (void)ino;   /* dir_emit の ino は inode が未キャッシュだと仮採番 (iunique) なので使わず、名前で lookup して実 inode をピンする */
  struct rd_ctx *r = (struct rd_ctx *)c;
  if (len == 1 && name[0] == '.') return true;
  if (len == 2 && name[0] == '.' && name[1] == '.') return true;
  struct inode *i = NULL;
  int rc = lookup_pin(r->v, r->dir, name, (size_t)len, &i);
  if (rc) { r->err = rc; return false; }
  if (r->cb(r->user, name, len, i->i_ino, type)) return false;
  return true;
}
int usbmem_exfat_readdir(usbmem_exfat *v, uint64_t dir_ino, usbmem_exfat_dir_cb cb, void *user) {
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

int64_t usbmem_exfat_read(usbmem_exfat *v, uint64_t ino, uint64_t off, void *buf, size_t len) {
  struct inode *i = pin_find(v, ino);
  if (!i) return -ENOENT;
  if (S_ISDIR(i->i_mode)) return -EISDIR;
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
