// SPDX-License-Identifier: GPL-2.0-or-later
// LinuxCompat 実行時 (1/3): メモリ・同期・inode キャッシュ・dentry・マウント。
// Linux の fs/inode.c, fs/dcache.c の意味論を、読み取り専用段階に必要な範囲で再現する。
#include <compat/kernel.h>
#include <compat/usbmem_ntfs.h>
#include <sched.h>
#include <time.h>

struct task_struct *current;

/* ---- 未実装機能の記録 (スタブから呼ばれる) */
void usbmem_unimplemented(const char *fn) {
  static const char *seen[256]; static int n;
  for (int i = 0; i < n; i++) if (seen[i] == fn) return;
  if (n < 256) seen[n++] = fn;
  fprintf(stderr, "usbmem-compat: unimplemented %s called\n", fn);
}

/* ---- 同期 */
void complete(struct completion *c) {
  pthread_mutex_lock(&c->m); c->done = 1; pthread_cond_broadcast(&c->c); pthread_mutex_unlock(&c->m);
}
void wait_for_completion(struct completion *c) {
  pthread_mutex_lock(&c->m); while (!c->done) pthread_cond_wait(&c->c, &c->m); pthread_mutex_unlock(&c->m);
}

/* ---- ビット操作 */
unsigned long find_next_bit(const unsigned long *a, unsigned long size, unsigned long off) {
  for (; off < size; off++) if (test_bit(off, a)) return off;
  return size;
}
unsigned long find_next_zero_bit(const unsigned long *a, unsigned long size, unsigned long off) {
  for (; off < size; off++) if (!test_bit(off, a)) return off;
  return size;
}
unsigned long bitmap_weight(const unsigned long *a, unsigned int bits) {
  unsigned long n = 0; for (unsigned i = 0; i < bits; i++) n += test_bit(i, a); return n;
}

/* ---- rbtree: 親ポインタ付きの二分探索木で代用 (色なし・非平衡)。
 * 呼び出し側 (fs/ntfs dir.c) は rb_link_node で自前降下 → rb_insert_color という Linux の作法で使う。
 * 機能は同一。挿入が単調だと偏るが、対象は index 先読みキャッシュ (小規模)。 */
#define RB_PARENT(n) ((struct rb_node *)(n)->__rb_parent_color)
void rb_insert_color(struct rb_node *n, struct rb_root *r) { (void)n; (void)r; }
struct rb_node *rb_first(const struct rb_root *r) {
  struct rb_node *n = r->rb_node; if (!n) return NULL;
  while (n->rb_left) n = n->rb_left;
  return n;
}
static void rb_transplant(struct rb_root *r, struct rb_node *u, struct rb_node *v) {
  struct rb_node *p = RB_PARENT(u);
  if (!p) r->rb_node = v; else if (p->rb_left == u) p->rb_left = v; else p->rb_right = v;
  if (v) v->__rb_parent_color = (unsigned long)p;
}
void rb_erase(struct rb_node *n, struct rb_root *r) {
  if (!n->rb_left) rb_transplant(r, n, n->rb_right);
  else if (!n->rb_right) rb_transplant(r, n, n->rb_left);
  else {
    struct rb_node *s = n->rb_right;
    while (s->rb_left) s = s->rb_left;
    if (s != n->rb_right) {
      rb_transplant(r, s, s->rb_right);
      s->rb_right = n->rb_right; s->rb_right->__rb_parent_color = (unsigned long)s;
    }
    rb_transplant(r, n, s);
    s->rb_left = n->rb_left; s->rb_left->__rb_parent_color = (unsigned long)s;
  }
}

