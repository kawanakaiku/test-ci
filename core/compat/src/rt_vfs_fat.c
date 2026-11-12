// SPDX-License-Identifier: GPL-2.0-or-later
// ホスト向け VFS 風入口 (FAT12/16/32 + vfat の長い名前・読み取り専用)。Linux の VFS が行う「lookup → i_op->lookup / i_fop->iterate_shared / read_mapping_folio」を直接呼ぶ。
// FAT の inode 番号は iunique() で実行時に採番され、ino から inode を引く iget(sb, ino) が無い (exFAT と同じ)。
// そこで lookup / readdir が返した inode を「ピン」(参照を保持) して ino → inode の表を持つ。unmount で全て解放する。
// 上流 fs/fat の module_init は 2 つ (inode.c = キャッシュの用意, namei_vfat.c = vfat の登録)。CMake が別名にして、ここで順に呼ぶ。nls_cp437 / nls_cp932 の登録も同じ。
#include <stdlib.h>
#include <compat/kernel.h>
#include <compat/usbmem_ntfs.h>

#include "usbmem/fat_c.h"
#include "usbmem/ntfs_c.h"   /* usbmem_bdev_* の宣言 */

#define FAT_ROOT_INO 1

/* 上流の module_init (CMake が翻訳単位ごとに名前を付ける) */
int usbmem_fat_init_core(void);
int usbmem_fat_init_vfat(void);
int usbmem_fat_init_cp437(void);
int usbmem_fat_init_cp932(void);
int usbmem_module_init_fat(void) {
  int rc = usbmem_fat_init_cp437();
  if (!rc) rc = usbmem_fat_init_cp932();
  if (!rc) rc = usbmem_fat_init_core();
  if (!rc) rc = usbmem_fat_init_vfat();
  return rc;
}

/* 時刻の解釈: FAT の時刻は「その機器のローカル時刻」。カーネルは sys_tz (tz_minuteswest) で UTC に直す。既定は UTC (0)。アプリは起動時に端末のタイムゾーンを設定する。 */
void usbmem_fat_set_timezone(int minutes_west) { sys_tz.tz_minuteswest = minutes_west; }   /* sys_tz は rt_bh.c にある (exFAT と共通) */

/* jiffies は書き込み経路 (namei_vfat.c の短い名前の生成) でしか使わない。ext4 の rt_ext4.c にも同じ定義があるので weak にする */
__attribute__((weak)) unsigned long usbmem_jiffies(void) { return 0; }

/* 上流 nfs.c の代わり (NFS エクスポートは使わない)。fat_fill_super が s_export_op に入れるだけ */
const struct export_operations fat_export_ops;
const struct export_operations fat_export_ops_nostale;
/* 書き込み経路 (fat_cont_expand など) が参照するだけで、読み取り専用では呼ばれない */
int cont_write_begin(const void *iocb, struct address_space *mapping, loff_t pos, unsigned len, struct folio **foliop, void **fsdata, void *get_block, loff_t *bytes) { return -EROFS; }

struct pin { uint64_t ino; struct inode *inode; struct pin *next; };
struct usbmem_fat { struct super_block *sb; struct pin *tab[1024]; };

static struct inode *pin_find(usbmem_fat *v, uint64_t ino) {
  for (struct pin *p = v->tab[ino & 1023]; p; p = p->next) if (p->ino == ino) return p->inode;
  return NULL;
}
/* i の参照 (呼び出し側が持つ 1 つ) をピンへ移す。すでに同じ ino がピン済みなら、その参照を返して i を手放す。 */
static struct inode *pin_add(usbmem_fat *v, struct inode *i) {
  struct inode *old = pin_find(v, i->i_ino);
  if (old) { iput(i); return old; }
  struct pin *p = malloc(sizeof *p);
  if (!p) { iput(i); return NULL; }
  p->ino = i->i_ino; p->inode = i; p->next = v->tab[i->i_ino & 1023]; v->tab[i->i_ino & 1023] = p;
  return i;
}

int usbmem_fat_mount(struct usbmem_bdev *bdev, int ro, usbmem_fat **out) {
  if (!ro) return -EROFS;   /* 書き込みは未対応 */
  struct block_device *bd = calloc(1, sizeof *bd);
  if (!bd) return -ENOMEM;
  bd->impl = bdev;
  usbmem_bdev_mapping_init(bd);
  struct super_block *sb = NULL;
  /* "utf8": 長い名前 (UTF-16) を UTF-8 で見せる (Linux の udisks と同じ。サロゲートペアも扱える)。他のオプションは既定 (shortname=winnt, codepage=437, errors=remount-ro, fmask/dmask=022) */
  static const char *const opts[] = { "utf8", NULL };
  int rc = usbmem_compat_mount_fs_opts(usbmem_module_init_fat, bd, 1, opts, &sb);
  if (rc) { usbmem_bdev_mapping_free(bd); free(bd); return rc; }
  usbmem_fat *v = calloc(1, sizeof *v);
  if (!v) { usbmem_compat_umount(sb); usbmem_bdev_mapping_free(bd); free(bd); return -ENOMEM; }
  v->sb = sb;
  struct inode *root = sb->s_root->d_inode;
  ihold(root);
  if (!pin_add(v, root)) { usbmem_fat_unmount(v); return -ENOMEM; }
  *out = v;
  return 0;
}

