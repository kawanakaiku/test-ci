// SPDX-License-Identifier: GPL-2.0-or-later
// fs/jbd2 + ext4 書き込みのための実行時 (USBMEM_KBH)。docs/ext4-write.md。
//
// 実行モデル: 互換層は単一スレッド。jbd2 のコミットスレッド kjournald2 は作らない。代わりに
//  - kthread_run("jbd2/...") は「このスレッドのジャーナル」を記録するだけ。
//  - 待ち (wait_event / schedule) に入ったとき usbmem_compat_run_deferred_work → usbmem_jbd2_poll_current が呼ばれ、
//    kjournald2 の 1 周分 (コミット要求があればコミット、アンマウント要求なら j_task を消す) を呼び出し元のスレッドで行う。
//  - 待っても条件が成り立たないなら、他に条件を変えられる主体はいない (デッドロック) ので、黙って止まらずに中断する。
// コミットはタイマーでは起きない: 明示的な同期 (sync_fs)・トランザクションの大きさ/ログの残り・アンマウントのときだけ。
#include <stdio.h>
#include <stdlib.h>
#include <compat/kernel.h>
#include <compat/usbmem_ntfs.h>
#include <linux/jbd2.h>
#include <linux/crc32.h>

/* ---- kjournald2 の代わり */
static struct task_struct g_kjournald_task;
static __thread journal_t *g_tls_journal;   /* このスレッドで操作中のジャーナル (マウント時と API の入口で設定) */

void usbmem_jbd2_set_current(journal_t *j) { g_tls_journal = j; }
journal_t *usbmem_jbd2_get_current(void) { return g_tls_journal; }

/* 生きているジャーナルの登録 (スレッドの「現在のジャーナル」が解放済みを指していても触らないように) */
static pthread_mutex_t g_jreg_mu = PTHREAD_MUTEX_INITIALIZER;
static journal_t *g_jreg[64];
static void jreg_set(journal_t *j, int on) {
  pthread_mutex_lock(&g_jreg_mu);
  for (int k = 0; k < 64; k++) {
    if (on && !g_jreg[k]) { g_jreg[k] = j; break; }
    if (!on && g_jreg[k] == j) { g_jreg[k] = NULL; break; }
  }
  pthread_mutex_unlock(&g_jreg_mu);
}
static int jreg_has(journal_t *j) {
  int r = 0;
  pthread_mutex_lock(&g_jreg_mu);
  for (int k = 0; k < 64; k++) if (g_jreg[k] == j) { r = 1; break; }
  pthread_mutex_unlock(&g_jreg_mu);
  return r;
}

struct task_struct *usbmem_kthread_run(int (*fn)(void *), void *data, const char *name, const char *arg) {
  (void)fn; (void)arg;
  if (name && !strncmp(name, "jbd2/", 5)) {
    g_tls_journal = data;   /* kjournald2(journal) */
    jreg_set(data, 1);
    return &g_kjournald_task;
  }
  /* ext4 の lazyinit / kmmpd は使わない (マウントオプションで不要にする)。来たら失敗させる */
  usbmem_printk("usbmem jbd2: kthread_run(%s) is not supported\n", name ? name : "?");
  return ERR_PTR(-ENOSYS);
}
int usbmem_kthread_stop(struct task_struct *t) { (void)t; return 0; }

