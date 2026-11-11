// SPDX-License-Identifier: GPL-2.0-or-later
// ホスト向け VFS 風入口。Linux の VFS が行う「iget → i_op->lookup / i_fop->iterate_shared / read_mapping_folio」を直接呼ぶ。
#include <compat/kernel.h>
#include <compat/usbmem_ntfs.h>

#include "usbmem/ntfs_c.h"
#include "inode.h"   /* ntfs_iget (fs/ntfs, -iquote で解決) */
#include "ntfs.h"    /* ntfs_set_volume_flags / NTFS_SB */

struct usbmem_ntfs { struct super_block *sb; int dirty_marked; };

/* 書き込みの前提: **最初の変更の前に** VOLUME_IS_DIRTY をメディアへ確定 (書き込み + デバイス flush) する。
 * 順序を保証しない USB キャッシュが他の更新だけ先に反映して電源断になっても、ダーティ印が残れば次回 Windows が chkdsk を走らせられる。
 * (上流の ntfs_set_volume_flags は非同期で、フラグが他の更新より後にメディアへ届きうる。) 失敗時は書き込みを拒否する。 */
static int begin_write(usbmem_ntfs *v) {
  if (sb_rdonly(v->sb)) return -EROFS;
  if (v->dirty_marked) return 0;
  int rc = ntfs_set_volume_flags(NTFS_SB(v->sb), VOLUME_IS_DIRTY);
  if (!rc) rc = usbmem_compat_sync(v->sb);
  if (!rc) v->dirty_marked = 1;
  return rc;
}

int usbmem_ntfs_mount(struct usbmem_bdev *bdev, int ro, usbmem_ntfs **out) {
#ifndef USBMEM_NTFS_WRITE
  if (!ro) return -EROFS;   /* 書き込みはビルドで無効 */
#endif
  struct block_device *bd = calloc(1, sizeof *bd);
  if (!bd) return -ENOMEM;
  bd->impl = bdev;
  usbmem_bdev_mapping_init(bd);
  struct super_block *sb = NULL;
  int rc = usbmem_compat_mount(bd, ro, &sb);
  if (rc) { usbmem_bdev_mapping_free(bd); free(bd); return rc; }
  if (!ro && !sb_rdonly(sb) && NTFS_SB(sb)->cluster_size < 4096) {
    /* 書き込み経路 (rt_write.c) は「1 folio (4096B) が 1 クラスタに収まる」ことを前提にする。クラスタ < 4096 は読み取り専用でのみ扱う */
    usbmem_compat_umount(sb); usbmem_bdev_mapping_free(bd); free(bd);
    return -EROFS;
  }
  /* 書き込み可能なマウントでは、保留中の空きクラスタ数の計算 (全 $Bitmap の読み取り) をここで済ませる。
   * 最初の書き込み (割り当て) の中で突然数分止まるより、時間のかかる場所を「マウント」に固定する方が安全で分かりやすい。
   * 読み取り専用のマウントでは実行しない (空きクラスタ数は読み取りに不要。7 TiB の USB ディスクで 3 分以上かかる)。 */
  if (!ro && !sb_rdonly(sb)) usbmem_compat_run_deferred_work();
  usbmem_ntfs *v = calloc(1, sizeof *v);
  v->sb = sb; *out = v; return 0;
}
/* 書き込み可能でマウントされているか (rw 要求でも、上流が異常を見つけて読み取り専用に落ちていることがある) */
int usbmem_ntfs_writable(usbmem_ntfs *v) { return v && !sb_rdonly(v->sb); }
void usbmem_ntfs_unmount(usbmem_ntfs *v) {
  if (!v) return;
  struct block_device *bd = v->sb->s_bdev;
  usbmem_compat_umount(v->sb);
  usbmem_bdev_mapping_free(bd);
  free(bd);
  free(v);
}

static struct inode *get_inode(usbmem_ntfs *v, uint64_t ino, int *err) {
  struct inode *i = ntfs_iget(v->sb, ino);
  if (IS_ERR(i)) { *err = (int)PTR_ERR(i); return NULL; }
  return i;
}

int usbmem_ntfs_lookup(usbmem_ntfs *v, uint64_t dir_ino, const char *name, size_t len, uint64_t *ino) {
  int err = 0; struct inode *dir = get_inode(v, dir_ino, &err);
  if (!dir) return err;
  if (!S_ISDIR(dir->i_mode) || !dir->i_op || !dir->i_op->lookup) { iput(dir); return -ENOTDIR; }
  struct dentry *d = usbmem_d_alloc(v->sb, NULL, name, (unsigned)len);
  struct dentry *r = ((struct dentry *(*)(struct inode *, struct dentry *, unsigned))dir->i_op->lookup)(dir, d, 0);
  int rc = 0;
  if (IS_ERR(r)) rc = (int)PTR_ERR(r);
  else {
    struct dentry *use = r ? r : d;
    if (!use->d_inode) rc = -ENOENT; else *ino = use->d_inode->i_ino;
    if (r && r != d) usbmem_d_free(r);
  }
  usbmem_d_free(d);
  iput(dir);
  return rc;
}

