# erofs (Linux fs/erofs) の移植 — 読み取り専用

2026-11-12。Linux mainline `fs/erofs` (コミット 067c5c24…、NTFS / exFAT / ext2 / ext4 / squashfs と同じ) を**無改変**で LinuxCompat に載せ、
ホスト (4 構成の ctest) と Android (AVD の instrumented test、NDK arm64-v8a / x86_64) で「読み取り専用マウント → 一覧 → stat → 読み取り → シンボリックリンク」まで動かした。実機 (F-51A + USB) では未確認。

## 先に結論 (実用上の範囲)
- 読み取り専用の形式。`FsVolume` の `fs_name()` は `"erofs"`、`writable()` は常に false。rw を要求されても読み取り専用でマウントする (`mount_auto`)。
- **対応するデータの配置**: 無圧縮 (inline tail packing あり/なし)、チャンク方式 (blockmap / index)、圧縮 (下記の 5 方式)。
- **対応する圧縮方式**: **lz4・lz4hc** (同じ形式)、**lzma**、**deflate** (libdeflate も同じ形式)、**zstd**。`mkfs.erofs` が作る pcluster の大きさ (4 KiB〜64 KiB 以上)・
  ブロックサイズ 1 KiB / 2 KiB / 4 KiB・legacy 圧縮 (`-Elegacy-compress`)・`-Eztailpacking`・`-Efragments`・`-Ededupe` の組み合わせを実イメージで検査した。
- **ブロックサイズは 4096 以下** (folio 4096 固定の制約)。`blkszbits` が 13 以上のイメージはマウントを拒否する (テスト済み)。4 KiB 未満の圧縮ブロックは上流が "EXPERIMENTAL" と警告するが読めた。
- 判定は `fs_probe`: オフセット 1024 の magic `0xE0F5E1E2`。既存のファイルシステム判定は変えていない。

## 構成
- `third_party/erofs/` : `fs-erofs/*.c *.h` (全 21 ファイル、Kconfig / Makefile は除く)、`COPYING`、`UPSTREAM.md`。**無改変**。
  - ビルドする: `data dir inode namei super` (基本) と `zutil zmap zdata decompressor decompressor_lzma decompressor_deflate decompressor_zstd` (圧縮; `CONFIG_EROFS_FS_ZIP`)。
  - ビルドしない: `xattr` (xattr は `xattr.h` のマクロで無効、ACL / security も無効)、`sysfs`、`fileio` (ファイルバックのマウント)、`ishare` (page cache share)、`decompressor_crypto` (アクセラレータ)。
- `usbmem_erofs` (CMake, `core/compat/zfs.cmake`) = 上のファイル + `rt_erofs.c` (ホスト入口 + 互換層の追加分) + `erofs_volume.cpp` (`FsVolume` 実装)。`usbmem_kdecomp` (カーネルの `lib/` の展開ライブラリ、無改変) と `usbmem_compat_rt` にリンク。
  `-DUSBMEM_MODULE_INIT_NAME=usbmem_module_init_erofs -DUSBMEM_FS_EROFS=1 -DCONFIG_EROFS_FS_ZIP=1 …`。他の FS と同じプロセスで同時にマウントできる (テスト `erofs_coexist`)。
- 展開ライブラリ (`third_party/kernel-lib/`) は squashfs と共用。erofs の lzma は `xz_dec_microlzma_*` を使うので `CONFIG_XZ_DEC_MICROLZMA` を `usbmem_kdecomp` に足した (squashfs には影響しない)。
- 公開 API: `usbmem/erofs_c.h` (C)、`usbmem/erofs_volume.h` (`usbmem::ErofsVolume`)。JNI の `mount_auto` が `FsType::Erofs` で `ErofsVolume::mount` を呼ぶ。UI は既存の「読み取り専用でマウント」ボタンが `fs == "erofs"` に出る。

## 設計の要点
1. **ホスト入口** (`rt_erofs.c`): ino → inode の「ピン」表。lookup が返した inode だけを保持し、呼び出し側が任意の ino を渡しても、ディスク上の任意の位置を inode として読まない。
   i_ino は erofs の nid (64 ビットでは nid そのもの)。ルートは `usbmem_erofs_root_ino()`。readdir は `iterate_shared` + actor で、各項目を名前で lookup して ino を確定する。
   通常ファイルは `read_mapping_folio`、シンボリックリンクは `i_op->get_link` でリンク先の文字列を返す (辿らない)。特殊ファイル (デバイス / FIFO / ソケット) の読み取りは `-EINVAL`。
