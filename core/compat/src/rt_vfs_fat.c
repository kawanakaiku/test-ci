// SPDX-License-Identifier: GPL-2.0-or-later
// ホスト向け VFS 風入口 (FAT12/16/32 + vfat の長い名前)。Linux の VFS が行う「lookup → i_op->lookup / i_fop->iterate_shared / read_mapping_folio」を直接呼ぶ。
// FAT の inode 番号は iunique() で実行時に採番され、ino から inode を引く iget(sb, ino) が無い (exFAT と同じ)。
// そこで lookup / readdir / create が返した inode を「ピン」(参照を保持) して ino → inode の表を持つ。消したファイルのピンは外す。unmount で全て解放する。
// 上流 fs/fat の module_init は 2 つ (inode.c = キャッシュの用意, namei_vfat.c = vfat の登録)。CMake が別名にして、ここで順に呼ぶ。nls_cp437 / nls_cp932 の登録も同じ。
// 書き込み (ビルドで USBMEM_NTFS_WRITE=ON かつ usbmem_fat_mount(read_only=0)): docs/fat-write.md。ジャーナルが無いので、安全は「書く順序」と「ダーティ印」で作る。
//  - 常に**読み取り専用でマウント**し、最初の書き込みの直前に rw へ切り替える: ブートセクタのダーティビット (fat_set_state = 書いて flush) → FAT16/32 は FAT[1] のクリーン bit を落とす → FAT32 の FSInfo を「不明」にする。マウントだけではメディアに触れない。
//  - 前提 (満たさなければ読み取り専用のまま: writable() == 0): ブートセクタのダーティ/メディアエラー bit なし、FAT[1] のクリーン bit あり (FAT16/32)、FAT が 1〜2 面。
//  - 書き出しの順序 (usbmem_fat_sync): ファイルのデータ → inode (ディレクトリエントリ) をキャッシュ上で更新 →
//      [確保系] 新クラスタの中身 (新しいディレクトリ) → FAT1 → FAT2 → ディレクトリエントリ (カーネルが汚した順。同じ操作の中は flush を挟む)
//      [解放系] ディレクトリエントリ → FAT1 → FAT2
//    確保系と解放系の操作が混ざる前に必ず同期する (解放したクラスタを同期の前に再利用しない / 解放中のファイルのチェーンを先に壊さない)。
//  - 取り外し (unmount): 全部書いた後、FAT32 は空きクラスタ数を数え直して FSInfo を確定 → FAT[1] のクリーン bit を戻す → (fat_put_super が) ブートセクタのダーティビットを消す。
#include <stdlib.h>
#include <compat/kernel.h>
#include <compat/usbmem_ntfs.h>

#include "usbmem/fat_c.h"
#include "usbmem/ntfs_c.h"   /* usbmem_bdev_* の宣言 */
#include "fat.h"             /* 上流 fs/fat: struct msdos_sb_info (-iquote で解決) */

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
struct pin { uint64_t ino; struct inode *inode; uint64_t parent; struct pin *next; };   /* parent: 見つけた(作った)ときのディレクトリの ino (rename で更新。移動先が自分の子孫でないことの検査に使う) */
enum { K_NONE = 0, K_ALLOC = 1, K_FREE = 2 };
struct dlog_ent { uint64_t off; unsigned op; };
struct usbmem_fat {
  struct super_block *sb;
  struct pin *tab[1024];
  int can_write;   /* 書き込み要求あり、かつ前提を満たす */
  int pending;     /* 同期前の変更の種類: K_ALLOC (確保・書き込み・名前の追加) / K_FREE (削除・縮小・上書き rename)。種類が変わる前に同期する */
  int dirtied;     /* 最初の書き込みの前のダーティ印をメディアへ確定済み (取り外しのときに戻す) */
  /* ディレクトリ領域のセクタが初めて dirty になった順 (カーネルは長い名前のスロットを先に、短い名前を最後に書く。その順を保つ) */
  struct dlog_ent *log; size_t log_n, log_cap;
  uint64_t *seen; size_t seen_cap, seen_n;   /* log 内の off の集合 (開番地法。値は off+1、0 は空) */
  unsigned op;     /* 公開操作の通し番号 (同じ操作の中のセクタは flush を挟んで順に書く) */
  uint64_t meta_start;   /* FAT の後ろ (ルートディレクトリ領域・データ領域) の先頭バイト */
};

static struct pin *pin_entry(usbmem_fat *v, uint64_t ino) {
  for (struct pin *p = v->tab[ino & 1023]; p; p = p->next) if (p->ino == ino) return p;
  return NULL;
}
static struct inode *pin_find(usbmem_fat *v, uint64_t ino) { struct pin *p = pin_entry(v, ino); return p ? p->inode : NULL; }
/* i の参照 (呼び出し側が持つ 1 つ) をピンへ移す。すでに同じ ino がピン済みなら、その参照を返して i を手放す。 */
static struct inode *pin_add(usbmem_fat *v, struct inode *i, uint64_t parent) {
  struct pin *old = pin_entry(v, i->i_ino);
  if (old) { iput(i); old->parent = parent; return old->inode; }
  struct pin *p = malloc(sizeof *p);
  if (!p) { iput(i); return NULL; }
  p->ino = i->i_ino; p->inode = i; p->parent = parent; p->next = v->tab[i->i_ino & 1023]; v->tab[i->i_ino & 1023] = p;
  return i;
}
/* ピンを外して参照を手放す (削除したファイル・ディレクトリ用)。他に参照が無ければここで inode が解放される (クラスタの解放を伴う)。 */
static void pin_remove(usbmem_fat *v, uint64_t ino) {
  for (struct pin **pp = &v->tab[ino & 1023]; *pp; pp = &(*pp)->next) {
    if ((*pp)->ino == ino) { struct pin *p = *pp; *pp = p->next; iput(p->inode); free(p); return; }
  }
}

