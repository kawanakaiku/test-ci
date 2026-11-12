// SPDX-License-Identifier: GPL-2.0-or-later
// ホスト向け VFS 風入口 (ext4)。Linux の VFS が行う「iget → i_op->lookup / i_fop->iterate_shared / read_mapping_folio」を直接呼ぶ。
// ext4 の inode 番号はディスク上の値なので、NTFS と同様に ino から ext4_iget() で引ける (exFAT のようなピン表は不要)。
//
// 読み取り専用 (read_only=1): ジャーナルは読み込まない (noload)。メディアに一切書かない。docs/ext4-port.md。
// 書き込み (ビルドで USBMEM_NTFS_WRITE=ON かつ read_only=0): docs/ext4-write.md。
//  - 書き込み可にする条件 (writable_precheck) を満たさなければ、読み取り専用 (noload) でマウントに成功する (writable() == 0)。
//  - 満たせば、ジャーナルを読み込んで**読み取り専用で**マウントする (needs_recovery ならここで jbd2 がジャーナルを再生する)。
//    最初の書き込みの直前に remount rw し、ext4 自身がスーパーブロックに needs_recovery を立てて FUA で書く (= ダーティ印が最初の変更より先に確定)。
//  - マウントオプション: nodelalloc (書き込み時に確保・ordered データ)、dioread_lock (未初期化エクステントを使わない)、
//    noinit_itable / no_prefetch_block_bitmaps (バックグラウンドのスレッドを作らない)。errors=remount-ro 相当。
//  - ファイルデータ (ordered): 書いた直後に usbmem_kbh_write_mapping でデバイスへ書き、jbd2 のコミット時のデータ書き出しも同じ関数で行う。
#include <stdlib.h>
#include <compat/kernel.h>
#include <compat/usbmem_ntfs.h>

#include "usbmem/ext4_c.h"
#include "usbmem/ntfs_c.h"   /* usbmem_bdev_* の宣言 */
#include "ext4.h"            /* 上流 fs/ext4 (-iquote): struct ext4_sb_info・EXT4_MOUNT_* */
#include "ext4_jbd2.h"

#define ext4_iget(sb, ino) __ext4_iget((sb), (ino), 0, __func__, __LINE__)

void usbmem_jbd2_set_current(journal_t *j);   /* src/rt_jbd2.c */
int usbmem_module_init_jbd2(void);            /* fs/jbd2/journal.c の module_init (slab の作成) */

struct usbmem_ext4 {
  struct super_block *sb;
  int can_write;      /* 書き込み要求あり、かつ前提を満たす */
};

