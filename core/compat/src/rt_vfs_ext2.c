// SPDX-License-Identifier: GPL-2.0-or-later
// ホスト向け VFS 風入口 (ext2)。Linux の VFS が行う「iget → i_op->lookup / i_fop->iterate_shared / read_mapping_folio」を直接呼ぶ。
// ext2 の inode 番号はディスク上の値なので、NTFS と同様に ino から ext2_iget() で引ける (exFAT のようなピン表は不要)。
// 書き込み (ビルドで USBMEM_NTFS_WRITE=ON かつ usbmem_ext2_mount(read_only=0)): docs/ext2-write.md。
//  - 常に**読み取り専用でマウント**し、最初の書き込みの直前に remount rw する (usbmem_compat_remount_rw)。マウントだけでメディアに触れない。
//    remount rw の中で上流がスーパーブロックの VALID_FS を落として書き、flush する = 最初の変更の前にダーティ印が確定する。
//  - 前提 (満たさなければ読み取り専用のまま: writable() == 0): s_state が VALID_FS でエラー無し (= 前回きれいに unmount された)、未知の ro_compat 機能なし。
#include <stdlib.h>
#include <compat/kernel.h>
#include <compat/usbmem_ntfs.h>

#include "usbmem/ext2_c.h"
#include "usbmem/ntfs_c.h"   /* usbmem_bdev_* の宣言 */
#include "ext2.h"            /* 上流 fs/ext2: struct ext2_sb_info と EXT2_VALID_FS / EXT2_MOUNT_* (-iquote で解決) */

struct usbmem_ext2 {
  struct super_block *sb;
  int can_write;      /* 書き込み要求あり、かつ前提を満たす */
  int free_pending;   /* 解放 (unlink/rmdir/truncate/上書き rename) が済んだが、まだ同期していない。同期するまで確保 (=解放ブロックの再利用) をしない */
};

/* 書き込み可能にしてよいか: s_state が VALID_FS のみで、未知の ro_compat 機能が無い。 */
static int writable_precheck(struct usbmem_bdev *bdev) {
  unsigned char sb[1024];
  if (usbmem_bdev_read(bdev, 1024, sb, sizeof sb)) return 0;
  unsigned magic = sb[0x38] | (sb[0x39] << 8), state = sb[0x3A] | (sb[0x3B] << 8);
  unsigned ro_compat = sb[0x64] | (sb[0x65] << 8) | (sb[0x66] << 16) | ((unsigned)sb[0x67] << 24);
  unsigned rev = sb[0x4C] | (sb[0x4D] << 8) | (sb[0x4E] << 16) | ((unsigned)sb[0x4F] << 24);
  if (magic != 0xEF53 || state != EXT2_VALID_FS) return 0;
  if (rev > 0 && (ro_compat & ~(unsigned)EXT2_FEATURE_RO_COMPAT_SUPP)) return 0;
  return 1;
}

int usbmem_ext2_mount(struct usbmem_bdev *bdev, int ro, usbmem_ext2 **out) {
#ifndef USBMEM_NTFS_WRITE
  if (!ro) return -EROFS;   /* 書き込みはビルドで無効 */
#endif
  struct block_device *bd = calloc(1, sizeof *bd);
  if (!bd) return -ENOMEM;
  bd->impl = bdev;
  usbmem_bdev_mapping_init(bd);
  struct super_block *sb = NULL;
  int rc = usbmem_compat_mount_fs(usbmem_module_init_ext2, bd, 1, &sb);
  if (rc) { usbmem_bdev_mapping_free(bd); free(bd); return rc; }
  usbmem_ext2 *v = calloc(1, sizeof *v);
  if (!v) { usbmem_compat_umount(sb); usbmem_bdev_mapping_free(bd); free(bd); return -ENOMEM; }
  v->sb = sb;
  sb->usbmem_bh_fs_ = 1;
  if (!ro && writable_precheck(bdev)) {
    v->can_write = 1;
    /* 異常を見つけたら読み取り専用に落とす (ext2 の errors=remount-ro)。既定の errors=continue だとエラー後も書き続けてしまう */
    struct ext2_sb_info *sbi = EXT2_SB(sb);
    sbi->s_mount_opt = (sbi->s_mount_opt & ~EXT2_MOUNT_ERRORS_MASK) | EXT2_MOUNT_ERRORS_RO;
  }
  *out = v;
  return 0;
}
int usbmem_ext2_writable(usbmem_ext2 *v) { return v && v->can_write; }

