// SPDX-License-Identifier: GPL-2.0-or-later
// ホスト向け VFS 風入口 (UDF / fs/udf・読み取り専用)。Linux の VFS が行う「iget → i_op->lookup / i_fop->iterate_shared / read_mapping_folio」を直接呼ぶ。
//
// inode 番号: udf の i_ino は ICB (File Entry) の**物理ブロック番号** (udf_get_lb_pblock)。ここから論理アドレス (パーティション参照番号 + 論理ブロック) へ戻す:
//  - 単純なボリューム (Type 1 物理 / Sparable だけ): 物理ブロックを含むパーティションを探して pblock - root。状態を持たない。
//  - 複雑なボリューム (VAT = 仮想パーティション / Metadata パーティションを含む): ICB のパーティション参照が物理位置と一対一に戻せない (VAT やメタデータファイルを経由する) ので、
//    lookup / readdir が返した ino → 論理アドレスを表に覚える。表に無い ino は -ESTALE。readdir は各名前を lookup して表を作る (ディレクトリ長に対し 2 乗の時間だが正しさを優先)。
#include <stdlib.h>
#include <compat/kernel.h>
#include <compat/usbmem_ntfs.h>

#include "usbmem/udf_c.h"
#include "usbmem/ntfs_c.h"   /* usbmem_bdev_* の宣言 */
#include "udfdecl.h"         /* 上流 fs/udf (-iquote で解決) */
#include "udf_sb.h"
#include "udf_i.h"

struct ce { uint64_t ino; uint32_t lbn; uint16_t part; uint8_t used; };
struct usbmem_udf {
  struct super_block *sb;
  int complex;            /* VAT / Metadata パーティションを含む */
  struct ce *tab; size_t cap, n;
};
extern int usbmem_module_init_udf(void);   /* USBMEM_MODULE_INIT_NAME (上流 fs/udf/super.c の module_init) */

/* file.c を入れない代わりの空の ops (ホスト入口は read_mapping_folio / i_op->lookup / iterate_shared を直接呼び、i_fop の read/mmap/ioctl は使わない) */
const struct file_operations udf_file_operations = { 0 };
const struct inode_operations udf_file_inode_operations = { 0 };
long udf_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) { (void)filp; (void)cmd; (void)arg; return -ENOTTY; }

/* 上流の udf_fiiter_release は iter->epos.bh を手放さない (inode_bmap / udf_next_aext が掴んだ続きブロック。ディレクトリが複数の拡張割り当て記述子に渡るとき)。
 * directory.c の中の名前は optical.cmake で usbmem_real_fiiter_release に変えてあるので、他のファイルからの呼び出しはここに来る。 */
extern void usbmem_real_fiiter_release(struct udf_fileident_iter *iter);
void udf_fiiter_release(struct udf_fileident_iter *iter) {
  struct buffer_head *b = iter->epos.bh;
  iter->epos.bh = NULL;
  usbmem_real_fiiter_release(iter);
  brelse(b);
}

/* CRC-ITU-T (多項式 0x1021, 非反転) */
uint16_t crc_itu_t(uint16_t crc, const uint8_t *buffer, size_t len) {
  static uint16_t table[256]; static int init;
  if (!init) {
    for (int i = 0; i < 256; i++) { uint16_t c = (uint16_t)(i << 8); for (int k = 0; k < 8; k++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1); table[i] = c; }
    init = 1;
  }
  while (len--) crc = (uint16_t)((crc << 8) ^ table[((crc >> 8) ^ *buffer++) & 0xFF]);
  return crc;
}

