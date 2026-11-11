/* SPDX-License-Identifier: GPL-2.0-or-later */
/* fs-verity: 使わない (CONFIG_FS_VERITY なし) の上流 linux/fsverity.h の「無効時」スタブと同じ形。verity 付きボリュームは別途機能フラグで拒否する。 */
#pragma once
#include <compat/kernel.h>
struct fsverity_info; struct fsverity_operations; struct fsverity_verification_context;
static inline struct fsverity_info *fsverity_get_info(const struct inode *inode) { (void)inode; return NULL; }
static inline bool fsverity_active(const struct inode *inode) { (void)inode; return false; }
static inline int fsverity_file_open(struct inode *inode, struct file *filp) { (void)inode; (void)filp; return 0; }
static inline int fsverity_ioctl_enable(struct file *f, const void __user *a) { (void)f; (void)a; return -EOPNOTSUPP; }
static inline int fsverity_ioctl_measure(struct file *f, void __user *a) { (void)f; (void)a; return -EOPNOTSUPP; }
static inline int fsverity_ioctl_read_metadata(struct file *f, const void __user *a) { (void)f; (void)a; return -EOPNOTSUPP; }
static inline void fsverity_enqueue_verify_work(struct work_struct *w) { (void)w; }
static inline bool fsverity_verify_folio(struct fsverity_info *vi, struct folio *f) { (void)vi; (void)f; return true; }
static inline void fsverity_verify_bio(struct fsverity_info *vi, struct bio *b) { (void)vi; (void)b; }
static inline void fsverity_readahead(struct fsverity_info *vi, pgoff_t index, unsigned long n) { (void)vi; (void)index; (void)n; }