2. **メタデータは `bd_mapping` の folio、データは `iomap_read_folio`**。erofs は 5.x 後半以降 iomap 経由でデータを読む。互換層に既にあった iomap の最小実装 (`rt_pagecache.c`) をそのまま使った。
   - **互換層の既存コードのバグを 1 つ直した**: 汎用の `iomap_bio_read_ops` の `submit_read` が NULL で、bio が提出されないままだった (exFAT は独自の `submit_read` を持つので気づかれなかった)。
     カーネルと同じ「完了時に folio へ通知して bio を解放する」既定の実装を足した (`rt_pagecache.c` の 12 行)。
3. **圧縮 (z_erofs) はすべて呼び出し元スレッドで同期実行**: `bio_in_atomic()` を 0、`submit_bio` は同期なので、`z_erofs_runqueue` → 展開が `read_folio` の中で終わる。ワーカー / kthread / per-CPU は使わない (`CONFIG_EROFS_FS_PCPU_KTHREAD` 無効)。
   互換層に足したもの (`more_erofs.h` / `rt_erofs.c`、erofs の翻訳単位だけ):
   - **`xarray` の最小実装** (`pcluster` の登録・検索・削除・全走査): 添字付きの素朴な表。pcluster は同時に数個〜数十個なので線形探索で足りる。空になったら解放する。
   - **`vm_map_ram` / `vm_unmap_ram`**: 離れたページ (それぞれ別の 4096 バイトのホストバッファ) を連続領域として見せる。実カーネルでは同じ物理ページが複数の位置にマップされると互いに見える (別名) が、ここは複写。
     **変化したバイトだけを書き戻す**のが要点。**dedupe 済みイメージ**では同じファイルページが 1 つの pcluster の出力ページ 0 と 32 の両方に現れ、それぞれが別のバイト範囲へ書く。
     最初の実装は丸ごと書き戻していて、後から書き戻した側が先の結果を古い内容で上書きし、ファイル末尾が 0 になっていた (`lz4_dedupe` で発見)。
   - **`folio->private` と `page->private` を同じ記憶域に**した (`fs.h`: `struct folio` の `private` と `page` を union で重ねた)。カーネルでは order-0 のページでそうなっており、erofs は両方を使う
     (短命ページの目印は `folio->private` に書き、`page->private` で読む)。分かれていたため、短命ページが認識されずに解放漏れが起き、ファジングで RSS が数百 MiB まで膨らんでいた (直して 15 MiB)。
   - `vmap` / `vunmap` (大域バッファ)、`lockref`、`kstrndup`、`idr`、`IS_ENABLED` (`__is_defined` 方式; 共有ヘッダでは常に 0 だった)、`wait_on_bit_lock`、`guard(mutex)`、`DECLARE_WAIT_QUEUE_HEAD` など。
     NTFS が使っている `vmap` スタブ (未実装) の挙動は変えないよう、erofs の翻訳単位だけマクロで差し替えた。
   - `crc32c` は weak 定義 (ext4 側の `rt_ext4.c` と同時にリンクしても衝突しない)。
4. **安全側の既定**: マウントは `ro` 固定。追加デバイス (`extra_devices` / デバイステーブル) があるイメージは、追加の BlockDevice を渡せないため拒否される (テスト済み)。
   スーパーブロックのチェックサム (CRC32C) は上流が検証する。壊れた値 (巨大な `blocks` / `meta_blkaddr` / `xattr_prefix_*` / `packed_nid` / `dirblkbits` など) は上流の検査と境界チェックで失敗するか、読み取り時に `-EFSCORRUPTED` / `-EIO` になる。

## テスト (`core/compat/tests/test_erofs.cpp`、ctest `erofs_*`)
- イメージは `tools/gen_erofs_fixture.py`: `mkfs.erofs` (erofs-utils 1.9.4) でルート権限なしに作る。ツリーは squashfs / ext2 と同じ生成器 (1000 ファイルのディレクトリ、日本語名、255 バイト名、8 階層、疎な大きなファイル、ブロック境界 ±1、
  乱数に近いデータ/繰り返しデータ、ハードリンク、シンボリックリンク 3 種、FIFO、ソケット) + `special_tar` (tar 経由でキャラクタ/ブロックデバイスを含む小さなイメージ)。
  作成後に **`fsck.erofs` で整合性を検査し、`fsck.erofs --extract` で展開し直した結果が元のツリーとバイト一致する**ことを確認する (ドライバの出力ではなく参照実装との比較)。
  mtime は `dump.erofs` で確認した (compact inode は mtime を持たないので、`plain_compact` の期待値はビルド時刻)。
