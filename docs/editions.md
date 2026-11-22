# エディション: 公開版 (public) と個人版 (personal) — ライセンスの棚卸しと切り出し (2026-11-21)

**この文書は法的な助言ではない。** 各ファイルの SPDX 表記・出所の記述 (git の履歴・docs・UPSTREAM.md・コメント) と、それを基にした判断の根拠を並べたもの。
公開の最終判断 (特に 3.5 章の「Apache-2.0 で出す自前コード」と、4 章の安全側の除外を戻すかどうか) はユーザーが行う。**この作業で公開 (push・可視性の変更・新しいリモートの作成) はしていない。**

## 1. 決定

> ライセンス的にアプリ全体の公開が必要なものが混ざっているのなら、公開用からは除外し、自分用にだけ使えるようにしたい。

- **公開版 (public edition)**: アプリ全体のソース公開を要求するライセンス (GPL 系。LGPL を含め、`A OR B` の片方が GPL 系のものも入れない) のコードを一切含まない。ライセンスは自前コードの既存の SPDX に沿って **Apache-2.0** (既定。ユーザーが後で変えてよい。`LICENSE` / `NOTICE` は公開版のツリーにだけ置く)。
- **個人版 (personal edition)**: 全部入り (カーネル由来の FS ドライバ・fuseshim を含む)。**配布しない** (= 公開義務は生じない)。ビルドの既定は個人版で、挙動・CI は変えない。
- 公開版は、個人版のリポジトリ (このリポジトリ) から `tools/export_public.sh` で**ツリーを書き出して**作る。履歴は引き継がない (公開用の新しい履歴を後で別に作る。7 章)。

## 2. 出所の分類 (origin)

`tools/public_manifest.txt` の規則 (1 行 1 規則。上から順に最初に当たったものが決める) が、ファイルごとの出所・動作・理由を持つ。`tools/license_inventory.py` が規則と各ファイルの SPDX から表を作る。

| 出所 | 意味 | 公開版 |
|---|---|---|
| `own` | 自前 (このプロジェクトで書いた。外部のコードを写していない)。SPDX は Apache-2.0 か未記載 | 入れる (SPDX が無いソースには書き出しで Apache-2.0 を付ける) |
| `own-gpl` | 自前だが SPDX が GPL 系 (GPL 系のライブラリとリンクするための安全側のラベル) | 原則除外。中身が自前と確かめられ、カーネルに依存しないものだけ `relicense` で SPDX を Apache-2.0 に書き換えて入れる (3.5 章) |
| `kernel` | Linux カーネルのソース (無改変の取り込み。GPL-2.0 系。一部 BSD / 0BSD / zlib / LGPL) | 除外 |
| `kernel-shim` | LinuxCompat 層: カーネルの内部 API の再実装。カーネルのヘッダ・実装に倣って書かれており、**派生物として扱う** | 除外 |
| `kernel-patch` | カーネル由来のコードの解析・パッチ | 除外 |
| `external-gpl` | 外部の GPL / LGPL プロジェクト (ntfs-3g / wimlib) を動かす層 | 除外 |
| `dev` | 開発機・CI・実機検証専用 (公開版では動かない・個人の環境に依存・公開する理由が無い) | 除外 (公開版のテストが使うものは `own` として入れる) |
| `external-permissive` | 寛容なライセンスの外部成果物 (Gradle ラッパー) | 入れる (NOTICE に記載) |

**どの規則にも当たらないファイルは「未分類 = 要確認」で、公開版から除外する** (許可リスト方式。`license_inventory.py --check` が未分類・矛盾 (公開版に入るのに GPL 系の SPDX、古くなった規則) を検出して失敗する)。

## 3. 棚卸しの結果

### 3.1 コンポーネント別

`tools/license_inventory.py --markdown` の出力 (`--update-doc` で再生成する。「公開版」はファイル数)。

