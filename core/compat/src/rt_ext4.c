// SPDX-License-Identifier: GPL-2.0-or-later
// ext4 用の最小スタブと小さなランタイム関数。上流 fs/ext4 (read path) は無改変で、読み取り専用マウントで使われない
// 部分 (jbd2 / mballoc の割り当て / orphan / fast_commit / ioctl / sysfs / page-io / mmp) をここで置き換える。docs/ext4-port.md 参照。
//  - 書き込み系 (ブロック割り当て・journal 開始) は呼ばれたら -EROFS 等で失敗させる (読み取り専用マウントでは到達しない)。
//  - mballoc は「group_info の配列」(ext4_get_group_info が読み取り経路でも参照する) だけ作る。
#include <time.h>
#include "ext4.h"
#include "ext4_jbd2.h"

/* ---- 小さなランタイム (crc32c / crc16 / jiffies) */
static uint32_t g_crc32c_tab[256];
static pthread_once_t g_crc_once = PTHREAD_ONCE_INIT;
static void crc_init(void) {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;   /* Castagnoli (反転表現) */
    g_crc32c_tab[i] = c;
  }
}
/* カーネルの crc32c(): seed をそのまま使い、最後の反転もしない (呼び出し側 ext4_chksum が ~0 から始める)。 */
u32 crc32c(u32 crc, const void *p, size_t len) {
  pthread_once(&g_crc_once, crc_init);
  const unsigned char *b = p;
  while (len--) crc = g_crc32c_tab[(crc ^ *b++) & 0xff] ^ (crc >> 8);
  return crc;
}
u16 usbmem_crc16(u16 crc, const void *p, size_t len) {   /* lib/crc16.c と同じ (多項式 0xA001・反転) */
  const unsigned char *b = p;
  while (len--) {
    crc ^= *b++;
    for (int k = 0; k < 8; k++) crc = (crc & 1) ? (crc >> 1) ^ 0xA001u : crc >> 1;
  }
  return crc;
}
unsigned long usbmem_jiffies(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (unsigned long)ts.tv_sec * 1000UL + (unsigned long)(ts.tv_nsec / 1000000);
}
void flush_work(struct work_struct *w) { (void)w; }

/* rbtree の後行順走査 (カーネルの lib/rbtree.c と同じ。親は __rb_parent_color の下位 2 ビットを除いたもの) */
static struct rb_node *rb_parent_of(const struct rb_node *n) { return (struct rb_node *)(n->__rb_parent_color & ~3UL); }
static struct rb_node *rb_left_deepest(const struct rb_node *n) {
  for (;;) { if (n->rb_left) n = n->rb_left; else if (n->rb_right) n = n->rb_right; else return (struct rb_node *)n; }
}
struct rb_node *usbmem_rb_next_postorder(const struct rb_node *n) {
  if (!n) return NULL;
  const struct rb_node *p = rb_parent_of(n);
  if (p && n == p->rb_left && p->rb_right) return rb_left_deepest(p->rb_right);
  return (struct rb_node *)p;
}
struct rb_node *usbmem_rb_first_postorder(const struct rb_root *root) { return root->rb_node ? rb_left_deepest(root->rb_node) : NULL; }

/* ---- buffer_head の I/O: bh は rt_bh.c が読み込み済み (uptodate) で渡すので、ここへ来るのは異常系 */


/* bmap (jbd2 がジャーナル inode の論理ブロック → 物理ブロックを引く)。上流の ext4_bmap は iomap_bmap 経由なので、ext4_map_blocks で直接引く。0 = 穴 */
int bmap(struct inode *inode, sector_t *block) {
  struct ext4_map_blocks map = { .m_lblk = (ext4_lblk_t)*block, .m_len = 1 };
  int ret = ext4_map_blocks(NULL, inode, &map, 0);
  *block = (ret > 0 && (map.m_flags & EXT4_MAP_MAPPED)) ? (sector_t)map.m_pblk : 0;
  return ret < 0 ? ret : 0;
}

/* resize.c / ioctl.c はビルドしない */
void ext4_kvfree_array_rcu(void *to_free) { kvfree(to_free); }
int ext4_update_overhead(struct super_block *sb, bool force) { (void)sb; (void)force; return 0; }   /* s_overhead_clusters の更新 (ioctl.c)。書かない */
bool ext4_fc_replay_check_excluded(struct super_block *sb, ext4_fsblk_t blk) { (void)sb; (void)blk; return false; }   /* fast commit は使わない (書き込みでは拒否) */

/* ---- jbd2 / mballoc / orphan は上流 (fs/jbd2, fs/ext4/mballoc.c, orphan.c) をそのままビルドする (docs/ext4-write.md)。 */

/* ---- page-io (書き戻し) / sysfs / mmp / ioctl / orphan / fast_commit */
int __init ext4_init_pageio(void) { return 0; }
void ext4_exit_pageio(void) {}
void ext4_end_io_rsv_work(struct work_struct *w) { (void)w; }
void ext4_iomap_end_io(struct work_struct *w) { (void)w; }
void ext4_iomap_end_bio(struct bio *bio) { (void)bio; }
void ext4_bio_write_folio(struct ext4_io_submit *io, struct folio *f, size_t len) { (void)io; (void)f; (void)len; }
struct ext4_io_end_vec *ext4_alloc_io_end_vec(ext4_io_end_t *io_end) { (void)io_end; return ERR_PTR(-EROFS); }
struct ext4_io_end_vec *ext4_last_io_end_vec(ext4_io_end_t *io_end) { (void)io_end; return NULL; }

