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
void bh_end_read(struct bio *bio) { (void)bio; }
void bh_end_write(struct bio *bio) { (void)bio; }
void bh_submit(struct buffer_head *bh, blk_opf_t op, bio_end_io_t end_io) { (void)bh; (void)op; (void)end_io; }

/* ---- jbd2: journal は読み込まない (sbi->s_journal == NULL)。ハンドルは常に無効 */
handle_t *jbd2__journal_start(journal_t *j, int blocks, int rsv, int rev, gfp_t gfp, unsigned int type, unsigned int line) { (void)j; (void)blocks; (void)rsv; (void)rev; (void)gfp; (void)type; (void)line; return ERR_PTR(-EROFS); }
handle_t *jbd2_alloc_handle(gfp_t gfp) { (void)gfp; return NULL; }
void jbd2_free_handle(handle_t *h) { (void)h; }
struct jbd2_inode *jbd2_alloc_inode(gfp_t gfp) { return kzalloc(sizeof(struct jbd2_inode), gfp); }
void jbd2_free_inode(struct jbd2_inode *ji) { kfree(ji); }
int jbd2__journal_restart(handle_t *h, int blocks, int rsv, gfp_t gfp) { (void)h; (void)blocks; (void)rsv; (void)gfp; return -EROFS; }
int jbd2_journal_start_reserved(handle_t *h, unsigned int type, unsigned int line) { (void)h; (void)type; (void)line; return -EROFS; }
int jbd2_journal_stop(handle_t *h) { (void)h; return 0; }
int jbd2_journal_extend(handle_t *h, int n, int rsv) { (void)h; (void)n; (void)rsv; return -EROFS; }
int jbd2_journal_free_reserved(handle_t *h) { (void)h; return 0; }
int jbd2_journal_get_write_access(handle_t *h, struct buffer_head *bh) { (void)h; (void)bh; return -EROFS; }
int jbd2_journal_get_create_access(handle_t *h, struct buffer_head *bh) { (void)h; (void)bh; return -EROFS; }
int jbd2_journal_dirty_metadata(handle_t *h, struct buffer_head *bh) { (void)h; (void)bh; return -EROFS; }
int jbd2_journal_forget(handle_t *h, struct buffer_head *bh) { (void)h; (void)bh; return 0; }
int jbd2_journal_revoke(handle_t *h, unsigned long long blk, struct buffer_head *bh) { (void)h; (void)blk; (void)bh; return 0; }
void jbd2_journal_set_triggers(struct buffer_head *bh, struct jbd2_buffer_trigger_type *t) { (void)bh; (void)t; }
int jbd2_handle_buffer_credits(handle_t *h) { (void)h; return 0; }
unsigned int jbd2_journal_blocks_per_folio(struct inode *inode) { (void)inode; return 1; }
int jbd2_journal_invalidate_folio(journal_t *j, struct folio *f, size_t off, size_t len) { (void)j; (void)f; (void)off; (void)len; return 1; }
int jbd2_journal_try_to_free_buffers(journal_t *j, struct folio *f) { (void)j; (void)f; return 1; }
void jbd2_journal_init_jbd_inode(struct jbd2_inode *ji, struct inode *inode) { memset(ji, 0, sizeof *ji); ji->i_vfs_inode = inode; }
void jbd2_journal_release_jbd_inode(journal_t *j, struct jbd2_inode *ji) { (void)j; (void)ji; }
int jbd2_journal_begin_ordered_truncate(journal_t *j, struct jbd2_inode *ji, loff_t new_size) { (void)j; (void)ji; (void)new_size; return 0; }
int jbd2_journal_inode_ranged_write(handle_t *h, struct jbd2_inode *ji, loff_t start, loff_t len) { (void)h; (void)ji; (void)start; (void)len; return 0; }
int jbd2_journal_inode_ranged_wait(handle_t *h, struct jbd2_inode *ji, loff_t start, loff_t len) { (void)h; (void)ji; (void)start; (void)len; return 0; }
int jbd2_journal_finish_inode_data_buffers(struct jbd2_inode *ji) { (void)ji; return 0; }
bool jbd2_jinode_get_dirty_range(const struct jbd2_inode *ji, loff_t *s, loff_t *e) { (void)ji; *s = *e = 0; return false; }
int jbd2_journal_start_commit(journal_t *j, tid_t *tid) { (void)j; (void)tid; return 0; }
int jbd2_journal_force_commit(journal_t *j) { (void)j; return 0; }
int jbd2_journal_force_commit_nested(journal_t *j) { (void)j; return 0; }
int jbd2_log_wait_commit(journal_t *j, tid_t tid) { (void)j; (void)tid; return 0; }
int jbd2_transaction_committed(journal_t *j, tid_t tid) { (void)j; (void)tid; return 1; }
int jbd2_trans_will_send_data_barrier(journal_t *j, tid_t tid) { (void)j; (void)tid; return 0; }
tid_t jbd2_get_latest_transaction(journal_t *j) { (void)j; return 0; }
void jbd2_journal_lock_updates(journal_t *j) { (void)j; }
void jbd2_journal_unlock_updates(journal_t *j) { (void)j; }
int jbd2_journal_flush(journal_t *j, unsigned int flags) { (void)j; (void)flags; return 0; }
void jbd2_journal_abort(journal_t *j, int errno_) { (void)j; (void)errno_; }
int jbd2_journal_errno(journal_t *j) { (void)j; return 0; }
int jbd2_journal_clear_err(journal_t *j) { (void)j; return 0; }
void jbd2_journal_update_sb_errno(journal_t *j) { (void)j; }
int jbd2_journal_destroy(journal_t *j) { (void)j; return 0; }
int jbd2_journal_wipe(journal_t *j, int write) { (void)j; (void)write; return 0; }
int jbd2_journal_load(journal_t *j) { (void)j; return -EROFS; }
journal_t *jbd2_journal_init_dev(struct block_device *bdev, struct block_device *fs_dev, unsigned long long start, int len, int bsize) { (void)bdev; (void)fs_dev; (void)start; (void)len; (void)bsize; return NULL; }
journal_t *jbd2_journal_init_inode(struct inode *inode) { (void)inode; return NULL; }
int jbd2_journal_check_available_features(journal_t *j, unsigned long a, unsigned long b, unsigned long c) { (void)j; (void)a; (void)b; (void)c; return 0; }
int jbd2_journal_set_features(journal_t *j, unsigned long a, unsigned long b, unsigned long c) { (void)j; (void)a; (void)b; (void)c; return 0; }
void jbd2_journal_clear_features(journal_t *j, unsigned long a, unsigned long b, unsigned long c) { (void)j; (void)a; (void)b; (void)c; }

