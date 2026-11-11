#pragma once
#include <compat/kernel.h>
/* CONFIG_QUOTA なし: カーネルの quotaops.h と同じく、クオータ自体は「何もしない・成功」。ただしブロックの確保・解放は inode の使用量 (i_blocks) を更新する
 * (カーネルでは dquot_alloc_space 系と dquot_free_space 系が inode_add_bytes / inode_sub_bytes を呼ぶ。ext2 は i_blocks をこれに頼っている)。 */
static inline void inode_add_bytes(struct inode *i, long long bytes) {
  unsigned long long b = (unsigned long long)bytes + i->i_bytes;
  i->i_blocks += (unsigned long)(b >> 9); i->i_bytes = (unsigned short)(b & 511);
}
static inline void inode_sub_bytes(struct inode *i, long long bytes) {
  long long t = (long long)(i->i_blocks) * 512 + i->i_bytes - bytes;
  if (t < 0) t = 0;
  i->i_blocks = (unsigned long)(t >> 9); i->i_bytes = (unsigned short)(t & 511);
}
struct dquot;
typedef long long qsize_t;
static inline int dquot_suspend(struct super_block *sb, int type) { (void)sb; (void)type; return 0; }
static inline int dquot_writeback_dquots(struct super_block *sb, int type) { (void)sb; (void)type; return 0; }
static inline int dquot_initialize(struct inode *i) { (void)i; return 0; }
static inline void dquot_drop(struct inode *i) { (void)i; }
static inline int dquot_alloc_block(struct inode *i, qsize_t n) { inode_add_bytes(i, n << i->i_blkbits); mark_inode_dirty_sync(i); return 0; }
static inline int dquot_alloc_block_nodirty(struct inode *i, qsize_t n) { inode_add_bytes(i, n << i->i_blkbits); return 0; }
static inline void dquot_free_block(struct inode *i, qsize_t n) { inode_sub_bytes(i, n << i->i_blkbits); mark_inode_dirty_sync(i); }
static inline void dquot_free_block_nodirty(struct inode *i, qsize_t n) { inode_sub_bytes(i, n << i->i_blkbits); }
static inline int dquot_alloc_inode(struct inode *i) { (void)i; return 0; }
static inline void dquot_free_inode(struct inode *i) { (void)i; }
static inline int dquot_transfer(const struct mnt_idmap *m, struct inode *i, struct iattr *a) { (void)m; (void)i; (void)a; return 0; }
static inline int dquot_file_open(struct inode *i, struct file *f) { (void)i; (void)f; return 0; }
static inline int dquot_disable(struct super_block *sb, int type, unsigned int flags) { (void)sb; (void)type; (void)flags; return 0; }
static inline int dquot_resume(struct super_block *sb, int type) { (void)sb; (void)type; return 0; }