#ifdef USBMEM_NTFS_WRITE
static int sync_all(usbmem_fat *v);
static int close_clean(usbmem_fat *v);
static void dirty_hook(void *ctx, u64 off, size_t len);
#endif

/* ---- 書き込み可の判定 (RO でマウントした後): ブートセクタのダーティ/メディアエラー bit なし、FAT[1] のクリーン bit あり (FAT16/32)、FAT 1〜2 面。 */
static int fat1_bit(struct msdos_sb_info *sbi, u32 *bit, size_t *eoff) {
  if (sbi->fat_bits == 16) { *bit = 0x8000u; *eoff = 2; return 1; }
  if (sbi->fat_bits == 32) { *bit = 0x08000000u; *eoff = 4; return 1; }
  return 0;   /* FAT12 には無い */
}
static int writable_precheck(struct super_block *sb) {
#ifdef USBMEM_NTFS_WRITE
  struct msdos_sb_info *sbi = MSDOS_SB(sb);
  if (sbi->dirty) return 0;
  if (sbi->fats < 1 || sbi->fats > 2) return 0;
  struct buffer_head *bh = sb_bread(sb, 0);
  if (!bh) return 0;
  unsigned char st = ((unsigned char *)bh->b_data)[sbi->fat_bits == 32 ? 0x41 : 0x25];
  brelse(bh);
  if (st & 0x03) return 0;   /* bit0 = ダーティ, bit1 = メディアエラー (表面検査が必要) */
  u32 bit; size_t eoff;
  if (fat1_bit(sbi, &bit, &eoff)) {
    for (int f = 0; f < sbi->fats; f++) {   /* どの面の FAT[1] でも、クリーン bit が落ちていたら書かない (Linux は 1 面目しか見ないが、2 面目だけ落ちている = 途中で止まった更新) */
      bh = sb_bread(sb, sbi->fat_start + (sector_t)f * sbi->fat_length);
      if (!bh) return 0;
      const unsigned char *p = (const unsigned char *)bh->b_data + eoff;
      u32 val = sbi->fat_bits == 16 ? (u32)(p[0] | (p[1] << 8)) : (u32)(p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24));
      brelse(bh);
      if (!(val & bit)) return 0;   /* 前回きれいに取り外されていない (Windows 流のダーティ印) */
    }
  }
  return 1;
#else
  (void)sb; return 0;
#endif
}

int usbmem_fat_mount(struct usbmem_bdev *bdev, int ro, usbmem_fat **out) {
#ifndef USBMEM_NTFS_WRITE
  if (!ro) return -EROFS;   /* 書き込みはビルドで無効 */
#endif
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
  sb->usbmem_bh_fs_ = 1;   /* FAT・ディレクトリエントリは buffer_head (bd_mapping)。umount の同期で bd の dirty も書く */
  { struct msdos_sb_info *sbi = MSDOS_SB(sb); v->meta_start = ((uint64_t)sbi->fat_start + (uint64_t)sbi->fats * sbi->fat_length) * sb->s_blocksize; }
  struct inode *root = sb->s_root->d_inode;
  ihold(root);
  if (!pin_add(v, root, 0)) { usbmem_fat_unmount(v); return -ENOMEM; }
  if (!ro && writable_precheck(sb)) {
    v->can_write = 1;
#ifdef USBMEM_NTFS_WRITE
    bd->usbmem_dirty_hook_ = dirty_hook; bd->usbmem_dirty_ctx_ = v;
#endif
    /* 異常を見つけたら読み取り専用に落ちる (errors=remount-ro は既定)。 */
  }
  *out = v;
  return 0;
}
int usbmem_fat_writable(usbmem_fat *v) { return v && v->can_write; }

