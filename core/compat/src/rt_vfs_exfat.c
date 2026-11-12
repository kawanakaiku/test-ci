// SPDX-License-Identifier: GPL-2.0-or-later
// ホスト向け VFS 風入口 (exFAT)。Linux の VFS が行う「lookup → i_op->lookup / i_fop->iterate_shared / read_mapping_folio」を直接呼ぶ。
// NTFS (rt_vfs.c) との違い: exFAT の inode 番号は iunique() で実行時に採番され、ino から inode を引く iget(sb, ino) が無い。
// そこで lookup / readdir / create が返した inode を「ピン」(参照を保持) して ino → inode の表を持つ。消したファイルのピンは外す。unmount で全て解放する。
// 書き込み (ビルドで USBMEM_NTFS_WRITE=ON かつ usbmem_exfat_mount(read_only=0)): docs/exfat-write.md。
//  - 常に**読み取り専用でマウント**し、最初の書き込みの直前に rw へ切り替えて VolumeDirty を確定する (exfat_set_volume_dirty = 書いて FUA/flush)。マウントだけではメディアに触れない。
//  - 前提 (満たさなければ読み取り専用のまま: writable() == 0): VolumeDirty / MediaFailure が立っていない (前回きれいに取り外された)、FAT が 1 面。
//  - 書き出しの順序 (usbmem_exfat_sync): データ → [確保系] FAT・ビットマップ → ディレクトリエントリ / [解放系] ディレクトリエントリ → FAT・ビットマップ。
//    確保系と解放系の操作が混ざる前に必ず同期する (解放したクラスタを同期の前に再利用しない / 解放中のファイルのチェーンを先に壊さない)。
#include <stdlib.h>
#include <compat/kernel.h>
#include <compat/usbmem_ntfs.h>

#include "usbmem/exfat_c.h"
#include "usbmem/ntfs_c.h"   /* usbmem_bdev_* の宣言 */
#include "exfat_raw.h"
#include "exfat_fs.h"        /* 上流 fs/exfat: struct exfat_sb_info / EXFAT_I / exfat_set_volume_dirty (-iquote で解決) */

struct pin { uint64_t ino; struct inode *inode; uint64_t parent; struct pin *next; };   /* parent: 見つけた(作った)ときのディレクトリの ino (rename で更新)。exFAT に ".." は無く、inode の dir.dir は親チェーンの途中のクラスタなので祖先をたどれない */
enum { K_NONE = 0, K_ALLOC = 1, K_FREE = 2 };
struct usbmem_exfat {
  struct super_block *sb;
  struct pin *tab[1024];
  int can_write;   /* 書き込み要求あり、かつ前提を満たす */
  unsigned char *bm_snap; size_t bm_snap_len;   /* 前回の同期時点の割り当てビットマップ (新しく確保されたクラスタを知るため) */
  int pending;     /* 同期前の変更の種類: K_ALLOC (確保・書き込み・名前の追加) / K_FREE (削除・縮小・上書き rename)。種類が変わる前に同期する */
};

static struct pin *pin_entry(usbmem_exfat *v, uint64_t ino) {
  for (struct pin *p = v->tab[ino & 1023]; p; p = p->next) if (p->ino == ino) return p;
  return NULL;
}
static struct inode *pin_find(usbmem_exfat *v, uint64_t ino) { struct pin *p = pin_entry(v, ino); return p ? p->inode : NULL; }
/* i の参照 (呼び出し側が持つ 1 つ) をピンへ移す。すでに同じ ino がピン済みなら、その参照を返して i を手放す。 */
static struct inode *pin_add(usbmem_exfat *v, struct inode *i, uint64_t parent) {
  struct pin *old = pin_entry(v, i->i_ino);
  if (old) { iput(i); old->parent = parent; return old->inode; }
  struct pin *p = malloc(sizeof *p);
  if (!p) { iput(i); return NULL; }
  p->ino = i->i_ino; p->inode = i; p->parent = parent; p->next = v->tab[i->i_ino & 1023]; v->tab[i->i_ino & 1023] = p;
  return i;
}
/* ピンを外して参照を手放す (削除したファイル・ディレクトリ用)。他に参照が無ければここで inode が解放される (クラスタの解放を伴う)。 */
static void pin_remove(usbmem_exfat *v, uint64_t ino) {
  for (struct pin **pp = &v->tab[ino & 1023]; *pp; pp = &(*pp)->next) {
    if ((*pp)->ino == ino) { struct pin *p = *pp; *pp = p->next; iput(p->inode); free(p); return; }
  }
}

