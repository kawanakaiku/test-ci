# Linux fs/exfat — 取り込み元

- リポジトリ: https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git
- コミット: 067c5c24cef23d913b67eaaf71ae67e7169a6620 (NTFS Resurrection と同じ)
- 取り込み範囲: `fs/exfat/*.c *.h` → `fs-exfat/`、`include/uapi/linux/exfat.h` → `include/uapi/linux/exfat.h` (Kconfig / Makefile は除く)
- ライセンス: GPL-2.0-or-later (各ファイルの SPDX を参照。COPYING は GPL-2.0 全文)
- **ファイルは無改変**。差分は `core/compat/` 側で吸収する。
