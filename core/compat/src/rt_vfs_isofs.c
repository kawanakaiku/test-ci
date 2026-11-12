// SPDX-License-Identifier: GPL-2.0-or-later
// ホスト向け VFS 風入口 (ISO 9660 / fs/isofs・読み取り専用)。Linux の VFS が行う「iget → i_op->lookup / i_fop->iterate_shared / read_mapping_folio」を直接呼ぶ。
//
// inode 番号: isofs の i_ino = (ブロック << (bufbits-5)) | (ディレクトリレコードの位置 >> 5)。これだけでは inode (ブロック, レコード位置) が引けない
// (位置の下位 5 ビットが落ちている) ので、ino から inode を引くとき、そのブロックを読んでレコードを先頭から数え、スロットに入るレコードの正確な位置を求める
// (ディレクトリレコードは最小 34 バイトなので、32 バイトのスロットに 2 つ始まることはない)。ディレクトリは常に位置 0 ("." レコード)。
#include <stdlib.h>
#include <compat/kernel.h>
#include <compat/usbmem_ntfs.h>

#include "usbmem/isofs_c.h"
#include "usbmem/ntfs_c.h"   /* usbmem_bdev_* の宣言 */
#include "isofs.h"           /* 上流 fs/isofs: ISOFS_SB / __isofs_iget (-iquote で解決) */

struct usbmem_isofs { struct super_block *sb; };
extern int usbmem_module_init_isofs(void);   /* USBMEM_MODULE_INIT_NAME (上流 fs/isofs/inode.c の module_init) */

const struct file_operations usbmem_optical_generic_ro_fops = { 0 };
const struct inode_operations usbmem_optical_page_symlink_iops = { .get_link = page_get_link };

/* Joliet の名前 (UTF-16BE, inlen は 16 ビット単位) → UTF-8。サロゲートペアを扱い、対にならないものは U+FFFD にする。 */
int usbmem_utf16s_to_utf8s_be(const void *in, int inlen, int endian, u8 *out, int maxout) {
  (void)endian; const u8 *p = in; int o = 0;
  for (int i = 0; i < inlen; i++) {
    u32 c = ((u32)p[2 * i] << 8) | p[2 * i + 1];
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < inlen) {
      u32 d = ((u32)p[2 * i + 2] << 8) | p[2 * i + 3];
      if (d >= 0xDC00 && d < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); i++; }
      else c = 0xFFFD;
    } else if (c >= 0xD800 && c < 0xE000) c = 0xFFFD;
    int n = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
    if (o + n > maxout) break;
    if (n == 1) out[o++] = (u8)c;
    else if (n == 2) { out[o++] = 0xC0 | (c >> 6); out[o++] = 0x80 | (c & 0x3F); }
    else if (n == 3) { out[o++] = 0xE0 | (c >> 12); out[o++] = 0x80 | ((c >> 6) & 0x3F); out[o++] = 0x80 | (c & 0x3F); }
    else { out[o++] = 0xF0 | (c >> 18); out[o++] = 0x80 | ((c >> 12) & 0x3F); out[o++] = 0x80 | ((c >> 6) & 0x3F); out[o++] = 0x80 | (c & 0x3F); }
  }
  return o;
}

/* マウント前に最小限の検査: 十分な大きさと、ボリューム記述子領域 (セクタ 16 以降) に "CD001" / High Sierra "CDROM" があること。
 * 詳しい検査は上流の isofs_fill_super がやる。ここは「ISO ではない」を -EINVAL で素早く返すだけ。 */
static int precheck(struct usbmem_bdev *bdev) {
  if (usbmem_bdev_size(bdev) < (uint64_t)(17 * 2048)) return -EINVAL;
  uint8_t d[16];
  for (int s = 16; s < 16 + 100; s++) {   /* マルチセッションの先頭は 0 のみ対応 (上流も ioctl が無ければ 0) */
    if ((uint64_t)(s + 1) * 2048 > usbmem_bdev_size(bdev)) break;
    int rc = usbmem_bdev_read(bdev, (uint64_t)s * 2048, d, sizeof d);
    if (rc) return rc < 0 ? rc : -EIO;
    if (!memcmp(d + 1, "CD001", 5)) return 0;
    if (!memcmp(d + 9, "CDROM", 5) && s == 16) return 0;   /* High Sierra: 識別子はオフセット 9 */
    if (d[0] == 255) break;                                  /* 終端記述子 */
  }
  return -EINVAL;
}

