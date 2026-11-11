# Linux fs/ext4 — 取り込み元

- リポジトリ: https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git
- コミット: 067c5c24cef23d913b67eaaf71ae67e7169a6620 (NTFS Resurrection / exFAT / ext2 と同じ)
- 取り込み範囲:
  - `fs/ext4/*.c *.h` → `fs-ext4/` (Kconfig / Makefile は除く。`*-test.c` (KUnit) は取り込むがビルドしない)
  - `include/trace/events/ext4.h` → `include/trace/events/ext4.h` (tracepoint 定義。互換層の TRACE_EVENT マクロで空になる)
  - `include/uapi/linux/ext4.h` → `include/uapi/linux/ext4.h`
  - `include/linux/fscrypt.h`、`include/uapi/linux/fscrypt.h` → `include/linux/`、`include/uapi/linux/` (CONFIG_FS_ENCRYPTION なし = 上流の「無効時」インライン関数で使う)
- ライセンス: GPL-2.0 / GPL-2.0-or-later (各ファイルの SPDX を参照。COPYING は GPL-2.0 全文)
- **ファイルは無改変**。差分は `core/compat/` 側で吸収する。
- **ビルドするのは読み取り経路のファイルだけ**: balloc bitmap block_validity dir ext4_jbd2 extents extents_status file fsync hash ialloc indirect inline inode namei readpage super symlink xattr xattr_hurd xattr_trusted xattr_user。
  jbd2 本体 (`fs/jbd2`)、mballoc の割り当て、orphan、fast_commit、ioctl、sysfs、page-io、mmp、resize、migrate、move_extent、fsmap、crypto、verity、acl、xattr_security は
  ビルドせず、`core/compat/src/rt_ext4.c` の最小スタブで置き換える (詳しくは `docs/ext4-port.md`)。
- `include/linux/jbd2.h` は上流を取り込まず、`core/compat/include/linux/jbd2.h` に最小限の手書き版を置く (上流の jbd2.h は journal_head / ktime / 各種ロックなど多数の内部型に依存し、journal を使わない構成では不要なため)。