/* マウント前に超ブロック (1024 バイト目) を自前で検査する。上流の ext4_fill_super も同様に拒否するが、ここでは理由を区別できる errno を返す。
 * - needs_recovery (未再生の journal): 読み取り専用では既定で -EUCLEAN。journal を再生せずに読むと、コミット済みなのにディスクへ未反映の更新が見えない (古い/不整合)。
 *   書き込み可のマウント (rw_ok=1) では再生するので通す。
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

static uint32_t le32_at(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

/* 書き込み可にしてよいか (明示のホワイトリスト。テストで確認した機能だけ)。0 = 読み取り専用に落とす。理由は *why に。 */
#define EXT4W_COMPAT_OK   (0x1u | 0x2u | 0x4u | 0x8u | 0x10u | 0x20u | 0x1000u)   /* dir_prealloc imagic has_journal ext_attr resize_inode dir_index orphan_file */
#define EXT4W_INCOMPAT_OK (0x2u | 0x4u | 0x40u | 0x80u | 0x200u | 0x2000u)        /* filetype recover extents 64bit flex_bg csum_seed */
#define EXT4W_ROCOMPAT_OK (0x1u | 0x2u | 0x8u | 0x10u | 0x20u | 0x40u | 0x400u | 0x10000u)   /* sparse_super large_file huge_file gdt_csum dir_nlink extra_isize metadata_csum orphan_present */
static int writable_precheck(struct usbmem_bdev *bdev, const char **why) {
  uint8_t sb[1024];
  *why = "read-only device";
  if (usbmem_bdev_is_ro(bdev)) return 0;
  *why = "read error";
  if (usbmem_bdev_read(bdev, 1024, sb, sizeof sb)) return 0;
  uint32_t compat = le32_at(sb + 0x5C), incompat = le32_at(sb + 0x60), rocompat = le32_at(sb + 0x64);
  unsigned state = sb[0x3A] | (sb[0x3B] << 8);
  uint32_t rev = le32_at(sb + 0x4C), log_bs = le32_at(sb + 0x18), jinum = le32_at(sb + 0xE0), jdev = le32_at(sb + 0xE4);
  uint32_t defm = le32_at(sb + 0x100);
  if (rev < 1) { *why = "revision 0"; return 0; }
  if (log_bs > 2) { *why = "block size > 4096"; return 0; }
  if (!(state & 1) || (state & 2)) { *why = "not cleanly unmounted / error state (s_state)"; return 0; }
  if (!(compat & 0x4) || !jinum || jdev) { *why = "no internal journal"; return 0; }
  if (compat & ~EXT4W_COMPAT_OK) { *why = "unsupported compat feature"; return 0; }
  if (incompat & ~EXT4W_INCOMPAT_OK) { *why = "unsupported incompat feature"; return 0; }
  if (rocompat & ~EXT4W_ROCOMPAT_OK) { *why = "unsupported ro_compat feature"; return 0; }
  if ((defm & 0x60) == 0x20 || (defm & 0x60) == 0x60) { *why = "default data mode is journal/writeback"; return 0; }
  *why = NULL;
  return 1;
}

static pthread_once_t g_jbd2_once = PTHREAD_ONCE_INIT;
static int g_jbd2_init_rc;
static void jbd2_init_once(void) { g_jbd2_init_rc = usbmem_module_init_jbd2(); }

/* jbd2 のコミット時の ordered データ書き出し (ext4 の ext4_journal_submit_inode_data_buffers の代わり) */
static int submit_inode_data(struct jbd2_inode *jinode) {
  struct inode *inode = jinode->i_vfs_inode;
#ifdef USBMEM_MUTATE_NO_ORDERED   /* 突然変異テスト: ordered データをコミット前に書かない (検出されるべき) */
  (void)inode; return 0;
#endif
  return usbmem_kbh_write_mapping(inode->i_mapping, 0, -1);   /* 範囲 (i_dirty_*_page) より広く、その inode の dirty なデータを全部書く */
}
static int finish_inode_data(struct jbd2_inode *jinode) { (void)jinode; return 0; }   /* I/O は同期 */

static int do_mount(struct usbmem_bdev *bdev, int dev_ro, const char *const *opts, struct super_block **psb) {
  struct block_device *bd = calloc(1, sizeof *bd);
  if (!bd) return -ENOMEM;
  bd->impl = bdev;
  bd->usbmem_ro_ = dev_ro;
  usbmem_kbh_bdev_attach_host(bd);   /* 読み取り専用のマウント: ext4 にもデバイスが読み取り専用に見える (エラー時のスーパーブロック書き込みもしない) */
  usbmem_bdev_mapping_init(bd);
  int rc = usbmem_compat_mount_fs_opts(usbmem_module_init_ext4, bd, 1, opts, psb);
  if (rc) { usbmem_bdev_mapping_free(bd); free(bd); }
  return rc;
}