/* 書き込みの入口: 最初の 1 回は remount rw (= ダーティ印の確定)。失敗したら以後も書き込みを拒否する。 */
static int begin_write(usbmem_ext2 *v) {
  if (!v || !v->can_write) return -EROFS;
  if (sb_rdonly(v->sb)) {
    int rc = usbmem_compat_remount_rw(v->sb);
    if (rc) { v->can_write = 0; return rc; }
  }
  return 0;
}
/* 確保を伴う操作の前: 保留中の解放があれば先に同期する (解放されたブロックが、ディスク上でまだ元のファイルのものとして残っているうちに再利用されない) */
static int begin_alloc(usbmem_ext2 *v) {
  int e = begin_write(v); if (e) return e;
  if (v->free_pending) { e = usbmem_compat_sync(v->sb); if (e) return e; v->free_pending = 0; }
  return 0;
}
/* 上流がエラーを見つけて読み取り専用に落ちていたら、以後の書き込みを止める */
static int after_op(usbmem_ext2 *v, int rc) {
  if (sb_rdonly(v->sb)) v->can_write = 0;
  return rc;
}
int usbmem_ext2_sync(usbmem_ext2 *v) {
  if (!v) return -EINVAL;
  if (sb_rdonly(v->sb)) return 0;
  int rc = usbmem_compat_sync(v->sb);
  if (!rc) v->free_pending = 0;
  return after_op(v, rc);
}

void usbmem_ext2_unmount(usbmem_ext2 *v) {
  if (!v) return;
  struct block_device *bd = v->sb->s_bdev;
  usbmem_compat_umount(v->sb);
  usbmem_bdev_mapping_free(bd);
  free(bd);
  free(v);
}

static struct inode *get_inode(usbmem_ext2 *v, uint64_t ino, int *err) {
  if (ino > 0xFFFFFFFFu) { *err = -ESTALE; return NULL; }
  struct inode *i = ext2_iget(v->sb, (unsigned long)ino);
  if (IS_ERR(i)) { *err = (int)PTR_ERR(i); return NULL; }
  return i;
}

