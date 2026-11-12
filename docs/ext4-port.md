# ext4 (Linux fs/ext4) の移植 — 読み取り専用・ジャーナル非再生

> 2026-11-13 追記: 書き込み (fs/jbd2 を無改変で追加、ジャーナルの再生は書き込み可のマウントのときだけ) を実装した。docs/ext4-write.md を参照。
> 読み取り専用のマウントは以下のとおり noload のまま (ジャーナルを読まない・ディスクに一切書かない)。下の「jbd2 本体は入れない」「手書きの jbd2.h」は当時の記述で、
> 現在は上流の jbd2.h と fs/jbd2 をビルドし、buffer_head は ext4/jbd2 専用のカーネル互換実装 (`src/rt_kbh.c`) を使う。
> シンボリックリンクのリンク先は `Ext4Volume::readlink` / `usbmem_ext4_readlink` でも読める (LinkResolver 用)。

2026-11-12。Linux mainline `fs/ext4` (コミット 067c5c24…、NTFS / exFAT / ext2 と同じ) を**無改変**で LinuxCompat に載せ、
ホスト (4 構成の ctest) と Android (AVD emulator-5556 の instrumented test、NDK arm64-v8a / x86_64) で「読み取り専用マウント → 一覧 → stat → 読み取り」まで動かした。実機 (USB ディスク) では未確認。
ext3 (ジャーナル付き・ブロックマップ) も同じドライバで読む。

## 先に結論 (実用上の範囲)
- 今どきの `mkfs.ext4` した USB ディスク (extents / flex_bg / 64bit / metadata_csum / huge_file / dir_nlink / extra_isize / ジャーナル) を読める。
  bigalloc / inline_data / meta_bg / resize_inode / htree / 深い extent 木 / 未書き込み (unwritten) extent も検証済み。
- **ジャーナルは再生しない、ディスクに一切書かない**。クリーンにアンマウントされた ext3/ext4 は読める。
  **未再生のジャーナルがある (`needs_recovery`) ボリュームは既定で拒否する** (`-EUCLEAN` = 117)。再生しないまま読むと古い内容が見えるため。
  API 上は `Ext4Volume::mount(dev, err, {allow_noload = true})` で明示的に許可できる (アプリは許可しない)。
- 暗号化 (encrypt)・大文字小文字無視 (casefold)・fs-verity・DAX・未知の incompat 機能は `-EOPNOTSUPP` (95) で拒否。
- ブロックサイズは 4096 以下 (folio 4096 固定)。書き込みは未対応。

## 取り込みとビルド
- `third_party/ext4/` : `fs-ext4/*` (51 ファイル / 70,910 行、無改変)、`include/{trace/events/ext4.h, uapi/linux/ext4.h, linux/fscrypt.h, uapi/linux/fscrypt.h}`、`COPYING`、`UPSTREAM.md`。
- ビルドするのは読み取り経路の 22 ファイル (約 42,000 行): balloc bitmap block_validity dir ext4_jbd2 extents extents_status file fsync hash ialloc indirect inline inode namei readpage super symlink xattr xattr_{hurd,trusted,user}。
  mballoc の割り当て・orphan・fast_commit・ioctl・sysfs・page-io・mmp・resize・migrate・move_extent・fsmap・crypto・verity・acl は入れず、`core/compat/src/rt_ext4.c` の最小スタブで置き換える。
- `usbmem_ext4` (CMake) = 上のファイル + `rt_ext4.c` + `rt_vfs_ext4.c` (ホスト入口) + `ext4_volume.cpp` (`FsVolume` 実装)。`-DUSBMEM_MODULE_INIT_NAME=usbmem_module_init_ext4 -DUSBMEM_RCU_DEFER=1`。NTFS / exFAT / ext2 と同じプロセスで同時にマウントできる。

## 設計の要点
1. **jbd2 本体は入れない**。`core/compat/include/linux/jbd2.h` を最小限の手書きにし (上流の jbd2.h は内部型が多すぎる)、`ext4_journal_start` 等は `sbi->s_journal == NULL` のとき `ERR_PTR(-EROFS)` / 何もしない経路を通る。
   ジャーナルのロード自体を避けるため、マウント時は常に **`noload` を実際の `ext4_parse_param` → `fs_parse` に通して渡す** (`usbmem_compat_mount_fs_opts`。`fs_parse` は flag のみ実装)。`noload` + `ro` のとき `ext4_fill_super` は `ext4_load_journal` を呼ばない。
2. **入口の事前検査** (`rt_vfs_ext4.c` の `precheck_sb`): スーパーブロック (オフセット 1024) を生で読み、マジック不正・2048 B 未満は `-EINVAL`、対応外の incompat・verity は `-EOPNOTSUPP`、`needs_recovery` は `-EUCLEAN`。上流の検査より前に、ジャーナルに触れる経路へ入らないことを保証する二重の守り。
   通す incompat: filetype 0x2 / (recover 0x4 は別途 `-EUCLEAN` 判定) / meta_bg 0x10 / extents 0x40 / 64bit 0x80 / mmp 0x100 / flex_bg 0x200 / ea_inode 0x400 / csum_seed 0x2000 / largedir 0x4000 / inline_data 0x8000。それ以外 (encrypt 0x10000、casefold 0x20000、外付けジャーナル、dirdata など) は `-EOPNOTSUPP`。
3. **メタデータは buffer_head、ファイルデータは実 bio**。`ext4_mpage_readpages` → `blk_crypto_submit_bio` (= `submit_bio`) → 互換層の同期 bio → `mpage_end_io` → `folio_end_read`。`block_read_full_folio` は互換層の `mpage_read_folio(folio, ext4_get_block)` に差し替え。
   inline_data・htree・extent 木 (深さ 2 まで確認)・unwritten extent・bigalloc を検証。