int usbmem_ntfs_stat_ino(usbmem_ntfs *v, uint64_t ino, usbmem_ntfs_stat *st) {
  int err = 0; struct inode *i = get_inode(v, ino, &err);
  if (!i) return err;
  st->ino = i->i_ino; st->size = (uint64_t)i->i_size; st->mode = i->i_mode; st->nlink = i->i_nlink;
  st->mtime_sec = i->i_mtime_.tv_sec; st->atime_sec = i->i_atime_.tv_sec; st->ctime_sec = i->i_ctime_.tv_sec;
  iput(i); return 0;
}

struct rd_ctx { struct dir_context c; usbmem_ntfs_dir_cb cb; void *user; int stop; };
static bool rd_actor(struct dir_context *c, const char *name, int len, loff_t pos, u64 ino, unsigned type) {
  (void)pos; struct rd_ctx *r = (struct rd_ctx *)c;
  if (r->cb(r->user, name, len, ino, type)) { r->stop = 1; return false; }
  return true;
}
int usbmem_ntfs_readdir(usbmem_ntfs *v, uint64_t dir_ino, usbmem_ntfs_dir_cb cb, void *user) {
  int err = 0; struct inode *dir = get_inode(v, dir_ino, &err);
  if (!dir) return err;
  if (!S_ISDIR(dir->i_mode) || !dir->i_fop || !dir->i_fop->iterate_shared) { iput(dir); return -ENOTDIR; }
  struct file f; memset(&f, 0, sizeof f);
  f.f_inode = dir; f.f_mapping = dir->i_mapping; f.f_mode = FMODE_READ;
  int rc = 0;
  if (dir->i_fop->open) rc = ((int (*)(struct inode *, struct file *))dir->i_fop->open)(dir, &f);
  if (!rc) {
    struct rd_ctx r = { { rd_actor, 0, NULL }, cb, user, 0 };
    down_read(&dir->i_rwsem);
    rc = ((int (*)(struct file *, struct dir_context *))dir->i_fop->iterate_shared)(&f, &r.c);
    up_read(&dir->i_rwsem);
    if (dir->i_fop->release) ((int (*)(struct inode *, struct file *))dir->i_fop->release)(dir, &f);
  }
  iput(dir); return rc;
}

int64_t usbmem_ntfs_read(usbmem_ntfs *v, uint64_t ino, uint64_t off, void *buf, size_t len) {
  int err = 0; struct inode *i = get_inode(v, ino, &err);
  if (!i) return err;
  if (S_ISDIR(i->i_mode)) { iput(i); return -EISDIR; }
  uint64_t size = (uint64_t)i->i_size;
  if (off >= size) { iput(i); return 0; }
  if (len > size - off) len = (size_t)(size - off);
  size_t done = 0;
  while (done < len) {
    uint64_t pos = off + done;
    struct folio *f = read_mapping_folio(i->i_mapping, (pgoff_t)(pos >> PAGE_SHIFT), NULL);
    if (IS_ERR(f)) { iput(i); return done ? (int64_t)done : PTR_ERR(f); }
    size_t po = (size_t)(pos & (PAGE_SIZE - 1)), n = PAGE_SIZE - po;
    if (n > len - done) n = len - done;
    memcpy((char *)buf + done, (char *)f->data + po, n);
    folio_put(f);
    done += n;
  }
  iput(i); return (int64_t)done;
}

int usbmem_ntfs_sync(usbmem_ntfs *v) { return v ? usbmem_compat_sync(v->sb) : -EINVAL; }

int64_t usbmem_ntfs_write_inplace(usbmem_ntfs *v, uint64_t ino, uint64_t off, const void *buf, size_t len) {
  { int e = begin_write(v); if (e) return e; }
  int err = 0; struct inode *i = get_inode(v, ino, &err);
  if (!i) return err;
  if (S_ISDIR(i->i_mode)) { iput(i); return -EISDIR; }
  struct ntfs_inode *ni = NTFS_I(i);
  if (!NInoNonResident(ni) || NInoCompressed(ni) || NInoSparse(ni) || NInoEncrypted(ni) || NInoWofCompressed(ni)) { iput(i); return -EOPNOTSUPP; }
  if (off + len > (uint64_t)i->i_size) { iput(i); return -EINVAL; }
  size_t done = 0;
  while (done < len) {
    uint64_t pos = off + done;
    struct folio *f = read_mapping_folio(i->i_mapping, (pgoff_t)(pos >> PAGE_SHIFT), NULL);
    if (IS_ERR(f)) { iput(i); return done ? (int64_t)done : PTR_ERR(f); }
    size_t po = (size_t)(pos & (PAGE_SIZE - 1)), n = PAGE_SIZE - po;
    if (n > len - done) n = len - done;
    folio_lock(f);
    memcpy((char *)f->data + po, (const char *)buf + done, n);
    iomap_dirty_folio(i->i_mapping, f);
    folio_unlock(f); folio_put(f);
    done += n;
  }
  iput(i); return (int64_t)done;
}

