# ext2 (Linux fs/ext2) の移植 — 読み取り専用

> 2026-11-12 追記: ext3 / ext4 は docs/ext4-port.md のとおり fs/ext4 で読めるようになった。下の「ext3/ext4 を拒否する」記述は ext2 単独の時点のもの。アプリは `fs_probe` で純粋な ext2 だけを fs/ext2 に回す。

2026-11-12。Linux mainline `fs/ext2` (コミット 067c5c24…、NTFS / exFAT と同じ) を**無改変**で LinuxCompat に載せ、
ホスト (4 構成の ctest) と Android (AVD の instrumented test、NDK arm64-v8a / x86_64) で「読み取り専用マウント → 一覧 → stat → 読み取り」まで動かした。実機 (F-51A + USB) では未確認。

## 先に結論 (実用上の範囲)
- **対応するのは ext2 だけ**: ジャーナルなし・extent なし・`metadata_csum` / `64bit` / `flex_bg` なしのボリューム。
- **今どきの Linux で `mkfs.ext4` した USB ディスクは、ext2 ドライバでは開けない**。ext4 は既定で extents / flex_bg / 64bit / metadata_csum などを持ち、これらは ext2 ドライバにとって未知の
  incompat 機能なので、`ext2_fill_super` が `-EINVAL` で拒否する (テスト `ext2_corrupt` の `ext4_empty`)。アプリは `fs_probe` で Ext3 / Ext4 を判別し、**NTFS として試さず明示的に拒否する** (`-ENODEV`)。
- ext3 (ジャーナル付きの ext2) は技術的には fs/ext2 で読めるが (`ext3_empty` はマウントできる)、**未再生のジャーナルがあると古い内容を見る**ためアプリ側では ext3 も拒否している。
- 古い機器 (カメラ、NAS、ルーターの USB ディスク、Linux の古いインストール) や、意図的に `mkfs.ext2` したメディアが現実的な対象。

## 構成
- `third_party/ext2/` : `fs-ext2/*.c *.h` (19 ファイル / 9,759 行)、`include/linux/ext2_fs.h` (uapi 側のヘッダ)、`COPYING`、`UPSTREAM.md`。無改変。
  - ビルドするのは上流 Makefile の最小構成 `ext2-y` = `balloc dir file ialloc inode ioctl namei super symlink trace` の 10 ファイル。
    `xattr*.c` / `acl.c` は `CONFIG_EXT2_FS_XATTR / POSIX_ACL / SECURITY` が無効の構成と同じく入れていない (取り込みはしてある)。
- `usbmem_ext2` (CMake) = 上の 10 ファイル + `rt_vfs_ext2.c` (ホスト入口) + `ext2_volume.cpp` (`FsVolume` 実装)。`usbmem_compat_rt` に PUBLIC リンク。
  `-DUSBMEM_MODULE_INIT_NAME=usbmem_module_init_ext2`。NTFS / exFAT と同じプロセスで同時にマウントできる。
- 互換層で追加・変更したもの
  - `rt_buffer.c` (新規, `usbmem_compat_rt`): `iget_locked` / `iget_failed` / `make_bad_inode`、`rb_next` / `rb_prev`、**`mpage_read_folio`**、`simple_get_link` / `page_get_link`、書き込み系 API のスタブ。
  - `compat/more_blockfs.h` (新規, `kernel.h` から取り込み): printk / WARN、LE ビットマップ操作、`guard()` / `scoped_guard()` (`__attribute__((cleanup))`)、uid/gid/時刻/デバイス番号の変換、`uuid_to_fsid`、
    `fs_umode_to_ftype`、`dir_pages`、inode のリンク数ヘルパ、folio の checked ビット、`kmem_cache_create_usercopy` など。
  - 空または最小のシム: `linux/{capability,printk,uaccess,swap,fiemap,rbtree,highuid,log2,percpu_counter,blockgroup_lock,tracepoint,mbcache,quotaops}.h`、`asm/current.h`、`trace/define_trace.h`。
    tracepoint は `TRACE_EVENT` → 何もしない inline 関数。mbcache・quota は何もしない/成功を返すスタブ。
  - 既存ファイルの変更: `fs.h` (`struct mapping_metadata_bhs` の定義をここへ)、`sync.h`、`consts.h`、`vfs_api.h`、`more.h`、`usbmem_ntfs.h`、`buffer_head.h`、`magic.h` (`EXT2_SUPER_MAGIC`)、
    `ops.h` (`tools/gen_ops_structs.py` で再生成)、`rt_pagecache.c` (`offset_in_folio` を関数からマクロへ。ポインタにも使われるため)、`rt_core.c` (後述の 2 点)。