/* ---- mballoc: group_info の配列だけを作る (読み取り経路の ext4_get_group_info が使う)。割り当て・解放・予約は行わない */
int ext4_mb_init(struct super_block *sb) {
  struct ext4_sb_info *sbi = EXT4_SB(sb);
  ext4_group_t ngroups = ext4_get_groups_count(sb);
  unsigned per = EXT4_DESC_PER_BLOCK(sb);
  unsigned nmeta = (ngroups + per - 1) / per;
  struct ext4_group_info ***tab = kvzalloc(sizeof(*tab) * (nmeta ? nmeta : 1), GFP_KERNEL);
  if (!tab) return -ENOMEM;
  sbi->s_group_info = (struct ext4_group_info ** __rcu *)tab;
  sbi->s_group_info_size = nmeta;
  size_t gi_size = sizeof(struct ext4_group_info) + sizeof(ext4_grpblk_t) * (sb->s_blocksize_bits + 2);
  for (unsigned m = 0; m < nmeta; m++) {
    tab[m] = kvzalloc(sizeof(struct ext4_group_info *) * per, GFP_KERNEL);
    if (!tab[m]) goto fail;
  }
  for (ext4_group_t g = 0; g < ngroups; g++) {
    struct ext4_group_info *gi = kvzalloc(gi_size, GFP_KERNEL);
    if (!gi) goto fail;
    gi->bb_state = 1UL << EXT4_GROUP_INFO_NEED_INIT_BIT;   /* ビットマップ由来の情報は未構築 */
    gi->bb_group = g;
    INIT_LIST_HEAD(&gi->bb_prealloc_list);
    init_rwsem(&gi->alloc_sem);
    tab[g / per][g % per] = gi;
  }
  return 0;
fail:
  ext4_mb_release(sb);
  return -ENOMEM;
}
void ext4_mb_release(struct super_block *sb) {
  struct ext4_sb_info *sbi = EXT4_SB(sb);
  struct ext4_group_info ***tab = (struct ext4_group_info ***)sbi->s_group_info;
  if (!tab) return;
  ext4_group_t ngroups = ext4_get_groups_count(sb);
  unsigned per = EXT4_DESC_PER_BLOCK(sb);
  for (unsigned m = 0; m < sbi->s_group_info_size; m++) {
    if (!tab[m]) continue;
    for (unsigned i = 0; i < per && (ext4_group_t)m * per + i < ngroups; i++) kvfree(tab[m][i]);
    kvfree(tab[m]);
  }
  kvfree(tab);
  sbi->s_group_info = NULL; sbi->s_group_info_size = 0;
}
int __init ext4_init_mballoc(void) { return 0; }
void ext4_exit_mballoc(void) {}
ext4_fsblk_t ext4_mb_new_blocks(handle_t *h, struct ext4_allocation_request *ar, int *errp) { (void)h; (void)ar; *errp = -EROFS; return 0; }
void ext4_free_blocks(handle_t *h, struct inode *inode, struct buffer_head *bh, ext4_fsblk_t block, unsigned long count, int flags) { (void)h; (void)inode; (void)bh; (void)block; (void)count; (void)flags; }
void ext4_mb_mark_bb(struct super_block *sb, ext4_fsblk_t block, int len, bool state) { (void)sb; (void)block; (void)len; (void)state; }
void ext4_discard_preallocations(struct inode *inode) { (void)inode; }
void ext4_process_freed_data(struct super_block *sb, tid_t commit_tid) { (void)sb; (void)commit_tid; }
ext4_group_t ext4_mb_prefetch(struct super_block *sb, ext4_group_t group, unsigned int nr, int *cnt) { (void)sb; if (cnt) *cnt = 0; return group + nr; }
void ext4_mb_prefetch_fini(struct super_block *sb, ext4_group_t group, unsigned int nr) { (void)sb; (void)group; (void)nr; }
void ext4_kvfree_array_rcu(void *to_free) { kvfree(to_free); }
int ext4_update_overhead(struct super_block *sb, bool force) { (void)sb; (void)force; return 0; }   /* 超ブロックへ書く処理: 読み取り専用では不要 */

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

/* orphan: 読み取り専用では孤立 inode の回収をしない (needs_recovery / orphan_present のボリュームは API 層が既定で拒否する) */
int ext4_orphan_add(handle_t *h, struct inode *inode) { (void)h; (void)inode; return -EROFS; }
int ext4_orphan_del(handle_t *h, struct inode *inode) { (void)h; (void)inode; return 0; }
void ext4_orphan_cleanup(struct super_block *sb, struct ext4_super_block *es) { (void)sb; (void)es; }
int ext4_init_orphan_info(struct super_block *sb) { (void)sb; return 0; }
void ext4_release_orphan_info(struct super_block *sb) { (void)sb; }
int ext4_orphan_file_empty(struct super_block *sb) { (void)sb; return 1; }
void ext4_orphan_file_block_trigger(struct jbd2_buffer_trigger_type *t, struct buffer_head *bh, void *data, size_t size) { (void)t; (void)bh; (void)data; (void)size; }

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