/* ---- メモリ */
struct kmem_cache { size_t size; void (*ctor)(void *); };
struct kmem_cache *kmem_cache_create(const char *name, size_t size, size_t align, unsigned long flags, void (*ctor)(void *)) {
  (void)name; (void)align; (void)flags;
  struct kmem_cache *c = malloc(sizeof *c); if (!c) return NULL;
  c->size = size; c->ctor = ctor; return c;
}
void *kmem_cache_alloc(struct kmem_cache *c, gfp_t g) {
  (void)g; void *p = calloc(1, c->size); if (p && c->ctor) c->ctor(p); return p;
}
void kmem_cache_free(struct kmem_cache *c, void *p) { (void)c; free(p); }
void kmem_cache_destroy(struct kmem_cache *c) { free(c); }
char *kstrdup(const char *s, gfp_t g) { (void)g; return s ? strdup(s) : NULL; }
void *kmemdup(const void *p, size_t n, gfp_t g) { (void)g; void *q = malloc(n); if (q) memcpy(q, p, n); return q; }
size_t strscpy(char *dst, const char *src, size_t n) {
  size_t l = strlen(src); if (!n) return 0;
  size_t c = l < n ? l : n - 1; memcpy(dst, src, c); dst[c] = 0; return l < n ? l : (size_t)-E2BIG;
}
char *strreplace(char *s, char from, char to) { for (char *p = s; *p; p++) if (*p == from) *p = to; return s; }
bool capable(int cap) { (void)cap; return false; }
int errseq_check(errseq_t *e, errseq_t since) { return *e != since ? -EIO : 0; }

/* ---- 時刻 */
void usbmem_now(struct timespec64 *ts) {
  struct timespec t; clock_gettime(CLOCK_REALTIME, &t); ts->tv_sec = t.tv_sec; ts->tv_nsec = t.tv_nsec;
}
struct timespec64 current_time(struct inode *i) { (void)i; struct timespec64 t; usbmem_now(&t); return t; }
struct timespec64 inode_set_ctime_current(struct inode *i) { struct timespec64 t; usbmem_now(&t); return i->i_ctime_ = t; }
kuid_t make_kuid(struct user_namespace *ns, uid_t id) { (void)ns; kuid_t k = { id }; return k; }
kgid_t make_kgid(struct user_namespace *ns, gid_t id) { (void)ns; kgid_t k = { id }; return k; }
__kernel_fsid_t u64_to_fsid(u64 v) { __kernel_fsid_t f = { { (u32)v, (u32)(v >> 32) } }; return f; }

/* ---- inode キャッシュ */
static pthread_mutex_t g_inode_lock = PTHREAD_MUTEX_INITIALIZER;
static struct inode *g_inode_hash[1024];
#define IH(h) (&g_inode_hash[(h) & 1023])

static void inode_init_always(struct super_block *sb, struct inode *inode) {
  inode->i_sb = sb;
  inode->i_mapping = &inode->i_data;
  inode->i_data.host = inode;
  inode->i_blkbits = sb->s_blocksize_bits ? sb->s_blocksize_bits : 9;
  atomic_set(&inode->i_count, 1);
  inode->i_nlink = 1;
  spin_lock_init(&inode->i_lock);
  pthread_rwlock_init(&inode->i_rwsem.l, NULL);
  pthread_rwlock_init(&inode->i_data.invalidate_lock.l, NULL);
  INIT_LIST_HEAD(&inode->i_data.private_list);
}
void inode_init_once(struct inode *inode) { memset(inode, 0, sizeof *inode); }

void *alloc_inode_sb(struct super_block *sb, struct kmem_cache *c, gfp_t g) { (void)sb; return kmem_cache_alloc(c, g); }

static struct inode *alloc_inode(struct super_block *sb) {
  struct inode *i;
  if (sb->s_op && sb->s_op->alloc_inode) i = ((struct inode *(*)(struct super_block *))sb->s_op->alloc_inode)(sb);
  else i = calloc(1, sizeof *i);
  if (!i) return NULL;
  inode_init_always(sb, i);
  return i;
}
static void destroy_inode(struct inode *i) {
  struct super_block *sb = i->i_sb;
  if (sb->s_op && sb->s_op->free_inode) ((void (*)(struct inode *))sb->s_op->free_inode)(i);
  else free(i);
}
static void hash_insert_locked(struct inode *i, unsigned long hv) {
  i->i_hashval_ = hv; i->i_hash_next_ = *IH(hv); *IH(hv) = i; i->i_hashed_ = true;
}
static void hash_remove_locked(struct inode *i) {
  if (!i->i_hashed_) return;
  for (struct inode **p = IH(i->i_hashval_); *p; p = &(*p)->i_hash_next_)
    if (*p == i) { *p = i->i_hash_next_; break; }
  i->i_hashed_ = false;
}
struct inode *new_inode(struct super_block *sb) { return alloc_inode(sb); }
void insert_inode_hash(struct inode *i) { pthread_mutex_lock(&g_inode_lock); hash_insert_locked(i, i->i_ino); pthread_mutex_unlock(&g_inode_lock); }
void remove_inode_hash(struct inode *i) { pthread_mutex_lock(&g_inode_lock); hash_remove_locked(i); pthread_mutex_unlock(&g_inode_lock); }
int inode_unhashed(struct inode *i) { return !i->i_hashed_; }

