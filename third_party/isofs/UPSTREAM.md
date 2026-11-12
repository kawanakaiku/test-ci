# Linux fs/isofs — 取り込み元

- リポジトリ: https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git (GitHub ミラー torvalds/linux から取得)
- コミット: 067c5c24cef23d913b67eaaf71ae67e7169a6620 (NTFS Resurrection / exFAT / ext2 / ext4 と同じ)
- 取り込み範囲: `fs/isofs/*.c *.h` → `fs-isofs/` (Kconfig / Makefile は除く)、`include/uapi/linux/iso_fs.h` → `include/uapi/linux/iso_fs.h`
- ライセンス: GPL-2.0 (各ファイルの SPDX を参照。COPYING は GPL-2.0 全文)
- **ファイルは無改変**。差分は `core/compat/` 側で吸収する。