int usbmem_ext4_mount(struct usbmem_bdev *bdev, int ro, int allow_noload, usbmem_ext4 **out) {
#ifndef USBMEM_NTFS_WRITE
  if (!ro) return -EROFS;   /* 書き込みはビルドで無効 */
#endif
  pthread_once(&g_jbd2_once, jbd2_init_once);
  if (g_jbd2_init_rc) return g_jbd2_init_rc;
  const char *why = NULL;
  int want_rw = !ro && writable_precheck(bdev, &why);
  if (!ro && why) usbmem_printk("usbmem ext4: mounting read-only: %s\n", why);
  int pc = precheck_sb(bdev, allow_noload || want_rw);
  if (pc) return pc;
  struct super_block *sb = NULL;
  static const char *const ro_opts[] = { "noload", NULL };   /* 読み取り専用: journal を読み込まない (ディスクに一切書かない) */
  static const char *const rw_opts[] = { "nodelalloc", "dioread_lock", "noinit_itable", "no_prefetch_block_bitmaps", NULL };
  usbmem_jbd2_set_current(NULL);
  int rc = do_mount(bdev, !want_rw, want_rw ? rw_opts : ro_opts, &sb);
  if (rc) return rc;
  usbmem_ext4 *v = calloc(1, sizeof *v);
  if (!v) { struct block_device *bd = sb->s_bdev; usbmem_compat_umount(sb); usbmem_bdev_mapping_free(bd); free(bd); return -ENOMEM; }
  v->sb = sb;
  if (want_rw) {
    struct ext4_sb_info *sbi = EXT4_SB(sb);
    if (sbi->s_journal) {
      v->can_write = 1;
      /* 異常を見つけたら読み取り専用に落とす (errors=remount-ro)。既定の errors=continue だとエラー後も書き続けてしまう */
      sbi->s_mount_opt = (sbi->s_mount_opt & ~EXT4_MOUNT_ERRORS_MASK) | EXT4_MOUNT_ERRORS_RO;
      sbi->s_journal->j_submit_inode_data_buffers = submit_inode_data;
      sbi->s_journal->j_finish_inode_data_buffers = finish_inode_data;
#ifdef USBMEM_MUTATE_WRITEBACK   /* 突然変異テスト: data=writeback (解放したデータブロックをコミット前に再利用する) */
      sbi->s_mount_opt = (sbi->s_mount_opt & ~EXT4_MOUNT_DATA_FLAGS) | EXT4_MOUNT_WRITEBACK_DATA;
#endif
    }
  }
  usbmem_jbd2_set_current(NULL);
  *out = v;
  return 0;
}
int usbmem_ext4_writable(usbmem_ext4 *v) { return v && v->can_write; }

static journal_t *journal_of(usbmem_ext4 *v) { return EXT4_SB(v->sb)->s_journal; }
/* API の入口/出口: このスレッドのジャーナルを設定する (待ちの中でコミット要求を処理する対象。src/rt_jbd2.c) */
static void enter(usbmem_ext4 *v) { usbmem_jbd2_set_current(journal_of(v)); }
static void leave(void) { usbmem_jbd2_set_current(NULL); }

