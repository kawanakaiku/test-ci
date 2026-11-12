# squashfs (Linux fs/squashfs) の移植 — 読み取り専用

2026-11-12。Linux mainline `fs/squashfs` (コミット 067c5c24…、他の FS と同じ) と、展開用のカーネルライブラリ (`lib/{zlib_inflate,lz4,lzo,xz,zstd}`) を**無改変**で LinuxCompat に載せ、
ホスト (4 構成の ctest) と Android (AVD の instrumented test、NDK arm64-v8a / x86_64) で「読み取り専用マウント → 一覧 → stat → 読み取り → シンボリックリンク」まで動かした。実機 (F-51A + USB) では未確認。
erofs の移植は docs/erofs-port.md (展開ライブラリと互換層の追加分を共用している)。

## 先に結論 (実用上の範囲)
- 読み取り専用の形式。`FsVolume` の `fs_name()` は `"squashfs"`、`writable()` は常に false。rw を要求されても読み取り専用でマウントする (`mount_auto`)。
- **対応する圧縮方式**: **gzip (zlib)・lzo・xz (BCJ フィルタ x86/arm/arm64 を含む)・lz4・zstd**、および無圧縮。ブロックサイズ 4 KiB〜1 MiB、フラグメント(あり/なし/`-tailends`)、エクスポートテーブル(あり/なし)、疎ファイル(あり/なし)。
- **非対応**: **LZMA1** (圧縮 ID 2)。上流の `squashfs_fill_super` は `-EINVAL` で拒否するので、ホスト入口が「magic と major が正しいのに圧縮方式が非対応」を見分けて **`-EOPNOTSUPP` (95)** に直す。
  未知の圧縮 ID も同様。
- 判定は `fs_probe`: 先頭 4 バイトが `hsqs`、major 4、`block_size` が 4 KiB〜1 MiB の 2 のべき乗。既存のファイルシステム判定は変えていない。

## 構成
- `third_party/squashfs/` : `fs-squashfs/*.c *.h`、`COPYING`、`UPSTREAM.md`。**無改変**。構成 (Kconfig 相当): `SQUASHFS_FILE_DIRECT`、`SQUASHFS_DECOMP_SINGLE`、フラグメントキャッシュ 3、`SQUASHFS_XATTR` 無効 (ディスク上の xattr は無視; 拒否はしない)、
  `SQUASHFS_ZLIB / LZ4 / LZO / XZ / ZSTD` 有効。
- `third_party/kernel-lib/` : 展開ライブラリ (**無改変**; 例外は `include/linux/crc32.h` の小さな自前シム)。`usbmem_kdecomp` (CMake) としてビルド。
- `usbmem_squashfs` = fs/squashfs + `rt_vfs_squashfs.c` (ホスト入口; ino のピン表、lookup / readdir / read / get_link) + `squashfs_volume.cpp` (`FsVolume` 実装)。
  互換層の追加分は `compat/more_zfs.h` と `rt_zfs.c` (bio / 待ち行列 / ページキャッシュ周り)。`zfs_lzo.c` は lzo の展開の薄いラッパ (`__BIG_ENDIAN` の `#undef` だけ)。

## 設計・移植上の注意
1. **i_ino はディスク上の inode 番号**。ホスト入口は lookup が返した inode をピンし、任意の ino を渡されても任意の位置を inode として読まない。
2. **特殊ファイル** (キャラクタ / ブロックデバイス / FIFO / ソケット) は一覧と stat に出るが、読み取りは `-EINVAL`。
3. **壊れた/巨大な値で大きなメモリを確保しない**: ブロックサイズ・各テーブルのサイズ・圧縮ブロック長は上流が検査する。`squashfs_fuzz` は反復ごとに画像を壊してマウント・走査し、最大 RSS を出力する。
4. **プラットフォーム差 (Android で見つけたもの)**:
   - **bionic は `<endian.h>` を明示的に入れないと `__LITTLE_ENDIAN` が定義されない**。zstd の `mem.h` はこれで `MEM_isLittleEndian()` を決めるので、未定義だと HUF テーブルが壊れ、`BIT_reloadDStream` の assert で落ちた。`usbmem_kdecomp` に `-include endian.h`。
   - NDK の `<linux/types.h>` は UAPI 版で `uint8_t` などを定義しない → `xz.h` のために `-include stdint.h -include stddef.h`。
   - glibc では `u64` が `unsigned long` になり、`__u64` との不一致で `-Wincompatible-pointer-types` が出る → 該当 TU だけ `-Wno-error=incompatible-pointer-types`。
5. UBSan: 展開ライブラリと fs/squashfs には、意図的な未整列アクセス / 符号付きシフトがあるので、`alignment` / `shift-base` / `nonnull-attribute` の検査は該当 TU だけ外した (`-fno-sanitize=…`)。

## テスト (`core/compat/tests/test_squashfs.cpp`、ctest `squashfs_*`)
- イメージは `tools/gen_squashfs_fixture.py`: `mksquashfs` でルート権限なしに作る。15 種類: `gzip_4k/128k/1m` `zstd_128k/8k(level 19)` `xz_128k` `xz_bcj` `lz4_128k` `lz4_hc` `lzo_128k` `nofrag` `tailends` `nocomp` `noexport` `nosparse`。
  加えて `lzma_refused.img` (LZMA1; 拒否テスト用)。**`unsquashfs` で展開し直した結果が元のツリーとバイト一致する**ことを生成時に確認する。
- `squashfs_read`: 名前集合と種別、全ファイルの size / mode / nlink / mtime、内容のバイト一致、不揃いな位置/長さ、EOF、疎ファイル、シンボリックリンク、特殊ファイル、`FsVolume` ラッパ、LZMA1 の拒否 (`== -EOPNOTSUPP`)。
- `squashfs_corrupt` / `squashfs_ioerr` / `squashfs_fuzz` / `squashfs_coexist`: erofs と同じ型 (壊れたスーパーブロック・テーブル・圧縮方式、I/O エラー注入、ランダム破壊、ext2 との同時使用)。
- AVD (`emulator-5556`) `SquashfsInstrumentedTest`: gzip_128k / gzip_4k / xz / zstd / lz4 / lzo / nocomp + LZMA1 の拒否。

## 限界・未解決
- **実機未確認**。**LZMA1 は非対応**。xattr は読まない。展開は単一スレッド (`DECOMP_SINGLE`)。
- 4 構成の結果は最終報告を参照 (erofs と同時に 1 回実行)。