void usbmem_fat_unmount(usbmem_fat *v) {
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
static int lookup_pin(usbmem_fat *v, struct inode *dir, const char *name, size_t len, struct inode **out) {
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

int usbmem_fat_lookup(usbmem_fat *v, uint64_t dir_ino, const char *name, size_t len, uint64_t *ino) {
  struct inode *dir = pin_find(v, dir_ino);
  if (!dir) return -ENOENT;
  struct inode *r = NULL;
  int rc = lookup_pin(v, dir, name, len, &r);
  if (!rc) *ino = r->i_ino;
  return rc;
}

int usbmem_fat_stat_ino(usbmem_fat *v, uint64_t ino, usbmem_fat_stat *st) {
  struct inode *i = pin_find(v, ino);
  if (!i) return -ENOENT;
  st->ino = i->i_ino; st->size = (uint64_t)i->i_size; st->mode = i->i_mode; st->nlink = i->i_nlink;
  st->mtime_sec = i->i_mtime_.tv_sec; st->atime_sec = i->i_atime_.tv_sec; st->ctime_sec = i->i_ctime_.tv_sec;
  return 0;
}

/* readdir 中は fat が sbi->s_lock を持つ。アクタ内から lookup (同じ s_lock を取る) を呼ぶとデッドロックするので、
 * アクタでは名前と種別を集めるだけにし、iterate を抜けてから名前ごとに lookup して実 inode をピンする。
 * (dir_emit の ino は inode が未キャッシュだと仮採番 (iunique) なので使えない。) */
struct rd_ent { char *name; int len; unsigned type; };
struct rd_ctx { struct dir_context c; struct rd_ent *e; size_t n, cap; int err; };
static bool rd_actor(struct dir_context *c, const char *name, int len, loff_t pos, u64 ino, unsigned type) {
  (void)pos; (void)ino;
  struct rd_ctx *r = (struct rd_ctx *)c;
  if (len == 1 && name[0] == '.') return true;
  if (len == 2 && name[0] == '.' && name[1] == '.') return true;
  if (r->n == r->cap) {
    size_t nc = r->cap ? r->cap * 2 : 64;
    struct rd_ent *ne = (struct rd_ent *)realloc(r->e, nc * sizeof *ne);
    if (!ne) { r->err = -ENOMEM; return false; }
    r->e = ne; r->cap = nc;
  }
  char *nm = (char *)malloc((size_t)len + 1);
  if (!nm) { r->err = -ENOMEM; return false; }
  memcpy(nm, name, (size_t)len); nm[len] = 0;
  r->e[r->n].name = nm; r->e[r->n].len = len; r->e[r->n].type = type; r->n++;
  return true;
}
int usbmem_fat_readdir(usbmem_fat *v, uint64_t dir_ino, usbmem_fat_dir_cb cb, void *user) {
  struct inode *dir = pin_find(v, dir_ino);
  if (!dir) return -ENOENT;
  if (!S_ISDIR(dir->i_mode) || !dir->i_fop || !dir->i_fop->iterate_shared) return -ENOTDIR;
  struct file f; memset(&f, 0, sizeof f);
  f.f_inode = dir; f.f_mapping = dir->i_mapping; f.f_mode = FMODE_READ;
  int rc = 0;
  if (dir->i_fop->open) rc = ((int (*)(struct inode *, struct file *))dir->i_fop->open)(dir, &f);
  if (rc) return rc;
  struct rd_ctx r; memset(&r, 0, sizeof r); r.c.actor = rd_actor;
  down_read(&dir->i_rwsem);
  rc = ((int (*)(struct file *, struct dir_context *))dir->i_fop->iterate_shared)(&f, &r.c);
  up_read(&dir->i_rwsem);
  if (!rc) rc = r.err;
  if (dir->i_fop->release) ((int (*)(struct inode *, struct file *))dir->i_fop->release)(dir, &f);
  int stop = 0;
  for (size_t k = 0; k < r.n; k++) {
    if (!rc && !stop) {
      struct inode *i = NULL;
      int lr = lookup_pin(v, dir, r.e[k].name, (size_t)r.e[k].len, &i);
      if (lr) rc = lr;
      else if (cb(user, r.e[k].name, r.e[k].len, i->i_ino, r.e[k].type)) stop = 1;   /* 呼び出し側が打ち切り */
    }
    free(r.e[k].name);
  }
  free(r.e);
  return rc;
}

int64_t usbmem_fat_read(usbmem_fat *v, uint64_t ino, uint64_t off, void *buf, size_t len) {
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