void usbmem_fat_unmount(usbmem_fat *v) {
  if (!v) return;
  int clean = 1;
#ifdef USBMEM_NTFS_WRITE
  if (!sb_rdonly(v->sb)) clean = sync_all(v) == 0;   /* 順序付きで全部書く */
#endif
  for (int b = 0; b < 1024; b++) {
    struct pin *p = v->tab[b];
    while (p) { struct pin *n = p->next; iput(p->inode); free(p); p = n; }
    v->tab[b] = NULL;
  }
#ifdef USBMEM_NTFS_WRITE
  if (!sb_rdonly(v->sb)) {
    if (sync_all(v)) clean = 0;
    if (clean && close_clean(v)) clean = 0;
    if (!clean) { v->sb->s_flags |= SB_RDONLY; }   /* 失敗したらクリーン印を書かない: ダーティのまま残す (fat_put_super の fat_set_state(0) も rdonly なら何もしない) */
  }
#endif
  struct block_device *bd = v->sb->s_bdev;
  if (bd) bd->usbmem_dirty_hook_ = NULL;
  usbmem_compat_umount(v->sb);
  usbmem_bdev_mapping_free(bd);
  free(bd);
#ifdef USBMEM_NTFS_WRITE
  free(v->log); free(v->seen);
#endif
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
    else { ihold(use->d_inode); struct inode *p = pin_add(v, use->d_inode, dir->i_ino); if (p) *out = p; else rc = -ENOMEM; }
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

/* ======================= 書き込み (ビルドで USBMEM_NTFS_WRITE=ON のときだけ中身がある) ======================= */
#ifdef USBMEM_NTFS_WRITE

#define IMMUTABLE_OR_APPEND(i) ((i)->i_flags & (S_IMMUTABLE | S_APPEND))
#define RECOUNT_LIMIT (16u * 1024 * 1024)   /* FAT32 の空きクラスタ数を取り外しのとき数え直す上限 (max_cluster がこれを超えるなら FSInfo は「不明」のまま) */

/* ---- ディレクトリ領域の dirty 順 (mark_buffer_dirty のフック) */
static int seen_add(usbmem_fat *v, uint64_t off) {   /* 1 = 新規, 0 = 既出, -1 = メモリ不足 */
  if ((v->seen_n + 1) * 2 > v->seen_cap) {
    size_t nc = v->seen_cap ? v->seen_cap * 2 : 1024;
    uint64_t *ns = calloc(nc, sizeof *ns);
    if (!ns) return -1;
    for (size_t i = 0; i < v->seen_cap; i++) if (v->seen[i]) {
      size_t h = (size_t)((v->seen[i] * 0x9E3779B97F4A7C15ull) >> 20) & (nc - 1);
      while (ns[h]) h = (h + 1) & (nc - 1);
      ns[h] = v->seen[i];
    }
    free(v->seen); v->seen = ns; v->seen_cap = nc;
  }
  uint64_t key = off + 1;
  size_t h = (size_t)((key * 0x9E3779B97F4A7C15ull) >> 20) & (v->seen_cap - 1);
  while (v->seen[h]) { if (v->seen[h] == key) return 0; h = (h + 1) & (v->seen_cap - 1); }
  v->seen[h] = key; v->seen_n++;
  return 1;
}
static void dirty_hook(void *ctx, u64 off, size_t len) {
  usbmem_fat *v = (usbmem_fat *)ctx;
  (void)len;
  if (off < v->meta_start) return;   /* FAT・予約領域は順序の対象外 (別の段階で書く) */
  if (seen_add(v, off) != 1) return;
  if (v->log_n == v->log_cap) {
    size_t nc = v->log_cap ? v->log_cap * 2 : 256;
    struct dlog_ent *nl = realloc(v->log, nc * sizeof *nl);
    if (!nl) return;   /* 記録できなくても最後の掃き出しで書かれる (順序の保証だけが弱くなる) */
    v->log = nl; v->log_cap = nc;
  }
  v->log[v->log_n].off = off; v->log[v->log_n].op = v->op; v->log_n++;
}
static void log_reset(usbmem_fat *v) {
  v->log_n = 0;
  if (v->seen_n) {
    if (v->seen_cap > (1u << 16)) { free(v->seen); v->seen = NULL; v->seen_cap = 0; }
    else memset(v->seen, 0, v->seen_cap * sizeof *v->seen);
    v->seen_n = 0;
  }
}

/* ---- 範囲の補助 (ranges は昇順・重ならない) */
static int in_ranges(const struct usbmem_range *r, size_t n, uint64_t x) {
  size_t lo = 0, hi = n;
  while (lo < hi) { size_t m = (lo + hi) / 2; if (r[m].off + r[m].len <= x) lo = m + 1; else hi = m; }
  return lo < n && r[lo].off <= x;
}
static int overlaps_ranges(const struct usbmem_range *r, size_t n, uint64_t off, uint64_t len) {
  for (size_t i = 0; i < n; i++) if (r[i].off < off + len && off < r[i].off + r[i].len) return 1;
  return 0;
}
static int range_push(struct usbmem_range **r, size_t *n, size_t *cap, uint64_t off, uint64_t len) {
  if (*n && (*r)[*n - 1].off + (*r)[*n - 1].len == off) { (*r)[*n - 1].len += len; return 0; }
  if (*n == *cap) {
    size_t nc = *cap ? *cap * 2 : 64;
    struct usbmem_range *t = realloc(*r, nc * sizeof **r);
    if (!t) return -ENOMEM;
    *r = t; *cap = nc;
  }
  (*r)[*n].off = off; (*r)[*n].len = len; (*n)++;
  return 0;
}

/* ---- 新しく確保されたクラスタ (FAT の dirty なセクタの新旧を、ディスク上の内容と比べる) */
static int read_cached(struct super_block *sb, uint64_t off, size_t len, unsigned char *dst) {
  size_t bs = sb->s_blocksize;
  while (len) {
    sector_t blk = off / bs; size_t o = (size_t)(off % bs), n = bs - o;
    if (n > len) n = len;
    struct buffer_head *bh = sb_bread(sb, blk);
    if (!bh) return -EIO;
    memcpy(dst, (const char *)bh->b_data + o, n);
    brelse(bh);
    dst += n; off += n; len -= n;
  }
  return 0;
}
static uint32_t fat12_get(const unsigned char *t, uint32_t n) {
  size_t o = n + n / 2; uint32_t x = t[o] | ((uint32_t)t[o + 1] << 8);
  return (n & 1) ? x >> 4 : x & 0xFFF;
}
/* 昇順・結合済みの、新しく確保されたクラスタ (データ領域のバイト範囲) を返す。呼び出し側が free */
static int new_alloc_ranges(usbmem_fat *v, const struct usbmem_range *dr, size_t nd, struct usbmem_range **out, size_t *nout) {
  struct super_block *sb = v->sb; struct msdos_sb_info *sbi = MSDOS_SB(sb);
  uint64_t bs = sb->s_blocksize, fat0 = (uint64_t)sbi->fat_start * bs, flen = (uint64_t)sbi->fat_length * bs;
  struct usbmem_range *r = NULL; size_t n = 0, cap = 0;
  int rc = 0;
  *out = NULL; *nout = 0;
#define ADD(c) do { uint64_t o_ = ((uint64_t)sbi->data_start + (uint64_t)((c) - 2) * sbi->sec_per_clus) * bs; if ((rc = range_push(&r, &n, &cap, o_, sbi->cluster_size))) goto done; } while (0)
  if (!overlaps_ranges(dr, nd, fat0, flen)) return 0;
  if (sbi->fat_bits == 12) {   /* 全体でも 6KiB 程度。エントリが境界をまたぐので全体を比べる */
    unsigned char *o = malloc(flen), *w = malloc(flen);
    if (!o || !w) { free(o); free(w); return -ENOMEM; }
    rc = usbmem_bdev_read(sb->s_bdev->impl, fat0, o, (size_t)flen);
    if (!rc) rc = read_cached(sb, fat0, (size_t)flen, w);
    for (uint32_t c = 2; !rc && c < sbi->max_cluster; c++) if (!fat12_get(o, c) && fat12_get(w, c)) ADD(c);
    free(o); free(w);
  } else {
    unsigned esz = sbi->fat_bits == 32 ? 4 : 2;
    for (size_t k = 0; k < nd && !rc; k++) {
      uint64_t a = dr[k].off, b = dr[k].off + dr[k].len;
      if (b <= fat0 || a >= fat0 + flen) continue;
      if (a < fat0) a = fat0;
      if (b > fat0 + flen) b = fat0 + flen;
      unsigned char *o = malloc(b - a), *w = malloc(b - a);
      if (!o || !w) { free(o); free(w); rc = -ENOMEM; break; }
      rc = usbmem_bdev_read(sb->s_bdev->impl, a, o, (size_t)(b - a));
      if (!rc) rc = read_cached(sb, a, (size_t)(b - a), w);
      for (uint64_t i = 0; !rc && i < (b - a) / esz; i++) {
        uint64_t c = (a - fat0) / esz + i;
        if (c < 2 || c >= sbi->max_cluster) continue;
        uint32_t eo, ew;
        if (esz == 4) { eo = (o[i*4] | (o[i*4+1] << 8) | (o[i*4+2] << 16) | ((uint32_t)o[i*4+3] << 24)) & 0x0FFFFFFF; ew = (w[i*4] | (w[i*4+1] << 8) | (w[i*4+2] << 16) | ((uint32_t)w[i*4+3] << 24)) & 0x0FFFFFFF; }
        else { eo = o[i*2] | (o[i*2+1] << 8); ew = w[i*2] | (w[i*2+1] << 8); }
        if (!eo && ew) { ADD((uint32_t)c); }
      }
      free(o); free(w);
    }
  }
#undef ADD
done:
  if (rc) { free(r); return rc; }
  *out = r; *nout = n;
  return 0;
}
/* a ∩ b (どちらも昇順・結合済み) */
static struct usbmem_range *intersect_ranges(const struct usbmem_range *a, size_t na, const struct usbmem_range *b, size_t nb, size_t *n) {
  struct usbmem_range *r = malloc((na + nb + 1) * sizeof *r);
  size_t i = 0, j = 0, c = 0;
  if (!r) { *n = 0; return NULL; }
  while (i < na && j < nb) {
    uint64_t lo = a[i].off > b[j].off ? a[i].off : b[j].off;
    uint64_t ae = a[i].off + a[i].len, be = b[j].off + b[j].len, hi = ae < be ? ae : be;
    if (lo < hi) { r[c].off = lo; r[c].len = hi - lo; c++; }
    if (ae < be) i++; else j++;
  }
  *n = c;
  return r;
}

/* ---- 同期 */
#define WB(rs, n, inside) usbmem_bdev_writeback_ranges(sb->s_bdev, (rs), (n), (inside))
#define STEP(expr) do { if (!rc) rc = (expr); if (!rc) rc = usbmem_bdev_flush(sb->s_bdev->impl); } while (0)

/* ディレクトリ領域 (ルートディレクトリ・ディレクトリのクラスタ) のセクタを、dirty になった順に書く。同じ操作 (op) の中のセクタの間には flush を挟む
 * (長い名前のスロット → 短い名前の順を守る)。違う操作のセクタは同じ段にまとめてよい。 */
static int write_dirs_ordered(usbmem_fat *v, const struct usbmem_range *dr, size_t nd) {
  struct super_block *sb = v->sb;
  int rc = 0;
  uint64_t bs = sb->s_blocksize;
  size_t bn = 0, bcap = 16; unsigned last_op = 0;
  struct usbmem_range *b = malloc(bcap * sizeof *b);
  if (!b) return -ENOMEM;
#ifdef USBMEM_MUTATE_LOG_REVERSE   /* 突然変異テスト専用: 逆順 (短い名前のスロットを先に書く) で、同じ操作の中の flush も挟まない */
  for (size_t k = v->log_n; k-- > 0 && !rc;) {
    struct dlog_ent *e = &v->log[k];
    if (!in_ranges(dr, nd, e->off)) continue;
    struct usbmem_range one = { e->off, bs };
    rc = WB(&one, 1, 1);
  }
  (void)bn; (void)last_op;
#else
  for (size_t k = 0; k < v->log_n && !rc; k++) {
    struct dlog_ent *e = &v->log[k];
    if (!in_ranges(dr, nd, e->off)) continue;
    if (bn && e->op == last_op) {   /* 同じ操作の次のセクタ: 先の分を確定してから */
      STEP(WB(b, (int)bn, 1)); bn = 0;
    }
    if (rc) break;
    if (bn == bcap) { struct usbmem_range *t = realloc(b, bcap * 2 * sizeof *b); if (!t) { rc = -ENOMEM; break; } b = t; bcap *= 2; }
    b[bn].off = e->off; b[bn].len = bs; bn++; last_op = e->op;
  }
  if (!rc && bn) STEP(WB(b, (int)bn, 1));
#endif
  free(b);
  if (!rc) {   /* 記録から漏れたもの (フックの記録失敗など) を掃き出す */
    struct usbmem_range rest = { v->meta_start, (uint64_t)1 << 60 };
    if (overlaps_ranges(dr, nd, rest.off, rest.len)) { rc = WB(&rest, 1, 1); if (!rc) rc = usbmem_bdev_flush(sb->s_bdev->impl); }
  }
  return rc;
}

/* inode 更新後のメタデータ (bd の dirty) を順序を守って書く */
static int write_meta(usbmem_fat *v) {
  struct super_block *sb = v->sb; struct msdos_sb_info *sbi = MSDOS_SB(sb);
  int rc = 0, alloc_first = v->pending != K_FREE;
  uint64_t bs = sb->s_blocksize;
  struct usbmem_range *dr = NULL; size_t nd = 0;
  rc = usbmem_bdev_dirty_ranges(sb->s_bdev, &dr, &nd);
  if (rc || !nd) { free(dr); return rc; }
#ifdef USBMEM_MUTATE_ORDER   /* 突然変異テスト専用: 順序を逆にする (tools/run_fat_crash_tests.sh が検出できることを確かめる) */
  alloc_first = !alloc_first;
#endif
  struct usbmem_range fr[4];
  int nf = sbi->fats > 4 ? 4 : sbi->fats;
  for (int k = 0; k < nf; k++) { fr[k].off = ((uint64_t)sbi->fat_start + (uint64_t)k * sbi->fat_length) * bs; fr[k].len = (uint64_t)sbi->fat_length * bs; }
#define FATS() do { for (int k = 0; k < nf && !rc; k++) if (overlaps_ranges(dr, nd, fr[k].off, fr[k].len)) STEP(WB(&fr[k], 1, 1)); } while (0)
  if (alloc_first) {
#ifndef USBMEM_MUTATE_NO_NEWCLU_FIRST   /* 突然変異テスト専用: 新クラスタの中身を先に書く規則を外す */
    struct usbmem_range *na = NULL; size_t nn = 0;
    rc = new_alloc_ranges(v, dr, nd, &na, &nn);
    if (!rc && nn) {
      size_t ni = 0; struct usbmem_range *ix = intersect_ranges(na, nn, dr, nd, &ni);   /* 新クラスタのうち実際に dirty なもの (新しいディレクトリの中身) */
      if (!ix) rc = -ENOMEM;
      else if (ni) STEP(WB(ix, (int)ni, 1));
      free(ix);
    }
    free(na);
#endif
    FATS();
    if (!rc) rc = write_dirs_ordered(v, dr, nd);
  } else {
    rc = write_dirs_ordered(v, dr, nd);
    FATS();
  }
#undef FATS
  free(dr);
  return rc;
}

static int sync_all(usbmem_fat *v) {
  struct super_block *sb = v->sb;
  if (sb_rdonly(sb)) return 0;
  int first_err = 0, wrote = 0;
  /* 1. データ (新しく確保したクラスタ・既存の範囲内への上書き。ここで途切れても、どのファイルのメタデータもまだ変わっていない) */
#ifndef USBMEM_MUTATE_NO_DATA_FIRST   /* 突然変異テスト専用: データをメタデータの後に書く */
  for (int b = 0; b < 1024; b++) for (struct pin *p = v->tab[b]; p; p = p->next) {
    struct inode *i = p->inode;
    if (!S_ISDIR(i->i_mode) && usbmem_mapping_has_dirty(i->i_mapping)) {
      int rc = usbmem_mapping_writeback(i->i_mapping, 1);
      if (rc && !first_err) first_err = rc; else if (!rc) i->i_state &= ~(unsigned long)I_DIRTY_PAGES;
      wrote = 1;
    }
  }
  if (wrote && !first_err) { int rc = usbmem_bdev_flush(sb->s_bdev->impl); if (rc) first_err = rc; }
#endif
  /* 2. inode のメタデータ (write_inode: ディレクトリエントリを更新 = bd の dirty に)。inode ごとに別の「操作」として記録 (順序の制約はない) */
  for (int b = 0; b < 1024 && !first_err; b++) for (struct pin *p = v->tab[b]; p; p = p->next) {
    struct inode *i = p->inode;
    if (i->i_state & (I_DIRTY_SYNC | I_DIRTY_DATASYNC)) { v->op++; int rc = sync_inode_metadata(i, 0); if (rc && !first_err) first_err = rc; }
  }
  /* 3. メタデータ (FAT・ディレクトリ)。順序は write_meta */
  if (!first_err) first_err = write_meta(v);
#ifdef USBMEM_MUTATE_NO_DATA_FIRST
  for (int b = 0; b < 1024; b++) for (struct pin *p = v->tab[b]; p; p = p->next) {
    struct inode *i = p->inode;
    if (!S_ISDIR(i->i_mode) && usbmem_mapping_has_dirty(i->i_mapping)) {
      int rc = usbmem_mapping_writeback(i->i_mapping, 1);
      if (rc && !first_err) first_err = rc; else if (!rc) i->i_state &= ~(unsigned long)I_DIRTY_PAGES;
      wrote = 1;
    }
  }
  if (wrote && !first_err) { int rc = usbmem_bdev_flush(sb->s_bdev->impl); if (rc) first_err = rc; }
#endif
  if (!first_err) { v->pending = K_NONE; log_reset(v); }
  if (sb_rdonly(sb)) v->can_write = 0;   /* 上流がエラーを見つけて読み取り専用に落ちた */
  return first_err;
}
int usbmem_fat_sync(usbmem_fat *v) { return v ? sync_all(v) : -EINVAL; }

/* namei_vfat.c の fat_remove_entries の呼び出しはここを通る (CMake の -Dfat_remove_entries=usbmem_fat_remove_entries)。
 * rename は「新しい名前を追加 → 古い名前を削除」で、同じディレクトリのセクタに両方の変更が入ることがある (新しい名前の長い名前スロットの後ろと、古い名前の先頭スロット)。
 * セクタ単位の順序づけでは、古い名前の短い名前 (後ろのセクタ) の削除より先に古い名前の先頭スロットの削除が届く場合があり、電源断のあと「長い名前を失った短い名前」が現れる。
 * そこで、ここまでの確保系 (新しい名前・FAT) を先に確定してから削除に進む (削除は解放系の順序: 短い名前 → 長い名前のスロット)。 */
int usbmem_fat_remove_entries(struct inode *dir, struct fat_slot_info *sinfo) {
  struct super_block *sb = dir->i_sb;
  usbmem_fat *v = sb->s_bdev ? (usbmem_fat *)sb->s_bdev->usbmem_dirty_ctx_ : NULL;
#ifndef USBMEM_MUTATE_NO_RENAME_BOUNDARY   /* 突然変異テスト専用: 段階境界を入れない */
  if (v && v->pending == K_ALLOC && v->log_n) {
    int rc = sync_all(v);
    if (rc) return rc;
    v->pending = K_FREE;
  }
#else
  (void)v;
#endif
  return fat_remove_entries(dir, sinfo);
}

/* dir.c (fat_add_entries) の fat_chain_add の呼び出しはここを通る。ディレクトリを拡張するとき、上流は (1) 既存の末尾の空きスロットに長い名前のスロットの前半を書き、
 * (2) 新しいクラスタに残り (長い名前の最後のスロットと短い名前) を書き、(3) クラスタをつなぐ。(3) のつなぎ (FAT) が届いた時点で (2) の短い名前が見えるようになるので、
 * (1) の前半が先に届いていなければ「長い名前を失った短い名前」が現れる。つなぐ前にここまでを確定する (新しいクラスタの中身 → FAT → ディレクトリ。まだつながっていないので見えない)。 */
int usbmem_fat_chain_add(struct inode *inode, int new_dclus, int nr_cluster) {
  struct super_block *sb = inode->i_sb;
  usbmem_fat *v = sb->s_bdev ? (usbmem_fat *)sb->s_bdev->usbmem_dirty_ctx_ : NULL;
#ifndef USBMEM_MUTATE_NO_DIRGROW_BOUNDARY   /* 突然変異テスト専用: 段階境界を入れない */
  if (v && v->pending == K_ALLOC && v->log_n) {
    int rc = sync_all(v);
    if (rc) return rc;
    v->pending = K_ALLOC;
  }
#else
  (void)v;
#endif
  return fat_chain_add(inode, new_dclus, nr_cluster);
}

/* ---- ダーティ印 */
/* FAT[1] のクリーン bit (FAT16: bit15 / FAT32: bit27) を全ての FAT で更新して確定する。FAT12 には無い。呼ぶとき FAT の先頭セクタに未確定の変更が無いこと (最初の書き込みの前と、全部書いた後) */
static int fat1_set(usbmem_fat *v, int clean) {
  struct super_block *sb = v->sb; struct msdos_sb_info *sbi = MSDOS_SB(sb);
  u32 bit; size_t eoff;
  if (!fat1_bit(sbi, &bit, &eoff)) return 0;
  int rc = 0;
  for (int k = 0; k < sbi->fats && !rc; k++) {
    struct buffer_head *bh = sb_bread(sb, sbi->fat_start + (sector_t)k * sbi->fat_length);
    if (!bh) return -EIO;
    unsigned char *p = (unsigned char *)bh->b_data + eoff;
    u32 val = sbi->fat_bits == 16 ? (u32)(p[0] | (p[1] << 8)) : (u32)(p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24));
    u32 nv = clean ? (val | bit) : (val & ~bit);
    if (nv != val) {
      p[0] = (unsigned char)nv; p[1] = (unsigned char)(nv >> 8);
      if (sbi->fat_bits == 32) { p[2] = (unsigned char)(nv >> 16); p[3] = (unsigned char)(nv >> 24); }
      mark_buffer_dirty(bh);
      rc = sync_dirty_buffer(bh);   /* 書いて flush */
    }
    brelse(bh);
  }
  return rc;
}
/* FAT32 の FSInfo の空きクラスタ数・次の空き位置を「不明」(0xFFFFFFFF) にして確定する (書き込み中にクラッシュしても古い数を信じさせない) */
static int fsinfo_invalidate(usbmem_fat *v) {
  struct super_block *sb = v->sb; struct msdos_sb_info *sbi = MSDOS_SB(sb);
  if (sbi->fat_bits != 32 || !sbi->fsinfo_sector) return 0;
  struct buffer_head *bh = sb_bread(sb, sbi->fsinfo_sector);
  if (!bh) return -EIO;
  unsigned char *p = (unsigned char *)bh->b_data;
  int rc = 0;
  if (p[0] == 'R' && p[1] == 'R' && p[2] == 'a' && p[3] == 'A' && p[484] == 'r' && p[485] == 'r' && p[486] == 'A' && p[487] == 'a') {
    memset(p + 488, 0xFF, 8);
    mark_buffer_dirty(bh);
    rc = sync_dirty_buffer(bh);
  }
  brelse(bh);
  return rc;
}
static int mark_dirty_start(usbmem_fat *v) {
  struct super_block *sb = v->sb;
#ifdef USBMEM_MUTATE_NO_DIRTY   /* 突然変異テスト専用: ダーティ印を一切書かない (最初の変更の前に確定する規則を外す) */
  sb->s_flags &= ~(unsigned long)SB_RDONLY;
  return 0;
#else
  int rc = usbmem_compat_remount_rw(sb);   /* fat_reconfigure → fat_set_state(1, 1): ブートセクタのダーティビットを書いて flush */
  if (rc == -EROFS && sb_rdonly(sb)) { sb->s_flags &= ~(unsigned long)SB_RDONLY; rc = 0; }   /* 互換層は rw への切り替えを自分では反映しない */
  if (!rc) rc = fat1_set(v, 0);
  if (!rc) rc = fsinfo_invalidate(v);
  if (!rc) v->dirtied = 1;
  return rc;
#endif
}
/* 取り外しの最後: FAT32 の FSInfo を確定 → FAT[1] のクリーン bit を戻す。ブートセクタのダーティビットは fat_put_super が消す */
static int close_clean(usbmem_fat *v) {
  struct super_block *sb = v->sb; struct msdos_sb_info *sbi = MSDOS_SB(sb);
  if (!v->dirtied) return 0;
  int rc = 0;
  if (sbi->fat_bits == 32) {
    if (sbi->max_cluster <= RECOUNT_LIMIT) {
      sbi->free_clusters = (unsigned)-1; sbi->free_clus_valid = 0;
      rc = fat_count_free_clusters(sb);   /* 数え直し (FSInfo の inode を dirty にする) */
    } else sbi->free_clusters = (unsigned)-1;   /* 大きすぎるときは「不明」のまま (次の位置のヒントだけ書く) */
    if (!rc) { __mark_inode_dirty(sbi->fsinfo_inode, I_DIRTY_SYNC); rc = sync_inode_metadata(sbi->fsinfo_inode, 1); }
    if (!rc) {
      struct usbmem_range res = { 0, (uint64_t)sbi->fat_start * sb->s_blocksize };
      rc = WB(&res, 1, 1);
      if (!rc) rc = usbmem_bdev_flush(sb->s_bdev->impl);
    }
  }
  if (!rc) rc = fat1_set(v, 1);
  if (!rc) v->dirtied = 0;
  return rc;
}