void usbmem_ext4_unmount(usbmem_ext4 *v) {
  if (!v) return;
  struct block_device *bd = v->sb->s_bdev;
  enter(v);
  if (!sb_rdonly(v->sb)) {
    /* 書き残しのデータがあれば先に書く (ext4 の evict は dirty なページを捨てる) */
    size_t n; struct inode **list = usbmem_snapshot_inodes_(v->sb, &n);
    for (size_t k = 0; k < n; k++) { if (S_ISREG(list[k]->i_mode)) usbmem_kbh_write_mapping(list[k]->i_mapping, 0, -1); iput(list[k]); }
    free(list);
  }
  usbmem_compat_umount(v->sb);
  leave();
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

/* ======================= 書き込み (ビルドで USBMEM_NTFS_WRITE=ON のときだけ中身がある) ======================= */
#ifdef USBMEM_NTFS_WRITE
/* 書き込みの入口: 最初の 1 回は remount rw (= ext4 がスーパーブロックに needs_recovery を立てて FUA で書く: ダーティ印の確定)。失敗したら以後も書き込みを拒否する。 */
static int begin_write(usbmem_ext4 *v) {
  if (!v || !v->can_write) return -EROFS;
  enter(v);
  if (sb_rdonly(v->sb)) {
    int rc = usbmem_compat_remount_rw(v->sb);
    if (rc) { v->can_write = 0; leave(); return rc; }
  }
  return 0;
}
/* 上流がエラーを見つけて読み取り専用に落ちていたら (errors=remount-ro / ジャーナル中断)、以後の書き込みを止める */
static int after_op(usbmem_ext4 *v, int rc) {
  if (sb_rdonly(v->sb) || ext4_emergency_state(v->sb)) v->can_write = 0;
  leave();
  return rc;
}
#define begin_alloc begin_write
/* データ書き込みを拒否する inode: data=journal 指定 (chattr +j)・inline data・暗号化・verity (このビルドでは扱わない) */
static int data_write_refused(struct inode *i) {
  return ext4_test_inode_flag(i, EXT4_INODE_JOURNAL_DATA) || ext4_test_inode_flag(i, EXT4_INODE_INLINE_DATA) ||
         ext4_test_inode_flag(i, EXT4_INODE_ENCRYPT) || ext4_test_inode_flag(i, EXT4_INODE_VERITY) || ext4_test_inode_flag(i, EXT4_INODE_EA_INODE);
}
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
static int ns_op(usbmem_ext4 *v, int kind, uint64_t dir_ino, const char *name, size_t len, uint32_t mode, uint64_t *ino) {
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
    break;
  }
  up_write(&dir->i_rwsem);
  free_d(d);   /* d の inode を iput: 削除済み (nlink 0) ならここで evict される = ブロックと inode の解放 */
  iput(dir);
  return after_op(v, rc);
}
int usbmem_ext4_create(usbmem_ext4 *v, uint64_t d, const char *n, size_t l, uint32_t m, uint64_t *ino) { return ns_op(v, 0, d, n, l, m, ino); }
int usbmem_ext4_mkdir(usbmem_ext4 *v, uint64_t d, const char *n, size_t l, uint32_t m, uint64_t *ino) { return ns_op(v, 1, d, n, l, m, ino); }
int usbmem_ext4_unlink(usbmem_ext4 *v, uint64_t d, const char *n, size_t l) { return ns_op(v, 2, d, n, l, 0, NULL); }
int usbmem_ext4_rmdir(usbmem_ext4 *v, uint64_t d, const char *n, size_t l) { return ns_op(v, 3, d, n, l, 0, NULL); }

/* anc が dir の祖先 (または dir 自身) か。".." をたどる。壊れて輪になっていても止まる。 */
static int is_ancestor(usbmem_ext4 *v, uint64_t anc, uint64_t dir) {
  uint64_t cur = dir;
  for (int n = 0; n < 4096; n++) {
    if (cur == anc) return 1;
    if (cur == USBMEM_EXT4_ROOT_INO) return 0;
    uint64_t parent;
    if (usbmem_ext4_lookup(v, cur, "..", 2, &parent) || parent == cur) return 0;
    cur = parent;
  }
  return 1;   /* 深すぎる/輪: 安全側 (移動を拒否) */
}

int usbmem_ext4_rename(usbmem_ext4 *v, uint64_t od, const char *on, size_t ol, uint64_t nd, const char *nn, size_t nl) {
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
  }
  free_d(nd_); free_d(od_);
  iput(ndir); iput(odir);
  return after_op(v, rc);
}