int usbmem_ext2_lookup(usbmem_ext2 *v, uint64_t dir_ino, const char *name, size_t len, uint64_t *ino) {
  int err = 0; struct inode *dir = get_inode(v, dir_ino, &err);
  if (!dir) return err;
  if (!S_ISDIR(dir->i_mode) || !dir->i_op || !dir->i_op->lookup) { iput(dir); return -ENOTDIR; }
  struct dentry *d = usbmem_d_alloc(v->sb, NULL, name, (unsigned)len);
  if (!d) { iput(dir); return -ENOMEM; }
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

int usbmem_ext2_stat_ino(usbmem_ext2 *v, uint64_t ino, usbmem_ext2_stat *st) {
  int err = 0; struct inode *i = get_inode(v, ino, &err);
  if (!i) return err;
  st->ino = i->i_ino; st->size = (uint64_t)i->i_size; st->mode = i->i_mode; st->nlink = i->i_nlink;
  st->mtime_sec = i->i_mtime_.tv_sec; st->atime_sec = i->i_atime_.tv_sec; st->ctime_sec = i->i_ctime_.tv_sec;
  iput(i); return 0;
}

struct rd_ctx { struct dir_context c; usbmem_ext2_dir_cb cb; void *user; };
static bool rd_actor(struct dir_context *c, const char *name, int len, loff_t pos, u64 ino, unsigned type) {
  (void)pos; struct rd_ctx *r = (struct rd_ctx *)c;
  return r->cb(r->user, name, len, ino, type) == 0;
}
int usbmem_ext2_readdir(usbmem_ext2 *v, uint64_t dir_ino, usbmem_ext2_dir_cb cb, void *user) {
  int err = 0; struct inode *dir = get_inode(v, dir_ino, &err);
  if (!dir) return err;
  if (!S_ISDIR(dir->i_mode) || !dir->i_fop || !dir->i_fop->iterate_shared) { iput(dir); return -ENOTDIR; }
  struct file f; memset(&f, 0, sizeof f);
  f.f_inode = dir; f.f_mapping = dir->i_mapping; f.f_mode = FMODE_READ;
  int rc = 0;
  if (dir->i_fop->open) rc = ((int (*)(struct inode *, struct file *))dir->i_fop->open)(dir, &f);
  if (!rc) {
    struct rd_ctx r = { { rd_actor, 0, NULL }, cb, user };
    down_read(&dir->i_rwsem);
    rc = ((int (*)(struct file *, struct dir_context *))dir->i_fop->iterate_shared)(&f, &r.c);
    up_read(&dir->i_rwsem);
    if (dir->i_fop->release) ((int (*)(struct inode *, struct file *))dir->i_fop->release)(dir, &f);
  }
  iput(dir); return rc;
}

int64_t usbmem_ext2_read(usbmem_ext2 *v, uint64_t ino, uint64_t off, void *buf, size_t len) {
  int err = 0; struct inode *i = get_inode(v, ino, &err);
  if (!i) return err;
  if (S_ISDIR(i->i_mode)) { iput(i); return -EISDIR; }
  if (!S_ISREG(i->i_mode) && !S_ISLNK(i->i_mode)) { iput(i); return -EINVAL; }   /* 特殊ファイルの読み取りは意味がない */
  if (S_ISLNK(i->i_mode)) {
    /* シンボリックリンク: リンク先の文字列を返す (get_link: 高速リンクは i_data、低速リンクはページキャッシュ) */
    if (!i->i_op || !i->i_op->get_link) { iput(i); return -EINVAL; }
    struct dentry dummy; memset(&dummy, 0, sizeof dummy);
    struct delayed_call done = { NULL, NULL };
    const char *t = ((const char *(*)(struct dentry *, struct inode *, struct delayed_call *))i->i_op->get_link)(&dummy, i, &done);
    int64_t ret;
    if (IS_ERR(t)) ret = PTR_ERR(t);
    else {
      size_t tl = strnlen(t, (size_t)i->i_size < PAGE_SIZE ? (size_t)i->i_size : PAGE_SIZE);
      if (off >= tl) ret = 0;
      else { size_t n = tl - (size_t)off; if (n > len) n = len; memcpy(buf, t + off, n); ret = (int64_t)n; }
    }
    if (done.fn) done.fn(done.arg);
    iput(i); return ret;
  }
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

/* シンボリックリンクの内容 (NUL なし。buf に最大 cap バイト)。戻り値: 長さ / 負 = -errno。読み取り専用のマウントでも使える。 */
int usbmem_ext2_readlink(usbmem_ext2 *v, uint64_t ino, char *buf, size_t cap) {
  int err = 0; struct inode *i = get_inode(v, ino, &err);
  if (!i) return err;
  int lnk = S_ISLNK(i->i_mode);
  iput(i);
  if (!lnk) return -EINVAL;
  int64_t n = usbmem_ext2_read(v, ino, 0, buf, cap);
  return n < 0 ? (int)n : (int)n;
}

/* ======================= 書き込み (ビルドで USBMEM_NTFS_WRITE=ON のときだけ中身がある) ======================= */
#ifdef USBMEM_NTFS_WRITE

#define IMMUTABLE_OR_APPEND(i) ((i)->i_flags & (S_IMMUTABLE | S_APPEND))

/* lookup 用 dentry の解放: 親 dentry (ディレクトリを指す) も一緒に */
static void free_d(struct dentry *d) {
  if (!d) return;
  struct dentry *p = d->d_parent;
  usbmem_d_free(d);
  if (p && p != d) usbmem_d_free(p);
}
/* 1 つの名前の lookup (負の dentry も返す)。名前の検査は VFS の lookup_one と同等: 空・長すぎ・'/'・NUL・"."・".." は拒否。
 * 上流の ext2_add_link は dentry->d_parent の inode を親ディレクトリとして使うので、親 dentry を作ってつなぐ。 */
static int lookup_d(struct inode *dir, const char *name, size_t len, struct dentry **out) {
  if (!S_ISDIR(dir->i_mode) || !dir->i_op || !dir->i_op->lookup) return -ENOTDIR;
  if (!len) return -EINVAL;
  if (len > 255) return -ENAMETOOLONG;
  if (memchr(name, '/', len) || memchr(name, 0, len) || (len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.')) return -EINVAL;
  struct dentry *pd = usbmem_d_alloc(dir->i_sb, NULL, "", 0);
  if (!pd) return -ENOMEM;
  ihold(dir); pd->d_inode = dir;   /* usbmem_d_free(pd) が iput */
  struct dentry *d = usbmem_d_alloc(dir->i_sb, pd, name, (unsigned)len);
  if (!d) { usbmem_d_free(pd); return -ENOMEM; }
  struct dentry *r = ((struct dentry *(*)(struct inode *, struct dentry *, unsigned))dir->i_op->lookup)(dir, d, 0);
  if (IS_ERR(r)) { int e = (int)PTR_ERR(r); free_d(d); return e; }
  if (r && r != d) { r->d_parent = pd; d->d_parent = d; usbmem_d_free(d); d = r; }
  *out = d; return 0;
}

/* 作成した inode の所有者を親ディレクトリに合わせる (取り外し可能な媒体では「作った端末のユーザ」に意味が無い。PC に戻したとき親と同じ持ち主で扱える) */
static void inherit_owner(struct inode *dir, struct inode *i) {
  i->i_uid = dir->i_uid; i->i_gid = dir->i_gid;
  mark_inode_dirty(i);
}

/* kind: 0 create / 1 mkdir / 2 unlink / 3 rmdir */
static int ns_op(usbmem_ext2 *v, int kind, uint64_t dir_ino, const char *name, size_t len, uint32_t mode, uint64_t *ino) {
  { int e = (kind >= 2) ? begin_write(v) : begin_alloc(v); if (e) return e; }
  int err = 0; struct inode *dir = get_inode(v, dir_ino, &err);
  if (!dir) return err;
  if (IMMUTABLE_OR_APPEND(dir)) { iput(dir); return -EPERM; }
  struct dentry *d = NULL;
  int rc = lookup_d(dir, name, len, &d);
  if (rc) { iput(dir); return rc; }
  down_write(&dir->i_rwsem);
  switch (kind) {
  case 0: case 1:
    if (d->d_inode) { rc = -EEXIST; break; }
    if (kind == 0) {
      if (!dir->i_op->create) { rc = -EOPNOTSUPP; break; }
      rc = ((int (*)(const struct mnt_idmap *, struct inode *, struct dentry *, umode_t))dir->i_op->create)(&nop_mnt_idmap, dir, d, (umode_t)(S_IFREG | (mode & 07777)));
    } else {
      if (!dir->i_op->mkdir) { rc = -EOPNOTSUPP; break; }
      struct dentry *r = ((struct dentry *(*)(const struct mnt_idmap *, struct inode *, struct dentry *, umode_t))dir->i_op->mkdir)(&nop_mnt_idmap, dir, d, (umode_t)(S_IFDIR | (mode & 07777)));
      rc = IS_ERR(r) ? (int)PTR_ERR(r) : 0;
      if (!rc && r && r != d) { usbmem_d_free(d); d = r; }
    }
    if (!rc) { if (!d->d_inode) rc = -EIO; else { inherit_owner(dir, d->d_inode); if (ino) *ino = d->d_inode->i_ino; } }
    break;
  case 2: case 3:
    if (!d->d_inode) { rc = -ENOENT; break; }
    if (IMMUTABLE_OR_APPEND(d->d_inode)) { rc = -EPERM; break; }
    if (kind == 2 && S_ISDIR(d->d_inode->i_mode)) { rc = -EISDIR; break; }
    if (kind == 3 && !S_ISDIR(d->d_inode->i_mode)) { rc = -ENOTDIR; break; }
    if (kind == 2) rc = dir->i_op->unlink ? ((int (*)(struct inode *, struct dentry *))dir->i_op->unlink)(dir, d) : -EOPNOTSUPP;
    else rc = dir->i_op->rmdir ? ((int (*)(struct inode *, struct dentry *))dir->i_op->rmdir)(dir, d) : -EOPNOTSUPP;
    if (!rc) v->free_pending = 1;
    break;
  }
  up_write(&dir->i_rwsem);
  free_d(d);   /* d の inode を iput: 削除済み (nlink 0) ならここで evict される = ブロックと inode の解放 */
  iput(dir);
  return after_op(v, rc);
}
int usbmem_ext2_create(usbmem_ext2 *v, uint64_t d, const char *n, size_t l, uint32_t m, uint64_t *ino) { return ns_op(v, 0, d, n, l, m, ino); }
int usbmem_ext2_mkdir(usbmem_ext2 *v, uint64_t d, const char *n, size_t l, uint32_t m, uint64_t *ino) { return ns_op(v, 1, d, n, l, m, ino); }
int usbmem_ext2_unlink(usbmem_ext2 *v, uint64_t d, const char *n, size_t l) { return ns_op(v, 2, d, n, l, 0, NULL); }
int usbmem_ext2_rmdir(usbmem_ext2 *v, uint64_t d, const char *n, size_t l) { return ns_op(v, 3, d, n, l, 0, NULL); }

/* anc が dir の祖先 (または dir 自身) か。".." をたどる。壊れて輪になっていても止まる。 */
static int is_ancestor(usbmem_ext2 *v, uint64_t anc, uint64_t dir) {
  uint64_t cur = dir;
  for (int n = 0; n < 4096; n++) {
    if (cur == anc) return 1;
    if (cur == USBMEM_EXT2_ROOT_INO) return 0;
    uint64_t parent;
    if (usbmem_ext2_lookup(v, cur, "..", 2, &parent) || parent == cur) return 0;
    cur = parent;
  }
  return 1;   /* 深すぎる/輪: 安全側 (移動を拒否) */
}

int usbmem_ext2_rename(usbmem_ext2 *v, uint64_t od, const char *on, size_t ol, uint64_t nd, const char *nn, size_t nl) {
  { int e = begin_alloc(v); if (e) return e; }
  int err = 0; struct inode *odir = get_inode(v, od, &err); if (!odir) return err;
  struct inode *ndir = (nd == od) ? (ihold(odir), odir) : get_inode(v, nd, &err);
  if (!ndir) { iput(odir); return err; }
  struct dentry *od_ = NULL, *nd_ = NULL;
  int rc = lookup_d(odir, on, ol, &od_);
  if (!rc) { rc = lookup_d(ndir, nn, nl, &nd_); if (rc) free_d(od_); }
  if (rc) { iput(ndir); iput(odir); return rc; }
  struct inode *src = od_->d_inode, *dst = nd_->d_inode;
  if (!src) rc = -ENOENT;
  else if (IMMUTABLE_OR_APPEND(odir) || IMMUTABLE_OR_APPEND(ndir) || IMMUTABLE_OR_APPEND(src) || (dst && IMMUTABLE_OR_APPEND(dst))) rc = -EPERM;
  else if (S_ISDIR(src->i_mode) && is_ancestor(v, src->i_ino, ndir->i_ino)) rc = -EINVAL;   /* 自分の中へは動かせない (Linux は型の検査より先に -EINVAL) */
  else if (dst && dst->i_ino == src->i_ino) rc = 0;   /* 同じ inode への rename は何もしない (VFS と同じ) */
  else if (dst && S_ISDIR(src->i_mode) != S_ISDIR(dst->i_mode)) rc = S_ISDIR(src->i_mode) ? -ENOTDIR : -EISDIR;
  else if (!odir->i_op->rename) rc = -EOPNOTSUPP;
  else {
    down_write(&odir->i_rwsem); if (ndir != odir) down_write(&ndir->i_rwsem);
    rc = ((int (*)(struct mnt_idmap *, struct inode *, struct dentry *, struct inode *, struct dentry *, unsigned))odir->i_op->rename)(&nop_mnt_idmap, odir, od_, ndir, nd_, 0);
    if (ndir != odir) up_write(&ndir->i_rwsem); up_write(&odir->i_rwsem);
    if (!rc && dst) v->free_pending = 1;   /* 上書きされた側が解放される */
  }
  free_d(nd_); free_d(od_);
  iput(ndir); iput(odir);
  return after_op(v, rc);
}

/* ハードリンク: dir/name を target_ino の新しい名前にする。ディレクトリは不可 (VFS と同じ -EPERM)。 */
int usbmem_ext2_link(usbmem_ext2 *v, uint64_t dir_ino, const char *name, size_t len, uint64_t target_ino) {
  { int e = begin_alloc(v); if (e) return e; }
  int err = 0; struct inode *dir = get_inode(v, dir_ino, &err);
  if (!dir) return err;
  struct inode *t = get_inode(v, target_ino, &err);
  if (!t) { iput(dir); return err; }
  struct dentry *d = NULL;
  int rc = 0;
  if (S_ISDIR(t->i_mode) || IMMUTABLE_OR_APPEND(t) || IMMUTABLE_OR_APPEND(dir)) rc = S_ISDIR(t->i_mode) ? -EPERM : -EPERM;
  else if (!t->i_nlink) rc = -ENOENT;
  else if (!dir->i_op || !dir->i_op->link) rc = -EOPNOTSUPP;
  if (!rc) rc = lookup_d(dir, name, len, &d);
  if (!rc && d->d_inode) rc = -EEXIST;
  if (!rc) {
    struct dentry *old = usbmem_d_alloc(t->i_sb, NULL, "l", 1);
    if (!old) rc = -ENOMEM;
    else {
      ihold(t); old->d_inode = t;   /* usbmem_d_free が iput */
      down_write(&dir->i_rwsem);
      rc = ((int (*)(struct dentry *, struct inode *, struct dentry *))dir->i_op->link)(old, dir, d);
      up_write(&dir->i_rwsem);
      usbmem_d_free(old);
    }
  }
  if (d) free_d(d);
  iput(t); iput(dir);
  return after_op(v, rc);
}

/* シンボリックリンクの作成。target は NUL を含まない文字列 (長さ tlen)。 */
int usbmem_ext2_symlink(usbmem_ext2 *v, uint64_t dir_ino, const char *name, size_t len, const char *target, size_t tlen, uint64_t *ino) {
  { int e = begin_alloc(v); if (e) return e; }
  if (!tlen || tlen >= PAGE_SIZE || memchr(target, 0, tlen)) return -EINVAL;
  int err = 0; struct inode *dir = get_inode(v, dir_ino, &err);
  if (!dir) return err;
  if (IMMUTABLE_OR_APPEND(dir)) { iput(dir); return -EPERM; }
  struct dentry *d = NULL;
  int rc = lookup_d(dir, name, len, &d);
  if (rc) { iput(dir); return rc; }
  if (d->d_inode) rc = -EEXIST;
  else if (!dir->i_op->symlink) rc = -EOPNOTSUPP;
  else {
    char *tmp = malloc(tlen + 1);
    if (!tmp) rc = -ENOMEM;
    else {
      memcpy(tmp, target, tlen); tmp[tlen] = 0;
      down_write(&dir->i_rwsem);
      rc = ((int (*)(struct mnt_idmap *, struct inode *, struct dentry *, const char *))dir->i_op->symlink)(&nop_mnt_idmap, dir, d, tmp);
      up_write(&dir->i_rwsem);
      free(tmp);
      if (!rc) { if (!d->d_inode) rc = -EIO; else { inherit_owner(dir, d->d_inode); if (ino) *ino = d->d_inode->i_ino; } }
    }
  }
  free_d(d); iput(dir);
  return after_op(v, rc);
}

/* 書き込み 1 回分の後始末: このファイルのデータページをデバイスへ書く (メモリに溜めない。確保済みブロックにしか書かないので既存のファイルは壊れない) */
static int64_t write_common(usbmem_ext2 *v, uint64_t ino, uint64_t off, const void *buf, size_t len, int inplace) {
  { int e = begin_alloc(v); if (e) return e; }
  int err = 0; struct inode *i = get_inode(v, ino, &err);
  if (!i) return err;
  if (S_ISDIR(i->i_mode)) { iput(i); return -EISDIR; }
  if (!S_ISREG(i->i_mode)) { iput(i); return -EINVAL; }
  if (IMMUTABLE_OR_APPEND(i)) { iput(i); return -EPERM; }
  if (inplace && off + len > (uint64_t)i->i_size) { iput(i); return -EINVAL; }
  if (!len) { iput(i); return 0; }
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
    if (usbmem_mapping_has_dirty(i->i_mapping)) {
      int wr = usbmem_mapping_writeback(i->i_mapping, 0);
      if (wr && ret >= 0) ret = wr;
      else if (!wr) i->i_state &= ~(unsigned long)I_DIRTY_PAGES;
    }
  }
  iput(i);
  after_op(v, 0);
  return ret;
}
int64_t usbmem_ext2_write(usbmem_ext2 *v, uint64_t ino, uint64_t off, const void *buf, size_t len) { return write_common(v, ino, off, buf, len, 0); }
int64_t usbmem_ext2_write_inplace(usbmem_ext2 *v, uint64_t ino, uint64_t off, const void *buf, size_t len) { return write_common(v, ino, off, buf, len, 1); }

/* サイズ変更 (SAF の "w" = 0 への切り詰めなど)。縮小は解放を伴う。 */
int usbmem_ext2_truncate(usbmem_ext2 *v, uint64_t ino, uint64_t size) {
  { int e = begin_alloc(v); if (e) return e; }
  int err = 0; struct inode *i = get_inode(v, ino, &err);
  if (!i) return err;
  if (S_ISDIR(i->i_mode)) { iput(i); return -EISDIR; }
  if (!S_ISREG(i->i_mode)) { iput(i); return -EINVAL; }
  if (IMMUTABLE_OR_APPEND(i)) { iput(i); return -EPERM; }
  if (!i->i_op || !i->i_op->setattr) { iput(i); return -EOPNOTSUPP; }
  if ((uint64_t)i->i_size == size) { iput(i); return 0; }
  if (size > (uint64_t)i->i_sb->s_maxbytes) { iput(i); return -EFBIG; }
  struct dentry *d = usbmem_d_alloc(i->i_sb, NULL, "t", 1);
  if (!d) { iput(i); return -ENOMEM; }
  ihold(i); d->d_inode = i;   /* usbmem_d_free が iput */
  struct iattr a; memset(&a, 0, sizeof a);
  a.ia_valid = ATTR_SIZE; a.ia_size = (loff_t)size;
  int shrink = size < (uint64_t)i->i_size;
  down_write(&i->i_rwsem);
  int rc = ((int (*)(struct mnt_idmap *, struct dentry *, struct iattr *))i->i_op->setattr)(&nop_mnt_idmap, d, &a);
  up_write(&i->i_rwsem);
  if (!rc && shrink) v->free_pending = 1;
  usbmem_d_free(d); iput(i);
  return after_op(v, rc);
}

#else  /* !USBMEM_NTFS_WRITE: 書き込み入口は全て -EROFS */
int usbmem_ext2_create(usbmem_ext2 *v, uint64_t d, const char *n, size_t l, uint32_t m, uint64_t *ino) { (void)v; (void)d; (void)n; (void)l; (void)m; (void)ino; return -EROFS; }
int usbmem_ext2_mkdir(usbmem_ext2 *v, uint64_t d, const char *n, size_t l, uint32_t m, uint64_t *ino) { (void)v; (void)d; (void)n; (void)l; (void)m; (void)ino; return -EROFS; }
int usbmem_ext2_unlink(usbmem_ext2 *v, uint64_t d, const char *n, size_t l) { (void)v; (void)d; (void)n; (void)l; return -EROFS; }
int usbmem_ext2_rmdir(usbmem_ext2 *v, uint64_t d, const char *n, size_t l) { (void)v; (void)d; (void)n; (void)l; return -EROFS; }
int usbmem_ext2_rename(usbmem_ext2 *v, uint64_t od, const char *on, size_t ol, uint64_t nd, const char *nn, size_t nl) { (void)v; (void)od; (void)on; (void)ol; (void)nd; (void)nn; (void)nl; return -EROFS; }
int usbmem_ext2_link(usbmem_ext2 *v, uint64_t d, const char *n, size_t l, uint64_t t) { (void)v; (void)d; (void)n; (void)l; (void)t; return -EROFS; }
int usbmem_ext2_symlink(usbmem_ext2 *v, uint64_t d, const char *n, size_t l, const char *t, size_t tl, uint64_t *ino) { (void)v; (void)d; (void)n; (void)l; (void)t; (void)tl; (void)ino; return -EROFS; }
int64_t usbmem_ext2_write(usbmem_ext2 *v, uint64_t ino, uint64_t off, const void *buf, size_t len) { (void)v; (void)ino; (void)off; (void)buf; (void)len; return -EROFS; }
int64_t usbmem_ext2_write_inplace(usbmem_ext2 *v, uint64_t ino, uint64_t off, const void *buf, size_t len) { (void)v; (void)ino; (void)off; (void)buf; (void)len; return -EROFS; }
int usbmem_ext2_truncate(usbmem_ext2 *v, uint64_t ino, uint64_t size) { (void)v; (void)ino; (void)size; return -EROFS; }
#endif