/* 書き込みの入口: 最初の 1 回は rw へ切り替えてダーティ印を確定する (最初の変更の前)。種類が変わるときは先に同期する。 */
static int begin_op(usbmem_fat *v, int kind) {
  if (!v || !v->can_write) return -EROFS;
  if (sb_rdonly(v->sb)) {
    int rc = mark_dirty_start(v);
    if (rc) { v->sb->s_flags |= SB_RDONLY; v->can_write = 0; return rc; }
  }
#ifndef USBMEM_MUTATE_NO_KIND_SYNC   /* 突然変異テスト専用: 確保系と解放系の切り替えで同期しない (解放したクラスタを同期前に再利用する) */
  if (v->pending != K_NONE && v->pending != kind) { int rc = sync_all(v); if (rc) return rc; }
#endif
  v->pending = kind;
  v->op++;
  return 0;
}
static int after_op(usbmem_fat *v, int rc) {
  if (sb_rdonly(v->sb)) v->can_write = 0;
  return rc;
}

/* ---- 名前空間の操作 */
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

static struct inode *dir_of(usbmem_fat *v, uint64_t ino, int *err) {
  struct inode *i = pin_find(v, ino);
  if (!i) { *err = -ENOENT; return NULL; }
  return i;
}

/* kind: 0 create / 1 mkdir / 2 unlink / 3 rmdir */
static int ns_op(usbmem_fat *v, int kind, uint64_t dir_ino, const char *name, size_t len, uint32_t mode, uint64_t *ino) {
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
int usbmem_fat_create(usbmem_fat *v, uint64_t d, const char *n, size_t l, uint32_t m, uint64_t *ino) { return ns_op(v, 0, d, n, l, m, ino); }
int usbmem_fat_mkdir(usbmem_fat *v, uint64_t d, const char *n, size_t l, uint32_t m, uint64_t *ino) { return ns_op(v, 1, d, n, l, m, ino); }
int usbmem_fat_unlink(usbmem_fat *v, uint64_t d, const char *n, size_t l) { return ns_op(v, 2, d, n, l, 0, NULL); }
int usbmem_fat_rmdir(usbmem_fat *v, uint64_t d, const char *n, size_t l) { return ns_op(v, 3, d, n, l, 0, NULL); }

/* src が dir の祖先 (または dir 自身) か。親は pin の parent でたどる (dir は呼び出し側が path をたどって得たものなので、祖先は全てピン済み)。たどれなければ安全側 (移動を拒否) */
static int is_ancestor(usbmem_fat *v, struct inode *src, struct inode *dir) {
  struct inode *cur = dir;
  for (int n = 0; n < 4096; n++) {
    if (cur == src) return 1;
    if (cur->i_ino == FAT_ROOT_INO) return 0;
    struct pin *p = pin_entry(v, cur->i_ino);
    if (!p) return 1;
    cur = pin_find(v, p->parent);
    if (!cur) return 1;
  }
  return 1;
}

static int do_vfs_rename(struct inode *odir, struct dentry *od, struct inode *ndir, struct dentry *nd) {
  int rc;
  ihold(odir); ihold(ndir);
  down_write(&odir->i_rwsem); if (ndir != odir) down_write(&ndir->i_rwsem);
  rc = ((int (*)(struct mnt_idmap *, struct inode *, struct dentry *, struct inode *, struct dentry *, unsigned))odir->i_op->rename)(&nop_mnt_idmap, odir, od, ndir, nd, 0);
  if (ndir != odir) up_write(&ndir->i_rwsem);
  up_write(&odir->i_rwsem);
  iput(ndir); iput(odir);
  return rc;
}

int usbmem_fat_rename(usbmem_fat *v, uint64_t od, const char *on, size_t ol, uint64_t nd, const char *nn, size_t nl) {
  int err = 0; struct inode *odir = dir_of(v, od, &err); if (!odir) return err;
  struct inode *ndir = dir_of(v, nd, &err); if (!ndir) return err;
  struct dentry *od_ = NULL, *nd_ = NULL;
  int rc = lookup_d(odir, on, ol, &od_);
  if (!rc) { rc = lookup_d(ndir, nn, nl, &nd_); if (rc) free_d(od_); }
  if (rc) return rc;
  struct inode *src = od_->d_inode, *dst = nd_->d_inode;
  /* 大文字小文字だけが違う名前への変更 (同じ inode): vfat は同じ短い名前の追加を拒む (-EEXIST) ので、一時名を経由する (どの時点でも元のファイルは見つかる) */
  int case_only = src && dst && dst == src && !(odir == ndir && ol == nl && memcmp(on, nn, ol) == 0);
  if (!src) rc = -ENOENT;
  else if (IMMUTABLE_OR_APPEND(odir) || IMMUTABLE_OR_APPEND(ndir) || IMMUTABLE_OR_APPEND(src) || (dst && IMMUTABLE_OR_APPEND(dst))) rc = -EPERM;
  else if (S_ISDIR(src->i_mode) && is_ancestor(v, src, ndir)) rc = -EINVAL;   /* 自分の中へは動かせない (Linux は型の検査より先に -EINVAL) */
  else if (dst && dst == src && !case_only) rc = 0;   /* 同じ名前への rename は何もしない (VFS と同じ) */
  else if (dst && !case_only && S_ISDIR(src->i_mode) != S_ISDIR(dst->i_mode)) rc = S_ISDIR(src->i_mode) ? -ENOTDIR : -EISDIR;
  else if (!odir->i_op->rename) rc = -EOPNOTSUPP;
  else if (case_only) {
    rc = begin_op(v, K_ALLOC);
    if (!rc) {
      char tmp[48]; int tl = 0;
      struct dentry *td = NULL;
      for (unsigned n = 0; n < 64 && !td; n++) {   /* 存在しない一時名を探す */
        tl = snprintf(tmp, sizeof tmp, ".usbmem-ren-%u-%u", (unsigned)src->i_ino, n);
        struct dentry *c = NULL;
        if (lookup_d(odir, tmp, (size_t)tl, &c)) break;
        if (!c->d_inode) td = c; else free_d(c);
      }
      if (!td) rc = -EEXIST;
      else {
        rc = do_vfs_rename(odir, od_, odir, td);   /* 元の名前 → 一時名 */
        free_d(td);
        if (!rc) {
          struct dentry *td2 = NULL, *nd2 = NULL;
          rc = lookup_d(odir, tmp, (size_t)tl, &td2);   /* 一時名の正式な dentry */
          if (!rc) { int e2 = 0; nd2 = new_d(ndir, nn, nl, 0, &e2); if (!nd2) rc = e2; }
          if (!rc) rc = do_vfs_rename(odir, td2, ndir, nd2);   /* 一時名 → 新しい名前 */
          free_d(td2); free_d(nd2);
        }
      }
    }
  } else {
    rc = begin_op(v, dst ? K_FREE : K_ALLOC);
    if (!rc) {
      rc = do_vfs_rename(odir, od_, ndir, nd_);
      if (!rc) { struct pin *sp = pin_entry(v, src->i_ino); if (sp) sp->parent = ndir->i_ino; }   /* 移動した先を親にする */
      if (!rc && dst) pin_remove(v, dst->i_ino);   /* 上書きされた側: ピンを外す → dentry 解放時に追い出され、クラスタが解放される */
    }
  }
  free_d(nd_); free_d(od_);
  return after_op(v, rc);
}

/* ---- ファイルの内容 */
/* 書き込み 1 回分の後始末: このファイルのデータページをデバイスへ書く (メモリに溜めない) */
static int64_t write_common(usbmem_fat *v, uint64_t ino, uint64_t off, const void *buf, size_t len, int inplace) {
  { int e = begin_op(v, K_ALLOC); if (e) return e; }
  int err = 0; struct inode *i = dir_of(v, ino, &err);
  if (!i) return err;
  if (S_ISDIR(i->i_mode)) return -EISDIR;
  if (!S_ISREG(i->i_mode)) return -EINVAL;
  if (inplace && off + len > (uint64_t)i->i_size) return -EINVAL;
  if (!len) return 0;
  if (off >= (uint64_t)i->i_sb->s_maxbytes) return -EFBIG;
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
#ifndef USBMEM_MUTATE_NO_DATA_FIRST   /* 突然変異テスト専用: 書き込みのたびのデータの書き出しも止める (データは同期の中でメタデータの後に書かれる) */
    if (usbmem_mapping_has_dirty(i->i_mapping)) {
      int wr = usbmem_mapping_writeback(i->i_mapping, 0);
      if (wr && ret >= 0) ret = wr;
      else if (!wr) i->i_state &= ~(unsigned long)I_DIRTY_PAGES;
    }
#endif
  }
  iput(i);
  after_op(v, 0);
  return ret;
}
int64_t usbmem_fat_write(usbmem_fat *v, uint64_t ino, uint64_t off, const void *buf, size_t len) { return write_common(v, ino, off, buf, len, 0); }
int64_t usbmem_fat_write_inplace(usbmem_fat *v, uint64_t ino, uint64_t off, const void *buf, size_t len) { return write_common(v, ino, off, buf, len, 1); }