/* ハードリンク: dir/name を target_ino の新しい名前にする。ディレクトリは不可 (VFS と同じ -EPERM)。 */
int usbmem_ext4_link(usbmem_ext4 *v, uint64_t dir_ino, const char *name, size_t len, uint64_t target_ino) {
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
int usbmem_ext4_symlink(usbmem_ext4 *v, uint64_t dir_ino, const char *name, size_t len, const char *target, size_t tlen, uint64_t *ino) {
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
static int64_t write_common(usbmem_ext4 *v, uint64_t ino, uint64_t off, const void *buf, size_t len, int inplace) {
  { int e = begin_alloc(v); if (e) return e; }
  int err = 0; struct inode *i = get_inode(v, ino, &err);
  if (!i) return err;
  if (S_ISDIR(i->i_mode)) { iput(i); return -EISDIR; }
  if (!S_ISREG(i->i_mode)) { iput(i); return -EINVAL; }
  if (IMMUTABLE_OR_APPEND(i)) { iput(i); return -EPERM; }
  if (data_write_refused(i)) { iput(i); return -EOPNOTSUPP; }
  if (inplace && off + len > (uint64_t)i->i_size) { iput(i); return -EINVAL; }
  if (!len) { iput(i); return 0; }
  if (!i->i_fop || !i->i_fop->write_iter) { iput(i); return -EOPNOTSUPP; }
  struct file f; memset(&f, 0, sizeof f);
  static struct vfsmount mnt;   /* ext4_file_open が最初の open でマウント位置を記録しようとする (d_path は失敗して何もしない) */
  f.f_path.mnt = &mnt;
  f.f_inode = i; f.f_mapping = i->i_mapping; f.f_mode = FMODE_READ | FMODE_WRITE;
  int rc = 0;
  if (i->i_fop->open) rc = ((int (*)(struct inode *, struct file *))i->i_fop->open)(i, &f);
  int64_t ret = rc;
  if (!rc) {
    struct kiocb kio = { .ki_filp = &f, .ki_pos = (loff_t)off, .ki_flags = 0 };
    struct iov_iter it = { .buf = buf, .count = len };
    ret = ((ssize_t (*)(struct kiocb *, struct iov_iter *))i->i_fop->write_iter)(&kio, &it);
    if (i->i_fop->release) ((int (*)(struct inode *, struct file *))i->i_fop->release)(i, &f);
    /* ordered データ: 確保したブロックへ今すぐ書く (メタデータはまだジャーナルのトランザクションの中。コミットはこの後) */
#ifdef USBMEM_MUTATE_NO_ORDERED
    int wr = 0;   /* データはアンマウント時まで書かない */
#else
    int wr = usbmem_kbh_write_mapping(i->i_mapping, 0, -1);
#endif
    if (wr && ret >= 0) ret = wr;
  }
  iput(i);
  after_op(v, 0);
  return ret;
}
int64_t usbmem_ext4_write(usbmem_ext4 *v, uint64_t ino, uint64_t off, const void *buf, size_t len) { return write_common(v, ino, off, buf, len, 0); }
int64_t usbmem_ext4_write_inplace(usbmem_ext4 *v, uint64_t ino, uint64_t off, const void *buf, size_t len) { return write_common(v, ino, off, buf, len, 1); }

/* サイズ変更 (SAF の "w" = 0 への切り詰めなど)。縮小は解放を伴う。 */
int usbmem_ext4_truncate(usbmem_ext4 *v, uint64_t ino, uint64_t size) {
  { int e = begin_alloc(v); if (e) return e; }
  int err = 0; struct inode *i = get_inode(v, ino, &err);
  if (!i) return err;
  if (S_ISDIR(i->i_mode)) { iput(i); return -EISDIR; }
  if (!S_ISREG(i->i_mode)) { iput(i); return -EINVAL; }
  if (IMMUTABLE_OR_APPEND(i)) { iput(i); return -EPERM; }
  if (data_write_refused(i)) { iput(i); return -EOPNOTSUPP; }
  if (!i->i_op || !i->i_op->setattr) { iput(i); return -EOPNOTSUPP; }
  if ((uint64_t)i->i_size == size) { iput(i); return 0; }
  if (size > (uint64_t)i->i_sb->s_maxbytes) { iput(i); return -EFBIG; }
  struct dentry *d = usbmem_d_alloc(i->i_sb, NULL, "t", 1);
  if (!d) { iput(i); return -ENOMEM; }
  ihold(i); d->d_inode = i;   /* usbmem_d_free が iput */
  struct iattr a; memset(&a, 0, sizeof a);
  a.ia_valid = ATTR_SIZE; a.ia_size = (loff_t)size;
  down_write(&i->i_rwsem);
  int rc = ((int (*)(struct mnt_idmap *, struct dentry *, struct iattr *))i->i_op->setattr)(&nop_mnt_idmap, d, &a);
  up_write(&i->i_rwsem);
  if (!rc) { int w = usbmem_kbh_write_mapping(i->i_mapping, 0, -1); if (w) rc = w; }   /* 末尾ブロックのゼロ埋め (ordered データ) */
  usbmem_d_free(d); iput(i);
  return after_op(v, rc);
}

/* dirty なデータ・メタデータをデバイスへ: データを書いてから、ext4 の sync_fs (jbd2 のコミットを要求して完了を待つ。コミットブロックは PREFLUSH|FUA) */
int usbmem_ext4_sync(usbmem_ext4 *v) {
  if (!v) return -EINVAL;
  if (sb_rdonly(v->sb)) return 0;
  enter(v);
  int rc = 0;
#ifdef USBMEM_MUTATE_NO_ORDERED   /* 突然変異: メタデータを先にコミットしてからデータを書く */
  if (v->sb->s_op && v->sb->s_op->sync_fs) ((int (*)(struct super_block *, int))v->sb->s_op->sync_fs)(v->sb, 1);
#endif
  size_t n; struct inode **list = usbmem_snapshot_inodes_(v->sb, &n);
  for (size_t k = 0; k < n; k++) {
    if (S_ISREG(list[k]->i_mode)) { int w = usbmem_kbh_write_mapping(list[k]->i_mapping, 0, -1); if (w && !rc) rc = w; }
    iput(list[k]);
  }
  free(list);
  if (v->sb->s_op && v->sb->s_op->sync_fs) { int r = ((int (*)(struct super_block *, int))v->sb->s_op->sync_fs)(v->sb, 1); if (r && !rc) rc = r; }
#ifndef USBMEM_MUTATE_NO_FLUSH
  { int r = usbmem_bdev_flush(v->sb->s_bdev->impl); if (r && !rc) rc = r; }
#endif
  return after_op(v, rc);
}

#else  /* !USBMEM_NTFS_WRITE: 書き込み入口は全て -EROFS */
int usbmem_ext4_sync(usbmem_ext4 *v) { (void)v; return 0; }
int usbmem_ext4_create(usbmem_ext4 *v, uint64_t d, const char *n, size_t l, uint32_t m, uint64_t *ino) { (void)v; (void)d; (void)n; (void)l; (void)m; (void)ino; return -EROFS; }
int usbmem_ext4_mkdir(usbmem_ext4 *v, uint64_t d, const char *n, size_t l, uint32_t m, uint64_t *ino) { (void)v; (void)d; (void)n; (void)l; (void)m; (void)ino; return -EROFS; }
int usbmem_ext4_unlink(usbmem_ext4 *v, uint64_t d, const char *n, size_t l) { (void)v; (void)d; (void)n; (void)l; return -EROFS; }
int usbmem_ext4_rmdir(usbmem_ext4 *v, uint64_t d, const char *n, size_t l) { (void)v; (void)d; (void)n; (void)l; return -EROFS; }
int usbmem_ext4_rename(usbmem_ext4 *v, uint64_t od, const char *on, size_t ol, uint64_t nd, const char *nn, size_t nl) { (void)v; (void)od; (void)on; (void)ol; (void)nd; (void)nn; (void)nl; return -EROFS; }
int usbmem_ext4_link(usbmem_ext4 *v, uint64_t d, const char *n, size_t l, uint64_t t) { (void)v; (void)d; (void)n; (void)l; (void)t; return -EROFS; }
int usbmem_ext4_symlink(usbmem_ext4 *v, uint64_t d, const char *n, size_t l, const char *t, size_t tl, uint64_t *ino) { (void)v; (void)d; (void)n; (void)l; (void)t; (void)tl; (void)ino; return -EROFS; }
int64_t usbmem_ext4_write(usbmem_ext4 *v, uint64_t ino, uint64_t off, const void *buf, size_t len) { (void)v; (void)ino; (void)off; (void)buf; (void)len; return -EROFS; }
int64_t usbmem_ext4_write_inplace(usbmem_ext4 *v, uint64_t ino, uint64_t off, const void *buf, size_t len) { (void)v; (void)ino; (void)off; (void)buf; (void)len; return -EROFS; }
int usbmem_ext4_truncate(usbmem_ext4 *v, uint64_t ino, uint64_t size) { (void)v; (void)ino; (void)size; return -EROFS; }
#endif