## 設計の要点
1. **メタデータは buffer_head、ファイルデータ・ディレクトリは `read_folio`**。ext2 は inode・ビットマップ・間接ブロックを `sb_bread()` で読むので、exFAT で作った buffer_head 層 (bd_mapping の folio への薄い参照) がそのまま使える。
   ファイル本体とディレクトリは `a_ops->read_folio = ext2_read_folio → mpage_read_folio(folio, ext2_get_block)`。
   `mpage_read_folio` は folio (4096 B 固定) をブロック単位に `get_block()` で引き、マップされたブロックはデバイスから直接読み、**穴 (未マップ) は 0 で埋める**。
   穴は 1 ブロックずつ進める (最初の実装は `b_size` ぶん進めてしまい、穴の直後のデータを 0 にしていた。疎ファイルのテストで発見)。
2. **ブロックサイズは 4096 以下** (folio 4096 固定の制約)。ext2 は 1024 / 2048 / 4096 が一般的なので実用上は十分。8K 以上のブロックを持つ (他アーキテクチャで作られた) ボリュームは、バッファ層が対応しないためマウントできない想定 (イメージを作っての確認は未実施)。
3. **inode 番号は盤上の番号そのもの** (`ext2_iget(sb, ino)`、ルート = 2)。exFAT のような ino → inode のピン表は不要で、DocumentsProvider の docId にも ino を使ってよい。不正な ino (0 / 1 / 範囲外 / 未使用) は `-ESTALE` / `-EFSCORRUPTED` で失敗する。
4. **ホスト入口** (`rt_vfs_ext2.c`, API は `usbmem/ext2_c.h`): mount / unmount / lookup / stat_ino / readdir (`iterate_shared` + actor) / read。通常ファイルは `read_mapping_folio`、シンボリックリンクは `i_op->get_link` で**リンク先の文字列を返す** (辿らない)。特殊ファイルは `-EINVAL`。
   `read_only = 0` は `-EROFS`。mount オプションは解析しない (既定値)。
5. **後始末**:
   - `rt_core.c`: `fs_context` の `ops->free` を `get_tree` の後 (成功・失敗の両方) に呼ぶ (Linux の `put_fs_context` 相当)。ext2 は解析済みオプション用の領域を確保するため、これが無いとマウントごとに 48 バイトずつ漏れていた (LeakSanitizer で発見)。
   - `usbmem_compat_umount`: **ルートの dentry を先に落としてから `evict_inodes`** する (`generic_shutdown_super` と同順)。以前は逆順で、`put_super` の後にルート inode が退避され、ext2 の `evict_inode` が解放済みの `s_fs_info` を参照していた (UBSan で発見)。NTFS / exFAT の挙動は変わらない (全テスト通過)。
6. **スーパーブロックの機能検査は上流に任せた**: 未知の incompat (extents / 64bit / flex_bg 等) は上流が拒否する。アプリ側の `mount_auto` が `fs_probe` で Ext3 / Ext4 を先に弾く二重の守り。

## テスト (`core/compat/tests/test_ext2.cpp`、ctest `ext2_*`)
- イメージは `tools/gen_ext2_fixture.py`: `mke2fs -t ext2 -d <ツリー>` でルート権限なしに作る (Linux ドライバ・FUSE は使わない)。ブロックサイズ 1024 / 2048 / 4096 の 3 種 (inode サイズ 128 / 256 / 256、96 MiB)。
  作成後に `e2fsck -fn` が clean であることを確認し、`dumpe2fs -h` の機能一覧 (`ext_attr resize_inode dir_index filetype sparse_super large_file`、ext2 のみ) を `*.features` に残す。
  加えて `ext3_empty.img` / `ext4_empty.img` (拒否テスト用、内容なし) を作る。ツリーの内容と mtime は決定的。
  内容: 空・11 B・1 ブロック ± 1・**直接 12 ブロックの境界**・**単間接の境界**・**二重間接**の最初の範囲と間接ブロックをまたぐ疎ファイル・**三重間接**の疎ファイル (最大 約 4 GiB の論理サイズ)・穴だらけのファイル・全部穴のファイル・
  日本語名・255 バイト名・空白/引用符入りの名前・8 階層のディレクトリ・1000 ファイルのディレクトリ (複数ブロック/ページ)・ハードリンク・シンボリックリンク 3 種 (高速 = i_block 内、低速 = データブロック、ぶら下がり)。
