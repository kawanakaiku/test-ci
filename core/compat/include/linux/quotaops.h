#pragma once
#include <compat/kernel.h>
/* CONFIG_QUOTA なし: カーネルの quotaops.h と同じく、クオータ関連はすべて「何もしない・成功」。 */
struct dquot;
typedef long long qsize_t;
static inline int dquot_suspend(struct super_block *sb, int type) { (void)sb; (void)type; return 0; }
static inline int dquot_writeback_dquots(struct super_block *sb, int type) { (void)sb; (void)type; return 0; }
static inline int dquot_initialize(struct inode *i) { (void)i; return 0; }
static inline void dquot_drop(struct inode *i) { (void)i; }
static inline int dquot_alloc_block(struct inode *i, qsize_t n) { (void)i; (void)n; return 0; }
static inline int dquot_alloc_block_nodirty(struct inode *i, qsize_t n) { (void)i; (void)n; return 0; }
static inline void dquot_free_block(struct inode *i, qsize_t n) { (void)i; (void)n; }
static inline void dquot_free_block_nodirty(struct inode *i, qsize_t n) { (void)i; (void)n; }
static inline int dquot_alloc_inode(struct inode *i) { (void)i; return 0; }
static inline void dquot_free_inode(struct inode *i) { (void)i; }
static inline int dquot_transfer(const struct mnt_idmap *m, struct inode *i, struct iattr *a) { (void)m; (void)i; (void)a; return 0; }
static inline int dquot_file_open(struct inode *i, struct file *f) { (void)i; (void)f; return 0; }
static inline int dquot_disable(struct super_block *sb, int type, unsigned int flags) { (void)sb; (void)type; (void)flags; return 0; }
static inline int dquot_resume(struct super_block *sb, int type) { (void)sb; (void)type; return 0; }