int64_t usbmem_ntfs_write(usbmem_ntfs *v, uint64_t ino, uint64_t off, const void *buf, size_t len) {
  { int e = begin_write(v); if (e) return e; }
  int err = 0; struct inode *i = get_inode(v, ino, &err);
  if (!i) return err;
  if (S_ISDIR(i->i_mode)) { iput(i); return -EISDIR; }
  if (!i->i_fop || !i->i_fop->write_iter) { iput(i); return -EOPNOTSUPP; }
  struct file f; memset(&f, 0, sizeof f);
  f.f_inode = i; f.f_mapping = i->i_mapping; f.f_mode = FMODE_READ | FMODE_WRITE;
  int rc = 0;
  if (i->i_fop->open) rc = ((int (*)(struct inode *, struct file *))i->i_fop->open)(i, &f);
  int64_t ret = rc;
  if (!rc) {
    struct kiocb kio = { .ki_filp = &f, .ki_pos = (loff_t)off, .ki_flags = 0 };
    struct iov_iter it = { .buf = buf, .count = len };
    ret = ((ssize_t (*)(struct kiocb *, struct iov_iter *))i->i_fop->write_iter)(&kio, &it);
    if (i->i_fop->release) ((int (*)(struct inode *, struct file *))i->i_fop->release)(i, &f);
  }
  iput(i); return ret;
}

/* ファイルサイズの変更 (SAF の "w" = 切り詰め)。圧縮・暗号化ファイルは上流が -EOPNOTSUPP。 */
int usbmem_ntfs_truncate(usbmem_ntfs *v, uint64_t ino, uint64_t size) {
  { int e = begin_write(v); if (e) return e; }
  int err = 0; struct inode *i = get_inode(v, ino, &err);
  if (!i) return err;
  if (S_ISDIR(i->i_mode)) { iput(i); return -EISDIR; }
  if (!i->i_op || !i->i_op->setattr) { iput(i); return -EOPNOTSUPP; }
  if ((uint64_t)i->i_size == size) { iput(i); return 0; }
  struct dentry *d = usbmem_d_alloc(i->i_sb, NULL, "t", 1);
  if (!d) { iput(i); return -ENOMEM; }
  ihold(i); d->d_inode = i;   /* usbmem_d_free が d の inode を 1 つ iput する */
  struct iattr a; memset(&a, 0, sizeof a);
  a.ia_valid = ATTR_SIZE; a.ia_size = (loff_t)size;
  down_write(&i->i_rwsem);
  int rc = ((int (*)(const struct mnt_idmap *, struct dentry *, struct iattr *))i->i_op->setattr)(NULL, d, &a);
  up_write(&i->i_rwsem);
  if (!rc) { struct timespec64 t = current_time(i); inode_set_mtime_to_ts(i, t); inode_set_ctime_to_ts(i, t); mark_inode_dirty(i); }
  usbmem_d_free(d); iput(i);
  return rc;
}