- 21 種類のイメージ: `plain_4k/1k/2k` `plain_noinline` `plain_compact` `plain_extended` `chunk_blockmap` `chunk_index` `chunk_index_4k`
  `lz4_4k` `lz4hc_4k` `lz4_big_pcluster` `lz4_1k` `lz4_tailpacking` `lz4_fragments` `lz4_dedupe` `lz4_legacy` `lzma_4k` `deflate_4k` `zstd_4k` `special_tar`。
- `erofs_read`: 名前集合と種別 (重複なし)、全ファイルの size / mode / nlink / mtime、内容のバイト一致、不揃いな位置/長さ、EOF 以降 = 0、疎ファイル、シンボリックリンク、特殊ファイル、`FsVolume` ラッパ (`readlink`)、
  ホスト API の境界 (存在しない ino、ディレクトリを read、ファイルを readdir 等)。
- `erofs_corrupt` (123 ケース): スーパーブロックの各フィールドの破壊 (magic / blkszbits / 機能フラグ / extra_devices / 各種オフセット / 巨大値)、デバイスの切り詰め、ゼロ化、メタデータの先頭ブロックの破壊。クラッシュ・ハングしない。
- `erofs_ioerr`: マウント中・走査中の読み取りエラーの注入 (n 回目の読み取りで失敗)。
- `erofs_fuzz`: 9 種のイメージ (圧縮を含む) に 1〜24 バイトの破壊 (スーパーブロック / メタデータ領域 / 圧縮インデックス / 先頭 1 MiB / 全体) を 2700 回 (seed 固定)。**最大 RSS 15 MiB** (ASan なしの plain 構成)。
- `erofs_coexist`: erofs 2 つ + ext2 を同一プロセスで同時に使い、片方を外しても残りが使え、再マウントできる。
- AVD (`emulator-5556`) `ErofsInstrumentedTest`: 上の主要イメージ (無圧縮 6 種 + 圧縮 10 種 + special_tar) を端末に置いて、JNI → `NtfsVolume.mountReadOnly` 経由で全ツリーを検証。

## 限界・未解決
- **実機未確認**: F-51A + 実 USB では未確認。erofs の USB ディスクは現実にはほぼ無く、主に Android のシステムイメージ・コンテナイメージ・テスト用途を想定。
- **xattr / ACL / SELinux ラベルは読まない** (無効構成)。ディスク上にあっても無視する (拒否はしない)。
- **追加デバイス (マルチデバイス) 構成は未対応** (拒否)。`blobdev` を使うチャンク形式も同様。
- **ファイルバック / fscache / DAX / page cache share / sysfs / アクセラレータ (crypto) は無効**。
- 展開は**単一スレッドの同期実行**のみ。大きな pcluster は連続領域の複写を伴うので、遅い (性能は未計測; 正しさ優先)。
- `iomap_readahead` は未実装 (互換層は `read_folio` しか呼ばない)。
- 圧縮ブロックが 4 KiB 未満のイメージは上流が "EXPERIMENTAL" と警告する (読めることは確認した)。
- メタデータ圧縮 (`-m`、`metabox`) とファイルシステム全体の 48 ビット化は、対応する機能のイメージを作って試していない (**未検証**)。
- 4 構成 (plain / san / wsan / wrel) の結果は下記および最終報告を参照。

## 4 構成の結果
`tools/run_host_tests.sh` と同じ 4 構成を `-j4` で 1 回ずつ通した結果 (erofs 5 本: erofs_read / corrupt / ioerr / fuzz / coexist を含む)。

| 構成 | ビルド | ctest | 所要時間 |
|---|---|---|---|
| plain (Debug) | OK | 37/37 成功 | 593 秒 |
| san (ASan+UBSan+LSan) | OK | 37/37 成功 | 1684 秒 |
| wsan (san + NTFS 書き込み) | OK | 43/43 成功 | 2162 秒 |
| wrel (Release + NTFS 書き込み) | OK | 43/43 成功 | 622 秒 (erofs_fuzz 232 秒) |

- erofs_fuzz は 14 イメージ × 400 反復。LSan でリークなし、plain の最大 RSS は約 15 MiB で頭打ち。
- wrel の初回ビルドは `erofs_fs.h` の `BUILD_BUG_ON(__builtin_constant_p(fmh) ? ... : 0)` が `_Static_assert` の定数式にならず失敗した。
  erofs のターゲットだけ `USBMEM_BUILD_BUG_ON_RELAXED` を定義し、`kernel.h` の `BUILD_BUG_ON` を「定数と判定できたときだけリンクエラーにする」形にして解決 (上流ソースは無改変)。修正後に wrel をやり直して成功。
- 修正は erofs ターゲットだけに効くので、plain / san / wsan の結果には影響しない (それらは修正前のビルドの結果)。
