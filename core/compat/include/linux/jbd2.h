/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * jbd2 (ジャーナル) の最小互換ヘッダ。fs/ext4 の読み取り専用移植用 (docs/ext4-port.md)。
 * jbd2 本体 (fs/jbd2) は取り込まない: ジャーナルの再生・書き込みはディスクを書き換えるため、このアプリの読み取り専用方針と両立しない。
 * マウント時は sbi->s_journal == NULL (ジャーナル無しの ext4 と同じ状態) になり、ここの関数 (jbd2_journal_*) は呼ばれない。
 * 呼ばれたら src/rt_ext4.c のスタブが usbmem_unimplemented で記録して失敗/何もしない。
 * 上流の linux/jbd2.h にある定義のうち、ext4 の読み取り経路がコンパイル上必要とするものだけを、同名・同じ形で持つ。
 */
#ifndef USBMEM_LINUX_JBD2_H
#define USBMEM_LINUX_JBD2_H
#include <compat/kernel.h>
#include <linux/buffer_head.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int tid_t;
struct jbd2_inode;
struct journal_s;
typedef struct journal_superblock_s { __be32 s_nr_users; __be32 s_blocksize; __be32 s_maxlen; __be32 s_first; __be32 s_sequence; __be32 s_start; __be32 s_errno; __be32 s_feature_compat, s_feature_incompat, s_feature_ro_compat; __u8 s_uuid[16]; __be32 s_nr_users_; } journal_superblock_t;
typedef struct journal_s journal_t;
enum { T_RUNNING, T_LOCKED, T_SWITCH, T_FLUSH, T_COMMIT, T_COMMIT_DFLUSH, T_COMMIT_JFLUSH, T_COMMIT_CALLBACK, T_FINISHED };
typedef struct transaction_s { tid_t t_tid; int t_state; struct journal_s *t_journal; } transaction_t;
struct journal_head { struct buffer_head *b_bh; transaction_t *b_transaction, *b_next_transaction; };

/* ジャーナル機能フラグ (jbd2 の journal superblock の値。上流と同じ) */
#define JBD2_FEATURE_COMPAT_CHECKSUM      0x00000001
#define JBD2_FEATURE_INCOMPAT_REVOKE      0x00000001
#define JBD2_FEATURE_INCOMPAT_64BIT       0x00000002
#define JBD2_FEATURE_INCOMPAT_ASYNC_COMMIT 0x00000004
#define JBD2_FEATURE_INCOMPAT_CSUM_V2     0x00000008
#define JBD2_FEATURE_INCOMPAT_CSUM_V3     0x00000010
#define JBD2_FEATURE_INCOMPAT_FAST_COMMIT 0x00000020
/* journal_t の j_flags */
#define JBD2_UNMOUNT   0x001
#define JBD2_ABORT     0x002
#define JBD2_ACK_ERR   0x004
#define JBD2_FLUSHED   0x008
#define JBD2_LOADED    0x010
#define JBD2_BARRIER   0x020
#define JBD2_ABORT_ON_SYNCDATA_ERR 0x040
#define JBD2_CYCLE_RECORD 0x080
#define JBD2_DEFAULT_MAX_COMMIT_AGE 5

struct jbd2_buffer_trigger_type { void (*t_frozen)(struct jbd2_buffer_trigger_type *, struct buffer_head *, void *, size_t); void (*t_abort)(struct jbd2_buffer_trigger_type *, struct buffer_head *); };

struct jbd2_inode {
  transaction_t *i_transaction, *i_next_transaction;
  struct list_head i_list;
  struct inode *i_vfs_inode;
  unsigned long i_flags;
  loff_t i_dirty_start, i_dirty_end;
};

struct journal_s {
  unsigned long j_flags;
  int j_errno;
  struct block_device *j_dev;
  struct inode *j_inode;
  rwlock_t j_state_lock;
  struct task_struct *j_task;
  unsigned long j_start;
  void *j_private;
  unsigned long j_len, j_total_len, j_blocks;
  unsigned int j_inum, j_quota_inum;
  void (*j_unregister)(journal_t *);
  u32 j_hash;
  transaction_t *j_committing_transaction, *j_running_transaction;
  struct journal_superblock_s *j_superblock;
  int (*j_submit_inode_data_buffers)(struct jbd2_inode *);
  int (*j_finish_inode_data_buffers)(struct jbd2_inode *);
  unsigned int j_min_batch_time, j_max_batch_time, j_commit_interval;
  tid_t j_commit_sequence;
  void (*j_commit_callback)(journal_t *, transaction_t *);
  int (*j_bmap)(journal_t *, sector_t *);
};