/* ---- ino → 論理アドレスの表 (複雑なボリューム用) */
static struct ce *tab_find(struct usbmem_udf *v, uint64_t ino) {
  if (!v->cap) return NULL;
  size_t h = (size_t)((ino * 0x9E3779B97F4A7C15ull) >> 20) & (v->cap - 1);
  for (size_t k = 0; k < v->cap; k++) {
    struct ce *e = &v->tab[(h + k) & (v->cap - 1)];
    if (!e->used) return NULL;
    if (e->ino == ino) return e;
  }
  return NULL;
}
static int tab_put(struct usbmem_udf *v, uint64_t ino, const struct kernel_lb_addr *lb) {
  if (tab_find(v, ino)) return 0;
  if ((v->n + 1) * 2 > v->cap) {
    size_t nc = v->cap ? v->cap * 2 : 1024;
    struct ce *nt = calloc(nc, sizeof *nt);
    if (!nt) return -ENOMEM;
    for (size_t i = 0; i < v->cap; i++) if (v->tab[i].used) {
      size_t h = (size_t)((v->tab[i].ino * 0x9E3779B97F4A7C15ull) >> 20) & (nc - 1);
      while (nt[h].used) h = (h + 1) & (nc - 1);
      nt[h] = v->tab[i];
    }
    free(v->tab); v->tab = nt; v->cap = nc;
  }
  size_t h = (size_t)((ino * 0x9E3779B97F4A7C15ull) >> 20) & (v->cap - 1);
  while (v->tab[h].used) h = (h + 1) & (v->cap - 1);
  v->tab[h] = (struct ce){ ino, lb->logicalBlockNum, lb->partitionReferenceNum, 1 };
  v->n++;
  return 0;
}

/* マウント前の最小限の検査: ボリューム構造記述子 (VRS) が 32768 バイト以降にあること。ISO 9660 の記述子が先に並ぶこともある (ブリッジ)。詳しい検査は上流がやる。 */
static int precheck(struct usbmem_bdev *bdev) {
  uint64_t size = usbmem_bdev_size(bdev);
  uint8_t d[8];
  if (size < 32768 + 2048) return -EINVAL;
  for (int k = 0; k < 64; k++) {
    uint64_t off = 32768 + (uint64_t)k * 2048;
    if (off + sizeof d > size) break;
    int rc = usbmem_bdev_read(bdev, off, d, sizeof d);
    if (rc) return rc < 0 ? rc : -EIO;
    if (!memcmp(d + 1, "BEA01", 5) || !memcmp(d + 1, "NSR02", 5) || !memcmp(d + 1, "NSR03", 5)) return 0;
    if (!memcmp(d + 1, "TEA01", 5)) break;
  }
  return -EINVAL;
}

int usbmem_udf_mount(struct usbmem_bdev *bdev, int ro, usbmem_udf **out) {
  if (!ro) return -EROFS;   /* 書き込みは未対応 */
  int pc = precheck(bdev);
  if (pc) return pc;
  struct block_device *bd = calloc(1, sizeof *bd);
  if (!bd) return -ENOMEM;
  bd->impl = bdev;
  usbmem_bdev_mapping_init(bd);
  struct super_block *sb = NULL;
  static const char *const opts[] = { "utf8", NULL };   /* ファイル名は UTF-8 のまま (iocharset を使わない) */
  int rc = usbmem_compat_mount_fs_opts(usbmem_module_init_udf, bd, 1, opts, &sb);
  if (rc) { usbmem_bdev_mapping_free(bd); free(bd); return rc; }
  usbmem_udf *v = calloc(1, sizeof *v);
  if (!v) { usbmem_compat_umount(sb); usbmem_bdev_mapping_free(bd); free(bd); return -ENOMEM; }
  v->sb = sb;
  struct udf_sb_info *sbi = UDF_SB(sb);
  for (int i = 0; i < sbi->s_partitions; i++) {
    unsigned t = sbi->s_partmaps[i].s_partition_type;
    if (t != UDF_TYPE1_MAP15 && t != UDF_SPARABLE_MAP15) v->complex = 1;
  }
  if (getenv("USBMEM_UDF_FORCE_COMPLEX_FOR_TEST")) v->complex = 1;   /* テスト用: 通常のボリュームで ino 表の経路 (VAT / Metadata と同じ) を検査する */
  if (v->complex && sb->s_root && sb->s_root->d_inode) tab_put(v, sb->s_root->d_inode->i_ino, &UDF_I(sb->s_root->d_inode)->i_location);   /* ルートは lookup で出てこないので先に登録 */
  *out = v;
  return 0;
}

