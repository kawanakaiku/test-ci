# FAT12/16/32 + vfat (Linux fs/fat) の移植 — 読み取り専用

2026-11-12。Linux mainline `fs/fat` (コミット 067c5c24…、NTFS Resurrection / exFAT / ext2 / ext4 と同じ) を**無改変**で LinuxCompat に載せ、
ホスト (4 構成の ctest) と Android (AVD emulator-5556 の instrumented test、NDK arm64-v8a / x86_64) で「読み取り専用マウント → 一覧 → stat → 読み取り」まで動かした。
実機 (USB ディスク) では未確認 (手順は末尾)。**書き込みは別ドキュメント (docs/fat-write.md。2026-11-13 に追加、`USBMEM_NTFS_WRITE` ビルドのみ)**。書き込み無効のビルドでは `read_only=0` は -EROFS で、JNI は rw を要求されても読み取り専用でマウントする。

## 構成
- `third_party/fat/` : `fs-fat/{cache,dir,fatent,file,inode,misc,namei_vfat}.c` と `fat.h`、`include/{linux,uapi/linux}/msdos_fs.h`、`nls/nls_cp437.c nls_cp932.c`、`COPYING`、`UPSTREAM.md`。**全て無改変**。
  8.3 名だけの旧 `msdos` ドライバ (`namei_msdos.c`)、NFS エクスポート (`nfs.c`)、KUnit (`fat_test.c`) は入れない。
- `core/compat/`: ライブラリ `usbmem_fat` = fs/fat + `rt_vfs_fat.c` (ホスト入口、API は `usbmem/fat_c.h`) + `fat_volume.cpp` (`FatVolume` : `FsVolume`、`fs_name()` = "fat")。
  `usbmem_compat_rt` に PUBLIC リンク。同じプロセスで NTFS / exFAT / ext2 / ext4 と共存できる (`fat_coexist`)。
- **FAT 専用の shim は `core/compat/include_fat/`** に分けた (`linux/{hash,backing-dev-defs,nls}.h` と、強制インクルードする `compat/fat_shim.h`)。
  共有の互換層ヘッダを広げずに済み、他の FS のビルドは変わらない。`fat_shim.h` の中身: `__getname` を `void *` を返す malloc に、`inode_set_ctime` を timespec64 を返す形に、
  `DEFINE_SPINLOCK` の初期化、`fsparam_*`、`MODULE_ALIAS_NLS` の無効化、`get_bh` の参照カウントなど。
- コンパイルオプション: `-fshort-wchar` (カーネルの `wchar_t` は 16 ビット。FAT と nls_cp*.c は UTF-16 を `wchar_t` で持つ)、
  `-Wno-incompatible-pointer-types` (nls_cp*.c の表は `char2uni` を `wchar_t *` で定義するため、互換層の表の型 (`void *`) との関数ポインタ引数型の違いを GCC14 がエラーにする)、
  `USBMEM_RCU_DEFER` (ext4 と同じ。`fat_put_super` が `sbi` を `call_rcu` で解放した後に、`fat_inode` / `fsinfo_inode` の evict が `sbi` を読むので、解放を umount の最後に遅らせる)。
- `module_init` が 2 つ (`inode.c` = `init_fat_fs`、`namei_vfat.c` = `init_vfat_fs`) と NLS 2 つあるので、翻訳単位ごとに `USBMEM_MODULE_INIT_NAME` を別名にし、`usbmem_module_init_fat` が順に呼ぶ。

