// SPDX-License-Identifier: GPL-2.0-or-later
// LinuxCompat 実行時 (書き込み): iomap のバッファ書き込み / ゼロ化、iov_iter (単一バッファ)、書き込み前後の VFS 補助。
// 前提: クラスタサイズ >= PAGE_SIZE (1 folio は必ず 1 クラスタ内に収まる)。それ未満のボリュームは書き込みマウントを拒否する (rt_vfs.c)。
#include <compat/kernel.h>
#include <compat/usbmem_ntfs.h>

size_t iov_iter_count(const struct iov_iter *i) { return i->count; }
size_t fault_in_iov_iter_readable(const struct iov_iter *i, size_t n) { (void)i; (void)n; return 0; }
size_t copy_folio_from_iter_atomic(struct folio *f, size_t off, size_t bytes, struct iov_iter *i) {
  if (bytes > i->count) bytes = i->count;
  if (off + bytes > PAGE_SIZE) bytes = PAGE_SIZE - off;
  memcpy((char *)f->data + off, i->buf, bytes);
  i->buf += bytes; i->count -= bytes;
  return bytes;
}
int generic_write_checks(struct kiocb *iocb, struct iov_iter *from) {
  struct inode *inode = iocb->ki_filp->f_mapping->host;
  if (iocb->ki_flags & IOCB_APPEND) iocb->ki_pos = i_size_read(inode);
  if (iocb->ki_pos < 0) return -EINVAL;
  return (int)iov_iter_count(from);
}
ssize_t generic_write_sync(struct kiocb *iocb, ssize_t count) { (void)iocb; return count; }   /* 永続化は usbmem_compat_sync で明示的に行う */
static int touch_times(struct inode *i) {
  struct timespec64 t = current_time(i);
  inode_set_mtime_to_ts(i, t); inode_set_ctime_to_ts(i, t);
  mark_inode_dirty_sync(i);
  return 0;
}
int kiocb_modified(struct kiocb *iocb) { return touch_times(iocb->ki_filp->f_mapping->host); }
int file_modified(struct file *f) { return touch_times(f->f_mapping->host); }
int file_update_time(struct file *f) { return touch_times(f->f_mapping->host); }
int file_remove_privs(struct file *f) { (void)f; return 0; }

/* ---- setattr: 実用はサイズ変更 (truncate) と mode。権限検査は無い (単一ユーザの端末内利用) */
int setattr_prepare(struct mnt_idmap *idmap, struct dentry *d, struct iattr *a) {
  (void)idmap; (void)d;
  if ((a->ia_valid & ATTR_SIZE) && a->ia_size < 0) return -EINVAL;
  return 0;
}
void setattr_copy(struct mnt_idmap *idmap, struct inode *i, const struct iattr *a) {
  (void)idmap;
  if (a->ia_valid & ATTR_MODE) i->i_mode = (i->i_mode & S_IFMT) | (a->ia_mode & ~S_IFMT);
  mark_inode_dirty(i);
}

/* ---- 1 folio 分の準備 (iomap_write_begin 相当) */
static int fill_folio(const struct iomap_iter *it, struct folio *f) {
  loff_t fpos = folio_pos(f), isize = it->inode->i_size;
  const struct iomap *im = &it->iomap;
  memset(f->data, 0, PAGE_SIZE);
  if (fpos >= isize) return 0;
  size_t n = (size_t)((isize - fpos) < PAGE_SIZE ? (isize - fpos) : PAGE_SIZE);
  if (im->type == IOMAP_INLINE) {
    if (fpos == 0) { size_t m = n < im->length ? n : (size_t)im->length; memcpy(f->data, im->inline_data, m); }
    return 0;
  }
  if (im->type != IOMAP_MAPPED || (im->flags & IOMAP_F_NEW) || im->addr == IOMAP_NULL_ADDR) return 0;   /* 穴 / 新規確保 = ゼロ */
  loff_t disk = (loff_t)im->addr + (fpos - (loff_t)im->offset);
  return usbmem_bdev_read(im->bdev->impl, (u64)disk, f->data, n);
}
static int get_write_folio(struct iomap_iter *it, loff_t pos, size_t bytes, const struct iomap_write_ops *ops, struct folio **out) {
  struct folio *f;
  if (ops && ops->get_folio) f = ops->get_folio(it, pos, (unsigned)bytes);
  else f = filemap_grab_folio(it->inode->i_mapping, (pgoff_t)(pos >> PAGE_SHIFT));
  if (IS_ERR(f)) return (int)PTR_ERR(f);
  if (!folio_test_uptodate(f)) {
    size_t off = offset_in_folio(f, pos);
    if (!(off == 0 && bytes == PAGE_SIZE)) {
      int rc = fill_folio(it, f);
      if (rc) { folio_unlock(f); folio_put(f); return rc; }
    }
  }
  *out = f; return 0;
}
static void put_write_folio(struct iomap_iter *it, loff_t pos, size_t copied, struct folio *f, const struct iomap_write_ops *ops) {
  if (ops && ops->put_folio) ops->put_folio(it->inode, pos, (unsigned)copied, f);
  else { folio_unlock(f); folio_put(f); }
}
/* 書いた後の状態: uptodate + dirty (INLINE は MFT レコード側へ書き戻す)。 */
static void finish_write(struct iomap_iter *it, struct folio *f, loff_t pos, size_t copied, size_t dirty_len) {
  folio_mark_uptodate(f);
  if (it->iomap.type == IOMAP_INLINE) {
    size_t foff = offset_in_folio(f, pos);
    memcpy((char *)it->iomap.inline_data + (pos - (loff_t)it->iomap.offset), (char *)f->data + foff, copied);
    mark_inode_dirty(it->inode);
  } else {
    /* 書いた範囲だけ dirty (実カーネルの iomap と同じ)。範囲外 (穴・未初期化) は書き戻さない */
    usbmem_folio_dirty_range(f, offset_in_folio(f, pos), dirty_len);
    if (it->inode && f->mapping && f->mapping->host) f->mapping->host->i_state |= I_DIRTY_PAGES;
  }
  if (pos + (loff_t)copied > it->inode->i_size) i_size_write(it->inode, pos + (loff_t)copied);
}

