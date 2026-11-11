# Phase 0: NTFS Resurrection 移植分析

対象: Linux mainline `fs/ntfs`(コミット 067c5c24cef2, 2026-11-10 時点の torvalds/linux master)。
再現: `tools/analyze_ntfs_deps.sh <linux-src> > docs/ntfs-deps-report.txt`(生データは `ntfs-deps-report.txt`)。
以下は **この grep 集計とソース確認で確かめた事実** と、**未検証の推定** を分けて書く。

## 事実

- 57 ファイル / 44,073 行。ライセンスは `GPL-2.0-or-later`(super.c の SPDX。ファイル単位の確認は Phase 7 で全数チェック)。
- 旧 `fs/ntfs`(読み取り専用)ではなく、書き込み対応の新実装(Copyright 2025 LG Electronics)。
  buffer_head は 0 件。ページキャッシュは **folio + iomap** 前提。
- `module_init`/`register_filesystem`/`fs_context`/sysctl/proc は super.c・sysctl.c に集中(34/12 件程度)。→ 差し替え対象は少数ファイル。
- `inode.c`(4153 行)・`mft.c`(3437 行)・`file.c`・`namei.c`・`named_stream.c` は `struct inode` 操作が多い(33〜63 行)。
- `mft.c` は folio/pagecache 系が 144 行ヒット。**MFT 自体をページキャッシュ(address_space)越しに読み書きしている**。
- ブロック I/O は `bdev-io.c`(bio 12)・`mft.c`(29)・`compress.c`(16)・`super.c`(16)。ここが BlockDevice に接続する点。
- 依存が薄く、ほぼそのまま使えそうなファイル(VFS 系ヒット 0 に近い): `layout.h`(ディスク構造定義), `runlist.c`, `mst.c`, `collate.c`,
  `unistr.c`, `upcase.c`, `lib/*`(LZX/XPRESS 展開), `debug.c`。→ ただし `kmalloc`/`ERR_PTR`/`le*_to_cpu`/nls は残る。
- `iomap.c`(819 行)は `iomap_ops` 実装。iomap フレームワーク(`iomap_*`)自体は fs/ntfs の外にある。

## 設計上の含意

`fs/ntfs` は「純粋なパーサ」ではなく **Linux VFS 上で動くファイルシステムそのもの**。必要な互換面は少なくとも:

| 面 | 必要なもの | 規模感 |
|---|---|---|
| メモリ/同期 | kmalloc/vmalloc/kmem_cache, mutex/rwsem/wait/completion, atomic, bitops, byteorder, unaligned | 小(機械的) |
| 型/ユーティリティ | list, sort, overflow, math64, ERR_PTR, printk/pr_* | 小 |
| NLS/UTF-16 | nls_table, utf16s_to_utf8s 等 | 小〜中 |
| blk I/O | bio/bdev → BlockDevice.readAt/writeAt | 小〜中 |
| VFS オブジェクト | super_block, inode(iget5_locked 等), dentry, file, address_space | **大** |
| ページキャッシュ | folio(確保/lock/dirty/writeback/uptodate), filemap_*, mapping | **大**(MFT がこれに載る) |
| iomap | iomap_begin/end, buffered/dio read/write ヘルパ | **大**(カーネル側実装の移植 or 再実装) |
| ACL/xattr/security | posix_acl, xattr handler, fileattr | 中(初期は無効化可能な見込み) |

## 方式の比較(Phase 2 開始時に spike で決める)

- **A. 最小 VFS エミュレーション(第一候補・要件書の方針)**: 共通 `LinuxCompat` に inode/address_space/folio/bio/iomap の最小実装を置き、
  `fs/ntfs` をできるだけ無改変でビルド。利点: 差分最小・上流追従が容易、他の Linux FS(ext4 等)にも流用可。
  リスク: iomap/folio の再実装規模、動作の等価性検証。
- **B. 部分移植**: 低依存ファイル(runlist/mst/attrib 解析等)だけ流用し、inode/MFT キャッシュ層をユーザー空間で書き直す。
  A が過大な場合の退避策(要件書「必要な部分のみ置き換え」)。ただし上流との差分が増える。
- **C. NTFS-3G(libntfs-3g)**: 実績あり・ユーザー空間ネイティブ。GPL-2.0+(ライブラリは LGPL 版の提供形態もあるが **要確認**)。比較対象として性能/互換を測る。

**Phase 2 の最初の作業(go/no-go spike)**: 読み取り専用で `$MFT` 読み込み → ルートディレクトリ列挙 → 1 ファイル読み出しまでを
A 方式で通す。必要になった互換 API 数と行数を記録し、A / B を判断する。判断理由は本書に追記する。

## 未検証(推定)

- A 方式の総工数。iomap/folio の最小実装でどこまで足りるかは spike まで不明。
- xattr/ACL/reparse/WOF 圧縮を無効化しても基本の読み書きが成立するか(Kconfig 上 WOF は独立オプション: 確認済み)。
- Android bionic / clang で `fs/ntfs` がコンパイルできるか(GNU 拡張・`typeof` 等)。