typedef struct jbd2_journal_handle {
  struct jbd2_journal_handle *h_rsv_handle;
  transaction_t *h_transaction;
  journal_t *h_journal;
  int h_total_credits, h_revoke_credits, h_revoke_credits_requested;
  int h_ref, h_err;
  unsigned int h_invalid:1, h_sync:1, h_jdata:1, h_reserved:1, h_aborted:1, h_type:8, h_line_no:16;
  unsigned long h_start_jiffies;
  unsigned int h_requested_credits;
  unsigned int saved_alloc_context;
} handle_t;

/* jbd2 の関数 (実体は src/rt_ext4.c のスタブ。s_journal == NULL なので呼ばれない) */
handle_t *jbd2__journal_start(journal_t *, int, int, int, gfp_t, unsigned int, unsigned int);
int jbd2_journal_stop(handle_t *);
int jbd2_journal_get_write_access(handle_t *, struct buffer_head *);
int jbd2_journal_get_create_access(handle_t *, struct buffer_head *);
int jbd2_journal_dirty_metadata(handle_t *, struct buffer_head *);
int jbd2_journal_forget(handle_t *, struct buffer_head *);
int jbd2_journal_revoke(handle_t *, unsigned long long, struct buffer_head *);
int jbd2_journal_extend(handle_t *, int, int);
int jbd2__journal_restart(handle_t *, int, int, gfp_t);
int jbd2_journal_flush(journal_t *, unsigned int);
void jbd2_journal_lock_updates(journal_t *);
void jbd2_journal_unlock_updates(journal_t *);
int jbd2_journal_load(journal_t *);
int jbd2_journal_destroy(journal_t *);
int jbd2_journal_errno(journal_t *);
int jbd2_journal_clear_err(journal_t *);
void jbd2_journal_abort(journal_t *, int);
void jbd2_journal_update_sb_errno(journal_t *);
int jbd2_journal_wipe(journal_t *, int);
int jbd2_journal_force_commit(journal_t *);
int jbd2_journal_force_commit_nested(journal_t *);
int jbd2_journal_start_commit(journal_t *, tid_t *);
int jbd2_log_wait_commit(journal_t *, tid_t);
int jbd2_transaction_committed(journal_t *, tid_t);
int jbd2_trans_will_send_data_barrier(journal_t *, tid_t);
int jbd2_journal_set_features(journal_t *, unsigned long, unsigned long, unsigned long);
void jbd2_journal_clear_features(journal_t *, unsigned long, unsigned long, unsigned long);
int jbd2_journal_check_available_features(journal_t *, unsigned long, unsigned long, unsigned long);
journal_t *jbd2_journal_init_dev(struct block_device *, struct block_device *, unsigned long long, int, int);
journal_t *jbd2_journal_init_inode(struct inode *);
void jbd2_journal_init_jbd_inode(struct jbd2_inode *, struct inode *);
void jbd2_journal_release_jbd_inode(journal_t *, struct jbd2_inode *);
int jbd2_journal_begin_ordered_truncate(journal_t *, struct jbd2_inode *, loff_t);
int jbd2_journal_inode_ranged_write(handle_t *, struct jbd2_inode *, loff_t, loff_t);
int jbd2_journal_inode_ranged_wait(handle_t *, struct jbd2_inode *, loff_t, loff_t);
int jbd2_journal_finish_inode_data_buffers(struct jbd2_inode *);
int jbd2_journal_try_to_free_buffers(journal_t *, struct folio *);
int jbd2_journal_invalidate_folio(journal_t *, struct folio *, size_t, size_t);
void jbd2_journal_set_triggers(struct buffer_head *, struct jbd2_buffer_trigger_type *);
int jbd2_journal_free_reserved(handle_t *);
int jbd2_journal_start_reserved(handle_t *, unsigned int, unsigned int);
int jbd2_journal_abort_handle(handle_t *);
int jbd2_handle_buffer_credits(handle_t *);
bool jbd2_jinode_get_dirty_range(const struct jbd2_inode *, loff_t *, loff_t *);
tid_t jbd2_get_latest_transaction(journal_t *);
unsigned int jbd2_journal_blocks_per_folio(struct inode *);
struct jbd2_inode *jbd2_alloc_inode(gfp_t);
void jbd2_free_inode(struct jbd2_inode *);
handle_t *jbd2_alloc_handle(gfp_t);
void jbd2_free_handle(handle_t *);

#define jbd2_journal_start(j, nblocks) jbd2__journal_start((j), (nblocks), 0, 0, GFP_NOFS, 0, 0)
#define jbd2_journal_restart(h, n) jbd2__journal_restart((h), (n), 0, GFP_NOFS)
#define journal_current_handle() (NULL)
#define is_handle_aborted(h) (1)

#ifdef __cplusplus
}
#endif
#endif