struct inode *iget5_locked(struct super_block *sb, unsigned long hv, int (*test)(struct inode *, void *),
                           int (*set)(struct inode *, void *), void *data) {
  pthread_mutex_lock(&g_inode_lock);
  for (struct inode *i = *IH(hv); i; i = i->i_hash_next_)
    if (i->i_sb == sb && i->i_hashval_ == hv && !(i->i_state & (I_FREEING | I_WILL_FREE)) && test(i, data)) {
      atomic_inc(&i->i_count);
      pthread_mutex_unlock(&g_inode_lock);
      return i;  /* I_NEW が立っている間の並行取得は単一スレッド前提のため待たない */
    }
  pthread_mutex_unlock(&g_inode_lock);
  struct inode *n = alloc_inode(sb);
  if (!n) return NULL;
  if (set(n, data)) { destroy_inode(n); return NULL; }
  n->i_state = I_NEW;
  pthread_mutex_lock(&g_inode_lock);
  hash_insert_locked(n, hv);
  pthread_mutex_unlock(&g_inode_lock);
  return n;
}
struct inode *ilookup5(struct super_block *sb, unsigned long hv, int (*test)(struct inode *, void *), void *data) {
  pthread_mutex_lock(&g_inode_lock);
  for (struct inode *i = *IH(hv); i; i = i->i_hash_next_)
    if (i->i_sb == sb && i->i_hashval_ == hv && !(i->i_state & (I_FREEING | I_WILL_FREE)) && test(i, data)) {
      atomic_inc(&i->i_count); pthread_mutex_unlock(&g_inode_lock); return i;
    }
  pthread_mutex_unlock(&g_inode_lock);
  return NULL;
}
struct inode *find_inode_nowait(struct super_block *sb, unsigned long hv, int (*match)(struct inode *, u64, void *), void *data) {
  struct inode *ret = NULL;
  pthread_mutex_lock(&g_inode_lock);
  for (struct inode *i = *IH(hv); i; i = i->i_hash_next_) {
    if (i->i_sb != sb || i->i_hashval_ != hv) continue;
    int m = match(i, hv, data);
    if (m == 1) { ret = i; break; } else if (m < 0) break;  /* Linux: 1=一致, -1=打ち切り */
  }
  pthread_mutex_unlock(&g_inode_lock);
  return ret;
}
struct inode *find_inode_rcu(struct super_block *sb, unsigned long hv, int (*test)(struct inode *, void *), void *data) {
  for (struct inode *i = *IH(hv); i; i = i->i_hash_next_)
    if (i->i_sb == sb && i->i_hashval_ == hv && test(i, data)) return i;
  return NULL;
}
void unlock_new_inode(struct inode *i) { i->i_state &= ~I_NEW; }
void discard_new_inode(struct inode *i) { i->i_state &= ~I_NEW; iput(i); }
struct inode *igrab(struct inode *i) {
  if (i->i_state & (I_FREEING | I_WILL_FREE)) return NULL;
  atomic_inc(&i->i_count); return i;
}
void ihold(struct inode *i) { atomic_inc(&i->i_count); }
void clear_inode(struct inode *i) { i->i_state = I_FREEING | 0x100 /* I_CLEAR */; }
int inode_generic_drop(struct inode *i) { return !i->i_nlink || !i->i_hashed_; }