/* ---- 書き込み可の判定 (ブートセクタ): VolumeDirty / MediaFailure なし、FAT 1 面、セクタ 512〜4096 */
static int writable_precheck(struct usbmem_bdev *bdev) {
#ifdef USBMEM_NTFS_WRITE
  unsigned char b[512];
  if (usbmem_bdev_read(bdev, 0, b, sizeof b)) return 0;
  if (memcmp(b + 3, "EXFAT   ", 8) != 0 || b[510] != 0x55 || b[511] != 0xAA) return 0;
  unsigned flags = b[106] | (b[107] << 8), num_fats = b[110], sect_bits = b[108];
  if (flags & (VOLUME_DIRTY | MEDIA_FAILURE)) return 0;
  if (num_fats != 1) return 0;
  if (sect_bits < 9 || sect_bits > 12) return 0;
  return 1;
#else
  (void)bdev; return 0;
#endif
}

int usbmem_exfat_mount(struct usbmem_bdev *bdev, int ro, usbmem_exfat **out) {
#ifndef USBMEM_NTFS_WRITE
  if (!ro) return -EROFS;   /* 書き込みはビルドで無効 */
#endif
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
  sb->usbmem_bh_fs_ = 1;          /* FAT・ビットマップ・ディレクトリエントリは buffer_head (bd_mapping)。umount の同期で bd の dirty も書く */
  sb->usbmem_fill_via_read_ = 1;  /* クラスタ < 4096 では 1 folio に離れたエクステントが載る: 書き込み前の folio 補充は read_folio で行う */
  struct inode *root = sb->s_root->d_inode;
  ihold(root);
  if (!pin_add(v, root, 0)) { usbmem_exfat_unmount(v); return -ENOMEM; }
  if (!ro && writable_precheck(bdev)) {
    v->can_write = 1;
    /* 異常を見つけたら読み取り専用に落とす (errors=remount-ro は既定)。 */
  }
  *out = v;
  return 0;
}
int usbmem_exfat_writable(usbmem_exfat *v) { return v && v->can_write; }

