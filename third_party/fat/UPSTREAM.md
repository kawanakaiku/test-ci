# Linux fs/fat — 取り込み元

- リポジトリ: https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git
- コミット: 067c5c24cef23d913b67eaaf71ae67e7169a6620 (NTFS Resurrection / exFAT / ext2 と同じ)
- 取り込み範囲: `fs/fat/*.c *.h` → `fs-fat/` (`fat_test.c` = KUnit は取り込まない。Kconfig / Makefile は参考のため置いてある)、
  `include/linux/msdos_fs.h` → `include/linux/msdos_fs.h`、`include/uapi/linux/msdos_fs.h` → `include/uapi/linux/msdos_fs.h`
- ビルドするのは vfat 構成: `cache dir fatent file inode misc namei_vfat` (`namei_msdos.c` = 8.3 名だけの旧 msdos ドライバは入れない)。
  `nfs.c` は NFS エクスポート用で、`core/compat/src/rt_vfs_fat.c` に最小の `fat_export_ops` を置いて代える (読み取り専用のマウントでは使われない)。
- NLS (文字コード表): 短い名前 (8.3) の文字コード変換用に、上流の `fs/nls/nls_cp437.c` / `nls_cp932.c` を `nls/` にそのまま置いてビルドする (表も含めて無改変)。
  互換層は `register_nls` / `load_nls` の登録簿 (`core/compat/src/rt_pagecache.c`) を持ち、各表の `module_init` を `rt_vfs_fat.c` の `usbmem_module_init_fat` が呼んで登録する。
  既定の iocharset は "utf8" (長い名前は UTF-16 → UTF-8)、短い名前の codepage は 437 (cp932 は表のみ登録済みで、マウントオプションはまだ受け付けない)。
- ライセンス: GPL-2.0 / GPL-2.0-or-later (各ファイルの SPDX を参照。COPYING は GPL-2.0 全文)
- **ファイルは無改変**。差分は `core/compat/` 側で吸収する。
