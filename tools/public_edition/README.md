# android-usbmem (公開版)

root 権限なしで、USB ストレージやディスクイメージを扱う Android アプリです。USB ホスト (Mass Storage / BOT) として直接つなぎ、パーティション表の表示と編集、暗号ボリュームの解錠、ディスクの検査と初期化、救出とバックアップなどができます。

> **公開版 (public edition)**: アプリ全体のソース公開を求めるライセンス (GPL 系) のコードを一切含まないビルドです。Linux カーネル由来のファイルシステムドライバを含まないため、**ファイルを読み書きできるファイルシステムは F2FS・Btrfs・XFS だけ**です (下の「対応しているもの」)。NTFS・exFAT・FAT・ext2/3/4・ISO 9660・UDF・squashfs・erofs を開くには、これらを含む別のビルド (個人用。配布されません) が必要です。

## 免責 (必ずお読みください)

- **書き込みは実験的です。** データを失う可能性があります。重要なメディアでは、読み取り専用で使ってください。パーティション編集・mkfs・復元など、メディアの内容を書き換える機能は、確認ダイアログを出しますが、すべてのメディア・すべての機器で検証されているわけではありません。
- 実機での検証の範囲は限られています (特定の USB メモリ・USB-SATA ブリッジ・Android 端末での動作を確かめたものです)。ご自分の機器での動作は保証できません。
- 本ソフトウェアは Apache License 2.0 の下で、**無保証 (AS IS)** で提供されます (LICENSE)。

## 対応しているもの (この版)

| 分類 | 内容 | 備考 |
|---|---|---|
| USB | USB Mass Storage (BOT / SCSI) への直接アクセス、複数 LUN (カードリーダー)、回転ディスクとフラッシュの判別、スピンダウン | root 不要 (Android の USB ホスト API) |
| パーティション表 | MBR / GPT (拡張・論理・バックアップ GPT) の表示、**編集**、バックアップと復元、ラベル | docs/partition-edit.md |
| ディスクイメージ | RAW / QCOW2 / VHD / VHDX / VMDK (読み取り)、COW オーバーレイ、NBD クライアント | docs/blockdev-formats.md, docs/nbd.md |
| 暗号ボリューム | **LUKS1 / LUKS2** (AES-XTS / CBC、PBKDF2 / Argon2) と **BitLocker** の解錠と読み取り (書き込みは既定で無効・実験的) | docs/crypto-volumes.md。暗号は自前実装 (OpenSSL 等に依存しません) |
| ディスク検査・初期化 | FAT / exFAT / ext / NTFS の整合性検査 (読み取りのみ)、FAT32 / exFAT / ext2 の mkfs | docs/disk-tools.md |
| 救出・バックアップ | ddrescue 互換の mapfile による救出、Partclone 形式 (+ LZ4) のバックアップと復元 | docs/recovery-backup.md |
| ファイルシステム | **F2FS・Btrfs・XFS** の読み取り (圧縮されたファイルを含む。展開は自前の展開器)。書き込みは実験的で、書き込み有効のビルド (`-PusbmemNtfsWrite=true`) だけ | docs/f2fs-port.md, docs/btrfs-port.md, docs/xfs-port.md, docs/decomp.md, docs/provenance-btrfs-xfs.md |
| ネストマウント | ボリューム内のディスクイメージを開く (F2FS / Btrfs / XFS の中のイメージ) | docs/nested-mount.md |

**この版で未対応のファイルシステム**: NTFS、exFAT、FAT12/16/32、ext2/3/4、ISO 9660、UDF、squashfs、erofs。パーティション一覧には種類が表示されますが、マウントの入口は出ず、「この版では未対応」と案内されます。パーティション表の編集、検査、救出、バックアップは、これらのファイルシステムでも使えます。

(`docs/` には、完全版 (個人用ビルドを含む) の設計メモを一部載せています。この版に含まれない機能の記述が混ざることがあります。)

## インストール

リリースの APK を入手して、端末にインストールします (署名・配布先は未定)。必要なもの: Android 8.0 (API 26) 以上、USB ホスト (OTG) に対応した端末。

## ビルド

必要なもの: JDK 17、Android SDK (platform 36 / build-tools 36.0.0)、NDK 30.0.16248370、CMake 4.1.2 (`android/app/build.gradle.kts` の指定)。

```
cd android
./gradlew :app:assembleDebug -PusbmemEdition=public     # 公開版 (この版では必ず指定)
./gradlew :app:testDebugUnitTest -PusbmemEdition=public # JVM 単体テスト
```

書き込み経路を有効にしたビルド (実験的): `-PusbmemNtfsWrite=true` を足します。

## テスト

ホスト (Linux) で core をビルドして ctest を実行します。

```
cmake -S core -B build -G Ninja -DUSBMEM_EDITION=public
cmake --build build
(cd build && PATH="$PATH:/sbin:/usr/sbin" ctest -j2 --output-on-failure)
```

フィクスチャの生成に、`sfdisk` / `sgdisk` / `mkfs.*` / `cryptsetup` / `qemu-img` / `f2fs-tools` / `btrfs-progs` / `xfsprogs` / `zstd` / `lzop` などの外部ツールを使います (無いテストは SKIP されます)。`.github/workflows/ci.yml` が、必要なパッケージと手順を示しています。実機 (本物の USB メモリ) を使うテストは、この版には含まれません。

## 設計

`docs/architecture.md` (全体)、各機能の `docs/*.md`。ソースの構成: `core/` (C++17。パーティション・暗号・イメージ・検査・救出・USB)、`core/compat/` (この版では FsVolume 共通 IF・ネストマウント・F2FS / Btrfs / XFS だけ)、`android/` (Kotlin + Jetpack Compose + JNI)。

## ライセンス

Apache License 2.0 (LICENSE、NOTICE)。すべてのソースファイルに SPDX 表記があります。この版は GPL / LGPL のコードを含みません。第三者のコンポーネント (AndroidX / Compose / Gradle ラッパー / zlib) の扱いは NOTICE に書いています。

## 報告

不具合の報告は Issue へ。**データ破損の報告には、診断画面の出力、ファイルシステムの種類、メディアの機種 (USB の VID:PID)、行った操作を添えてください。**

## プライバシー

このアプリは通信しません (AndroidManifest に INTERNET 権限はありません。要求する権限は USB ホストの利用と生体認証 (パスワードの記憶の保護) だけです)。計測・解析・広告の SDK は含みません。