## 共有ファイルへの変更 (他 FS の並行移植と衝突しうる所。どれも追加のみ)
| ファイル | 変更 |
|---|---|
| `core/compat/CMakeLists.txt` | 末尾に FAT ブロック (ライブラリ `usbmem_fat`、テスト `usbmem_fat_tests`、ctest `fat_*`) を追加 |
| `core/compat/include/compat/fs.h` | `struct nls_table` を Linux 相当に拡張 (`alias` `charset2lower/upper` `owner` `next` を末尾に追加。先頭 3 つは従来どおり)、`struct kstat` に `ino` |
| `core/compat/include/compat/vfs_api.h` | `register_nls` / `unregister_nls` の宣言 |
| `core/compat/src/rt_pagecache.c` | NLS の登録簿 (`register_nls` / `load_nls`)、既定の utf8 表に ASCII の大文字小文字変換、`utf16s_to_utf8s` を上流どおり NUL 終端に |
| `core/compat/include/linux/buffer_head.h` + `src/rt_bh.c` | `struct buffer_head` に `b_refs` (get_bh の追加参照数)、`__brelse` が `b_refs` を数える (他の FS は 0 のまま = 従来どおり) |
| `core/compat/include/compat/ops.h` + `tools/gen_ops_structs.py` | 再生成 (`write_begin/write_end` を呼び出される型付きポインタに、`fatent_operations` は fatent.c の定義と衝突するので生成しない) |
| `android/app/src/main/cpp/{CMakeLists.txt,jni_ntfs.cpp}` | `usbmem_fat` をリンク、`mount_auto` に FAT12/16/32 の分岐を **既存の分岐の後ろ** (NTFS 既定の手前) に追加 |
| `android/app/src/main/java/.../ui/UsbScreen.kt`, `NativeBridge.kt` | パーティション一覧の「マウント (読み取り専用)」ボタンの条件に fat12/16/32 を追加、コメント |

`strings.xml` は変更なし (既存の「マウント (読み取り専用)」を使う)。`fs_probe` も変更なし: 既に BPB を厳密に検証して FAT12/16/32 を返し、exFAT / NTFS の判定が先にある。

## 設計の要点
1. **inode 番号**: FAT の ino は `iunique()` で実行時採番 (ディスク上に無い) なので、exFAT と同じく lookup / readdir が返した inode を**ピン**して ino → inode の表を持つ。同じマウントの間は安定、マウントをまたぐと変わる (DocumentsProvider の docId はパスにすること)。
2. **readdir のデッドロック回避**: `fat_readdir` は `sbi->s_lock` を持ったまま `dir_emit` を呼び、`vfat_lookup` も同じ mutex を取る。そのため actor 内で lookup しない。actor では名前と種別を集めるだけにし、
   `iterate_shared` を抜けてから名前ごとに lookup して実 inode をピンする。
3. **文字コード**: 長い名前 (UTF-16) は `iocharset=utf8` で UTF-8 に (サロゲートペアも可)。短い名前 (8.3) は codepage 437 (cp932 の表も登録済みだが、マウントオプションの切り替えは未対応 → 下記の制限)。
   NLS の表 (nls_cp437.c / nls_cp932.c) は上流のファイルをそのままビルドし、互換層の `register_nls` / `load_nls` 登録簿に載せる。
4. **大文字小文字**: 上流は iocharset=utf8 だと表が恒等で大文字小文字を区別する (マウント時に警告を出す)。FAT の利用者の期待に合わせて、互換層の既定 utf8 表を **ASCII の A–Z / a–z だけ変換**する表にした (UTF-8 の 2 バイト目以降は変えないので他の文字を取り違えない)。
   非 ASCII の大文字小文字同一視 (é と É など) はしない。ログに `FAT-fs (usbmem): utf8 is not a recommended IO charset…` が毎回出る (上流の警告そのまま)。
5. **時刻**: FAT の時刻は書いた端末のローカル時刻 (タイムゾーン情報なし)。カーネルと同じく `sys_tz` で UTC に直す。ホストの既定は UTC (= フィールドをそのまま UTC として読む)、
   Android は JNI がマウント前に `localtime_r` の `tm_gmtoff` から `usbmem_fat_set_timezone()` を呼ぶ (マウント時点のオフセットを全日付に使う。夏時間をまたぐ日付は 1 時間ずれうる — Linux のカーネルも同じ)。精度は 2 秒。1980 年より前の値は 1980-01-01。