<!-- inventory:table -->
| コンポーネント | ファイル | SPDX の内訳 | 出所 | 判断 | 公開版 | 説明 |
|---|--:|---|---|---|--:|---|
| `.github (CI)` | 14 | (なし) 13, Apache-2.0 1 | dev 13, own 1 | 一部だけ入れる (入る 1 / 除外 13) | 1 | 個人版の CI |
| `ルート` | 6 | (なし) 6 | own 4, dev 2 | 一部だけ入れる (入る 4 / 除外 2) | 4 | README など |
| `android: ビルド設定・リソース・ラッパー` | 28 | (なし) 21, Apache-2.0 6, GPL-2.0-or-later 1 | own 23, own-gpl 4, external-permissive 1 | 一部だけ入れる (入る 24 / 除外 4) | 24 | Gradle / マニフェスト / リソース / Gradle ラッパー (Apache-2.0) |
| `android: 計装テスト` | 37 | (なし) 37 | dev 37 | 公開版から除外 | 0 | 実機・エミュレータ用。カーネル FS のフィクスチャ・個人の機材を前提にする |
| `android: JNI` | 13 | Apache-2.0 9, GPL-2.0-or-later 3, (なし) 1 | own 10, own-gpl 3 | 公開版に入れる (SPDX を Apache-2.0 へ書き換え 3) | 13 | 自前の JNI (FS の分岐は USBMEM_FS_* の条件コンパイル) |
| `android: Kotlin アプリ` | 43 | (なし) 39, Apache-2.0 4 | own 39, external-gpl 4 | 一部だけ入れる (入る 39 / 除外 4) | 39 | 自前の Kotlin。AndroidX / Compose (Apache-2.0) に依存 |
| `android: JVM 単体テスト` | 8 | (なし) 8 | own 8 | 公開版に入れる | 8 | JUnit |
| `core/*.cmake, core/CMakeLists.txt` | 8 | Apache-2.0 8 | own 6, own-gpl 1, external-gpl 1 | 一部だけ入れる (入る 6 / 除外 2) | 6 | 自前: ビルド定義 |
| `core/compat: ビルド定義` | 6 | Apache-2.0 6 | kernel 5, own 1 | 一部だけ入れる (入る 1 / 除外 5) | 1 | 各カーネル FS のビルド定義 |
| `core/compat: Btrfs / XFS (自前)` | 20 | Apache-2.0 20 | own 20 | 公開版に入れる | 20 | 自前の読み書き (2026-11-23 に出所を調べ直した: docs/provenance-btrfs-xfs.md)。展開は core の自前の展開器。Apache-2.0、公開版に入る |
| `core/compat: F2FS (自前)` | 9 | Apache-2.0 8, GPL-2.0-or-later 1 | own 8, own-gpl 1 | 一部だけ入れる (入る 8 / 除外 1) | 8 | 自前: オンディスク形式から書き起こした読み書き。圧縮の展開は core の自前の展開器 (core/src/decomp*.cpp) |
| `core/compat: LinuxCompat ヘッダ` | 165 | (なし) 93, GPL-2.0-or-later 68, GPL-2.0 3, GPL-2.0 WITH Linux-syscall-note 1 | kernel-shim 165 | 公開版から除外 | 0 | カーネルの内部 API の再実装 (<linux/*.h>) |
| `core/compat: カーネル FS ドライバのラッパ・公開ヘッダ` | 28 | GPL-2.0-or-later 28 | kernel 28 | 公開版から除外 | 0 | NTFS / exFAT / ext2 / ext4 / FAT / ISO 9660 / UDF / squashfs / erofs を呼ぶ薄い層 (GPL-2.0-or-later) |
| `core/compat: FsVolume 共通 IF・ネストマウント` | 5 | GPL-2.0-or-later 5 | own-gpl 5 | 公開版に入れる (SPDX を Apache-2.0 へ書き換え 5) | 5 | 自前: FS 共通のインタフェースと、その上のネストマウント (GPL 表記は安全側。公開版では Apache-2.0 へ) |
| `core/compat: LinuxCompat 実行時` | 20 | GPL-2.0-or-later 19, Apache-2.0 1 | kernel-shim 20 | 公開版から除外 | 0 | inode / dentry / ページキャッシュ / bio / buffer_head / jbd2 / VFS の再現 |
| `core/compat: カーネル FS / LinuxCompat のテスト` | 22 | GPL-2.0-or-later 19, Apache-2.0 3 | kernel 19, own-gpl 2, own 1 | 一部だけ入れる (入る 2 / 除外 20) | 2 | 上のドライバのテスト (GPL-2.0-or-later 表記が大半) |
| `core/fuseshim (ntfs-3g / wimlib)` | 26 | GPL-2.0-or-later 22, Apache-2.0 2, LGPL-2.1-or-later 2 | external-gpl 26 | 公開版から除外 | 0 | 外部 GPL / LGPL を動かす試作層 (GPL-2.0-or-later) |
| `core/include` | 27 | Apache-2.0 27 | own 27 | 公開版に入れる | 27 | 自前: 公開 API ヘッダ |
| `core/kfsd: プロトコル・クライアント・サーバ共通部 (Apache-2.0)` | 8 | Apache-2.0 8 | own 8 | 公開版に入れる | 8 | 自前: 本体と kfsd の RPC (kfsd_proto.h) と RpcVolume・serve。ドライバを含まない |
| `core/kfsd: kfsd の実行ファイル・ドライバ呼び出し (GPL-2.0-or-later)` | 5 | GPL-2.0-or-later 5 | kernel 5 | 公開版から除外 | 0 | カーネル FS ドライバを呼ぶ別プロセスの実行ファイル。個人版と別配布のプラグイン (docs/kfsd.md) |
| `core/src` | 60 | Apache-2.0 60 | own 60 | 公開版に入れる | 60 | 自前: パーティション表・ディスク編集・USB MSC / SCSI・暗号 (AES / SHA / HMAC / PBKDF2 / XTS / Argon2)・LUKS1/2・BitLocker・QCOW2 / VHD / VHDX / VMDK / NBD・DiskTools (fsck / mkfs)・救出 / バックアップ |
| `core/tests` | 29 | Apache-2.0 28, (なし) 1 | own 28, generated 1 | 公開版に入れる | 29 | 自前: 上のテスト (暗号ベクタは公開値) |
| `docs` | 50 | (なし) 50 | own-doc 50 | 一部だけ入れる (入る 19 / 除外 31) | 19 | ドキュメント (公開版に入るのは公開版の機能に関するものだけ) |
| `third_party/その他` | 1 | (なし) 1 | kernel 1 | 公開版から除外 | 0 | Linux カーネル由来 |
| `third_party/erofs` | 23 | GPL-2.0-only 13, GPL-2.0-or-later 7, GPL-2.0 WITH Linux-syscall-note 1, (なし) 1, MIT 1 | kernel 23 | 公開版から除外 | 0 | Linux fs/erofs。GPL-2.0 |
| `third_party/exfat` | 17 | GPL-2.0-or-later 14, GPL-2.0 WITH Linux-syscall-note 2, (なし) 1 | kernel 17 | 公開版から除外 | 0 | Linux fs/exfat。GPL-2.0-or-later |
| `third_party/ext2` | 22 | GPL-2.0 19, GPL-2.0 WITH Linux-syscall-note 1, (なし) 1, GPL-2.0-only 1 | kernel 22 | 公開版から除外 | 0 | Linux fs/ext2。GPL-2.0 |
| `third_party/ext4 (+jbd2)` | 66 | GPL-2.0 48, GPL-2.0+ 9, GPL-2.0 WITH Linux-syscall-note 3, LGPL-2.1 3, (なし) 2, ほか 1 種 | kernel 66 | 公開版から除外 | 0 | Linux fs/ext4 / fs/jbd2。GPL-2.0 (一部 LGPL-2.1) |
| `third_party/fat (+nls)` | 18 | GPL-2.0-only 9, GPL-2.0 4, (なし) 3, GPL-2.0 WITH Linux-syscall-note 2 | kernel 18 | 公開版から除外 | 0 | Linux fs/fat・nls。GPL-2.0 |
| `third_party/isofs` | 14 | GPL-2.0 8, GPL-2.0 WITH Linux-syscall-note 2, GPL-2.0-or-later 2, (なし) 1, GPL-2.0-only 1 | kernel 14 | 公開版から除外 | 0 | Linux fs/isofs。GPL-2.0 |
| `third_party/kernel-lib` | 64 | GPL-2.0+ OR BSD-3-Clause 31, (なし) 15, 0BSD 9, (文面: GPL 系) 3, GPL-2.0 2, ほか 3 種 | kernel 64 | 公開版から除外 | 0 | Linux lib/ の zlib・lz4・lzo・xz・zstd・xxhash (GPL-2.0 / BSD / 0BSD / zlib の混在) |
| `third_party/ntfs-resurrection` | 67 | GPL-2.0-or-later 55, (なし) 6, GPL-2.0 WITH Linux-syscall-note 2, MIT 2, GPL-2.0-only 1, ほか 1 種 | kernel 67 | 公開版から除外 | 0 | Linux fs/ntfs (NTFS Resurrection)。GPL-2.0-or-later |
| `third_party/squashfs` | 34 | GPL-2.0-or-later 24, GPL-2.0-only 8, GPL-2.0 WITH Linux-syscall-note 1, (なし) 1 | kernel 34 | 公開版から除外 | 0 | Linux fs/squashfs。GPL-2.0 |
| `third_party/udf` | 23 | GPL-2.0-only 14, GPL-2.0 4, (なし) 3, GPL-2.0 WITH Linux-syscall-note 1, LGPL-2.0+ 1 | kernel 23 | 公開版から除外 | 0 | Linux fs/udf。GPL-2.0 (一部 LGPL-2.0+) |
| `tools` | 164 | (なし) 128, Apache-2.0 34, GPL-2.0-or-later 2 | dev 115, own 42, kernel-patch 3, kernel-shim 2, external-gpl 2 | 一部だけ入れる (入る 42 / 除外 122) | 42 | テスト・フィクスチャ生成・CI の補助 (公開版に入るのは公開版のテストが使うものだけ) |
| `tools: FUSE アダプター・fuseshim・xfstests` | 10 | (なし) 9, GPL-2.0-or-later 1 | own-gpl 6, external-gpl 4 | 公開版から除外 | 0 | GPL 表記の自前アダプターと、ntfs-3g / wimlib の取得・試験 |
<!-- /inventory:table -->

ファイル単位の機械可読な表: `tools/license_inventory.py --tsv` (パス / SPDX / SPDX の種別 / 出所 / 動作 / 規則の行 / 注記)、`--json`。

### 3.2 ファイルシステムごとの扱い

| FS | 実体 | ライセンス | 公開版 | 備考 |
|---|---|---|---|---|
| NTFS | `third_party/ntfs-resurrection` (Linux `fs/ntfs` の復活版) + LinuxCompat + `ntfs_volume.cpp`。**自前ではない** | GPL-2.0-or-later | **除外** | 書き込みも同じコード。`$MFTMirr` のビルド時パッチも対象 |
| exFAT | `third_party/exfat` (Linux `fs/exfat`)。自前ではない | GPL-2.0-or-later | **除外** | 自前なのは検査・mkfs (`core/src/fscheck_exfat.cpp`, `mkfs_exfat.cpp`) だけ。これは入る |
| FAT12/16/32 + vfat | `third_party/fat` (Linux `fs/fat` + nls)。自前ではない | GPL-2.0 | **除外** | 検査・mkfs (`fscheck_fat.cpp`, `mkfs_fat32.cpp`) は自前で入る |
| ext2 | `third_party/ext2` (Linux `fs/ext2`) | GPL-2.0 | **除外** | 検査・mkfs (`fscheck_ext.cpp`, `mkfs_ext2.cpp`) は自前で入る |
| ext3 / ext4 | `third_party/ext4` (Linux `fs/ext4` + `fs/jbd2`)。一部 LGPL-2.1 | GPL-2.0 (LGPL-2.1 を含む) | **除外** | |
| ISO 9660 / UDF | `third_party/isofs`, `third_party/udf` (Linux)。UDF に LGPL-2.0+ | GPL-2.0 (LGPL-2.0+ を含む) | **除外** | |
| squashfs / erofs | `third_party/squashfs`, `third_party/erofs` (Linux) + `third_party/kernel-lib` (lib/ の展開器) | GPL-2.0 | **除外** | |
| **Btrfs** | **自前** (`btrfs_volume.cpp` + `btrfs_write*.inc`)。btrfs wiki の On-disk Format から書き起こした。圧縮の展開は core の展開器 (zlib = システムの libz、LZO1X・zstd = 自前: docs/decomp.md) | Apache-2.0 (2026-11-23 に SPDX を直した) | **入れる** | 出所の調査と機械照合は docs/provenance-btrfs-xfs.md。書き込みは個人版と同じ扱い (実験的・`USBMEM_NTFS_WRITE` が既定 OFF) |
| **XFS** | **自前** (`xfs_volume.cpp` + `xfs_write*.inc`)。公開仕様 "XFS Algorithms & Data Structures" から書き起こした。**形式の数値の確認にカーネルの形式定義ヘッダ (xfs_format.h など。GPL-2.0) を読んだ**ことは、コードの冒頭と docs/xfs-port.md に隠さず書いてある | Apache-2.0 (2026-11-23 に SPDX を直した) | **入れる** | カーネル全体 (v7.3 / v6.12) との機械照合で、10 トークン以上の連続一致・コメントの写しは無かった (定型の `rc = f(); if (rc) return rc;` と形式の定数表だけ)。docs/provenance-btrfs-xfs.md。**形式の定義を GPL のヘッダから取ったことの法的な評価は、ユーザーが著作権者として決める** (同ドキュメント 5 章) |
| **F2FS** | **自前** (`f2fs_volume.cpp` + `f2fs_write*.inc`)。オンディスク形式から書き起こした。`fs/f2fs` のコードは写していない (docs/f2fs-port.md)。機械照合で見つかった写し 2 か所 (ディレクトリのハッシュ、ノードの種別の判定) は、形式の記述から書き直した | Apache-2.0 | **入れる** | 圧縮クラスタ (lz4 / lzo / lzo-rle / zstd) の展開も core の自前の展開器 (2026-11-23。以前は公開版だけ `EOPNOTSUPP`) |
| fuseshim 経由の NTFS / WIM | `core/fuseshim` (shim・アダプター。GPL-2.0-or-later) + ntfs-3g (GPL-2.0+、fuse-lite は LGPL-2.0)・wimlib (LGPL-3.0+ / GPL-3.0+)。ソースはリポジトリに無い (CI が取得) | GPL / LGPL | **除外** | 本番方針は別プロセス (docs/fuse-layer.md 4 章)。別プロセスでも、配布する側には GPL / LGPL の義務 (ソース・通知) は残る (公開版には入れない) |

> ユーザーが「自前の NTFS / exFAT / FAT」と考えていたものは、**実際にはカーネル由来**である (ファイルを読み書きするドライバ本体)。自前なのは、整合性検査 (fsck 相当) と mkfs (`core/src/fscheck_*.cpp`, `mkfs_*.cpp`) で、これらは公開版に入る。`docs/fs-feature-matrix.md` の「XFS・Btrfs・F2FS (と NTFS) は自前」の「と NTFS」は誤りで、直した。

### 3.3 公開版に入る機能 (自前・Apache-2.0)

| 機能 | 場所 | 出所の根拠 |
|---|---|---|
| パーティション表 (MBR / GPT の解析・編集・バックアップ / 復元・ラベル) | `core/src/partition*.cpp`, `gpt_inspect.cpp`, `fs_label.cpp`, `fs_probe.cpp` | 公開仕様 (UEFI / MBR) から書いた。SPDX は全て Apache-2.0。docs/partition-edit.md (libfdisk は採用しない) |
| 暗号プリミティブ (AES・SHA-1/256/512・HMAC・PBKDF2・CBC/XTS/CCM・BLAKE2b・Argon2) | `core/src/crypto_*.cpp`, `sha256.cpp`, `crc32.cpp` | FIPS 197 / 180-4 / RFC 2104, 7693, 8018, 9106 / IEEE 1619 から書いた。**third_party・compat のものを借りていない** (OpenSSL・mbedTLS・libbde・libcryptsetup はリンクしない)。S-box は GF(2^8) の逆元から起動時に生成。ベクタは公開値 (hashlib / cryptography / argon2-cffi の出力) |
| LUKS1 / LUKS2、BitLocker の解錠・読み取り (書き込みは実験的・既定で無効) | `core/src/transform*.cpp`, `mini_json.cpp` | cryptsetup の on-disk-format ドキュメントと公開されている BitLocker の解析資料から。libbde・cryptsetup はテスト時の外部ツールで、コードは取り込まない (docs/licenses.md) |
| ディスクイメージ (QCOW2 / VHD / VHDX / VMDK)・COW オーバーレイ・NBD クライアント | `core/src/blockdev_*.cpp` | 公開仕様 (QEMU docs/interop、MS-VHDX、VMware VDF、NBD proto.md)。zlib はシステムライブラリ |
| DiskTools (NTFS / exFAT / FAT / ext の検査と、exFAT / FAT32 / ext2 の mkfs) | `core/src/fscheck*.cpp`, `mkfs*.cpp` | 自前。e2fsprogs / exfatprogs / dosfstools / ntfs-3g のコードは取り込まない (docs/disk-tools.md)。`ntfs-3g` のソースは `$MFTMirr` の挙動を**確認するために読んだ**と docs に書いてある (コードは写していない) |
| データ救出 (ddrescue 互換 mapfile)・バックアップ (Partclone 形式 + LZ4) | `core/src/recovery.cpp`, `backup_*.cpp` | 形式の互換のために再実装 (partclone 0.3.49 の実出力の解析。ソースは取り込まない) |
| USB Mass Storage / SCSI | `core/src/usb_msc.cpp`, `usbdevfs_transport.cpp`, `devices.cpp` | 自前 (`<linux/usbdevice_fs.h>` はシステムのヘッダを include するだけ) |
| FsVolume 共通 IF・ネストマウント | `core/compat/public/usbmem/fs_volume.h` ほか (3.5 章) | 自前 |
| F2FS・Btrfs・XFS (読み書き) と圧縮データの展開器 | 3.2 章、`core/src/decomp*.cpp` (docs/decomp.md) | 自前 (出所は docs/provenance-btrfs-xfs.md) |
| Android アプリ | `android/app/src/main/java` (Kotlin)、`cpp` (JNI)、`res` | 自前。AndroidX / Compose / Material3 / Kotlin 標準ライブラリ (Apache-2.0) に依存 (ビルド時に Maven から取得。リポジトリには無い)。Gradle ラッパーの jar (Apache-2.0) だけが外部のバイナリ |

### 3.4 判断に迷って除外側に倒したもの

| 対象 | 理由 | 戻すには |
|---|---|---|
| `core/compat/tests/test_f2fs_write.cpp` | SPDX が GPL-2.0-or-later (自前のテスト)。F2FS の書き込み試験は実カーネルの VM (`tools/f2fs_vm`) との比較が中心で、公開版の CI では回せない | `relicense` に移し、f2fs の書き込みテストを CI に足す |
| `core/compat/tests/bench_write_io.cpp` | 書き込みの I/O 形を測る個人版のベンチ | — |
| `docs/` の大半 | カーネル FS の移植記録・実機の検証記録・CI・監査・TODO。個人の機材 (機種名・テスト用リポジトリのアカウント名) を含む | 公開用に書き直す |
| `android/app/src/androidTest/**` | カーネル FS のフィクスチャと実機 (個人の USB メモリ) を前提にする | F2FS / 暗号 / パーティション編集の計装テストだけを選んで入れる |
| `tools/` の大半 | カーネル FS・NTFS・実機・開発機のパス (カーネルパッケージ・VM) に依存 | 必要になったものから入れる |

「自前」と書かれていても Linux カーネル (や ntfs-3g) の記述に倣った可能性があるものは、`license_inventory.py --lists` の「カーネルへの言及があるもの」で一覧する (現状の公開版の対象は、F2FS / Btrfs / XFS の「カーネルのコードは含まない」という否定の記述と、XFS の出所の注記)。

### 3.5 自前だが GPL 表記で、公開版では Apache-2.0 として出すもの (relicense)

次の 8 ファイルは、個人版でカーネル由来の FS ドライバと同じライブラリに入るため SPDX が GPL-2.0-or-later だが、**中身は自前の API 定義・薄い層で、カーネルのコードを含まない** (git の履歴の作者は 1 種類、コードはカーネルのヘッダ・実装を参照していない)。著作権者 (ユーザー) は自分のコードを別のライセンスでも出せるので、書き出しの際に SPDX の行だけを Apache-2.0 に書き換える (個人版のファイルは変えない)。

<!-- inventory:lists -->
### SPDX を Apache-2.0 へ書き換えて出すもの (relicense: 10)

| ファイル | 元の SPDX | 理由 |
|---|---|---|
| `android/app/src/main/cpp/jni_nested.cpp` | GPL-2.0-or-later | 自前 (ネストマウントの JNI) |
| `android/app/src/main/cpp/jni_ntfs.cpp` | GPL-2.0-or-later | 自前の JNI (FS の分岐は USBMEM_FS_* の条件コンパイル)。GPL 表記は個人版でカーネル FS をリンクするための安全側 |
| `android/app/src/main/cpp/jni_vol.h` | GPL-2.0-or-later | 自前 (Vol ハンドルの定義) |
| `core/compat/public/usbmem/fs_volume.h` | GPL-2.0-or-later | 自前の共通インタフェース。SPDX の GPL 表記は個人版でカーネル FS と同居するための安全側 |
| `core/compat/public/usbmem/nested_mount.h` | GPL-2.0-or-later | 自前 (FsVolume の上の薄い層。カーネル FS には依存しない) |
| `core/compat/public/usbmem/vol_state.h` | GPL-2.0-or-later | 自前のビット定義 (C / C++ 共通) |
| `core/compat/src/fs_volume.cpp` | GPL-2.0-or-later | 自前 (FsVolume::resolve だけ) |
| `core/compat/src/nested_mount.cpp` | GPL-2.0-or-later | 自前 (ネストの系譜・ボリューム単位の mutex。FS ドライバを呼ばない) |
| `core/compat/tests/crash_device.h` | GPL-2.0-or-later | 自前の電源断模擬デバイス (Btrfs / XFS の書き込み試験が使う)。GPL 表記は個人版の他の試験との同居のための安全側 |
| `core/compat/tests/sha256.h` | GPL-2.0-or-later | 自前のテスト用 SHA-256 |

### 自前だが公開版から外すもの (13)

| ファイル | SPDX | 理由 |
|---|---|---|
| `android/kfsd-plugin/build.gradle.kts` | - | kfsd のプラグイン APK (別 APK で配る。libkfsd.so = GPL-2.0-or-later を載せる)。本体の公開版には入れない |
| `android/kfsd-plugin/src/main/AndroidManifest.xml` | - | kfsd のプラグイン APK (別 APK で配る。libkfsd.so = GPL-2.0-or-later を載せる)。本体の公開版には入れない |
| `android/kfsd-plugin/src/main/cpp/CMakeLists.txt` | GPL-2.0-or-later | kfsd のプラグイン APK (別 APK で配る。libkfsd.so = GPL-2.0-or-later を載せる)。本体の公開版には入れない |
| `android/kfsd-plugin/src/main/java/org/usbmem/kfsd/PluginService.kt` | - | kfsd のプラグイン APK (別 APK で配る。libkfsd.so = GPL-2.0-or-later を載せる)。本体の公開版には入れない |
| `core/compat/tests/bench_write_io.cpp` | Apache-2.0 | 書き込みの I/O 形を測る個人版のベンチ |
| `core/compat/tests/test_f2fs_write.cpp` | GPL-2.0-or-later | 書き込みの試験 (GPL-2.0-or-later 表記)。F2FS の書き込み試験は個人版 (tools/f2fs_vm の実カーネル比較を使う)。必要になったら relicense へ |
| `core/fuse.cmake` | Apache-2.0 | tools/fuse/usbmem_fuse.cpp (GPL-2.0-or-later 表記) を参照する xfstests 用アダプター |
| `tools/fuse/fsck_ntfs.sh` | - | 自前の FUSE アダプター usbmem_fuse.cpp (GPL-2.0-or-later 表記。libfuse3 とリンク) と、xfstests の実行・期待値。公開版に FUSE の入口は無い |
| `tools/fuse/mount.fuse.usbmem` | - | 自前の FUSE アダプター usbmem_fuse.cpp (GPL-2.0-or-later 表記。libfuse3 とリンク) と、xfstests の実行・期待値。公開版に FUSE の入口は無い |
| `tools/fuse/run_xfstests.py` | - | 自前の FUSE アダプター usbmem_fuse.cpp (GPL-2.0-or-later 表記。libfuse3 とリンク) と、xfstests の実行・期待値。公開版に FUSE の入口は無い |
| `tools/fuse/smoke.sh` | - | 自前の FUSE アダプター usbmem_fuse.cpp (GPL-2.0-or-later 表記。libfuse3 とリンク) と、xfstests の実行・期待値。公開版に FUSE の入口は無い |
| `tools/fuse/usbmem_fuse.cpp` | GPL-2.0-or-later | 自前の FUSE アダプター usbmem_fuse.cpp (GPL-2.0-or-later 表記。libfuse3 とリンク) と、xfstests の実行・期待値。公開版に FUSE の入口は無い |
| `tools/fuse/xfstests_expected.json` | - | 自前の FUSE アダプター usbmem_fuse.cpp (GPL-2.0-or-later 表記。libfuse3 とリンク) と、xfstests の実行・期待値。公開版に FUSE の入口は無い |

### 公開版に入る「自前」のうち、カーネルへの言及があるもの (要レビュー: 8)

自前と書かれたファイルの中に、`fs/<名前>/`・「Linux カーネル」・「カーネルのソース」などの言及がある行数。読んだだけ (仕様の確認) か、写したかを人が判断する。

| ファイル | 行数 | 例 |
|---|--:|---|
| `android/app/src/main/java/org/usbmem/app/Edition.kt` | 1 | L7 * 公開版 (public) は GPL 系のコード (Linux カーネル由来のドライバなど) を一切含まないビルドで、扱えるファイルシステムがネイティブ層に |
| `core/compat/public/usbmem/btrfs_volume.h` | 1 | L2 // Btrfs のボリューム (自前のリーダー + 実験的な書き込み。Linux の fs/btrfs のコードは含まない: docs/btrfs-port. |
| `core/compat/public/usbmem/f2fs_volume.h` | 1 | L2 // F2FS のリーダー + 書き込み (書き込みは USBMEM_NTFS_WRITE ビルドの mount_rw だけ。オンディスク形式からの独自実装。L |
| `core/compat/src/btrfs_volume.cpp` | 1 | L3 // 読み取りに必要な部分だけを自前で持つ (トランザクション / 遅延参照 / ワークキューなどはない)。Linux の fs/btrfs のコードは含まない |
| `core/compat/src/f2fs_volume.cpp` | 1 | L3 // Linux の fs/f2fs のコードは含まない (docs/f2fs-port.md)。ブロックサイズ 4 KiB、1 セグメント 512 ブロックの |
| `tools/provenance_fetch.sh` | 2 | L3 # 出所の機械照合 (tools/provenance_scan.py、docs/provenance-btrfs-xfs.md) のために、Linux カーネ |
| `tools/provenance_scan.py` | 1 | L3 """自前の Btrfs / XFS / F2FS のコードが、Linux カーネルのコードの写しでないことを機械的に確かめる (docs/provenance |
| `tools/xfs_vm/kreplay.py` | 1 | L159 if n.endswith("/fs/xfs/xfs.ko.zst") or n.endswith("/fs/xfs/xfs.ko"): |

### 未分類 (どの規則にも当たらない = 要確認 = 除外: 0)

(なし)
<!-- /inventory:lists -->

**ユーザーの確認が要る**: これらを Apache-2.0 で出してよいか (自分のコードであること。`fs_volume.h` は FS ドライバが実装する共通インタフェースで、NTFS / exFAT の名前 `NtfsStat` を含むが、カーネルの構造体とは無関係)。出したくなければ manifest の `relicense` を `exclude` にすれば、公開版から外れ (F2FS とネストマウントも使えなくなる)、`license_inventory.py --check` が矛盾を検出する。

## 4. 公開版の定義 (`tools/public_manifest.txt`)

許可リスト。書式は同ファイルの冒頭: `include|relicense|exclude <出所> <glob>`、`overlay <出所> <元> <置き場所>` (公開版専用の雛形を所定の場所へ置く)、`scrub <出所> s!<正規表現>!<置換>!` (公開版のツリーにだけ効く置換。テスト用リポジトリのアカウント名を `<owner>` に)。

- **overlay**: `README.md` (公開版向けの下書き。「書き込みは実験的」の免責つき)、`LICENSE` (Apache-2.0 全文)、`NOTICE`、`.gitignore`、`.github/workflows/ci.yml` (公開版の CI)、`tools/ci/{host_tests,android_build}.sh`、`core/compat/CMakeLists.txt` (公開版の compat: FsVolume 共通 IF と F2FS だけ)。雛形は `tools/public_edition/`。
- **SPDX の付与**: 公開版に入る自前のソース (Kotlin・Python・CMake・XML・`.inc` など。70 ファイル程度) で SPDX が無いものには、書き出しで `SPDX-License-Identifier: Apache-2.0` の行を足す (個人版のファイルは変えない。コミットの衝突を避けるため)。ドキュメント (`.md`)・`LICENSE`・`NOTICE`・`.gitignore`・バイナリには付けない。
- **含めないもの**: `.pyc` / `__pycache__` (追跡されていないが、検査が確認する)、個人環境の絶対パス、テスト用リポジトリのアカウント名、メールアドレス、実機の検証セット・開発機専用のスクリプト。
- 公開版でも `core/CMakeLists.txt` などに、個人版の分岐の**コメント**や参照 (`usbmem_ntfs` を並べる行など。`if(USBMEM_EDITION STREQUAL "public")` の else 側) が残る。コードとしては評価されない。

## 5. エディションの切り替え (個人版の挙動は変えない)

| 層 | 仕組み |
|---|---|
| CMake (`core/`) | `-DUSBMEM_EDITION=personal\|public` (既定 `personal`)。`public` は `core/compat/` に公開版の小さな CMakeLists.txt (overlay) を使い、`core/fuse.cmake` / `fuseshim.cmake` を読み込まない。公開版の `core/compat/CMakeLists.txt` は `f2fs.cmake` / `btrfs.cmake` / `xfs.cmake` を **個人版と同じファイルのまま** include する (個人版は `usbmem_fsvol` を `usbmem_compat_rt` の ALIAS にして、リンク先を同じ名前にした)。`USBMEM_NTFS_WRITE` (書き込み経路。既定 OFF) も同じ名前のオプションを公開版の CMake が持つ。圧縮データの展開は core の自前の展開器 (`core/decomp.cmake`)。**個人版の ctest の一覧は main と同一** (書き込み OFF で 145、ON で 165。確認済み) |
| Gradle (`android/app`) | `-PusbmemEdition=personal\|public` (既定 `personal`)。`BuildConfig.EDITION`、公開版は `versionName` に `-public`、`-DUSBMEM_EDITION=public` を CMake へ |
| JNI (`android/app/src/main/cpp`) | `jni_edition.h` が FS ごとのマクロ `USBMEM_FS_*` を定める (個人版は全部 1、公開版は CMake が `USBMEM_FS_F2FS` / `USBMEM_FS_BTRFS` / `USBMEM_FS_XFS` を定義)。`jni_ntfs.cpp` の `mount_auto` は FS ごとに `#ifdef`。公開版で FS を判別できたがリンクしていないときは `EPROTONOSUPPORT` (93)、判別できなければ `EINVAL`。`nativeEditionInfo()` が `{"edition":..,"fs":[..]}` を返す |
| Kotlin / UI | `Edition.kt` (`isPublic`、`supports(fs)`。FS の一覧は実行時に `nativeEditionInfo()` で問い合わせる)。`UsbScreen` は、公開版で入っていない FS のパーティションに「この版では未対応です (対応: BTRFS / F2FS / XFS)」を出し、マウントのボタンを出さない。`MainActivity.fsRefusalNotice` は 93 を「この版では未対応のファイルシステム」の案内にする。暗号ボリュームの中の FS も同様 (`CryptoMount.innerRanges`)。個人版は `Edition.supports` が常に true |

## 6. 書き出しと検査

```
tools/license_inventory.py --check          # 定義の矛盾 (公開版に入るのに GPL 系・未分類・古い規則)
tools/export_public.sh /dev/shm/usbmem-public [--forbid-word <テスト用リポジトリの所有者名>]
   → license_inventory.py --check → export_public.py (書き出し) → check_public_tree.py (検査)
```

- 出力先は**新しい (または空の) ディレクトリで、リポジトリの外、tmpfs** (`/dev/shm` の下。Data ディスクには作らない)。CI など意図した場所だけ `--allow-any-fs`。履歴は引き継がない (`git init` もしない)。
- 内容は `git ls-files` の追跡ファイルの作業ツリーの内容 (**コミットしていない変更も入る**)。
- `check_public_tree.py <ツリー>` の検査: GPL / LGPL / AGPL の SPDX (WITH の例外つき・`A OR B` の片方を含む) が無い、`COPYING*` / `LICENSES/` / GPL 全文 / 冒頭の GPL 文面が無い、`third_party/` が無い、ソースの全ファイルに許可した SPDX がある、`.pyc` / `__pycache__` / `.git` / イメージ・APK・鍵の拡張子・個人のメモが無い、個人環境の絶対パス (`/home/` `/run/media/` `/Users/`) が無い、メールアドレスらしき文字列が無い、`<アカウント>/test-ci` が無い (`<owner>/test-ci` は可。`--forbid-word` で指定した語も)、根に LICENSE (Apache-2.0 全文)・NOTICE・README.md がある、`#include "…"` がツリー内で解決できる・CMake が参照する `tools/` のファイルがある (単独でビルドできることの静的な確認)。ネットワーク不要。

## 7. 公開用の新しい履歴の作り方 (**実際には作らない**)

個人版のリポジトリの履歴には、`.pyc` (開発機の絶対パスを含む。docs/release-audit.md 2.3 章)・GPL 系のコード・テスト用リポジトリのアカウント名が残っているので、**公開版は履歴を引き継がず、書き出したツリーから新しい履歴を作る**。

```
tools/export_public.sh /dev/shm/usbmem-public --forbid-word <テスト用リポジトリの所有者名>
cd /dev/shm/usbmem-public
git init -b main
git config user.name  <公開してよい名前>          # 個人のメールアドレスは使わない (noreply など)
git config user.email <公開してよいアドレス>
git add -A && git commit -m "Initial public edition"
# 公開前の最終確認 (リモートへ出す前に必ず):
python3 -I <個人版のリポジトリ>/tools/check_public_tree.py .            # 再検査
gitleaks detect --no-git --source . ; gitleaks detect --log-opts=--all   # 秘密情報 (gitleaks を入れていれば)
git log --stat | head                                                   # 履歴が 1 コミットだけ・.pyc / GPL が無いこと
# ここまでが、この作業の範囲。リモートの作成・push・可視性の変更は、ユーザーの判断で別に行う。
```

以降の公開版の更新は、個人版で変更 → 再び `export_public.sh` → 差分を公開用の履歴へ (`rsync` + `git add -A`) の流れ。公開版のコミットは個人版のコミットと 1 対 1 にならない。docs 中の個人版のコミットハッシュ・CI の run の参照は、公開版では意味を持たない (公開版に入れる docs は、その参照を含まないものだけ)。

## 8. CI

- `.github/workflows/public-edition.yml` (個人版のリポジトリ。トリガ: `workflow_dispatch`、ブランチ `usbmem-public` / `usbmem-public-<名前>` / `usbmem-ci-full-<名前>` への push): export → 検査 (リポジトリの所有者名の混入も) → **書き出したツリーだけで** ホスト (cmake + ninja + ctest。plain と san) と Android (`./gradlew :app:assembleDebug :app:testDebugUnitTest -PusbmemEdition=public`。`tools/ci/retry_gradle.sh` で包む) をビルド・テスト。APK のネイティブライブラリに、カーネル FS のシンボルが無く F2FS だけがあることも確認する。手順は雛形 `tools/public_edition/ci_{host,android}.sh` が持ち、公開版の `ci.yml` と共用する。
- 個人版への影響の確認は `usbmem-ci-full-<名前>` (host tests の 4 構成 + エミュレータ)。通常の `usbmem-ci-<名前>` では public-edition.yml は動かない (android-emulator.yml も動くので)。
- 結果 (run ID) は 9 章の末尾の表。

## 9. 公開前のチェックリスト (ユーザー用)

1. [x] (2026-11-23、ユーザーが一任) 3.5 章の relicense 8 ファイルを Apache-2.0 で出す。カーネルの `#include` ・著作権表記が無く、すべてこのリポジトリで書かれたことを確認した。3.4 章の除外のうち Btrfs / XFS は、2026-11-23 に出所を調べ直して公開版へ戻した (docs/provenance-btrfs-xfs.md。XFS の形式定義の出所の評価はユーザーの判断: 同ドキュメント 5 章)。
2. [x] (2026-11-23、ユーザーが一任) 公開版のライセンスは Apache-2.0、NOTICE の著作権者は個人名を入れず「android-usbmem contributors」で確定。kfsd (別配布の GPL-2.0 のプロセス) の説明を NOTICE に足した。以下は元の項目: `tools/public_edition/NOTICE` の著作権者の表記 (「android-usbmem contributors」の仮置き) と `README.md` を直す。
3. [x] (2026-11-23) 実行して OK。機種名は manifest の `scrub` で番号付きの端末名 (端末 A〜D) に置換し、`check_public_tree.py` の `devices` 検査で再混入を防ぐ。以下は元の項目: `tools/export_public.sh` を実行し、`check_public_tree.py` が OK であること。出力を目で見る (特に `docs/` の抜粋、機種名: F-51A・J9260・XQ-FS44・Ratoc などがコメントに残る。docs/release-audit.md 2.5 章)。
4. [ ] 公開版の履歴を、7 章の手順で新しく作る (個人版の履歴は公開しない)。
5. [ ] 公開先のアカウント名が決まったら、`docs` / `README` のリポジトリ参照を確認する (`<owner>` の伏せ字)。
6. [ ] gitleaks と `git grep` を公開用のクローンで再実行する (docs/release-audit.md 1 章)。
7. [ ] Play 配布を考えるなら、アプリ内の「オープンソースライセンス」画面 (AndroidX 等の Apache-2.0 の通知) と exFAT 特許・targetSdk の確認 (docs/licenses.md の未解決事項。公開版には exFAT ドライバが無いが、検査・mkfs はある)。
8. [x] (2026-11-23、ユーザーが一任) 個人版と同じ `org.usbmem.app` のまま (個人版の上書きインストールを壊さないため)。両方を入れたいときは `applicationIdSuffix` を足す。以下は元の項目: 公開版の APK の `applicationId` が個人版と同じ (`org.usbmem.app`)。同じ端末に両方を入れたい場合は `applicationIdSuffix` を足す (今は足していない。署名鍵が違うと上書きインストールできない)。
9. [ ] `main` だけを push する (他のブランチ・個人版の履歴は公開しない)。

## 10. 残る課題

- **F2FS の圧縮ファイル / Btrfs / XFS** (2026-11-23 に対応済み): 公開版にも入れた。圧縮データの展開は自前の展開器 (docs/decomp.md。zstd を upstream の同梱にしなかった理由もそこ)、出所の機械照合は docs/provenance-btrfs-xfs.md。書き込みは個人版と同じ「実験的・既定 OFF」。公開版の CI に `write` 構成 (書き込み有効の Release で Btrfs / XFS / F2FS と展開器の試験を回す) を足した。
- **公開版の計装テスト** (エミュレータ): F2FS・暗号・パーティション編集・UI の未対応 FS の案内。今は JVM 単体テストと、APK のシンボル検査だけ。
- **公開版の docs の整備**: 今は機能に関する docs の一部だけ。カーネル FS への言及が混ざる。
- 個人版の `core/compat/f2fs.cmake` の書き込みテストは `test_f2fs_write.cpp` が無いと入らない条件を足した (公開版が書き込み有効でも configure が通るように)。
- `docs/licenses.md` を実態に合わせた (別途)。`docs/release-audit.md` 3.2 章に「決定: エディション分離」を反映した。

## 11. CI の結果 (2026-11-21、kawanakaiku/test-ci)

公開版 (`public-edition.yml`):

| run | ブランチ / コミット | 結果 |
|---|---|---|
| 44905193611 | `usbmem-public-editions` c63c3a5 | 失敗 (取り消し)。android: `UsbScreen.kt` の `Edition` の import 漏れ (**個人版も同じ漏れ。個人版の UI を直した**)。host: 公開版のテストが使う `gen_ext2_fixture.py` (gen_f2fs_fixture.py が共用) と `gen_ntfs_fixture.sh` が公開版に無かった |
| 44906176223 | 8cb6da7 | 失敗 (export の検査が closure で NG: 説明文中のスクリプト名を拾った。検査の正規表現を直した) |
| 44906274483 | 77d3894 | android 成功。host (plain) は `disktools_tools` が失敗 (ntfs-3g のフィクスチャの `fusermount3 -u` が AppArmor に拒否された) |
| 44907419036 | 33eee57 | **全ジョブ成功** (export + check / android / host plain 18 件 / host san) |
| 44911581265 | cae3de8 (最終) | **全ジョブ成功** |

個人版 (回帰の確認。`usbmem-ci-full-editions` 33eee57): Host tests run 44909403038 (plain / san / wsan / wrel の 4 構成すべて成功)、Android emulator tests run 44909402986 (成功)、public-edition run 44909402984 (成功)。ctest の一覧は main と同一 (ローカルで書き込み OFF 145 件・ON 165 件を確認済み)。cae3de8 は tools / manifest / docs だけの変更で、個人版のビルドには影響しない。

## 12. 別ブランチの機能が個人版へ統合されたときの公開版の扱い (例: wimfuse)

**wimfuse (wimlib の別プロセス化、`usbmem-wt-wimlib`) は 2026-11-22 に統合し、下の 1〜5 を対応済み** (`usbmem-wt-wimint`。12.1 に中身と CI の結果)。以下は、次に別ブランチの機能を統合するときの手順として残す。

公開版は許可リストなので、統合された新しいファイルは、`include` の glob (主に `android/app/src/main/java/**`、`core/src/**`、`core/include/**`) に当たるものだけが自動で公開版に入る。**GPL / LGPL を動かす層が、これらの glob に当たる場所に入ると、そのまま公開版へ入ってしまう**ので、統合の前後に次を行う。

1. **除外の追加**: `tools/public_manifest.txt` に `exclude`、またはまだ無いファイルなら先回りの `reserve` (当たるファイルが無くても古い規則と見なされない) で、パスを足す。**規則は上から先に当たる**ので、`android/app/src/main/java/**` の `include` より前に置く。wimlib (`usbmem-wt-wimlib`) の分は、先回りで入れてある: `android/app/src/main/java/org/usbmem/app/wimfuse/**`、`core/fuseshim/wimfuse/**`、`tools/wimfuse/**`、`.github/workflows/wimfuse-build.yml`。統合で場所が変わったら、パスを直す。
2. **参照の差し替え**: 公開版に残るファイルが除外したパッケージを参照すると、公開版のビルドが通らない (`check_public_tree.py` が「公開版に無いパッケージへの参照: org.usbmem.app.<名前>」で検出する)。`VolumeRegistry` / `MainActivity` / `NestedMountSection` / `WimFuseNative.kt` などの参照は、公開版のツリーでだけ別の実装 (`WimFuse.available()` が常に false のスタブ) に差し替える。方法は、`tools/public_edition/` にスタブを置いて `overlay` する。参照元のファイルが個人版と公開版で違ってよいなら、そのファイルも overlay にする (参照元を変えたくないなら、スタブのクラスを同じパッケージ名・同じシグネチャで作る)。
3. **JNI**: `WimFuseNative.kt` が宣言する `external fun` (`nativeOpenArchive` など) は、公開版のネイティブライブラリに無いので、呼ぶと実行時に `UnsatisfiedLinkError` になる。スタブ (上) へ置き換えるか、Kotlin 側を `Edition.isPublic` で呼ばないようにする。ネイティブ側は `-PusbmemWimFuse` のときだけ入る (公開版では指定しない)。
4. **AndroidManifest**: `:wimfuse` サービスの宣言は、`android/app/src/main/AndroidManifest.xml` に入る。除外したクラスを指すサービスが残らないよう、`scrub` (サービス要素の削除) か、公開版用のマニフェストの overlay を足す。`tools/check_manifest_permissions.py` (権限の確認) は公開版でも動く。
5. **確認**: `tools/license_inventory.py --check` (規則の矛盾)、`tools/export_public.sh` (検査)、公開版の CI (`public-edition.yml`、ブランチ `usbmem-public-<名前>`) が通ること。

新しい `third_party/*` や `core/compat/*` の追加は、`include` に当たらない (未分類 = 除外) ので、何もしなくても公開版には入らない。

### 12.1 wimfuse の対応 (2026-11-22、対応済み)

1. **除外**: 先回りの `reserve` (wimfuse の Kotlin パッケージ・`tools/wimfuse/**` ほか) が、統合後のパスにそのまま当たった (パスの修正は不要)。統合後は `exclude` に直した。`core/fuseshim/wimfuse/**` と `wimfuse-build.yml` は、元からある `core/fuseshim/**` と `.github/**` の除外が先に当たるので、`reserve` の行は消した (残すと「古い規則」の NG になる)。
2. **参照の差し替え**: `tools/public_edition/WimFuseStub.kt` を `overlay` で `org/usbmem/app/wimfuse/WimFuseStub.kt` に置く。`WimFuse.available(ctx)` は常に false、`WimInfo` / `WimImageInfo` / `WimFuseException` (UI が使う型) は個人版と同じ形、`info` / `open` / `openArchive` は ENOSYS の `WimFuseException` を投げる (`available()` が false なので `VolumeRegistry.inspectWim` が null を返し、WIM の画面・マウントには入らない)。参照元 (`VolumeRegistry` / `MainActivity` / `NestedMountSection`) は個人版と同じファイルで、公開版でも変えていない。
3. **JNI**: `WimFuseNative.kt` は公開版に入らず、スタブは `System.loadLibrary` も `external fun` も持たない (`UnsatisfiedLinkError` の経路が無い)。ネイティブ側 (`core/fuseshim/wimfuse/android/`) も公開版に無い。
4. **スイッチ**: `android/app/build.gradle.kts` は、公開版 (`-PusbmemEdition=public`) では `-PusbmemWimFuse` を警告つきで無視する (`BuildConfig.WIMFUSE_SUPPORTED` は false、`-DUSBMEM_WIMFUSE=ON` は渡さない、`jniLibs` も足さない)。`android/app/src/main/cpp/CMakeLists.txt` は `USBMEM_WIMFUSE AND NOT USBMEM_EDITION STREQUAL "public"` のときだけ `client.cmake` を読む。
5. **AndroidManifest**: `scrub` (manifest の末尾) で、`:wimfuse` の `<service>` を直前のコメントごと、公開版のツリーにだけ外す。`check_public_tree.py` が、公開版の manifest に `WimFuseService` / `:wimfuse` が残っていれば NG (scrub の正規表現が manifest の書き方の変更で外れたときの番人)。
6. **確認** (ローカル): `license_inventory.py --check` OK、`export_public.sh` (/dev/shm へ書き出し → 検査) OK (248 ファイル。GPL 系なし・SPDX あり・個人情報なし・単独で解決可能)。公開版の manifest に service なし、build.gradle.kts / CMakeLists.txt の wimfuse の記述はスイッチと無効化の注記だけ。

CI の結果は 12.2。

### 12.2 CI の結果 (2026-11-22、kawanakaiku/test-ci)

| run | ブランチ / コミット | 結果 |
|---|---|---|
| 44920520646 | `usbmem-public-wimint` a0ca0ad | **全ジョブ成功** (export + check / public android / host plain / host san。約 24 分) |
| 44922993079 | `usbmem-public-wimint` 7de6cf0 (manifest の reserve → exclude の整理と docs。以降は docs だけ) | **全ジョブ成功** |
| 44920529925 | `usbmem-ci-full-wimint` a0ca0ad | public-edition 成功 |

個人版 (回帰の確認、`usbmem-ci-full-wimint` a0ca0ad): Host tests run 44920529807 (plain / san / wsan / wrel の 4 構成すべて成功、117 分)、Android emulator tests run 44920529823 (全 7 グループ成功。`wim-fuse` は 52 件成功。初回の `ro-misc` の `DirtyConfirmUiTest.image_dialogNamesTheImage_andScrollsInLandscape` の 1 件は散発の UI 試験で、`gh run rerun --failed` で成功。docs/ci.md 末尾)。a0ca0ad 以降のコミットは manifest のコメントと docs だけ。

## 13. kfsd (GPL 系ドライバを別プロセスに分ける。2026-11-22、docs/kfsd.md)

公開版の本体は GPL 系のコードを一切含まず、カーネル由来のドライバは別の実行ファイル `kfsd` (GPL-2.0、ソースは別に公開) に載せて、**RPC だけ**で繋ぐ。公開版に入るのは次のファイルだけ (`tools/public_manifest.txt` の core/kfsd の節)。

| ファイル | 内容 | ライセンス |
|---|---|---|
| `core/kfsd/include/usbmem/kfsd_proto.h` | RPC の仕様 (ヘッダ + docs/kfsd.md) | Apache-2.0 |
| `core/kfsd/include/usbmem/kfsd_client.h`・`src/kfsd_client.cpp` | `RpcVolume` (`FsVolume` を RPC で実装。kfsd を起こす・監視する・止める) | Apache-2.0 |
| `core/kfsd/include/usbmem/kfsd_server.h`・`src/kfsd_server.cpp`・`src/kfsd_io.h` | kfsd 側の共通部 (RPC のサーバ・デバイスの callback の `BlockDevice`)。GPL のドライバは含まない | Apache-2.0 |
| `core/kfsd/tests/test_kfsd_proto.cpp`・`core/kfsd/CMakeLists.txt` | プロトコルの試験 (MemFs ベース。kfsd の実行ファイル無しで動く) | Apache-2.0 |

公開版に**入らない** (個人版 / kfsd の配布物だけ): `core/kfsd/src/kfsd_mount.{h,cpp}` (FS の判別とマウント = カーネル由来のドライバを呼ぶ)、`kfsd_main.cpp` (kfsd の実行ファイル)、`tests/test_kfsd_diff.cpp`・`bench_kfsd.cpp` (プロセス内との差分試験・性能測定)。
公開版の JNI (`jni_ntfs.cpp`) は、kfsd の扱う FS を判別できたら ENOPKG (65) を返し、UI は「この FS には kfsd (プラグイン) が必要です」を出す。Android で kfsd (`libkfsd.so` / プラグイン APK) へ繋ぐ層は段階 3 (docs/kfsd.md 8 章)。
`tools/license_inventory.py` に kfsd のコンポーネントを足し、`check_public_tree.py` のインクルード走査に `core/kfsd/include`・`core/kfsd/src` を足した (公開版の閉包の検査)。公開版の CI は `usbmem-ci-full-kfsd` で確認 (docs/ci.md)。

公開版と kfsd (段階 3): 公開版の JNI (`jni_kfsd.cpp`) は kfsd へつなぐ入口を持ち、既定は「kfsd 経由」(F2FS だけ自前でプロセス内)。本体は別 APK のプラグイン (`org.usbmem.kfsd`、GPL-2.0。`android/kfsd-plugin/`。公開版のツリーには入れない) を、署名が同じ場合だけ bound service で起こして使う。
プラグインが無いと ENOPKG (65) で、UI は「この FS には kfsd (プラグイン) が必要です」を出す。公開版のツリーに入る kfsd 関連: `android/kfsd-common/**` (起こす共通部。Apache-2.0)、`android/app/src/main/java/org/usbmem/app/kfsd/**`、`jni_kfsd.{h,cpp}`、`core/kfsd` の Apache-2.0 の部分。
公開版 CI (run 44960382936) で成功。個人版 (`-PusbmemKfsd=true`) は libkfsd.so を APK に持つ (GPL。配らない)。