- `ext2_read`: 名前集合と種別 (`.` / `..` を 1 回ずつ含む、重複なし)、全ファイルの size / mode / nlink / **mtime**、内容のバイト一致 (パターン、穴は 0)、不揃いな位置/長さ、EOF 以降 = 0、シンボリックリンクの `S_IFLNK` とリンク先の文字列、
  ハードリンクが同じ ino、大文字小文字を区別、ENOENT / ENOTDIR / EISDIR、不正な ino、書き込み・flush が 1 回も呼ばれない、`read_only=0` は `-EROFS`、2 回目のマウント。
- `ext2_corrupt`: マジック、`log_block_size`、未知の incompat (extents / 64bit)、`blocks_per_group` / `inodes_per_group` / `inode_size` / `blocks_count` / `inodes_count` の不正値、デバイス短縮 (1 KiB / 64 KiB / 1 MiB)、スーパーブロック全消去でマウントが**失敗**し、クラッシュしない。
  `ext4_empty` は拒否される。`first_data_block` / `rev_level` / `first_ino` の改変は上流が検査しないためマウントできるが、走査してもクラッシュしない。
- `ext2_ioerr`: n 回目の read から EIO (n = 0..59 と 60..400 の飛び飛び) でも安全に失敗する。
- `ext2_fuzz`: スーパーブロック / グループディスクリプタ / inode テーブル / ビットマップ / 先頭 4 MiB / 全域を壊して 600 反復 × 3 イメージ (1800 回、約 1460 回はマウントでき、その後の走査も含む)、ASan / UBSan で落ちない・ハングしない (30 秒の alarm)。
- `tools/run_host_tests.sh` の 4 構成すべて通過: plain 19 / san 19 / wsan 23 / wrel 23 件 (NTFS・exFAT を含む。NTFS 14・exFAT 4 件は従来どおり)。
- Android (AVD emulator-5556): `Ext2InstrumentedTest` 5 件 (b1k / b2k / b4k で 1019 ファイル・10 ディレクトリ・3 リンクを検証、ext3 / ext4 は `ENODEV` で拒否) 通過。全スイート 21 件 (従来 16 + 新規 5) 通過。NDK の arm64-v8a / x86_64 ともビルド成功。

## 未対応 / スタブにしたもの
- 書き込み: すべて `-EROFS` / `-ENOSYS` (`rt_buffer.c` の書き込み系は `usbmem_unimplemented` を呼ぶだけ)。マウントは常に読み取り専用。
- xattr / ACL / セキュリティラベル: 取り込んでいない (`security.*` などは見えない)。ファイルの権限ビットと uid/gid は stat で見える。
- quota・mbcache・tracepoint・fiemap・ioctl: 何もしない/成功を返すスタブ。`mount` オプション (`errors=`, `resuid=` など) は解析しない。
- ブロックサイズ 8 KiB 以上、実機 (USB) での確認、DocumentsProvider の docId に ino を使う切り替えは未実施。

## ext4 (fs/ext4 + fs/jbd2) の見積もり
`tools/fs_compat_probe.py` (実コンパイラの結果) とソース grep による。LinuxCompat の現状 (ext2 の対応後) に対する不足量。

| | ext2 | ext4 | jbd2 |
|---|---|---|---|
| .c ファイル / 行数 (h 含む) | 15 / 9,759 | 41 / 70,910 | 6 / 9,565 |
| 使うカーネルヘッダ種 / 互換層に無いもの | 50 / 1 | 113 / 47 | 36 / 13 |
| gcc エラー (空ヘッダで補った場合) | 175 (うち xattr/ACL が大半) | **14,853** | 614 |
| ユニーク: 関数 / 識別子 / 型 / 未定義構造体 / メンバ | 13 / 19 / 0 / 4 / 1 | 995 / 466 / 15 / 40 / 203 | 103 / 35 / 6 / 9 / 8 |

(ext2 は参考: 実際には最小構成の 10 ファイルだけをビルドし、シム追加後はエラー 0。)

