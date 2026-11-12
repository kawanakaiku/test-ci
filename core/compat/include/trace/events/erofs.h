/* SPDX-License-Identifier: GPL-2.0 */
/* erofs のトレースポイント (LinuxCompat ではトレースしない: すべて何もしない)。引数は評価されない。 */
#ifndef USBMEM_TRACE_EVENTS_EROFS_H
#define USBMEM_TRACE_EVENTS_EROFS_H
#define trace_erofs_lookup(...)            do { } while (0)
#define trace_erofs_fill_inode(...)        do { } while (0)
#define trace_erofs_read_folio(...)        do { } while (0)
#define trace_erofs_readahead(...)         do { } while (0)
#define trace_erofs_map_blocks_enter(...)  do { } while (0)
#define trace_erofs_map_blocks_exit(...)   do { } while (0)
#endif