/* ---- 同期 */
static void fat_bitmap_ranges(struct super_block *sb, struct usbmem_range *r) {
  struct exfat_sb_info *sbi = EXFAT_SB(sb);
  u64 ss = sb->s_blocksize;
  r[0].off = sbi->FAT1_start_sector * ss; r[0].len = (u64)sbi->num_FAT_sectors * ss;
  r[1].off = (u64)exfat_cluster_to_sector(sbi, sbi->map_clu) * ss; r[1].len = (u64)sbi->map_sectors * ss;
}
/* 割り当てビットマップの写しを取る (snap が NULL なら確保) */
static void bm_snapshot(usbmem_exfat *v) {
  struct exfat_sb_info *sbi = EXFAT_SB(v->sb);
  size_t bs = v->sb->s_blocksize, len = (size_t)sbi->map_sectors * bs;
  if (!v->bm_snap) { v->bm_snap = malloc(len); v->bm_snap_len = v->bm_snap ? len : 0; }
  if (!v->bm_snap) return;
  for (unsigned i = 0; i < sbi->map_sectors; i++) memcpy(v->bm_snap + (size_t)i * bs, sbi->vol_amap[i]->b_data, bs);
}
/* 前回の同期から新しく確保されたクラスタのバイト範囲 (連続は結合)。呼び出し側が free する。確保した範囲数を *n に返す */
static struct usbmem_range *new_cluster_ranges(usbmem_exfat *v, int *n) {
  struct exfat_sb_info *sbi = EXFAT_SB(v->sb);
  size_t bs = v->sb->s_blocksize; u64 ss = bs;
  int cap = 64, cnt = 0; struct usbmem_range *r = NULL;
  *n = 0;
  if (!v->bm_snap) return NULL;
  r = malloc((size_t)cap * sizeof *r);
  if (!r) return NULL;
  for (unsigned c = EXFAT_FIRST_CLUSTER; c < sbi->num_clusters + EXFAT_RESERVED_CLUSTERS; c++) {
    unsigned bit = c - EXFAT_RESERVED_CLUSTERS, sec = bit / (bs * 8), off = (bit % (bs * 8)) / 8, m = 1u << (bit % 8);
    if (sec >= sbi->map_sectors) break;
    int now = (sbi->vol_amap[sec]->b_data[off] & m) != 0, was = (v->bm_snap[(size_t)sec * bs + off] & m) != 0;
    if (!now || was) continue;
    u64 a = (u64)exfat_cluster_to_sector(sbi, c) * ss, l = sbi->cluster_size;
    if (cnt && r[cnt - 1].off + r[cnt - 1].len == a) { r[cnt - 1].len += l; continue; }
    if (cnt == cap) { cap *= 2; struct usbmem_range *t = realloc(r, (size_t)cap * sizeof *r); if (!t) { free(r); return NULL; } r = t; }
    r[cnt].off = a; r[cnt].len = l; cnt++;
  }
  *n = cnt;
  return r;
}
/* v->tab の dirty な inode を書き出す。順序は上のコメント。 */
static int sync_all(usbmem_exfat *v) {
  struct super_block *sb = v->sb;
  if (sb_rdonly(sb)) return 0;
  int first_err = 0, wrote = 0;
  struct usbmem_range fb[2]; fat_bitmap_ranges(sb, fb);
  /* 1. データ (新しく確保したクラスタ・既存の範囲内への上書き。ここで途切れても、どのファイルのメタデータもまだ変わっていない) */
  for (int b = 0; b < 1024; b++) for (struct pin *p = v->tab[b]; p; p = p->next) {
    struct inode *i = p->inode;
    if (!S_ISDIR(i->i_mode) && usbmem_mapping_has_dirty(i->i_mapping)) {
      int rc = usbmem_mapping_writeback(i->i_mapping, 1);
      if (rc && !first_err) first_err = rc; else if (!rc) i->i_state &= ~(unsigned long)I_DIRTY_PAGES;
      wrote = 1;
    }
  }
  if (wrote && !first_err) { int rc = usbmem_bdev_flush(sb->s_bdev->impl); if (rc) first_err = rc; }
  /* 2. inode のメタデータ (write_inode: ディレクトリエントリセットを更新 = bd の dirty に) */
  for (int b = 0; b < 1024 && !first_err; b++) for (struct pin *p = v->tab[b]; p; p = p->next) {
    struct inode *i = p->inode;
    if (i->i_state & (I_DIRTY_SYNC | I_DIRTY_DATASYNC)) { int rc = sync_inode_metadata(i, 0); if (rc && !first_err) first_err = rc; }
  }
  /* 3. メタデータ (bd の dirty)。ルートディレクトリはエントリを持たず「FAT のチェーン」だけが範囲を決めるので、FAT は新クラスタの中身とビットマップが届いた後に書く。
        確保系: 新クラスタの中身 (新しいディレクトリのゼロ埋め・伸びたディレクトリの新しい部分) → ビットマップ → FAT → ディレクトリエントリ。
        解放系: ディレクトリエントリ (参照を先に消す) → FAT → ビットマップ。どの段階の間にも flush を入れる。
        (新クラスタの中身を参照するものより先に書かないと、電源断後にディレクトリの中に古いクラスタの残骸が見える) */
  if (!first_err) {
    int alloc_first = v->pending != K_FREE;   /* 確保系 (または不明) */
    int rc = 0, nn = 0;
#define STEP(expr) do { if (!rc) rc = (expr); if (!rc) rc = usbmem_bdev_flush(sb->s_bdev->impl); } while (0)
    struct usbmem_range *nr = new_cluster_ranges(v, &nn);
#ifdef USBMEM_MUTATE_ORDER   /* 突然変異テスト専用: 順序を逆にする (tools/run_exfat_crash_tests.sh が検出できることを確かめる) */
    alloc_first = !alloc_first;
#endif
#ifndef USBMEM_MUTATE_NO_NEWCLU_FIRST   /* 突然変異テスト専用: 新クラスタの中身を先に書く規則を外す */
    if (nn) STEP(usbmem_bdev_writeback_ranges(sb->s_bdev, nr, nn, 1));
#endif
    free(nr);
    if (alloc_first) {
      STEP(usbmem_bdev_writeback_ranges(sb->s_bdev, &fb[1], 1, 1));   /* ビットマップ */
      STEP(usbmem_bdev_writeback_ranges(sb->s_bdev, &fb[0], 1, 1));   /* FAT */
      STEP(usbmem_bdev_writeback_ranges(sb->s_bdev, fb, 2, 0));       /* 残り = ディレクトリエントリ */
    } else {
      STEP(usbmem_bdev_writeback_ranges(sb->s_bdev, fb, 2, 0));
      STEP(usbmem_bdev_writeback_ranges(sb->s_bdev, &fb[0], 1, 1));
      STEP(usbmem_bdev_writeback_ranges(sb->s_bdev, &fb[1], 1, 1));
    }
#undef STEP
    if (rc) first_err = rc;
  }
  if (!first_err) { v->pending = K_NONE; bm_snapshot(v); }
  if (sb_rdonly(sb)) v->can_write = 0;   /* 上流がエラーを見つけて読み取り専用に落ちた */
  return first_err;
}
int usbmem_exfat_sync(usbmem_exfat *v) { return v ? sync_all(v) : -EINVAL; }

