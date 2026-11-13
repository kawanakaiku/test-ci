// SPDX-License-Identifier: GPL-2.0-or-later
// buffer_head 互換と、fs/exfat 移植で必要になった小さな補助 (時刻変換・UTF-8・iunique など)。
// buffer_head は block_device のページキャッシュ (bd_mapping) の folio への参照。読み取り専用段階ではメタデータの読み出しだけに使う。
#include <compat/kernel.h>
#include <compat/usbmem_ntfs.h>
#include <linux/buffer_head.h>

static struct buffer_head *bh_get(struct block_device *bdev, sector_t block, unsigned size) {
  if (!bdev || !bdev->bd_mapping || !size || size > PAGE_SIZE || (size & (size - 1))) return NULL;
  u64 off = (u64)block * size;
  if (off / size != (u64)block || off + size > usbmem_bdev_size(bdev->impl)) return NULL;   /* デバイス範囲外は NULL (カーネルと同じ) */
  struct folio *f = read_mapping_folio(bdev->bd_mapping, (pgoff_t)(off >> PAGE_SHIFT), NULL);
  if (IS_ERR(f)) return NULL;
  struct buffer_head *bh = calloc(1, sizeof *bh);
  if (!bh) { folio_put(f); return NULL; }
  bh->b_blocknr = block; bh->b_size = size; bh->b_folio = f; bh->b_bdev = bdev;
  bh->b_data = (char *)f->data + (size_t)(off & (PAGE_SIZE - 1));
  bh->b_state = 1UL << BH_Uptodate;
  return bh;
}
struct buffer_head *__bread(struct block_device *bdev, sector_t block, unsigned size) { return bh_get(bdev, block, size); }
struct buffer_head *__getblk(struct block_device *bdev, sector_t block, unsigned size) { return bh_get(bdev, block, size); }
void __brelse(struct buffer_head *bh) { if (!bh) return; if (bh->b_count.counter > 0) { bh->b_count.counter--; return; } folio_put(bh->b_folio); free(bh); }
void brelse(struct buffer_head *bh) { __brelse(bh); }
/* bforget: 変更を捨てて手放す。この範囲の dirty を外すので、後でそのブロックが別の用途 (ファイルデータなど) に再利用されても、
 * 古いメタデータが writeback で上書きすることはない (解放された間接ブロックなど)。 */
void bforget(struct buffer_head *bh) {
  if (!bh) return;
  bh->b_state &= ~(1UL << BH_Dirty);
  usbmem_folio_clean_range(bh->b_folio, (size_t)(bh->b_data - (char *)bh->b_folio->data), bh->b_size);
  __brelse(bh);
}

/* 書き込み経路。bh はデバイスのページキャッシュ (bd_mapping) の folio 内の範囲への参照なので、同じブロックの bh は同じメモリを指す。
 * mark_buffer_dirty は folio の該当範囲 (512B 単位) を dirty にするだけで、書き戻しは usbmem_bdev_writeback (sync 時) か sync_dirty_buffer (即時)。 */
void mark_buffer_dirty(struct buffer_head *bh) {
  bh->b_state |= 1UL << BH_Dirty;
  usbmem_folio_dirty_range(bh->b_folio, (size_t)(bh->b_data - (char *)bh->b_folio->data), bh->b_size);
  if (bh->b_bdev && bh->b_bdev->usbmem_dirty_hook_) bh->b_bdev->usbmem_dirty_hook_(bh->b_bdev->usbmem_dirty_ctx_, (u64)bh->b_blocknr * bh->b_size, bh->b_size);
}
void mark_buffer_dirty_inode(struct buffer_head *bh, struct inode *inode) { (void)inode; mark_buffer_dirty(bh); }
/* bh を今すぐデバイスへ書く。USB のキャッシュが他の書き込みを先に反映することがあるので、書いたら必ず flush する
 * (ext2 のスーパーブロックの更新 = ダーティ印の確定・クリーン印の書き戻しがこれを使う)。 */
int __sync_dirty_buffer(struct buffer_head *bh, blk_opf_t op) {
  (void)op;
  if (!buffer_dirty(bh)) return 0;
  int rc = usbmem_bdev_write(bh->b_bdev->impl, (u64)bh->b_blocknr * bh->b_size, bh->b_data, bh->b_size);
  if (rc) { bh->b_state |= 1UL << BH_Write_EIO; return rc; }
  bh->b_state &= ~(1UL << BH_Dirty);
  usbmem_folio_clean_range(bh->b_folio, (size_t)(bh->b_data - (char *)bh->b_folio->data), bh->b_size);
  return usbmem_bdev_flush(bh->b_bdev->impl);
}
int sync_dirty_buffer(struct buffer_head *bh) { return __sync_dirty_buffer(bh, REQ_SYNC); }