void usbmem_udf_unmount(usbmem_udf *v) {
  if (!v) return;
  struct block_device *bd = v->sb->s_bdev;
  usbmem_compat_umount(v->sb);
  usbmem_bdev_mapping_free(bd);
  free(bd);
  free(v->tab);
  free(v);
}

uint64_t usbmem_udf_root_ino(usbmem_udf *v) { return v->sb->s_root->d_inode->i_ino; }

/* ino → 論理アドレス */
static int ino_to_lb(struct usbmem_udf *v, uint64_t ino, struct kernel_lb_addr *lb) {
  if (ino == 0 || ino > 0xFFFFFFFEull) return -ESTALE;
  if (v->complex) {
    struct ce *e = tab_find(v, ino);
    if (!e) return -ESTALE;
    memset(lb, 0, sizeof *lb);
    lb->logicalBlockNum = e->lbn; lb->partitionReferenceNum = e->part;
    return 0;
  }
  struct udf_sb_info *sbi = UDF_SB(v->sb);
  for (int i = 0; i < sbi->s_partitions; i++) {
    struct udf_part_map *m = &sbi->s_partmaps[i];
    if (ino >= m->s_partition_root && ino - m->s_partition_root < m->s_partition_len) {
      memset(lb, 0, sizeof *lb);
      lb->logicalBlockNum = (uint32_t)(ino - m->s_partition_root); lb->partitionReferenceNum = (uint16_t)i;
      return 0;
    }
  }
  return -ESTALE;
}

static struct inode *get_inode(usbmem_udf *v, uint64_t ino, int *err) {
  struct kernel_lb_addr lb;
  int rc = ino_to_lb(v, ino, &lb);
  if (rc) { *err = rc; return NULL; }
  struct inode *i = udf_iget(v->sb, &lb);
  if (IS_ERR(i)) { *err = (int)PTR_ERR(i); return NULL; }
  if (!i) { *err = -EIO; return NULL; }
  if (v->complex) tab_put(v, i->i_ino, &UDF_I(i)->i_location);
  return i;
}

int usbmem_udf_lookup(usbmem_udf *v, uint64_t dir_ino, const char *name, size_t len, uint64_t *ino) {
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
    if (!use->d_inode) rc = -ENOENT;
    else { *ino = use->d_inode->i_ino; if (v->complex) tab_put(v, *ino, &UDF_I(use->d_inode)->i_location); }
    if (r && r != d) usbmem_d_free(r);
  }
  usbmem_d_free(d);
  iput(dir);
  return rc;
}

int usbmem_udf_stat_ino(usbmem_udf *v, uint64_t ino, usbmem_udf_stat *st) {
  int err = 0; struct inode *i = get_inode(v, ino, &err);
  if (!i) return err;
  if (st) {
    st->ino = i->i_ino; st->size = (uint64_t)i->i_size; st->mode = i->i_mode; st->nlink = i->i_nlink;
    st->mtime_sec = i->i_mtime_.tv_sec; st->atime_sec = i->i_atime_.tv_sec; st->ctime_sec = i->i_ctime_.tv_sec;
  }
  iput(i); return 0;
}

