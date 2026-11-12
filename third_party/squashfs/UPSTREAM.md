# Linux fs/squashfs — 取り込み元

- リポジトリ: https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git
- コミット: 067c5c24cef23d913b67eaaf71ae67e7169a6620 (他の FS と同じ)
- 取り込み範囲: `fs/squashfs/*.c *.h` → `fs-squashfs/` (Kconfig / Makefile は除く)
- ライセンス: GPL-2.0-only / GPL-2.0-or-later (各ファイルの SPDX。COPYING は GPL-2.0 全文)
- **ファイルは無改変**。差分は `core/compat/` 側 (more_zfs.h / rt_vfs_squashfs.c / rt_zfs.c など) で吸収する。
- 構成: FILE_DIRECT・DECOMP_SINGLE・フラグメントキャッシュ 3・xattr 無効 (ディスク上にあっても無視)・展開は `third_party/kernel-lib` (zlib / lz4 / lzo / xz / zstd 有効)。docs/squashfs-port.md