static void usbmem_d_free_root(struct dentry *d);
static void evict(struct inode *i) {
  struct super_block *sb = i->i_sb;
  i->i_state |= I_FREEING;
  pthread_mutex_lock(&g_inode_lock); hash_remove_locked(i); pthread_mutex_unlock(&g_inode_lock);
  if (sb->s_op && sb->s_op->evict_inode) ((void (*)(struct inode *))sb->s_op->evict_inode)(i);
  else { truncate_inode_pages_final(&i->i_data); clear_inode(i); }
  destroy_inode(i);
}
void iput(struct inode *i) {
  if (!i) return;
  if (atomic_dec_return(&i->i_count) > 0) return;
  struct super_block *sb = i->i_sb;
  int drop = (sb->s_op && sb->s_op->drop_inode) ? ((int (*)(struct inode *))sb->s_op->drop_inode)(i) : inode_generic_drop(i);
  if (drop) evict(i);  /* 非 drop はキャッシュに残す (count 0 のままハッシュに残留) */
}
/* sb に属し参照 0 のハッシュ済み inode を全て退避。evict が他 inode を iput しうるので不動点まで繰り返す。 */
static int evict_inodes_sb(struct super_block *sb) {
  int total = 0, n;
  do {
    n = 0;
    for (int b = 0; b < 1024 && !n; b++) {
      pthread_mutex_lock(&g_inode_lock);
      struct inode *i = g_inode_hash[b];
      while (i && !(i->i_sb == sb && atomic_read(&i->i_count) == 0)) i = i->i_hash_next_;
      pthread_mutex_unlock(&g_inode_lock);
      if (i) { evict(i); n = 1; }
    }
    total += n;
  } while (n);
  return total;
}
/* マウント解除: VFS の generic_shutdown_super と同順 (evict_inodes → put_super → 残り退避 → 解放)。 */
void usbmem_compat_umount(struct super_block *sb) {
  if (!sb) return;
  evict_inodes_sb(sb);
  if (sb->s_root) { usbmem_d_free_root(sb->s_root); sb->s_root = NULL; }
  if (sb->s_op && sb->s_op->put_super) ((void (*)(struct super_block *))sb->s_op->put_super)(sb);
  evict_inodes_sb(sb);
  free(sb);
}
void mark_inode_dirty(struct inode *i) { (void)i; }
void mark_inode_dirty_sync(struct inode *i) { (void)i; }
void __mark_inode_dirty(struct inode *i, int f) { (void)i; (void)f; }
int write_inode_now(struct inode *i, int s) { (void)i; (void)s; return 0; }
void inode_attach_wb(struct inode *i, struct folio *f) { (void)i; (void)f; }
void inode_dio_wait(struct inode *i) { (void)i; }
void init_special_inode(struct inode *i, umode_t m, dev_t d) { (void)d; i->i_mode = m; }
void inode_init_owner(struct mnt_idmap *m, struct inode *i, const struct inode *dir, umode_t mode) {
  (void)m; (void)dir; i->i_mode = mode;
}
int always_delete_dentry(const struct dentry *d) { (void)d; return 1; }