/* サイズ変更。縮小は解放を伴う (解放系)、拡張は確保 (確保系)。 */
int usbmem_fat_truncate(usbmem_fat *v, uint64_t ino, uint64_t size) {
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
int usbmem_fat_sync(usbmem_fat *v) { (void)v; return -EROFS; }
int usbmem_fat_create(usbmem_fat *v, uint64_t d, const char *n, size_t l, uint32_t m, uint64_t *ino) { (void)v; (void)d; (void)n; (void)l; (void)m; (void)ino; return -EROFS; }
int usbmem_fat_mkdir(usbmem_fat *v, uint64_t d, const char *n, size_t l, uint32_t m, uint64_t *ino) { (void)v; (void)d; (void)n; (void)l; (void)m; (void)ino; return -EROFS; }
int usbmem_fat_unlink(usbmem_fat *v, uint64_t d, const char *n, size_t l) { (void)v; (void)d; (void)n; (void)l; return -EROFS; }
int usbmem_fat_rmdir(usbmem_fat *v, uint64_t d, const char *n, size_t l) { (void)v; (void)d; (void)n; (void)l; return -EROFS; }
int usbmem_fat_rename(usbmem_fat *v, uint64_t od, const char *on, size_t ol, uint64_t nd, const char *nn, size_t nl) { (void)v; (void)od; (void)on; (void)ol; (void)nd; (void)nn; (void)nl; return -EROFS; }
int64_t usbmem_fat_write(usbmem_fat *v, uint64_t ino, uint64_t off, const void *buf, size_t len) { (void)v; (void)ino; (void)off; (void)buf; (void)len; return -EROFS; }
int64_t usbmem_fat_write_inplace(usbmem_fat *v, uint64_t ino, uint64_t off, const void *buf, size_t len) { (void)v; (void)ino; (void)off; (void)buf; (void)len; return -EROFS; }
int usbmem_fat_truncate(usbmem_fat *v, uint64_t ino, uint64_t size) { (void)v; (void)ino; (void)size; return -EROFS; }
#endif