4. **チェックサム**: `metadata_csum` は crc32c (Castagnoli、最終反転なし) を `rt_ext4.c` に実装し、inode / ビットマップ / グループディスクリプタ / extent / ディレクトリ / xattr の検証が通る。`uninit_bg` は crc16。壊した値ではマウントまたは読み取りが失敗することをテストで確認。
5. **mballoc は未ビルド**: `ext4_mb_init` だけ `rt_ext4.c` に置き、`ext4_get_group_info` が使う 2 段の `s_group_info` 配列を作る。
6. **RCU の遅延**: `call_rcu` を即時実行すると `ext4_kill_sb` 後に `sbi` が解放済みになる (ASan で発見)。`USBMEM_RCU_DEFER` で遅延キューに積み、`usbmem_compat_umount` の最後 (と fill_super 失敗後) に `usbmem_rcu_flush` で実行する。
7. **rbtree**: 互換層の rbtree は色ビットのない単純な二分木。`rbtree_postorder_for_each_entry_safe` 用に後行順の走査を追加 (system zone のノード解放漏れを LeakSanitizer で発見)。
8. **自動判別**: `fs_probe` の `probe_ext` は、ext2 ドライバが扱える機能 (compat `0x3F` / incompat filetype / ro_compat `0x7`) の外にある機能を持つものを Ext4 とする。ext3 (ジャーナル付き) も ext4 ドライバへ。純粋な ext2 だけ `Ext2Volume`。

## 互換層の変更 (NTFS / exFAT / ext2 の挙動は変えない)
- 新規: `compat/more_ext4.h` (型・マクロ・定数・スタブ宣言)、`linux/jbd2.h`、約 65 の shim ヘッダ (`linux/{bit_spinlock,crc16,crc32c,ctype,dax,delay,export,fcntl,...}.h`, `kunit/*`)、`src/rt_ext4.c`、`src/rt_vfs_ext4.c`、`src/ext4_volume.cpp`、`public/usbmem/ext4_{c,volume}.h`。
- 既存の変更: `kernel.h` (`#undef errno`)、`fs.h` / `misc.h` / `vfs_api.h` (メンバ追加、`fs_parameter_spec`)、`buffer_head.h` (BH ビット追加、`lock_buffer` を実ビットに)、`more_blockfs.h` (`nd_terminate_link` を上流と同じ意味に)、`sync.h` / `rt_core.c` (`kmem_cache_create` を引数数で分岐するマクロにし 5 引数版を `kmem_cache_create5` に)、`mbcache.h`、`rt_stubs.c` (`super_set_uuid` を無音に)、`ops.h` (`tools/gen_ops_structs.py` で再生成)。
- **Android NDK の注意**: `linux/fcntl.h` など同名の uapi ヘッダを互換層の shim が隠すと、bionic の `<fcntl.h>` が壊れる。shim は `__ANDROID__` のとき `#include_next` で次の検索パスへ回す。

## テスト (`core/compat/tests/test_ext4.cpp`、ctest `ext4_*` 6 件)
- イメージは `tools/gen_ext4_fixture.py` (`mke2fs -d` でルート権限なし、`e2fsck -fn` で clean 確認): ext4 既定 4K / 1K / チェックサムなし 2K / gdt_csum 4K / inline_data 1K / bigalloc 4K (512 MB) / meta_bg 4K、ext3 4K、ext2 4K、`needs_recovery` (debugfs)、拒否用 (encrypt / casefold / verity / 未知 incompat)。
  各イメージに 5000 ファイルの htree ディレクトリ、深さ 2 の extent 木、unwritten extent、inline ファイル、ハードリンク/シンボリックリンクなど。
- `ext4_read`: 全イメージで名前集合・種別・size / mode / nlink / mtime・内容のバイト一致、リンク先、エラー系、書き込みが 1 回も呼ばれない、`needs_recovery` は既定で拒否・`allow_noload` で読める、拒否イメージ。
- `ext4_corrupt` (チェックサム改変を含む)、`ext4_ioerr` (n 回目の read から EIO)、`ext4_fuzz` (600 反復、seed 1)、`ext4_coexist` (ext2 と ext4 を同一プロセスで)。
- Android (AVD): `Ext4InstrumentedTest` 8 件 (ext4 既定 4K / inline_data 1K / gdt_csum 4K / ext3 の検証、needs_recovery=EUCLEAN 117・encrypt / casefold / verity=EOPNOTSUPP 95 の拒否)。`Ext2InstrumentedTest` の ext3 / ext4 空イメージは「ext4 ドライバでマウントされる」に変更。

## 未対応 / スタブ / 制限
- 書き込み、ジャーナルの再生、fsck 相当の修復は行わない。xattr は取り込んだが ACL・セキュリティラベル・暗号化・fs-verity・DAX・casefold は無効。
- mballoc・orphan 処理・fast_commit・MMP・resize・ioctl・sysfs・page-io は何もしない/エラーを返すスタブ。
- ブロックサイズ 8 KiB 以上 (他アーキテクチャ製) は不可。実機 USB での確認、巨大ボリュームのマウント時間 (グループ数に比例して `s_group_info` とビットマップ検証) は未確認。
- 「ジャーナル未再生」の誤判定防止として、`needs_recovery` が立っていないがジャーナルに未再生のトランザクションがあるケースは検出できない (上流の定義に従う)。