6. **属性**: 読み取り専用属性 → 書き込みビットなし (mode)。隠し / システム属性は `FsStat` に項目が無いので UI には出ない (読める・一覧にも出る)。`showexec` / `sys_immutable` などのオプションは解析しない (既定値)。
7. **ルートディレクトリ**: FAT12/16 は固定サイズの領域 (クラスタ列ではない)、FAT32 はクラスタ列。どちらもテストで確認 (ルートに数十〜数百エントリ + 複数クラスタにまたがるサブディレクトリ)。
8. **buffer_head の参照カウント**: `fat_free_clusters` などは `get_bh` で参照を足して `brelse` を複数回呼ぶ。互換層の `brelse` は `b_refs` を数えて 0 のときだけ folio を手放す (これが無いと、壊れたイメージで inode の evict 経路に入ったとき二重解放になる — `fat_corrupt` で見つかった)。
   読み取り専用マウントでもデバイスへの書き込みは 1 回も発行されない (テストで `writes == 0` を確認)。
9. **ダーティフラグ**: 読み取り専用マウントでは FAT のダーティビットを立てない (上流の挙動どおり)。マウント時に FAT 全体は走査しない (空きクラスタ数は FSInfo / 遅延計算)。

## テスト (`core/compat/tests/test_fat.cpp`、ctest `fat_*`)
- イメージは `tools/gen_fat_fixture.py`: **mtools** (`mformat` / `mcopy -s -m` / `mattrib` / `mdel`) でフォーマットして書き込み、UTF-16 のサロゲートペア名 (mtools は BMP 外を扱えない) だけ LFN エントリのバイト列を Python で書き換える。
  できあがったイメージは **7-Zip (`7z x`) と mtools (`mcopy` 読み出し) の 2 つの独立したリーダ**で読み戻し、元のツリーと全ファイルがバイト一致することを生成時に確認する (一致しなければ生成失敗)。
  (`fsck.fat` / `mkfs.vfat` は環境に無いので使っていない。)
  | 名前 | 種別 | セクタ | クラスタ | 備考 |
  |---|---|---|---|---|
  | fat12_f1440 | FAT12 | 512 | 512 | 1.44 MB フロッピー、ルート 224 エントリ固定 |
  | fat12_c4k | FAT12 | 512 | 4 KiB | 12 MiB |
  | fat16_c512 | FAT16 | 512 | 512 | ルート 512 エントリ固定 |
  | fat16_c2k | FAT16 | 512 | 2 KiB | 64 MiB |
  | fat32_c512 | FAT32 | 512 | 512 | |
  | fat32_c4k | FAT32 | 512 | 4 KiB | 128 MiB |
  | fat32_s4k | FAT32 | **4096** | 4 KiB | 4Kn セクタ |
  | fat32_c8k | FAT32 | 512 | 8 KiB | 560 MB (疎ファイル) |
  内容: 空・クラスタ境界 ±1・複数クラスタ・**断片化したファイル**・大きいファイル (3 MiB)・日本語名 (UTF-16 LFN)・全角英数/全角スペース・サロゲートペア (U+1F600)・100 文字名/255 文字名・空白/ドット/`+,;=[]` を含む名前・
  小文字だけの 8.3 名 (NT 小文字フラグ)・大文字小文字混在・奇数秒の時刻 (2 秒に切り捨て)・1980 年/2099 年の時刻・読み取り専用/隠し/システム属性・3 階層のサブディレクトリ・空ディレクトリ・日本語ディレクトリ・
  120〜300 ファイルで複数クラスタにまたがるディレクトリ。マニフェスト = `D path` / `F path size seed mtime attrs`。