/* ---- dentry (最小: 名前・親・inode のみ。dcache は持たない) */
static struct dentry *dentry_alloc(struct super_block *sb, struct dentry *parent, const struct qstr *name) {
  struct dentry *d = calloc(1, sizeof *d);
  if (!d) return NULL;
  d->d_sb = sb; d->d_parent = parent ? parent : d;
  if (name) {
    unsigned char *n = malloc(name->len + 1);
    memcpy(n, name->name, name->len); n[name->len] = 0;
    d->d_name.name = n; d->d_name.len = name->len; d->d_name.hash = name->hash;
  }
  return d;
}
struct dentry *usbmem_d_alloc(struct super_block *sb, struct dentry *parent, const char *name, unsigned len) {
  struct qstr q = { (const unsigned char *)name, len, 0 };
  return dentry_alloc(sb, parent, &q);
}
static void usbmem_d_free_root(struct dentry *d) {
  if (d->d_inode) iput(d->d_inode);
  free((void *)d->d_name.name); free(d);
}
void usbmem_d_free(struct dentry *d) {
  if (!d) return;
  if (d->d_inode) iput(d->d_inode);  /* lookup が dentry に渡した inode 参照を返す */
  free((void *)d->d_name.name); free(d);
}
struct dentry *d_make_root(struct inode *i) {
  if (!i) return NULL;
  struct qstr q = { (const unsigned char *)"/", 1, 0 };
  struct dentry *d = dentry_alloc(i->i_sb, NULL, &q);
  if (d) d->d_inode = i;
  return d;
}
struct dentry *d_obtain_alias(struct inode *i) {
  if (IS_ERR(i)) return (struct dentry *)i;
  struct qstr q = { (const unsigned char *)"", 0, 0 };
  struct dentry *d = dentry_alloc(i->i_sb, NULL, &q);
  if (d) d->d_inode = i;
  return d;
}
struct dentry *d_find_alias(struct inode *i) { (void)i; return NULL; }
struct dentry *d_splice_alias(struct inode *i, struct dentry *d) {
  if (IS_ERR(i)) return (struct dentry *)i;
  d->d_inode = i; return d;  /* 呼び出し側 (lookup) が返した dentry をそのまま使う */
}
struct dentry *d_add_ci(struct dentry *d, struct inode *i, struct qstr *name) {
  (void)name; d->d_inode = i; return d;
}
void d_instantiate(struct dentry *d, struct inode *i) { d->d_inode = i; }
void d_instantiate_new(struct dentry *d, struct inode *i) { d->d_inode = i; i->i_state &= ~I_NEW; }
void dput(struct dentry *d) { (void)d; /* dcache が無いので解放は呼び出し側(usbmem_ntfs)が管理 */ }
struct inode *d_backing_inode(const struct dentry *d) { return d->d_inode; }
char *dentry_path_raw(const struct dentry *d, char *buf, int len) { (void)d; (void)buf; (void)len; return ERR_PTR(-ENOSYS); }
unsigned int full_name_hash(const void *salt, const char *s, unsigned n) {
  (void)salt; unsigned h = 5381; for (unsigned i = 0; i < n; i++) h = h * 33 + (unsigned char)s[i]; return h;
}
int is_subdir(struct dentry *a, struct dentry *b) { (void)a; (void)b; return 0; }
void set_default_d_op(struct super_block *sb, const struct dentry_operations *op) { (void)sb; (void)op; }
bool dir_emit(struct dir_context *ctx, const char *name, int len, u64 ino, unsigned type) {
  return ctx->actor ? ctx->actor(ctx, name, len, ctx->pos, ino, type) : false;
}
bool dir_emit_dots(struct file *f, struct dir_context *ctx) {
  (void)f;
  if (ctx->pos == 0) { if (!dir_emit(ctx, ".", 1, 0, DT_DIR)) return false; ctx->pos++; }
  if (ctx->pos == 1) { if (!dir_emit(ctx, "..", 2, 0, DT_DIR)) return false; ctx->pos++; }
  return true;
}

/* ---- file / VFS 小物 */
size_t iov_iter_count(const struct iov_iter *i) { return i->count; }
int generic_file_open(struct inode *i, struct file *f) { (void)i; (void)f; return 0; }
loff_t vfs_setpos(struct file *f, loff_t off, loff_t max) { if (off > max) return -EINVAL; f->f_pos = off; return off; }
int inode_permission(struct mnt_idmap *m, struct inode *i, int mask) { (void)m; (void)i; (void)mask; return 0; }
bool inode_owner_or_capable(struct mnt_idmap *m, const struct inode *i) { (void)m; (void)i; return true; }
int inode_newsize_ok(const struct inode *i, loff_t s) { (void)i; (void)s; return 0; }
void generic_fillattr(struct mnt_idmap *m, u32 mask, struct inode *i, struct kstat *st) {
  (void)m; (void)mask; st->mode = i->i_mode; st->size = i->i_size; st->blocks = i->i_blocks; st->blksize = 1u << i->i_blkbits;
}
void seq_printf(struct seq_file *s, const char *f, ...) { (void)s; (void)f; }
void seq_puts(struct seq_file *s, const char *t) { (void)s; (void)t; }
void rcu_barrier(void) {}
void sb_start_intwrite(struct super_block *s) { (void)s; }
void sb_end_intwrite(struct super_block *s) { (void)s; }
void sb_start_pagefault(struct super_block *s) { (void)s; }
void sb_end_pagefault(struct super_block *s) { (void)s; }
void set_delayed_call(struct delayed_call *c, void (*fn)(void *), void *a) { c->fn = fn; c->arg = a; }
void kfree_link(void *p) { free(p); }
int posix_acl_chmod(struct mnt_idmap *m, struct dentry *d, umode_t mode) { (void)m; (void)d; (void)mode; return 0; }
void simple_inode_init_ts(struct inode *i) { struct timespec64 t; usbmem_now(&t); i->i_atime_ = i->i_mtime_ = i->i_ctime_ = t; }
struct workqueue_struct *alloc_workqueue(const char *n, unsigned f, int m, ...) { (void)n; (void)f; (void)m; return (struct workqueue_struct *)1; }
void destroy_workqueue(struct workqueue_struct *w) { (void)w; }
bool queue_work(struct workqueue_struct *w, struct work_struct *work) { (void)w; if (work->func) work->func(work); return true; } /* 同期実行 */
bool cancel_work_sync(struct work_struct *w) { (void)w; return false; }