struct rd_ctx { struct dir_context c; usbmem_udf_dir_cb cb; void *user; usbmem_udf *v; uint64_t dir; };
static bool rd_actor(struct dir_context *c, const char *name, int len, loff_t pos, u64 ino, unsigned type) {
  struct rd_ctx *r = (struct rd_ctx *)c;
  (void)pos;
  if (r->v->complex && !(len == 1 && name[0] == '.') && !(len == 2 && name[0] == '.' && name[1] == '.')) {
    /* 複雑なボリュームでは readdir が返す物理ブロックを論理アドレスに戻せない: 名前を lookup して表に登録する */
    uint64_t real = 0;
    if (usbmem_udf_lookup(r->v, r->dir, name, (size_t)len, &real) == 0) ino = real;
  }
  return r->cb(r->user, name, len, ino, type) == 0;
}
int usbmem_udf_readdir(usbmem_udf *v, uint64_t dir_ino, usbmem_udf_dir_cb cb, void *user) {
  int err = 0; struct inode *dir = get_inode(v, dir_ino, &err);
  if (!dir) return err;
  if (!S_ISDIR(dir->i_mode) || !dir->i_fop || !dir->i_fop->iterate_shared) { iput(dir); return -ENOTDIR; }
  struct file f; memset(&f, 0, sizeof f);
  f.f_inode = dir; f.f_mapping = dir->i_mapping; f.f_mode = FMODE_READ;
  { uint64_t parent = dir_ino; if (usbmem_udf_lookup(v, dir_ino, "..", 2, &parent)) parent = dir_ino; f.f_pos = (loff_t)parent; }   /* dir_emit_dotdot (compat/optical.h) が使う */
  int rc = 0;
  if (dir->i_fop->open) rc = ((int (*)(struct inode *, struct file *))dir->i_fop->open)(dir, &f);
  if (!rc) {
    struct rd_ctx r = { { rd_actor, 0, NULL }, cb, user, v, dir_ino };
    down_read(&dir->i_rwsem);
    rc = ((int (*)(struct file *, struct dir_context *))dir->i_fop->iterate_shared)(&f, &r.c);
    up_read(&dir->i_rwsem);
    if (dir->i_fop->release) ((int (*)(struct inode *, struct file *))dir->i_fop->release)(dir, &f);
  }
  iput(dir); return rc;
}

int64_t usbmem_udf_read(usbmem_udf *v, uint64_t ino, uint64_t off, void *buf, size_t len) {
  int err = 0; struct inode *i = get_inode(v, ino, &err);
  if (!i) return err;
  if (S_ISDIR(i->i_mode)) { iput(i); return -EISDIR; }
  if (!S_ISREG(i->i_mode) && !S_ISLNK(i->i_mode)) { iput(i); return -EINVAL; }   /* 特殊ファイルの読み取りは意味がない */
  if (S_ISLNK(i->i_mode)) {
    /* シンボリックリンク: リンク先の文字列を返す (udf_get_link: パスコンポーネント列を / 区切りの文字列に直す) */
    if (!i->i_op || !i->i_op->get_link) { iput(i); return -EINVAL; }
    struct dentry dummy; memset(&dummy, 0, sizeof dummy);
    struct delayed_call done = { NULL, NULL };
    const char *t = ((const char *(*)(struct dentry *, struct inode *, struct delayed_call *))i->i_op->get_link)(&dummy, i, &done);
    int64_t ret;
    if (IS_ERR(t)) ret = PTR_ERR(t);
    else {
      size_t tl = strnlen(t, 4096);
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
  struct file ff; memset(&ff, 0, sizeof ff);   /* udf_read_folio は file_inode(file) を使う (ICB 内データ・通常の読み込みの分岐) ので、inode だけ持つ file を渡す */
  ff.f_inode = i; ff.f_mapping = i->i_mapping; ff.f_mode = FMODE_READ;
  while (done < len) {
    uint64_t pos = off + done;
    struct folio *f = read_mapping_folio(i->i_mapping, (pgoff_t)(pos >> PAGE_SHIFT), &ff);
    if (IS_ERR(f)) { iput(i); return done ? (int64_t)done : PTR_ERR(f); }
    size_t po = (size_t)(pos & (PAGE_SIZE - 1)), n = PAGE_SIZE - po;
    if (n > len - done) n = len - done;
    memcpy((char *)buf + done, (char *)f->data + po, n);
    folio_put(f);
    done += n;
  }
  iput(i); return (int64_t)done;
}