- **extents** (`extents.c` 6,464 + `extents_status.c` 2,397 + `ext4_extents.h`): ext4 の既定。これが無いと読めない。純粋なデータ構造 (木の走査) が中心で、移植しやすい部類。
- **読み取りだけに絞った場合** (inode / super / namei / dir / hash / indirect / ialloc / balloc / readpage / block_validity / bitmap / symlink / file / extents / extents_status / xattr と ext4.h・ext4_jbd2.* 等): 約 **44,800 行** のコンパイルが必要。
  `ext4_journal_start / stop / get_write_access / handle_dirty` の呼び出しが ext4 全体で 350 箇所、`jbd2_*` の関数が 59 種あり、読み取り専用でもヘッダ・マクロ・型 (`handle_t`, `journal_t`) としては残る。
  → 現実的な方針は **jbd2 を入れず `ext4_journal_*` を何もしないスタブにする**こと (ext2 でクォータをスタブにしたのと同じ発想)。ただし ext4 側の分岐は `sbi->s_journal` が NULL の前提で書かれた箇所が多く、検証が要る。
- **ジャーナル (jbd2) の安全性**: ext4 の `super.c` は `noload` (`EXT4_MOUNT_NOLOAD`) のとき `ext4_load_journal` を呼ばない。読み取り専用で `needs_recovery` (未再生のジャーナル) があり `noload` でない場合、上流は
  "write access unavailable, cannot proceed (try mounting with noload)" で失敗する (`super.c` 6220 行付近)。ディスクを書かない前提を守るには、**必ず `noload` 相当で開く**こと。
  その場合、未再生のトランザクションの内容は反映されない (**古いデータが見える/ファイルが欠ける**可能性がある)。再生 (`jbd2/recovery.c` 996 行 + `revoke.c` 745 行) は**ディスクを書く**ので、このアプリの読み取り専用方針とは両立しない。
  → 方針: `needs_recovery` が立っているボリュームは「安全に読めない可能性がある」と明示した上で、`noload` での読み取りを選べる形にする (既定は拒否)。jbd2 全体 (9,565 行) は読み取り専用では不要。
- **crc32c**: `metadata_csum` は既定で有効。ext4 内の使用は 11 ファイル 47 箇所 (`ext4_chksum` 経由)。`crc32c` 自体は小さなテーブル実装を足せば足りる (数十行)。検査を省略すると破損の検出力が落ちるので、実装して検証するのが望ましい。
- **fscrypt / fsverity / casefold / DAX**: 参照は fscrypt 92 行、fsverity 27 行、casefold/unicode 52 行、DAX 71 行。いずれも読み取りの必須機能ではなく、`CONFIG_` を無効にした構成 (静的なスタブ関数) にできる。
  ただし暗号化 (`encrypt`) や casefold を持つボリュームは名前・内容が読めない/比較が違うので、**機能フラグを見て拒否**する (`INCOMPAT_ENCRYPT 0x10000`, `INCOMPAT_CASEFOLD 0x20000`)。
- **互換層に足りないもの (probe の結果)**: ヘッダ 47 種 (`linux/jbd2.h`, `crc32c.h`, `fscrypt.h`, `fsverity.h`, `quota.h`, `kunit/*` など)、`ext4_sb_info` / `ext4_group_info` / `shrinker` / `seq_file` / `fsmap` 等の構造体、
  `iomap` を使う読み取り経路 (ext4 は `readpage.c` の `ext4_mpage_readpages` を使うため、ext2 で作った `mpage_read_folio` を `ext4_map_blocks` に差し替えるか、iomap 経路に寄せる)。
- **許可する機能の範囲** (incompat): `FILETYPE`, `EXTENTS (0x40)`, `64BIT (0x80)`, `FLEX_BG (0x200)`, `META_BG (0x10)`, `CSUM_SEED (0x2000)`, `LARGEDIR (0x4000)` は読める必要がある。
  `RECOVER (0x4)` は上記の方針、`ENCRYPT` / `CASEFOLD` / `INLINE_DATA` / `EA_INODE` / `MMP` / `COMPRESSION` / `DIRDATA` / `JOURNAL_DEV` は当面拒否 (INLINE_DATA は `inline.c` 2,018 行が必要なので、後から対応する余地)。
- **工数の目安**: ext2 は 9,759 行 (実質 10 ファイル) で、シムは約 15 ファイル + `rt_buffer.c` 111 行 + `more_blockfs.h` 152 行だった。ext4 は読み取り専用でも ext2 の約 4.5 倍のコードを通し、
  jbd2 を除いても 995 種の関数と 203 種のメンバ不足を埋める作業になる。**ext2 の数倍〜10 倍の作業量** (数日〜1 週間規模) で、特に「ジャーナルの扱い」と「検証 (e2fsck 済みの ext4 イメージを多数作るテスト)」が重い。
  なお、`mke2fs -t ext4` でジャーナルなし (`-O ^has_journal`) のイメージを作る方法もあるが、実際の USB ディスクはジャーナル付きが大半なので、`noload` の扱いは避けて通れない。