/* ---- filesystem 登録 / マウント */
static struct file_system_type *g_fs_type;
int register_filesystem(void *t) { g_fs_type = t; return 0; }
int unregister_filesystem(void *t) { (void)t; g_fs_type = NULL; return 0; }
static struct block_device *g_pending_bdev;
int get_tree_bdev(struct fs_context *fc, int (*fill_super)(struct super_block *, struct fs_context *)) {
  struct super_block *sb = calloc(1, sizeof *sb);
  if (!sb) return -ENOMEM;
  sb->s_bdev = g_pending_bdev;
  sb->s_fs_info = fc->s_fs_info; fc->s_fs_info = NULL;
  sb->s_flags = fc->sb_flags | SB_ACTIVE;
  sb->s_blocksize = 512; sb->s_blocksize_bits = 9;
  sb->s_maxbytes = MAX_LFS_FILESIZE;
  strcpy(sb->s_id, "usbmem");
  sb->s_fc_ = fc;
  int rc = fill_super(sb, fc);
  if (rc) { fprintf(stderr, "usbmem-compat: fill_super failed: %d\n", rc); free(sb); return rc; }
  fc->root = sb->s_root;
  return 0;
}
void kill_block_super(struct super_block *sb) { (void)sb; }
int sb_set_blocksize(struct super_block *sb, int size) {
  if (size < 512 || (size & (size - 1))) return 0;
  sb->s_blocksize = size; sb->s_blocksize_bits = (unsigned char)__builtin_ctz(size); return size;
}
int sb_min_blocksize(struct super_block *sb, int size) { return sb_set_blocksize(sb, size < 512 ? 512 : size); }
int fs_parse(struct fs_context *fc, const struct fs_parameter_spec *s, struct fs_parameter *p, struct fs_parse_result *r) {
  (void)fc; (void)s; (void)p; (void)r; return -ENOPARAM;
}

int usbmem_compat_mount(struct block_device *bdev, int read_only, struct super_block **out) {
  if (!g_fs_type) { int (*init)(void) = usbmem_module_init_ntfs; if (!init || init()) return -ENODEV; }
  struct fs_context *fc = calloc(1, sizeof *fc);
  if (!fc) return -ENOMEM;
  int (*init_fc)(struct fs_context *) = (int (*)(struct fs_context *))g_fs_type->init_fs_context;
  int rc = init_fc(fc);
  if (rc) { free(fc); return rc; }
  fc->sb_flags = (read_only ? SB_RDONLY : 0) | SB_SILENT;
  g_pending_bdev = bdev;
  rc = ((const struct fs_context_operations *)fc->ops)->get_tree ? ((int (*)(struct fs_context *))((const struct fs_context_operations *)fc->ops)->get_tree)(fc) : -ENOSYS;
  g_pending_bdev = NULL;
  if (rc) { free(fc); return rc; }
  *out = fc->root->d_sb;
  (*out)->s_fc_ = NULL; free(fc);
  return 0;
}