int usbmem_isofs_mount(struct usbmem_bdev *bdev, int ro, usbmem_isofs **out) {
  if (!ro) return -EROFS;   /* 書き込みは未対応 (光学メディア形式) */
  int pc = precheck(bdev);
  if (pc) return pc;
  struct block_device *bd = calloc(1, sizeof *bd);
  if (!bd) return -ENOMEM;
  bd->impl = bdev;
  usbmem_bdev_mapping_init(bd);
  struct super_block *sb = NULL;
  static const char *const opts[] = { "utf8", NULL };   /* Joliet の名前は UTF-8 に変換 (iocharset を使わない) */
  int rc = usbmem_compat_mount_fs_opts(usbmem_module_init_isofs, bd, 1, opts, &sb);
  if (rc) { usbmem_bdev_mapping_free(bd); free(bd); return rc; }
  usbmem_isofs *v = calloc(1, sizeof *v);
  if (!v) { usbmem_compat_umount(sb); usbmem_bdev_mapping_free(bd); free(bd); return -ENOMEM; }
  v->sb = sb;
  *out = v;
  return 0;
}

void usbmem_isofs_unmount(usbmem_isofs *v) {
  if (!v) return;
  struct block_device *bd = v->sb->s_bdev;
  usbmem_compat_umount(v->sb);
  usbmem_bdev_mapping_free(bd);
  free(bd);
  free(v);
}

uint64_t usbmem_isofs_root_ino(usbmem_isofs *v) { return v->sb->s_root->d_inode->i_ino; }

/* ino → (ブロック, レコード位置)。範囲外・レコードが無い場合は -ESTALE。 */
static int ino_to_loc(struct super_block *sb, uint64_t ino, unsigned long *block, unsigned long *off) {
  unsigned bits = sb->s_blocksize_bits;
  if (bits < 5 || ino == 0) return -ESTALE;
  unsigned shift = bits - 5;
  uint64_t b = ino >> shift;
  unsigned slot = (unsigned)(ino & ((1ull << shift) - 1));
  if (b == 0 || b >= ISOFS_SB(sb)->s_nzones) return -ESTALE;
  struct buffer_head *bh = sb_bread(sb, (sector_t)b);
  if (!bh) return -EIO;
  unsigned bs = 1u << bits, o = 0;
  int found = -1;
  while (o < bs) {
    unsigned len = (unsigned char)bh->b_data[o];
    if (len == 0) break;
    if ((o >> 5) == slot) { found = (int)o; break; }
    o += len;
  }
  brelse(bh);
  if (found < 0) return -ESTALE;
  *block = (unsigned long)b; *off = (unsigned long)found;
  return 0;
}

static struct inode *get_inode(usbmem_isofs *v, uint64_t ino, int *err) {
  unsigned long block, off;
  int rc = ino_to_loc(v->sb, ino, &block, &off);
  if (rc) { *err = rc; return NULL; }
  struct inode *i = isofs_iget(v->sb, block, off);
  if (IS_ERR(i)) { *err = (int)PTR_ERR(i); return NULL; }
  return i;
}