/* ---- 補助 */
void *memchr_inv(const void *s, int c, size_t n) {
  const unsigned char *p = s;
  for (size_t i = 0; i < n; i++) if (p[i] != (unsigned char)c) return (void *)(p + i);
  return NULL;
}
u32 get_random_u32(void) { static u32 x = 2463534242u; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; }
unsigned int iunique(struct super_block *sb, unsigned int max_reserved) {
  static unsigned int counter;
  (void)sb;
  /* 2 は FAT の FSInfo 用の予約番号 (MSDOS_FSINFO_INO)。fat_write_inode は ino == 2 を FSInfo の書き出しとみなすので、普通の inode に割り当てない
   * (カーネルは起動後のカウンタが大きいので当たらないが、ここでは最初の割り当てが 2 になりうる) */
  do { counter++; } while (counter <= max_reserved || counter == 2);
  return counter;
}
struct usbmem_timezone sys_tz;
int iomap_bmap(struct address_space *m, sector_t block, const void *ops) { (void)m; (void)block; (void)ops; usbmem_unimplemented("iomap_bmap"); return 0; }

/* UTF-8 を 1 文字 (UTF-32) に復号する。戻り値: 消費したバイト数 / 不正は -1。カーネルの nls_base.c と同じ範囲 (過長符号・サロゲートは不正)。 */
int utf8_to_utf32(const u8 *s, int len, unicode_t *pu) {
  if (len <= 0) return -1;
  u8 c = s[0];
  if (c < 0x80) { *pu = c; return 1; }
  int n; u32 cp, min;
  if ((c & 0xE0) == 0xC0) { n = 2; cp = c & 0x1F; min = 0x80; }
  else if ((c & 0xF0) == 0xE0) { n = 3; cp = c & 0x0F; min = 0x800; }
  else if ((c & 0xF8) == 0xF0) { n = 4; cp = c & 0x07; min = 0x10000; }
  else return -1;
  if (len < n) return -1;
  for (int i = 1; i < n; i++) { if ((s[i] & 0xC0) != 0x80) return -1; cp = (cp << 6) | (s[i] & 0x3F); }
  if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return -1;
  *pu = cp; return n;
}

/* ---- 時刻 (カーネルの time64_to_tm / mktime64 と同じ意味。1970 年より前も扱う) */
static s64 floor_div(s64 a, s64 b) { s64 q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }
void time64_to_tm(s64 totalsecs, int offset, struct tm *r) {
  s64 t = totalsecs + offset;
  s64 days = floor_div(t, 86400), rem = t - days * 86400;
  r->tm_hour = (int)(rem / 3600); r->tm_min = (int)(rem % 3600 / 60); r->tm_sec = (int)(rem % 60);
  r->tm_wday = (int)(((days + 4) % 7 + 7) % 7);   /* 1970-01-01 は木曜 */
  /* days → 民間暦 (Howard Hinnant の civil_from_days) */
  s64 z = days + 719468;
  s64 era = floor_div(z, 146097);
  s64 doe = z - era * 146097;
  s64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  s64 y = yoe + era * 400;
  s64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  s64 mp = (5 * doy + 2) / 153;
  int d = (int)(doy - (153 * mp + 2) / 5 + 1);
  int m = (int)(mp < 10 ? mp + 3 : mp - 9);
  if (m <= 2) y++;
  r->tm_year = (int)(y - 1900); r->tm_mon = m - 1; r->tm_mday = d;
  static const int cum[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
  int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
  r->tm_yday = cum[m - 1] + d - 1 + ((m > 2 && leap) ? 1 : 0);
}
s64 mktime64(unsigned int year, unsigned int mon, unsigned int day, unsigned int hour, unsigned int min, unsigned int sec) {
  int mm = (int)mon - 2;
  if (mm <= 0) { mm += 12; year -= 1; }
  return ((((s64)(year / 4 - year / 100 + year / 400 + 367 * mm / 12 + day) + (s64)year * 365 - 719499) * 24 + hour) * 60 + min) * 60 + sec;
}
