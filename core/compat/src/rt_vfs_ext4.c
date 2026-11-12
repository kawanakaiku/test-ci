// SPDX-License-Identifier: GPL-2.0-or-later
// ホスト向け VFS 風入口 (ext4・読み取り専用)。Linux の VFS が行う「iget → i_op->lookup / i_fop->iterate_shared / read_mapping_folio」を直接呼ぶ。
// ext4 の inode 番号はディスク上の値なので、NTFS と同様に ino から ext4_iget() で引ける (exFAT のようなピン表は不要)。
#include <stdlib.h>
#include <compat/kernel.h>
#include <compat/usbmem_ntfs.h>

#include "usbmem/ext4_c.h"
#include "usbmem/ntfs_c.h"   /* usbmem_bdev_* の宣言 */

struct inode *__ext4_iget(struct super_block *, unsigned long, int flags, const char *function, unsigned int line);   /* fs/ext4/ext4.h (上流)。flags=0: 通常の inode のみ */
#define ext4_iget(sb, ino) __ext4_iget((sb), (ino), 0, __func__, __LINE__)

struct usbmem_ext4 { struct super_block *sb; };

/* マウント前に超ブロック (1024 バイト目) を自前で検査する。上流の ext4_fill_super も同様に拒否するが、ここでは理由を区別できる errno を返す。
 * - needs_recovery (未再生の journal): 既定は -EUCLEAN。journal を再生せずに読むと、コミット済みなのにディスクへ未反映の更新が見えない (古い/不整合)。
 * - 暗号化 / casefold / fs-verity / journal 外付け / dirdata / 圧縮 / 不明な機能: -EOPNOTSUPP。 */
static int precheck_sb(struct usbmem_bdev *bdev, int allow_noload) {
  uint8_t sb[1024];
  if (usbmem_bdev_size(bdev) < 2048) return -EINVAL;
  int rc = usbmem_bdev_read(bdev, 1024, sb, sizeof sb);
  if (rc) return rc < 0 ? rc : -EIO;
  uint32_t magic = sb[0x38] | (sb[0x39] << 8);
  if (magic != 0xEF53) return -EINVAL;
  uint32_t incompat = sb[0x60] | (sb[0x61] << 8) | (sb[0x62] << 16) | ((uint32_t)sb[0x63] << 24);
  uint32_t rocompat = sb[0x64] | (sb[0x65] << 8) | (sb[0x66] << 16) | ((uint32_t)sb[0x67] << 24);
  /* 対応する incompat: filetype 0x2, recover 0x4 (後述), meta_bg 0x10, extents 0x40, 64bit 0x80, mmp 0x100, flex_bg 0x200, ea_inode 0x400, csum_seed 0x2000, largedir 0x4000, inline_data 0x8000 */
  const uint32_t supported = 0x2 | 0x4 | 0x10 | 0x40 | 0x80 | 0x100 | 0x200 | 0x400 | 0x2000 | 0x4000 | 0x8000;
  if (incompat & ~supported) return -EOPNOTSUPP;
  if (rocompat & 0x8000u) return -EOPNOTSUPP;   /* verity */
  if ((incompat & 0x4) && !allow_noload) return -EUCLEAN;
  return 0;
}

int usbmem_ext4_mount(struct usbmem_bdev *bdev, int ro, int allow_noload, usbmem_ext4 **out) {
  if (!ro) return -EROFS;   /* 書き込みは未対応 */
  int pc = precheck_sb(bdev, allow_noload);
  if (pc) return pc;
  struct block_device *bd = calloc(1, sizeof *bd);
  if (!bd) return -ENOMEM;
  bd->impl = bdev;
  usbmem_bdev_mapping_init(bd);
  struct super_block *sb = NULL;
  static const char *const opts[] = { "noload", NULL };   /* journal は常に読み込まない (jbd2 本体は無い) */
  int rc = usbmem_compat_mount_fs_opts(usbmem_module_init_ext4, bd, 1, opts, &sb);
  if (rc) { usbmem_bdev_mapping_free(bd); free(bd); return rc; }
  usbmem_ext4 *v = calloc(1, sizeof *v);
  if (!v) { usbmem_compat_umount(sb); usbmem_bdev_mapping_free(bd); free(bd); return -ENOMEM; }
  v->sb = sb;
  *out = v;
  return 0;
}

void usbmem_ext4_unmount(usbmem_ext4 *v) {
  if (!v) return;
  struct block_device *bd = v->sb->s_bdev;
  usbmem_compat_umount(v->sb);
  usbmem_bdev_mapping_free(bd);
  free(bd);
  free(v);
}

static struct inode *get_inode(usbmem_ext4 *v, uint64_t ino, int *err) {
  if (ino > 0xFFFFFFFFu) { *err = -ESTALE; return NULL; }
  struct inode *i = ext4_iget(v->sb, (unsigned long)ino);
  if (IS_ERR(i)) { *err = (int)PTR_ERR(i); return NULL; }
  return i;
}

int usbmem_ext4_lookup(usbmem_ext4 *v, uint64_t dir_ino, const char *name, size_t len, uint64_t *ino) {
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

int usbmem_ext4_stat_ino(usbmem_ext4 *v, uint64_t ino, usbmem_ext4_stat *st) {
  int err = 0; struct inode *i = get_inode(v, ino, &err);
  if (!i) return err;
  st->ino = i->i_ino; st->size = (uint64_t)i->i_size; st->mode = i->i_mode; st->nlink = i->i_nlink;
  st->mtime_sec = i->i_mtime_.tv_sec; st->atime_sec = i->i_atime_.tv_sec; st->ctime_sec = i->i_ctime_.tv_sec;
  iput(i); return 0;
}

struct rd_ctx { struct dir_context c; usbmem_ext4_dir_cb cb; void *user; };
static bool rd_actor(struct dir_context *c, const char *name, int len, loff_t pos, u64 ino, unsigned type) {
  (void)pos; struct rd_ctx *r = (struct rd_ctx *)c;
  return r->cb(r->user, name, len, ino, type) == 0;
}
int usbmem_ext4_readdir(usbmem_ext4 *v, uint64_t dir_ino, usbmem_ext4_dir_cb cb, void *user) {
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

int64_t usbmem_ext4_read(usbmem_ext4 *v, uint64_t ino, uint64_t off, void *buf, size_t len) {
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

/* シンボリックリンクの内容 (NUL なし。buf に最大 cap バイト)。戻り値: 長さ / 負 = -errno。読み取り専用のマウントで使える (LinkResolver 用)。
 * 高速リンク (i_data 内)・低速リンク (データブロック)・inline_data のリンクを上流の get_link (ext4_get_link / simple_get_link) で読む。 */
int usbmem_ext4_readlink(usbmem_ext4 *v, uint64_t ino, char *buf, size_t cap) {
  int err = 0; struct inode *i = get_inode(v, ino, &err);
  if (!i) return err;
  int lnk = S_ISLNK(i->i_mode);
  iput(i);
  if (!lnk) return -EINVAL;
  int64_t n = usbmem_ext4_read(v, ino, 0, buf, cap);
  return (int)n;
}