- `fat_read`: 一覧が期待どおり (名前集合・種別)、全ファイルのサイズ・mtime (2 秒精度)・読み取り専用属性・nlink・内容がバイト一致、不揃いな位置/長さの読み、EOF 以降 = 0、ENOENT/ENOTDIR/EISDIR、大文字小文字を区別しない lookup (ASCII)、
  サロゲートペア名の lookup、書き込みが 1 回も呼ばれない、`read_only=0` は -EROFS、2 回目のマウント、ASan/LeakSanitizer でリークなし。
- `fat_corrupt`: BPB のセクタ長 (0 / 非 2 冪 / 8192)・クラスタあたりセクタ数 (0 / 非 2 冪)・FAT 数 0・メディア 0・予約セクタ 0・FAT 長 0・ルートエントリ過大・総セクタ数過大・FAT32 ルートクラスタ 0/過大・署名・デバイス短縮・先頭セクタ全消去でマウントの成否を確認し、成功した場合も走査して落ちない。
- `fat_ioerr`: n 回目の read から EIO にして (n = 0..59)、マウント・走査が安全に失敗する。
- `fat_fuzz`: 領域別 (BPB / FAT / ルート / ディレクトリ・データ / 全域 / FSInfo / 先頭) に壊して 600 反復 x 8 イメージ、ASan/UBSan で落ちない・ハングしない (30 秒の alarm)、デバイスへの書き込みなし。
- `fat_coexist`: FAT + exFAT + ext2 を同一プロセスでマウントし、交互にアンマウント・再マウントしても内容が変わらない。
- `fat_probe`: `probe_filesystem` が FAT12/16/32 を取り違えず、exFAT / NTFS / ext2 を FAT と誤認せず、壊れた BPB の FAT を FAT と判定しない。
- Android: `FatInstrumentedTest` (6 イメージ x 約 331 ファイルをマニフェストと全バイト比較、mtime はマウント時の端末タイムゾーン補正込み、日本語名・サロゲートペア名・大文字小文字)。`tools/run_android_fat_test.sh`。

## 制限 / 未解決
- **書き込みは docs/fat-write.md** (`USBMEM_NTFS_WRITE` ビルドだけ。実験的)。既定のビルドは読み取り専用。
- **短い名前の codepage は 437 固定**: 長い名前 (LFN) を持たない 8.3 名が cp932 (日本語 DOS / 旧 Windows) で保存されている媒体は、短い名前が文字化けする (LFN があるファイルは正しい)。
  cp932 の表は登録済みなので、マウントオプション (`codepage=932`) を渡せるようにすれば切り替えられる。互換層の `usbmem_compat_mount_fs_opts` が今は「名前だけのフラグ」しか渡せないため未実装。
- 隠し / システム属性を UI に出す項目が無い。非 ASCII の大文字小文字同一視はしない。
- AVD のテストは小さめの 6 イメージ (4Kn セクタと 8K クラスタの大きいイメージはホストのみ)。
- ピン表の上限なし (巨大ディレクトリでは inode 数ぶんメモリが増える。exFAT と同じ)。
- 実機 (USB ディスク) では未確認。

## 実機で確認する手順 (未実施。実機には触れていない)
1. FAT32 / FAT16 / exFAT でない古い FAT12 のフロッピー互換 USB など、中身が分かっている FAT の USB ディスクを用意する (Android が自動マウントしない状態にする = アプリが USB を確保する前に OS の usb-storage が掴んでいたら取り出す)。
2. アプリで USB を開き、パーティション一覧に FAT12/16/32 のチップと「マウント (読み取り専用)」ボタンが出ることを確認 → マウント → ファイルアプリの「USB Storage Manager」で一覧が見えること、日本語名が正しく出ること、更新時刻が端末のタイムゾーンで合っていること、数 MB〜数 GB のファイルをコピーして中身が一致することを確認する。
3. 大容量 (数百 GB の FAT32、32 KiB クラスタ) では、マウントの所要時間とコピー速度を logcat (タグ `usbmem`) で確認する。FAT 全体の走査はしないので、マウントは速いはず。
