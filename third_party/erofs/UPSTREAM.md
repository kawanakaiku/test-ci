# Linux fs/erofs — 取り込み元

- リポジトリ: https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git
- コミット: 067c5c24cef23d913b67eaaf71ae67e7169a6620 (NTFS Resurrection / exFAT / ext2 / ext4 / squashfs と同じ)
- 取り込み範囲: `fs/erofs/*.c *.h` → `fs-erofs/` (Kconfig / Makefile は除く)
- ライセンス: GPL-2.0-only / GPL-2.0-or-later (各ファイルの SPDX を参照。COPYING は GPL-2.0 全文)
- **ファイルは無改変**。差分は `core/compat/` 側 (more_erofs.h / rt_erofs.c など) で吸収する。
- ビルドするもの: `data dir inode namei super` と、圧縮 (`CONFIG_EROFS_FS_ZIP`) の `zutil zmap zdata decompressor decompressor_lzma decompressor_deflate decompressor_zstd` (lz4 / lz4hc / lzma / deflate / zstd)。
  ビルドしない: `xattr` `sysfs` `fileio` (ファイルバック) `ishare` (page cache share) `decompressor_crypto` (取り込みはしてある)。詳細は docs/erofs-port.md。