ssize_t iomap_file_buffered_write(struct kiocb *iocb, struct iov_iter *from, iomap_iter_next_fn next,
                                  const struct iomap_write_ops *ops, void *priv) {
  struct inode *inode = iocb->ki_filp->f_mapping->host;
  struct iomap_iter it = { .inode = inode, .pos = iocb->ki_pos, .len = iov_iter_count(from), .flags = IOMAP_WRITE, .private = priv };
  ssize_t total = 0; int status = 0;
  for (;;) {
    int rc = next(&it, &it.iomap, &it.srcmap);
    if (rc <= 0) { if (rc < 0 && !status) status = rc; break; }
    it.iter_start_pos = it.pos;
    while (it.len && !status) {
      u64 mlen = iomap_length(&it);
      size_t off = (size_t)(it.pos & (PAGE_SIZE - 1)), bytes = PAGE_SIZE - off;
      if (bytes > mlen) bytes = (size_t)mlen;
      if (bytes > it.len) bytes = (size_t)it.len;
      if (!bytes) { status = -EIO; break; }
      struct folio *f;
      int e = get_write_folio(&it, it.pos, bytes, ops, &f);
      if (e) { status = e; break; }
      size_t copied = copy_folio_from_iter_atomic(f, off, bytes, from);
      finish_write(&it, f, it.pos, copied, copied);
      put_write_folio(&it, it.pos, copied, f, ops);
      total += (ssize_t)copied;
      if (iomap_iter_advance(&it, copied)) { status = -EIO; break; }
      if (it.pos >= (loff_t)(it.iomap.offset + it.iomap.length)) break;
    }
    if (status) it.len = 0;   /* 次の next() で end コールバックだけを呼ばせて (ロック解放)、ループを終える */
  }
  iocb->ki_pos = it.pos;
  return total ? total : status;
}

int iomap_zero_range(struct inode *inode, loff_t pos, loff_t len, bool *did_zero, iomap_iter_next_fn next,
                     const struct iomap_write_ops *ops, void *priv) {
  struct iomap_iter it = { .inode = inode, .pos = pos, .len = (u64)len, .flags = IOMAP_ZERO, .private = priv };
  int status = 0;
  for (;;) {
    int rc = next(&it, &it.iomap, &it.srcmap);
    if (rc <= 0) { if (rc < 0 && !status) status = rc; break; }
    it.iter_start_pos = it.pos;
    while (it.len && !status) {
      u64 mlen = iomap_length(&it);
      const struct iomap *src = iomap_iter_srcmap(&it);
      size_t off = (size_t)(it.pos & (PAGE_SIZE - 1)), bytes = PAGE_SIZE - off;
      if (bytes > mlen) bytes = (size_t)mlen;
      if (bytes > it.len) bytes = (size_t)it.len;
      if (!bytes) { status = -EIO; break; }
      if (src->type == IOMAP_HOLE || src->type == IOMAP_UNWRITTEN) goto skip;   /* 既にゼロ */
      {
        struct folio *f; int e = get_write_folio(&it, it.pos, bytes, ops, &f);
        if (e) { status = e; break; }
        folio_zero_segment(f, off, off + bytes);
        finish_write(&it, f, it.pos, 0, bytes);
        put_write_folio(&it, it.pos, bytes, f, ops);
        if (did_zero) *did_zero = true;
      }
skip:
      if (iomap_iter_advance(&it, bytes)) { status = -EIO; break; }
      if (it.pos >= (loff_t)(it.iomap.offset + it.iomap.length)) break;
    }
    if (status) it.len = 0;
  }
  return status;
}