/* 書き込みの入口: 最初の 1 回は rw へ切り替えて VolumeDirty を確定する (最初の変更の前)。種類が変わるときは先に同期する。 */
static int begin_op(usbmem_exfat *v, int kind) {
  if (!v || !v->can_write) return -EROFS;
  if (sb_rdonly(v->sb)) {
    v->sb->s_flags &= ~(unsigned long)SB_RDONLY;
#ifdef USBMEM_MUTATE_NO_DIRTY   /* 突然変異テスト専用: ダーティ印をメモリ上でだけ立て、ディスクへは書かない (最初の変更の前に確定する規則を外す) */
    { struct exfat_sb_info *sbi_ = EXFAT_SB(v->sb); struct boot_sector *pb = (struct boot_sector *)sbi_->boot_bh->b_data; sbi_->vol_flags |= VOLUME_DIRTY; pb->percent_in_use = 0xFF; }
#endif
    int rc = exfat_set_volume_dirty(v->sb);
    if (rc) { v->sb->s_flags |= SB_RDONLY; v->can_write = 0; return rc; }
    bm_snapshot(v);   /* ここまでの割り当て状態を基準にする (以降に確保されたクラスタを同期のとき見分ける) */
  }
#ifndef USBMEM_MUTATE_NO_KIND_SYNC   /* 突然変異テスト専用: 確保系と解放系の切り替えで同期しない (解放したクラスタを同期前に再利用する) */
  if (v->pending != K_NONE && v->pending != kind) { int rc = sync_all(v); if (rc) return rc; }
#endif
  v->pending = kind;
  return 0;
}
static int after_op(usbmem_exfat *v, int rc) {
  if (sb_rdonly(v->sb)) v->can_write = 0;
  return rc;
}