/* kjournald2 の 1 周 (fs/jbd2/journal.c の kjournald2 と同じ判断。待つ代わりに戻る) */
static void jbd2_poll(journal_t *j) {
  static __thread int depth;
  if (!j || depth || !jreg_has(j)) return;
  depth++;
  write_lock(&j->j_state_lock);
  if (!(j->j_flags & JBD2_UNMOUNT) && !j->j_task) j->j_task = &g_kjournald_task;
  while (!(j->j_flags & JBD2_UNMOUNT) && j->j_commit_sequence != j->j_commit_request) {
    transaction_t *t = j->j_running_transaction;
    if (!t) break;
    if (atomic_read(&t->t_updates) > 0) {
      /* このスレッド自身がハンドルを開いたまま待っている: コミットは t_updates が 0 になるのを待つので、ここでは進められない */
      usbmem_printk("usbmem jbd2: commit requested while a handle is open (tid %u)\n", t->t_tid);
      break;
    }
    write_unlock(&j->j_state_lock);
    jbd2_journal_commit_transaction(j);
    write_lock(&j->j_state_lock);
  }
  int gone = 0;
  if (j->j_flags & JBD2_UNMOUNT) { j->j_task = NULL; gone = 1; }
  write_unlock(&j->j_state_lock);
  if (gone) { jreg_set(j, 0); if (g_tls_journal == j) g_tls_journal = NULL; }
  wake_up(&j->j_wait_done_commit);
  depth--;
}
void usbmem_jbd2_poll_current(void) { jbd2_poll(g_tls_journal); }
void usbmem_jbd2_schedule(void) { usbmem_compat_run_deferred_work(); }
void usbmem_jbd2_stuck(const char *what, const char *file, int line) {
  usbmem_printk("usbmem jbd2: wait can never finish (single-threaded): %s at %s:%d\n", what, file, line);
  fprintf(stderr, "usbmem jbd2: wait can never finish (single-threaded): %s at %s:%d\n", what, file, line);
  abort();
}
static wait_queue_head_t g_bit_wq;
static pthread_once_t g_bit_once = PTHREAD_ONCE_INIT;
static void bit_wq_init(void) { init_waitqueue_head(&g_bit_wq); }
wait_queue_head_t *usbmem_bit_waitqueue(void *word, int bit) { (void)word; (void)bit; pthread_once(&g_bit_once, bit_wq_init); return &g_bit_wq; }

/* bd_mapping の host inode (カーネルでは bdev の inode。jbd2 の revoke / commit は mapping->host->i_sb が blockdev_superblock かで
 * 「ファイルデータかメタデータか」を見分ける)。ext4 のブロックデバイスにだけ付ける。 */
struct super_block usbmem_blockdev_sb;
static struct inode g_bdev_host_inode;
void usbmem_kbh_bdev_attach_host(struct block_device *bd) {
  g_bdev_host_inode.i_sb = &usbmem_blockdev_sb;
  g_bdev_host_inode.i_blkbits = 9;
  bd->bd_data_.host = &g_bdev_host_inode;
}

/* ---- crc32_be (JBD2 v1 チェックサム。lib/crc32.c と同じ: 多項式 0x04C11DB7、反転なし) */
u32 usbmem_crc32_be(u32 crc, const void *p, size_t len) {
  const unsigned char *b = p;
  while (len--) {
    crc ^= (u32)*b++ << 24;
    for (int k = 0; k < 8; k++) crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
  }
  return crc;
}

void truncate_inode_pages_range(struct address_space *m, loff_t start, loff_t end) {
  (void)m; (void)start; (void)end;
  usbmem_unimplemented("truncate_inode_pages_range");   /* jbd2_journal_flush の discard/zeroout (ioctl) だけが使う。使わない */
}

/* ---- ext4 の write_begin が使う folio の取得: ロック済み・参照付き。uptodate でなければ先に全体を読む
 * (カーネルは読まないが、読んでおくと ext4_block_write_begin が部分ブロックを個別に読む必要がなくなる。中身は同じ) */
struct folio *usbmem_write_begin_get_folio(struct address_space *m, pgoff_t idx) {
  struct folio *f = __filemap_get_folio(m, idx, 0, 0);
  if (!IS_ERR(f) && (folio_test_uptodate(f) || folio_buffers(f))) { folio_lock(f); return f; }
  if (!IS_ERR(f)) folio_put(f);
  f = read_mapping_folio(m, idx, NULL);
  if (IS_ERR(f)) return f;
  folio_lock(f);
  return f;
}

