# カーネルの展開ライブラリ (lib/) — 取り込み元

- リポジトリ: https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git
- コミット: 067c5c24cef23d913b67eaaf71ae67e7169a6620
- 取り込み範囲 (すべて **無改変**): `lib/lz4/{lz4_decompress.c,lz4defs.h}`、`lib/lzo/{lzo1x_decompress_safe.c,lzodefs.h}`、`lib/xz/*` (展開側のみ)、`lib/zlib_inflate/*`、
  `lib/zstd/{common,decompress}/*` と `zstd_{common,decompress}_module.c`、`lib/xxhash.c`、`include/linux/{lz4,lzo,xz,zlib,zutil,zconf,zstd,zstd_lib,zstd_errors,xxhash}.h`。
- 例外: `include/linux/crc32.h` だけは**本プロジェクトの自前の小さなシム** (上流のものではない。xz が `xz_crc32` 用に include するだけ)。
- ライセンス: ファイルごとに異なる (GPL-2.0 / BSD 系 / 0BSD など)。各ファイルの SPDX と冒頭コメントを参照。COPYING は GPL-2.0 全文。
- 使うのは squashfs (zlib / lz4 / lzo / xz / zstd) と、erofs の圧縮対応 (lz4。未実装: docs/erofs-port.md)。ビルドは `core/compat/zfs.cmake` の `usbmem_kdecomp`。