void usbmem_exfat_unmount(usbmem_exfat *v) {
  if (!v) return;
  if (!sb_rdonly(v->sb)) sync_all(v);   /* 順序付きで全部書く (その後 umount が VolumeDirty を消す) */
  for (int b = 0; b < 1024; b++) {
    struct pin *p = v->tab[b];
    while (p) { struct pin *n = p->next; iput(p->inode); free(p); p = n; }
    v->tab[b] = NULL;
  }
  struct block_device *bd = v->sb->s_bdev;
  usbmem_compat_umount(v->sb);
  usbmem_bdev_mapping_free(bd);
  free(bd);
  free(v->bm_snap);
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
    else { ihold(use->d_inode); struct inode *p = pin_add(v, use->d_inode, dir->i_ino); if (p) *out = p; else rc = -ENOMEM; }
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
static int check_name(const char *name, size_t len) {
  if (!len) return -EINVAL;
  if (len > 255 * 4) return -ENAMETOOLONG;   /* UTF-16 で 255 文字を超えるかは上流が判定する (-ENAMETOOLONG) */
  if (memchr(name, '/', len) || memchr(name, 0, len) || (len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.')) return -EINVAL;
  return 0;
}
/* 親 dentry (dir を指す) の下に name の dentry を作る。lookup する (見つかれば d_inode 付き、無ければ負の dentry)。 */
static struct dentry *new_d(struct inode *dir, const char *name, size_t len, int do_lookup, int *err) {
  struct dentry *pd = usbmem_d_alloc(dir->i_sb, NULL, "", 0);
  if (!pd) { *err = -ENOMEM; return NULL; }
  ihold(dir); pd->d_inode = dir;   /* usbmem_d_free(pd) が iput */
  struct dentry *d = usbmem_d_alloc(dir->i_sb, pd, name, (unsigned)len);
  if (!d) { usbmem_d_free(pd); *err = -ENOMEM; return NULL; }
  if (!do_lookup) return d;
  struct dentry *r = ((struct dentry *(*)(struct inode *, struct dentry *, unsigned))dir->i_op->lookup)(dir, d, 0);
  if (IS_ERR(r)) { *err = (int)PTR_ERR(r); free_d(d); return NULL; }
  if (r && r != d) { r->d_parent = pd; d->d_parent = d; usbmem_d_free(d); d = r; }
  return d;
}
static int lookup_d(struct inode *dir, const char *name, size_t len, struct dentry **out) {
  if (!S_ISDIR(dir->i_mode) || !dir->i_op || !dir->i_op->lookup) return -ENOTDIR;
  int rc = check_name(name, len); if (rc) return rc;
  int err = 0; struct dentry *d = new_d(dir, name, len, 1, &err);
  if (!d) return err;
  *out = d; return 0;
}

static struct inode *dir_of(usbmem_exfat *v, uint64_t ino, int *err) {
  struct inode *i = pin_find(v, ino);
  if (!i) { *err = -ENOENT; return NULL; }
  return i;
}

/* kind: 0 create / 1 mkdir / 2 unlink / 3 rmdir */
static int ns_op(usbmem_exfat *v, int kind, uint64_t dir_ino, const char *name, size_t len, uint32_t mode, uint64_t *ino) {
  { int e = begin_op(v, kind >= 2 ? K_FREE : K_ALLOC); if (e) return e; }
  int err = 0; struct inode *dir = dir_of(v, dir_ino, &err);
  if (!dir) return err;
  struct dentry *d = NULL;
  int rc = lookup_d(dir, name, len, &d);
  if (rc) return rc;
  ihold(dir);
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
    if (!rc) {
      if (!d->d_inode) rc = -EIO;
      else { ihold(d->d_inode); struct inode *p = pin_add(v, d->d_inode, dir->i_ino); if (!p) rc = -ENOMEM; else if (ino) *ino = p->i_ino; }
    }
    break;
  case 2: case 3:
    if (!d->d_inode) { rc = -ENOENT; break; }
    if (kind == 2 && S_ISDIR(d->d_inode->i_mode)) { rc = -EISDIR; break; }
    if (kind == 3 && !S_ISDIR(d->d_inode->i_mode)) { rc = -ENOTDIR; break; }
    if (kind == 2) rc = dir->i_op->unlink ? ((int (*)(struct inode *, struct dentry *))dir->i_op->unlink)(dir, d) : -EOPNOTSUPP;
    else rc = dir->i_op->rmdir ? ((int (*)(struct inode *, struct dentry *))dir->i_op->rmdir)(dir, d) : -EOPNOTSUPP;
    if (!rc) pin_remove(v, d->d_inode->i_ino);   /* ピンを外す。dentry を解放すると (nlink 0 なので) inode が追い出され、クラスタが解放される */
    break;
  }
  up_write(&dir->i_rwsem);
  iput(dir);
  free_d(d);
  return after_op(v, rc);
}
int usbmem_exfat_create(usbmem_exfat *v, uint64_t d, const char *n, size_t l, uint32_t m, uint64_t *ino) { return ns_op(v, 0, d, n, l, m, ino); }
int usbmem_exfat_mkdir(usbmem_exfat *v, uint64_t d, const char *n, size_t l, uint32_t m, uint64_t *ino) { return ns_op(v, 1, d, n, l, m, ino); }
int usbmem_exfat_unlink(usbmem_exfat *v, uint64_t d, const char *n, size_t l) { return ns_op(v, 2, d, n, l, 0, NULL); }
int usbmem_exfat_rmdir(usbmem_exfat *v, uint64_t d, const char *n, size_t l) { return ns_op(v, 3, d, n, l, 0, NULL); }

/* src が dir の祖先 (または dir 自身) か。親は pin の parent でたどる (dir は呼び出し側が path をたどって得たものなので、祖先は全てピン済み)。たどれなければ安全側 (移動を拒否) */
static int is_ancestor(usbmem_exfat *v, struct inode *src, struct inode *dir) {
  struct inode *cur = dir;
  for (int n = 0; n < 4096; n++) {
    if (cur == src) return 1;
    if (cur->i_ino == EXFAT_ROOT_INO) return 0;
    struct pin *p = pin_entry(v, cur->i_ino);
    if (!p) return 1;
    cur = pin_find(v, p->parent);
    if (!cur) return 1;
  }
  return 1;
}

int usbmem_exfat_rename(usbmem_exfat *v, uint64_t od, const char *on, size_t ol, uint64_t nd, const char *nn, size_t nl) {
  int err = 0; struct inode *odir = dir_of(v, od, &err); if (!odir) return err;
  struct inode *ndir = dir_of(v, nd, &err); if (!ndir) return err;
  struct dentry *od_ = NULL, *nd_ = NULL;
  int rc = lookup_d(odir, on, ol, &od_);
  if (!rc) { rc = lookup_d(ndir, nn, nl, &nd_); if (rc) free_d(od_); }
  if (rc) return rc;
  struct inode *src = od_->d_inode, *dst = nd_->d_inode;
  /* 大文字小文字だけが違う名前への変更 (同じ inode): 「重複」ではなく同じエントリの名前変更。移動先を負の dentry に置き換える (上流が自分自身を消さないように) */
  int case_only = src && dst && dst == src && !(odir == ndir && ol == nl && memcmp(on, nn, ol) == 0);
  if (case_only) {
    free_d(nd_); nd_ = new_d(ndir, nn, nl, 0, &rc);
    if (!nd_) { free_d(od_); return rc; }
    dst = NULL;
  }
  if (!src) rc = -ENOENT;
  else if (IMMUTABLE_OR_APPEND(odir) || IMMUTABLE_OR_APPEND(ndir) || IMMUTABLE_OR_APPEND(src) || (dst && IMMUTABLE_OR_APPEND(dst))) rc = -EPERM;
  else if (S_ISDIR(src->i_mode) && is_ancestor(v, src, ndir)) rc = -EINVAL;   /* 自分の中へは動かせない (Linux は型の検査より先に -EINVAL) */
  else if (dst && dst == src) rc = 0;   /* 同じ名前への rename は何もしない (VFS と同じ) */
  else if (dst && S_ISDIR(src->i_mode) != S_ISDIR(dst->i_mode)) rc = S_ISDIR(src->i_mode) ? -ENOTDIR : -EISDIR;
  else if (!odir->i_op->rename) rc = -EOPNOTSUPP;
  else {
    rc = begin_op(v, dst ? K_FREE : K_ALLOC);
    if (!rc) {
      ihold(odir); ihold(ndir);
      down_write(&odir->i_rwsem); if (ndir != odir) down_write(&ndir->i_rwsem);
      rc = ((int (*)(struct mnt_idmap *, struct inode *, struct dentry *, struct inode *, struct dentry *, unsigned))odir->i_op->rename)(&nop_mnt_idmap, odir, od_, ndir, nd_, 0);
      if (ndir != odir) up_write(&ndir->i_rwsem); up_write(&odir->i_rwsem);
      iput(ndir); iput(odir);
      if (!rc) { struct pin *sp = pin_entry(v, src->i_ino); if (sp) sp->parent = ndir->i_ino; }   /* 移動した先を親にする */
      if (!rc && dst) pin_remove(v, dst->i_ino);   /* 上書きされた側: ピンを外す → dentry 解放時に追い出され、クラスタが解放される */
    }
  }
  free_d(nd_); free_d(od_);
  return after_op(v, rc);
}

/* 書き込み 1 回分の後始末: このファイルのデータページをデバイスへ書く (メモリに溜めない) */
static int64_t write_common(usbmem_exfat *v, uint64_t ino, uint64_t off, const void *buf, size_t len, int inplace) {
  { int e = begin_op(v, K_ALLOC); if (e) return e; }
  int err = 0; struct inode *i = dir_of(v, ino, &err);
  if (!i) return err;
  if (S_ISDIR(i->i_mode)) return -EISDIR;
  if (!S_ISREG(i->i_mode)) return -EINVAL;
  if (inplace && off + len > (uint64_t)i->i_size) return -EINVAL;
  if (!len) return 0;
  if (!i->i_fop || !i->i_fop->write_iter) return -EOPNOTSUPP;
  struct file f; memset(&f, 0, sizeof f);
  f.f_inode = i; f.f_mapping = i->i_mapping; f.f_mode = FMODE_READ | FMODE_WRITE;
  int rc = 0;
  ihold(i);
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
int64_t usbmem_exfat_write(usbmem_exfat *v, uint64_t ino, uint64_t off, const void *buf, size_t len) { return write_common(v, ino, off, buf, len, 0); }
int64_t usbmem_exfat_write_inplace(usbmem_exfat *v, uint64_t ino, uint64_t off, const void *buf, size_t len) { return write_common(v, ino, off, buf, len, 1); }

/* サイズ変更。縮小は解放を伴う (解放系)、拡張は確保 (確保系)。 */
int usbmem_exfat_truncate(usbmem_exfat *v, uint64_t ino, uint64_t size) {
  int err = 0; struct inode *i = dir_of(v, ino, &err);
  if (!i) return err;
  if (S_ISDIR(i->i_mode)) return -EISDIR;
  if (!S_ISREG(i->i_mode)) return -EINVAL;
  if (!i->i_op || !i->i_op->setattr) return -EOPNOTSUPP;
  if ((uint64_t)i->i_size == size) return v->can_write ? 0 : -EROFS;
  if (size > (uint64_t)i->i_sb->s_maxbytes) return -EFBIG;
  int shrink = size < (uint64_t)i->i_size;
  { int e = begin_op(v, shrink ? K_FREE : K_ALLOC); if (e) return e; }
  struct dentry *d = usbmem_d_alloc(i->i_sb, NULL, "t", 1);
  if (!d) return -ENOMEM;
  ihold(i); d->d_inode = i;   /* usbmem_d_free が iput */
  struct iattr a; memset(&a, 0, sizeof a);
  a.ia_valid = ATTR_SIZE; a.ia_size = (loff_t)size;
  down_write(&i->i_rwsem);
  int rc = ((int (*)(struct mnt_idmap *, struct dentry *, struct iattr *))i->i_op->setattr)(&nop_mnt_idmap, d, &a);
  up_write(&i->i_rwsem);
  usbmem_d_free(d);
  return after_op(v, rc);
}

#else  /* !USBMEM_NTFS_WRITE: 書き込み入口は全て -EROFS */
int usbmem_exfat_create(usbmem_exfat *v, uint64_t d, const char *n, size_t l, uint32_t m, uint64_t *ino) { (void)v; (void)d; (void)n; (void)l; (void)m; (void)ino; return -EROFS; }
int usbmem_exfat_mkdir(usbmem_exfat *v, uint64_t d, const char *n, size_t l, uint32_t m, uint64_t *ino) { (void)v; (void)d; (void)n; (void)l; (void)m; (void)ino; return -EROFS; }
int usbmem_exfat_unlink(usbmem_exfat *v, uint64_t d, const char *n, size_t l) { (void)v; (void)d; (void)n; (void)l; return -EROFS; }
int usbmem_exfat_rmdir(usbmem_exfat *v, uint64_t d, const char *n, size_t l) { (void)v; (void)d; (void)n; (void)l; return -EROFS; }
int usbmem_exfat_rename(usbmem_exfat *v, uint64_t od, const char *on, size_t ol, uint64_t nd, const char *nn, size_t nl) { (void)v; (void)od; (void)on; (void)ol; (void)nd; (void)nn; (void)nl; return -EROFS; }
int64_t usbmem_exfat_write(usbmem_exfat *v, uint64_t ino, uint64_t off, const void *buf, size_t len) { (void)v; (void)ino; (void)off; (void)buf; (void)len; return -EROFS; }
int64_t usbmem_exfat_write_inplace(usbmem_exfat *v, uint64_t ino, uint64_t off, const void *buf, size_t len) { (void)v; (void)ino; (void)off; (void)buf; (void)len; return -EROFS; }
int usbmem_exfat_truncate(usbmem_exfat *v, uint64_t ino, uint64_t size) { (void)v; (void)ino; (void)size; return -EROFS; }
#endif