/* ---- ゼロ埋め (未初期化エクステントの変換など)。ページキャッシュを通さずデバイスへ書く (カーネルの blkdev_issue_zeroout と同じ) */
int blkdev_issue_zeroout(struct block_device *bdev, sector_t sector, sector_t nr_sects, gfp_t gfp, unsigned flags) {
  (void)gfp; (void)flags;
  static const char zero[65536];
  u64 off = (u64)sector << 9, len = (u64)nr_sects << 9;
  while (len) {
    size_t n = len > sizeof zero ? sizeof zero : (size_t)len;
    int rc = usbmem_bdev_write(bdev->impl, off, zero, n);
    if (rc) return -EIO;
    off += n; len -= n;
  }
  return 0;
}
int sb_issue_zeroout(struct super_block *sb, sector_t block, sector_t nr, gfp_t gfp) {
  unsigned shift = sb->s_blocksize_bits - 9;
  return blkdev_issue_zeroout(sb->s_bdev, block << shift, nr << shift, gfp, 0);
}

/* ---- xarray (添字順の配列。mballoc の group 一覧で使う: 挿入・削除は memmove、検索は二分探索) */
struct uxa { size_t n, cap; unsigned long *idx; void **p; };
static size_t uxa_lb(const struct uxa *a, unsigned long i) {
  size_t lo = 0, hi = a->n;
  while (lo < hi) { size_t mid = (lo + hi) / 2; if (a->idx[mid] < i) lo = mid + 1; else hi = mid; }
  return lo;
}
void usbmem_xa_init(struct xarray *xa) { xa->impl_ = NULL; }
void usbmem_xa_destroy(struct xarray *xa) { struct uxa *a = xa->impl_; if (a) { free(a->idx); free(a->p); free(a); } xa->impl_ = NULL; }
int usbmem_xa_empty(const struct xarray *xa) { const struct uxa *a = xa->impl_; return !a || !a->n; }
void *usbmem_xa_load(struct xarray *xa, unsigned long i) {
  struct uxa *a = xa->impl_; if (!a) return NULL;
  size_t k = uxa_lb(a, i);
  return (k < a->n && a->idx[k] == i) ? a->p[k] : NULL;
}
int usbmem_xa_insert(struct xarray *xa, unsigned long i, void *entry, gfp_t gfp) {
  (void)gfp;
  struct uxa *a = xa->impl_;
  if (!a) { a = calloc(1, sizeof *a); if (!a) return -ENOMEM; xa->impl_ = a; }
  size_t k = uxa_lb(a, i);
  if (k < a->n && a->idx[k] == i) return -EBUSY;
  if (a->n == a->cap) {
    size_t cap = a->cap ? a->cap * 2 : 64;
    unsigned long *ni = realloc(a->idx, cap * sizeof *ni); if (!ni) return -ENOMEM; a->idx = ni;
    void **np = realloc(a->p, cap * sizeof *np); if (!np) return -ENOMEM; a->p = np;
    a->cap = cap;
  }
  memmove(a->idx + k + 1, a->idx + k, (a->n - k) * sizeof *a->idx);
  memmove(a->p + k + 1, a->p + k, (a->n - k) * sizeof *a->p);
  a->idx[k] = i; a->p[k] = entry; a->n++;
  return 0;
}
void *usbmem_xa_erase(struct xarray *xa, unsigned long i) {
  struct uxa *a = xa->impl_; if (!a) return NULL;
  size_t k = uxa_lb(a, i);
  if (k >= a->n || a->idx[k] != i) return NULL;
  void *old = a->p[k];
  memmove(a->idx + k, a->idx + k + 1, (a->n - k - 1) * sizeof *a->idx);
  memmove(a->p + k, a->p + k + 1, (a->n - k - 1) * sizeof *a->p);
  a->n--;
  return old;
}
void *usbmem_xa_find(struct xarray *xa, unsigned long *i, unsigned long max) {
  struct uxa *a = xa->impl_; if (!a) return NULL;
  size_t k = uxa_lb(a, *i);
  if (k >= a->n || a->idx[k] > max) return NULL;
  *i = a->idx[k]; return a->p[k];
}
void *usbmem_xa_find_after(struct xarray *xa, unsigned long *i, unsigned long max) {
  if (*i == ~0UL) return NULL;
  unsigned long j = *i + 1;
  void *p = usbmem_xa_find(xa, &j, max);
  if (p) *i = j;
  return p;
}