/* ---- 名前空間の変更: VFS の vfs_create / vfs_mkdir / vfs_unlink / vfs_rmdir / vfs_rename 相当 */
static int lookup_d(struct inode *dir, const char *name, size_t len, struct dentry **out) {
  if (!S_ISDIR(dir->i_mode) || !dir->i_op || !dir->i_op->lookup) return -ENOTDIR;
  if (!len || len > 255 || memchr(name, '/', len) || (len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.')) return -EINVAL;
  struct dentry *d = usbmem_d_alloc(dir->i_sb, NULL, name, (unsigned)len);
  if (!d) return -ENOMEM;
  struct dentry *r = ((struct dentry *(*)(struct inode *, struct dentry *, unsigned))dir->i_op->lookup)(dir, d, 0);
  if (IS_ERR(r)) { int e = (int)PTR_ERR(r); usbmem_d_free(d); return e; }
  if (r && r != d) { usbmem_d_free(d); d = r; }
  *out = d; return 0;
}
static int ns_op(usbmem_ntfs *v, int kind, uint64_t dir_ino, const char *name, size_t len, uint32_t mode, uint64_t *ino) {
  { int e = begin_write(v); if (e) return e; }
  int err = 0; struct inode *dir = get_inode(v, dir_ino, &err);
  if (!dir) return err;
  struct dentry *d = NULL;
  int rc = lookup_d(dir, name, len, &d);
  if (rc) { iput(dir); return rc; }
  down_write(&dir->i_rwsem);
  switch (kind) {
  case 0: case 1:   /* create / mkdir */
    if (d->d_inode) { rc = -EEXIST; break; }
    if (kind == 0) {
      if (!dir->i_op->create) { rc = -EOPNOTSUPP; break; }
      rc = ((int (*)(const struct mnt_idmap *, struct inode *, struct dentry *, umode_t))dir->i_op->create)(NULL, dir, d, (umode_t)(mode & 07777));
    } else {
      if (!dir->i_op->mkdir) { rc = -EOPNOTSUPP; break; }
      struct dentry *r = ((struct dentry *(*)(const struct mnt_idmap *, struct inode *, struct dentry *, umode_t))dir->i_op->mkdir)(NULL, dir, d, (umode_t)(S_IFDIR | (mode & 07777)));
      rc = IS_ERR(r) ? (int)PTR_ERR(r) : 0;
      if (!rc && r && r != d) { usbmem_d_free(d); d = r; }
    }
    if (!rc) { if (!d->d_inode) rc = -EIO; else if (ino) *ino = d->d_inode->i_ino; }
    break;
  case 2: case 3:   /* unlink / rmdir */
    if (!d->d_inode) { rc = -ENOENT; break; }
    if (kind == 2 && S_ISDIR(d->d_inode->i_mode)) { rc = -EISDIR; break; }
    if (kind == 3 && !S_ISDIR(d->d_inode->i_mode)) { rc = -ENOTDIR; break; }
    if (kind == 2) rc = dir->i_op->unlink ? ((int (*)(struct inode *, struct dentry *))dir->i_op->unlink)(dir, d) : -EOPNOTSUPP;
    else rc = dir->i_op->rmdir ? ((int (*)(struct inode *, struct dentry *))dir->i_op->rmdir)(dir, d) : -EOPNOTSUPP;
    break;
  }
  up_write(&dir->i_rwsem);
  usbmem_d_free(d);   /* d の inode を iput: 削除済み (nlink 0) ならここで evict される */
  iput(dir);
  return rc;
}
int usbmem_ntfs_create(usbmem_ntfs *v, uint64_t d, const char *n, size_t l, uint32_t m, uint64_t *ino) { return ns_op(v, 0, d, n, l, m, ino); }
int usbmem_ntfs_mkdir(usbmem_ntfs *v, uint64_t d, const char *n, size_t l, uint32_t m, uint64_t *ino) { return ns_op(v, 1, d, n, l, m, ino); }
int usbmem_ntfs_unlink(usbmem_ntfs *v, uint64_t d, const char *n, size_t l) { return ns_op(v, 2, d, n, l, 0, NULL); }
int usbmem_ntfs_rmdir(usbmem_ntfs *v, uint64_t d, const char *n, size_t l) { return ns_op(v, 3, d, n, l, 0, NULL); }
int usbmem_ntfs_rename(usbmem_ntfs *v, uint64_t od, const char *on, size_t ol, uint64_t nd, const char *nn, size_t nl) {
  { int e = begin_write(v); if (e) return e; }
  int err = 0; struct inode *odir = get_inode(v, od, &err); if (!odir) return err;
  struct inode *ndir = (nd == od) ? (ihold(odir), odir) : get_inode(v, nd, &err);
  if (!ndir) { iput(odir); return err; }
  struct dentry *od_ = NULL, *nd_ = NULL;
  int rc = lookup_d(odir, on, ol, &od_);
  if (!rc) { rc = lookup_d(ndir, nn, nl, &nd_); if (rc) usbmem_d_free(od_); }
  if (rc) { iput(ndir); iput(odir); return rc; }
  if (!od_->d_inode) rc = -ENOENT;
  else if (!odir->i_op->rename) rc = -EOPNOTSUPP;
  else {
    down_write(&odir->i_rwsem); if (ndir != odir) down_write(&ndir->i_rwsem);
    rc = ((int (*)(const struct mnt_idmap *, struct inode *, struct dentry *, struct inode *, struct dentry *, unsigned))odir->i_op->rename)(NULL, odir, od_, ndir, nd_, 0);
    if (ndir != odir) up_write(&ndir->i_rwsem); up_write(&odir->i_rwsem);
  }
  usbmem_d_free(nd_); usbmem_d_free(od_);
  iput(ndir); iput(odir);
  return rc;
}