int usbmem_isofs_lookup(usbmem_isofs *v, uint64_t dir_ino, const char *name, size_t len, uint64_t *ino) {
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

int usbmem_isofs_stat_ino(usbmem_isofs *v, uint64_t ino, usbmem_isofs_stat *st) {
  int err = 0; struct inode *i = get_inode(v, ino, &err);
  if (!i) return err;
  st->ino = i->i_ino; st->size = (uint64_t)i->i_size; st->mode = i->i_mode; st->nlink = i->i_nlink;
  st->mtime_sec = i->i_mtime_.tv_sec; st->atime_sec = i->i_atime_.tv_sec; st->ctime_sec = i->i_ctime_.tv_sec;
  iput(i); return 0;
}

/* readdir の d_ino: 上流の dir.c は通常ファイルのレコードに**ディレクトリ内の論理ブロック**から作った番号を出す (inode の i_ino とは別物。ディレクトリのレコードだけは
 * 正規化済みで正しい)。ホストは readdir の ino をそのまま stat に使うので、i_ino と一致する値に直す:
 * レコードの位置 pos から物理ブロック (= ディレクトリの先頭エクステント + 論理ブロック) とレコード位置を求め、ディレクトリのレコード (flags の 0x02) でなければ get_ino で作る。 */
struct rd_ctx { struct dir_context c; usbmem_isofs_dir_cb cb; void *user; struct inode *dir; };
static bool rd_actor(struct dir_context *c, const char *name, int len, loff_t pos, u64 ino, unsigned type) {
  struct rd_ctx *r = (struct rd_ctx *)c;
  if (!(len == 1 && name[0] == '.') && !(len == 2 && name[0] == '.' && name[1] == '.')) {
    struct super_block *sb = r->dir->i_sb;
    unsigned bits = sb->s_blocksize_bits, bs = 1u << bits;
    unsigned long b = ISOFS_I(r->dir)->i_first_extent + (unsigned long)(pos >> bits);
    unsigned off = (unsigned)(pos & (bs - 1));
    struct buffer_head *bh = sb_bread(sb, (sector_t)b);
    if (bh) {
      if (off + 26 <= bs && !(((unsigned char)bh->b_data[off + 25]) & 2)) ino = isofs_get_ino(b, off, bits);
      brelse(bh);
    }
  }
  return r->cb(r->user, name, len, ino, type) == 0;
}
int usbmem_isofs_readdir(usbmem_isofs *v, uint64_t dir_ino, usbmem_isofs_dir_cb cb, void *user) {
  int err = 0; struct inode *dir = get_inode(v, dir_ino, &err);
  if (!dir) return err;
  if (!S_ISDIR(dir->i_mode) || !dir->i_fop || !dir->i_fop->iterate_shared) { iput(dir); return -ENOTDIR; }
  struct file f; memset(&f, 0, sizeof f);
  f.f_inode = dir; f.f_mapping = dir->i_mapping; f.f_mode = FMODE_READ;
  { uint64_t parent = dir_ino; if (usbmem_isofs_lookup(v, dir_ino, "..", 2, &parent)) parent = dir_ino; f.f_pos = (loff_t)parent; }   /* dir_emit_dotdot (compat/optical.h) が使う */
  int rc = 0;
  if (dir->i_fop->open) rc = ((int (*)(struct inode *, struct file *))dir->i_fop->open)(dir, &f);
  if (!rc) {
    struct rd_ctx r = { { rd_actor, 0, NULL }, cb, user, dir };
    down_read(&dir->i_rwsem);
    rc = ((int (*)(struct file *, struct dir_context *))dir->i_fop->iterate_shared)(&f, &r.c);
    up_read(&dir->i_rwsem);
    if (dir->i_fop->release) ((int (*)(struct inode *, struct file *))dir->i_fop->release)(dir, &f);
  }
  iput(dir); return rc;
}

/* zisofs (Rock Ridge ZF) で圧縮されたファイルは先頭が固定のマジックで始まる。圧縮を解かずに生の圧縮データを返すと中身が壊れて見えるので、明示的に拒否する。 */
static int is_zisofs(struct inode *i) {
  static const unsigned char magic[8] = { 0x37, 0xE4, 0x53, 0x96, 0xC9, 0xDB, 0xD6, 0x07 };
  if (i->i_size < 16) return 0;
  struct folio *f = read_mapping_folio(i->i_mapping, 0, NULL);
  if (IS_ERR(f)) return 0;
  int z = !memcmp(f->data, magic, 8);
  folio_put(f);
  return z;
}

int64_t usbmem_isofs_read(usbmem_isofs *v, uint64_t ino, uint64_t off, void *buf, size_t len) {
  int err = 0; struct inode *i = get_inode(v, ino, &err);
  if (!i) return err;
  if (S_ISDIR(i->i_mode)) { iput(i); return -EISDIR; }
  if (!S_ISREG(i->i_mode) && !S_ISLNK(i->i_mode)) { iput(i); return -EINVAL; }   /* 特殊ファイルの読み取りは意味がない */
  if (S_ISLNK(i->i_mode)) {
    /* シンボリックリンク (Rock Ridge SL): リンク先の文字列を返す (page_get_link) */
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
  if (is_zisofs(i)) { iput(i); return -EOPNOTSUPP; }
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