int __init ext4_init_sysfs(void) { return 0; }
void ext4_exit_sysfs(void) {}
/* sysfs は無い。ただし ext4_put_super は kobject 解放の完了を待つので、登録時点で「完了済み」にしておく */
int ext4_register_sysfs(struct super_block *sb) {
  struct ext4_sb_info *sbi = EXT4_SB(sb);
  init_completion(&sbi->s_kobj_unregister);
  complete(&sbi->s_kobj_unregister);
  return 0;
}
void ext4_unregister_sysfs(struct super_block *sb) { (void)sb; }
void ext4_notify_error_sysfs(struct ext4_sb_info *sbi) { (void)sbi; }

int ext4_multi_mount_protect(struct super_block *sb, ext4_fsblk_t mmp_block) { (void)sb; (void)mmp_block; return 0; }   /* 読み取り専用では MMP ブロックを書かない */
void ext4_stop_mmpd(struct ext4_sb_info *sbi) { (void)sbi; }

long ext4_ioctl(struct file *f, unsigned int cmd, unsigned long arg) { (void)f; (void)cmd; (void)arg; return -ENOTTY; }
int ext4_fileattr_get(struct dentry *d, struct file_kattr *fa) { (void)d; (void)fa; return -ENOTTY; }
int ext4_fileattr_set(const struct mnt_idmap *idmap, struct dentry *d, struct file_kattr *fa) { (void)idmap; (void)d; (void)fa; return -EROFS; }
int ext4_force_shutdown(struct super_block *sb, u32 flags) { (void)sb; (void)flags; return -EROFS; }

void ext4_fc_init(struct super_block *sb, journal_t *j) { (void)sb; (void)j; }
void ext4_fc_init_inode(struct inode *inode) {   /* リストの初期化は必要 (ext4_free_in_core_inode が空であることを検査する) */
  struct ext4_inode_info *ei = EXT4_I(inode);
  INIT_LIST_HEAD(&ei->i_fc_list);
  INIT_LIST_HEAD(&ei->i_fc_dilist);
  ei->i_fc_snap = NULL;
}
void ext4_fc_del(struct inode *inode) { (void)inode; }
int ext4_fc_commit(journal_t *j, tid_t tid) { (void)j; (void)tid; return 0; }
int __init ext4_fc_init_dentry_cache(void) { return 0; }
void ext4_fc_destroy_dentry_cache(void) {}
void ext4_fc_replay_cleanup(struct super_block *sb) { (void)sb; }
int ext4_fc_record_regions(struct super_block *sb, int ino, ext4_lblk_t lblk, ext4_fsblk_t pblk, int len, int replay) { (void)sb; (void)ino; (void)lblk; (void)pblk; (void)len; (void)replay; return 0; }
void ext4_fc_mark_ineligible(struct super_block *sb, int reason, handle_t *h) { (void)sb; (void)reason; (void)h; }
void ext4_fc_track_range(handle_t *h, struct inode *inode, ext4_lblk_t s, ext4_lblk_t e) { (void)h; (void)inode; (void)s; (void)e; }
void ext4_fc_track_unlink(handle_t *h, struct dentry *d) { (void)h; (void)d; }
void ext4_fc_track_link(handle_t *h, struct inode *inode, struct dentry *d) { (void)h; (void)inode; (void)d; }
void ext4_fc_track_create(handle_t *h, struct dentry *d) { (void)h; (void)d; }
void ext4_fc_track_inode(handle_t *h, struct inode *inode) { (void)h; (void)inode; }
void __ext4_fc_track_link(handle_t *h, struct inode *inode, struct dentry *d) { (void)h; (void)inode; (void)d; }
void __ext4_fc_track_unlink(handle_t *h, struct inode *inode, struct dentry *d) { (void)h; (void)inode; (void)d; }
void __ext4_fc_track_create(handle_t *h, struct inode *inode, struct dentry *d) { (void)h; (void)inode; (void)d; }

/* page-io の補助 (書き戻し用)。ext4_do_writepages が参照するだけで、読み取り専用では呼ばれない */
void ext4_io_submit_init(struct ext4_io_submit *io, struct writeback_control *wbc) { (void)io; (void)wbc; }
void ext4_io_submit(struct ext4_io_submit *io) { (void)io; }
ext4_io_end_t *ext4_init_io_end(struct inode *inode, gfp_t flags) { (void)inode; (void)flags; return NULL; }
ext4_io_end_t *ext4_get_io_end(ext4_io_end_t *io_end) { return io_end; }
int ext4_put_io_end(ext4_io_end_t *io_end) { (void)io_end; return 0; }
void ext4_put_io_end_defer(ext4_io_end_t *io_end) { (void)io_end; }
