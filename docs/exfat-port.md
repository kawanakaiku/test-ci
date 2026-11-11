# exFAT (Linux fs/exfat) の移植 — Step 1: ホストで読み取り専用

2026-11-11。Linux mainline `fs/exfat` (コミット 067c5c24…、NTFS Resurrection と同じ) を**無改変**で LinuxCompat に載せ、
ホスト (Linux x86_64) で「読み取り専用マウント → ディレクトリ一覧 → ファイル読み取り」まで動かした。Android (JNI/Kotlin/AVD) は未着手。

## 構成
- `third_party/exfat/` : `fs-exfat/*.c *.h` (11 .c / 約 8,000 行)、`include/uapi/linux/exfat.h`、`COPYING`、`UPSTREAM.md`。無改変 (patches なし)。
- `core/compat/` (共通ランタイム `usbmem_compat_rt` + `usbmem_ntfs` + `usbmem_exfat` の 3 ライブラリに分けた)
  - `usbmem_compat_rt` = `rt_core.c rt_pagecache.c rt_write.c rt_stubs.c rt_bh.c bdev_bridge.cpp` (FS に依存しない互換層)
  - `usbmem_ntfs` = fs/ntfs + `rt_vfs.c` + `ntfs_volume.cpp`、`usbmem_exfat` = fs/exfat + `rt_vfs_exfat.c`。どちらも rt に PUBLIC リンク。
  - FS モジュールの init 関数名は FS ごとに CMake が `-DUSBMEM_MODULE_INIT_NAME=...` で指定 (`module_init(f)` マクロ)。`usbmem_compat_mount_fs(init, ...)` が
    FS ごとに init を 1 度だけ実行して `file_system_type` を記録する (同じプロセスで NTFS と exFAT を両方マウントできる)。
- 追加した互換 API: `linux/buffer_head.h` + `src/rt_bh.c` (buffer_head)、`compat/more.h` (hlist/rcu/ビット操作/ratelimit/名前ハッシュ/時刻/uid・vfsuid/スタブ類)、
  `linux/{atomic,bitmap,cred,fsnotify,init,magic,mount,mpage,random,rmap,statfs}.h` (空のシム)。`ops.h` は `tools/gen_ops_structs.py` で fs/ntfs + fs/exfat の和集合から再生成。

## 設計の要点
1. **buffer_head**: exFAT は FAT・ビットマップ・ディレクトリエントリを `sb_bread()` で扱う。`bd_mapping` (ブロックデバイスのページキャッシュ、4096B folio) の
   folio を参照する薄い `struct buffer_head` にした (`b_data` = folio 内のポインタ、`brelse` = `folio_put`)。同じブロックを複数の bh で読んでも同一メモリを指す。
   ブロックサイズは 512〜4096 (exFAT のセクタサイズの範囲)。書き込み側 (`mark_buffer_dirty` / `sync_dirty_buffer`) は用意したが、bd_mapping の writeback は未実装 (書き込み対応時に整える)。
2. **ファイルデータ**は iomap (`exfat_iomap_next` → 既存の `iomap_read_folio` 互換) 経由で読む。`valid_size` 以降は UNWRITTEN → 0 として読めることをテストで確認。
3. **ホスト入口** (`rt_vfs_exfat.c`, API は `usbmem/exfat_c.h`): exFAT の inode 番号は `iunique()` で実行時に採番され、ino から inode を引く手段が無い
   (NTFS は MFT 参照番号で `ntfs_iget` できる)。そこで lookup / readdir が返した inode を**ピン** (参照を保持) して ino → inode の表を持つ。
   readdir は `dir_emit` の ino (未キャッシュの inode は仮採番) を使わず、名前で lookup して実 inode をピンする (exFAT は直前の lookup のヒントで走査が償却 O(1))。
   ピンは unmount まで保持 (巨大ディレクトリでは inode 数ぶんメモリが増える。必要なら LRU 化)。
4. **解放**: umount と fill_super 失敗のときに `file_system_type.kill_sb` を呼ぶ (VFS の `deactivate_locked_super` 相当。exFAT は sbi・アップケーステーブルをここで解放する)。NTFS の kill_sb は `kill_block_super` (互換層では何もしない) なので挙動は変わらない。
5. mount オプションは解析しない (`fs_parse` は -ENOPARAM = 既定値): iocharset=utf8、errors=remount-ro、fmask/dmask は `current->fs->umask` (022)。読み取り専用マウントのみ (`read_only=0` は -EROFS)。

## テスト (`core/compat/tests/test_exfat.cpp`、ctest `exfat_*`)
- イメージは `tools/gen_exfat_fixture.py`: `mkfs.exfat` で空のボリュームを作り、**Linux ドライバとは独立に書いた Python** でファイル/ディレクトリを書き足し、`fsck.exfat -n` で clean を確認
  (セクタ 512/クラスタ 4096・512、セクタ 4096/クラスタ 32768 の 3 種)。内容: 空・小・1 クラスタ・複数クラスタ・1 MiB・**FAT チェーンで断片化**したファイル・valid_size < size・日本語名・サロゲートペア名・255 文字名・100 文字名・
  大文字小文字違い・サブディレクトリ (3 階層)・空ディレクトリ・300 ファイルで複数クラスタにまたがるディレクトリ。
- `exfat_read`: 一覧が期待どおり (名前集合・種別)、全ファイルのサイズ・mtime・内容がバイト一致、不揃いな位置/長さの読み、EOF 以降 = 0、ENOENT/ENOTDIR/EISDIR、大文字小文字を区別しない lookup、書き込みが 1 回も呼ばれない、`read_only=0` は -EROFS、2 回目のマウント。ASan/LeakSanitizer でリークなし。
- `exfat_corrupt`: boot 署名・fs 名・チェックサム・セクタ/クラスタ shift・ルートクラスタ・先頭セクタ全消去・デバイス短縮でマウントが**失敗**する (クラッシュなし)。
- `exfat_ioerr`: n 回目の read から EIO にして (n = 0..59、60..400)、マウント・走査が安全に失敗する。
- `exfat_fuzz`: 領域別 (boot[チェックサム再計算あり] / FAT / ヒープ先頭 / ディレクトリ / 全域) に壊して 600 反復 x 3 イメージ、ASan/UBSan で落ちない・ハングしない (30 秒の alarm)。

## 未対応 / 次にやること
- Android: `android/app/src/main/cpp/CMakeLists.txt` に `usbmem_exfat` を足し、JNI (`jni_exfat.cpp`) と Kotlin 側 (ファイルシステム種別で NTFS/exFAT を振り分け。`fs_probe` は exFAT を識別済み) を作る。
  DocumentsProvider の docId は ino でなくパス (または親 docId + 名前) にする方が安全 (ino は実行時採番で、ピンの寿命に依存する)。
- 書き込み: `mark_buffer_dirty` → bd_mapping の writeback、`exfat_set_volume_dirty` の順序保証 (NTFS の `begin_write` 相当)、create-only ポリシーを入口に実装してから。
- ピン表の上限 (LRU)。巨大ボリューム (数 TiB) でのマウント時間: `exfat_load_bitmap` が割り当てビットマップ全体を読む (7.3 TiB / クラスタ 128 KiB で 7 MiB 程度)。
- UBSan の alignment 検査は NTFS と同じく上流コードだけ外している (上流は非整列アクセス前提)。
