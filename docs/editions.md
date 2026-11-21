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
| `.github (CI)` | 12 | (なし) 11, Apache-2.0 1 | dev 11, own 1 | 一部だけ入れる (入る 1 / 除外 11) | 1 | 個人版の CI |
| `ルート` | 6 | (なし) 6 | own 4, dev 2 | 一部だけ入れる (入る 4 / 除外 2) | 4 | README など |
| `android: ビルド設定・リソース・ラッパー` | 20 | (なし) 18, Apache-2.0 2 | own 19, external-permissive 1 | 公開版に入れる | 20 | Gradle / マニフェスト / リソース / Gradle ラッパー (Apache-2.0) |
| `android: 計装テスト` | 34 | (なし) 34 | dev 34 | 公開版から除外 | 0 | 実機・エミュレータ用。カーネル FS のフィクスチャ・個人の機材を前提にする |
| `android: JNI` | 11 | Apache-2.0 7, GPL-2.0-or-later 3, (なし) 1 | own 8, own-gpl 3 | 公開版に入れる (SPDX を Apache-2.0 へ書き換え 3) | 11 | 自前の JNI (FS の分岐は USBMEM_FS_* の条件コンパイル) |
| `android: Kotlin アプリ` | 35 | (なし) 35 | own 35 | 公開版に入れる | 35 | 自前の Kotlin。AndroidX / Compose (Apache-2.0) に依存 |
| `android: JVM 単体テスト` | 8 | (なし) 8 | own 8 | 公開版に入れる | 8 | JUnit |
| `core/*.cmake, core/CMakeLists.txt` | 7 | Apache-2.0 7 | own 5, own-gpl 1, external-gpl 1 | 一部だけ入れる (入る 5 / 除外 2) | 5 | 自前: ビルド定義 |
| `core/compat: ビルド定義` | 8 | Apache-2.0 8 | kernel 7, own 1 | 一部だけ入れる (入る 1 / 除外 7) | 1 | 各カーネル FS のビルド定義 |
| `core/compat: F2FS (自前)` | 10 | Apache-2.0 8, GPL-2.0-or-later 2 | own 8, own-gpl 2 | 一部だけ入れる (入る 8 / 除外 2) | 8 | 自前: オンディスク形式から書き起こした読み書き。圧縮の展開だけ個人版はカーネル lib |
| `core/compat: LinuxCompat ヘッダ` | 165 | (なし) 93, GPL-2.0-or-later 68, GPL-2.0 3, GPL-2.0 WITH Linux-syscall-note 1 | kernel-shim 165 | 公開版から除外 | 0 | カーネルの内部 API の再実装 (<linux/*.h>) |
| `core/compat: Btrfs / XFS (自前だが GPL 表記)` | 12 | GPL-2.0-or-later 12 | own-gpl 12 | 公開版から除外 | 0 | 自前の読み書き。SPDX が GPL-2.0-or-later (カーネルの lib・形式定義への依存) のため公開版から除外 |
| `core/compat: カーネル FS ドライバのラッパ・公開ヘッダ` | 28 | GPL-2.0-or-later 28 | kernel 28 | 公開版から除外 | 0 | NTFS / exFAT / ext2 / ext4 / FAT / ISO 9660 / UDF / squashfs / erofs を呼ぶ薄い層 (GPL-2.0-or-later) |
| `core/compat: FsVolume 共通 IF・ネストマウント` | 5 | GPL-2.0-or-later 5 | own-gpl 5 | 公開版に入れる (SPDX を Apache-2.0 へ書き換え 5) | 5 | 自前: FS 共通のインタフェースと、その上のネストマウント (GPL 表記は安全側。公開版では Apache-2.0 へ) |
| `core/compat: LinuxCompat 実行時` | 20 | GPL-2.0-or-later 19, Apache-2.0 1 | kernel-shim 20 | 公開版から除外 | 0 | inode / dentry / ページキャッシュ / bio / buffer_head / jbd2 / VFS の再現 |
| `core/compat: カーネル FS / LinuxCompat のテスト` | 25 | GPL-2.0-or-later 22, Apache-2.0 3 | kernel 24, own 1 | 公開版から除外 | 0 | 上のドライバのテスト (GPL-2.0-or-later 表記が大半) |
| `core/fuseshim (ntfs-3g / wimlib)` | 13 | GPL-2.0-or-later 13 | external-gpl 13 | 公開版から除外 | 0 | 外部 GPL / LGPL を動かす試作層 (GPL-2.0-or-later) |
| `core/include` | 26 | Apache-2.0 26 | own 26 | 公開版に入れる | 26 | 自前: 公開 API ヘッダ |
| `core/src` | 58 | Apache-2.0 58 | own 58 | 公開版に入れる | 58 | 自前: パーティション表・ディスク編集・USB MSC / SCSI・暗号 (AES / SHA / HMAC / PBKDF2 / XTS / Argon2)・LUKS1/2・BitLocker・QCOW2 / VHD / VHDX / VMDK / NBD・DiskTools (fsck / mkfs)・救出 / バックアップ |
| `core/tests` | 28 | Apache-2.0 27, (なし) 1 | own 27, generated 1 | 公開版に入れる | 28 | 自前: 上のテスト (暗号ベクタは公開値) |
| `docs` | 47 | (なし) 47 | own-doc 47 | 一部だけ入れる (入る 13 / 除外 34) | 13 | ドキュメント (公開版に入るのは公開版の機能に関するものだけ) |
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
| `tools` | 158 | (なし) 127, Apache-2.0 30, GPL-2.0-or-later 1 | dev 128, own 24, kernel-patch 3, kernel-shim 2, external-gpl 1 | 一部だけ入れる (入る 24 / 除外 134) | 24 | テスト・フィクスチャ生成・CI の補助 (公開版に入るのは公開版のテストが使うものだけ) |
| `tools: FUSE アダプター・fuseshim・xfstests` | 9 | (なし) 8, GPL-2.0-or-later 1 | own-gpl 6, external-gpl 3 | 公開版から除外 | 0 | GPL 表記の自前アダプターと、ntfs-3g / wimlib の取得・試験 |
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
| Btrfs | 自前のリーダー / ライター (`btrfs_volume.cpp` ほか)。**ただし SPDX は GPL-2.0-or-later**、圧縮の展開はカーネルの lib (`btrfs_decomp.c`)、構造体の並びは uapi ヘッダ (`btrfs_tree.h`) を参照 | GPL-2.0-or-later 表記 | **除外 (安全側)** | 要確認。自前と確かめて SPDX を直し、展開を差し替えれば入れられる (10 章) |
| XFS | 自前のリーダー / ライター (`xfs_volume.cpp` ほか)。**`fs/xfs` の形式定義 (xfs_format.h など。GPL-2.0) を仕様として読んで書いた**とコードの冒頭に明記 | GPL-2.0-or-later 表記 | **除外 (安全側。派生の疑い)** | 「独立に実装」とあるが、`fs/xfs` は uapi ではなく GPL-2.0 のため、除外側に倒した |
| **F2FS** | **自前** (`f2fs_volume.cpp` + `f2fs_write*.inc`)。オンディスク形式から書き起こした。`fs/f2fs` のコードは写していない (docs/f2fs-port.md。カーネルのソースは構造体のレイアウトを確かめる仕様書として読んだだけ) | Apache-2.0 | **入れる** | 圧縮クラスタの展開 (lz4 / lzo / zstd) だけカーネルの lib なので、公開版は `f2fs_decomp_stub.c` (展開せず `EOPNOTSUPP`)。圧縮されていないファイルは読める |
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
| F2FS | 3.2 章 | |
| Android アプリ | `android/app/src/main/java` (Kotlin)、`cpp` (JNI)、`res` | 自前。AndroidX / Compose / Material3 / Kotlin 標準ライブラリ (Apache-2.0) に依存 (ビルド時に Maven から取得。リポジトリには無い)。Gradle ラッパーの jar (Apache-2.0) だけが外部のバイナリ |

### 3.4 判断に迷って除外側に倒したもの

| 対象 | 理由 | 戻すには |
|---|---|---|
| Btrfs (`btrfs_*`) | 自前だが SPDX が GPL-2.0-or-later。展開はカーネルの lib。形式は uapi の `btrfs_tree.h` を参照 | 自前であることの確認 → SPDX を Apache-2.0 へ → 展開を zlib / 自前デコーダに差し替え → manifest の `exclude` を `relicense` に |
| XFS (`xfs_*`) | `fs/xfs` の形式定義 (GPL-2.0) を仕様として読んだと明記。SPDX も GPL-2.0-or-later | 同上 (ただし派生かどうかの判断が必要。ユーザーが著作権者として判断する) |
| `core/compat/tests/test_f2fs_write.cpp` | SPDX が GPL-2.0-or-later (自前のテスト)。公開版の CI は読み取りビルドなので不要 | `relicense` に移し、f2fs の書き込みテストを CI に足す |
| `core/compat/tests/bench_write_io.cpp` | btrfs も測る個人版のベンチ | — |
| `docs/` の大半 | カーネル FS の移植記録・実機の検証記録・CI・監査・TODO。個人の機材 (機種名・テスト用リポジトリのアカウント名) を含む | 公開用に書き直す |
| `android/app/src/androidTest/**` | カーネル FS のフィクスチャと実機 (個人の USB メモリ) を前提にする | F2FS / 暗号 / パーティション編集の計装テストだけを選んで入れる |
| `tools/` の大半 | カーネル FS・NTFS・実機・開発機のパス (カーネルパッケージ・VM) に依存 | 必要になったものから入れる |

「自前」と書かれていても Linux カーネル (や ntfs-3g) の記述に倣った可能性があるものは、`license_inventory.py --lists` の「カーネルへの言及があるもの」で一覧する (現状の公開版の対象は F2FS の「写していない」という否定の記述だけ)。

### 3.5 自前だが GPL 表記で、公開版では Apache-2.0 として出すもの (relicense)

次の 8 ファイルは、個人版でカーネル由来の FS ドライバと同じライブラリに入るため SPDX が GPL-2.0-or-later だが、**中身は自前の API 定義・薄い層で、カーネルのコードを含まない** (git の履歴の作者は 1 種類、コードはカーネルのヘッダ・実装を参照していない)。著作権者 (ユーザー) は自分のコードを別のライセンスでも出せるので、書き出しの際に SPDX の行だけを Apache-2.0 に書き換える (個人版のファイルは変えない)。

<!-- inventory:lists -->
### SPDX を Apache-2.0 へ書き換えて出すもの (relicense: 8)

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

### 自前だが公開版から外すもの (22)

| ファイル | SPDX | 理由 |
|---|---|---|
| `core/compat/public/usbmem/btrfs_volume.h` | GPL-2.0-or-later | 上と同じ |
| `core/compat/public/usbmem/xfs_volume.h` | GPL-2.0-or-later | 上と同じ |
| `core/compat/src/btrfs_decomp.c` | GPL-2.0-or-later | カーネルの展開ライブラリ (zlib / lzo / zstd。GPL 系) を呼ぶラッパ |
| `core/compat/src/btrfs_volume.cpp` | GPL-2.0-or-later | 自前の Btrfs リーダー / ライター。ただし SPDX は GPL-2.0-or-later、展開はカーネルの lib (btrfs_decomp.c)、形式は uapi ヘッダを参照 → 要確認 = 除外 (docs/btrfs-port.md) |
| `core/compat/src/btrfs_write.inc` | GPL-2.0-or-later | 上と同じ (書き込みの実装) |
| `core/compat/src/btrfs_write_ops.inc` | GPL-2.0-or-later | 上と同じ (書き込みの実装) |
| `core/compat/src/f2fs_decomp.c` | GPL-2.0-or-later | カーネルの展開ライブラリ (usbmem_kdecomp。GPL 系) を呼ぶラッパ |
| `core/compat/src/xfs_volume.cpp` | GPL-2.0-or-later | 自前の XFS リーダー / ライター。ただし fs/xfs の形式定義 (xfs_format.h など GPL-2.0) を仕様として読んで書いたと明記 → 派生の疑い = 除外 (docs/xfs-port.md) |
| `core/compat/src/xfs_write.inc` | GPL-2.0-or-later | 上と同じ (書き込みの実装) |
| `core/compat/src/xfs_write_alloc.inc` | GPL-2.0-or-later | 上と同じ (書き込みの実装) |
| `core/compat/src/xfs_write_full.inc` | GPL-2.0-or-later | 上と同じ (書き込みの実装) |
| `core/compat/src/xfs_write_node.inc` | GPL-2.0-or-later | 上と同じ (書き込みの実装) |
| `core/compat/src/xfs_write_ops.inc` | GPL-2.0-or-later | 上と同じ (書き込みの実装) |
| `core/compat/tests/bench_write_io.cpp` | Apache-2.0 | btrfs (GPL 表記) も測る個人版のベンチ |
| `core/compat/tests/test_f2fs_write.cpp` | GPL-2.0-or-later | 書き込みの試験 (GPL-2.0-or-later 表記)。公開版の CI は読み取りビルドのため。必要になったら relicense へ |
| `core/fuse.cmake` | Apache-2.0 | tools/fuse/usbmem_fuse.cpp (GPL-2.0-or-later 表記) を参照する xfstests 用アダプター |
| `tools/fuse/fsck_ntfs.sh` | - | 自前の FUSE アダプター usbmem_fuse.cpp (GPL-2.0-or-later 表記。libfuse3 とリンク) と、xfstests の実行・期待値。公開版に FUSE の入口は無い |
| `tools/fuse/mount.fuse.usbmem` | - | 自前の FUSE アダプター usbmem_fuse.cpp (GPL-2.0-or-later 表記。libfuse3 とリンク) と、xfstests の実行・期待値。公開版に FUSE の入口は無い |
| `tools/fuse/run_xfstests.py` | - | 自前の FUSE アダプター usbmem_fuse.cpp (GPL-2.0-or-later 表記。libfuse3 とリンク) と、xfstests の実行・期待値。公開版に FUSE の入口は無い |
| `tools/fuse/smoke.sh` | - | 自前の FUSE アダプター usbmem_fuse.cpp (GPL-2.0-or-later 表記。libfuse3 とリンク) と、xfstests の実行・期待値。公開版に FUSE の入口は無い |
| `tools/fuse/usbmem_fuse.cpp` | GPL-2.0-or-later | 自前の FUSE アダプター usbmem_fuse.cpp (GPL-2.0-or-later 表記。libfuse3 とリンク) と、xfstests の実行・期待値。公開版に FUSE の入口は無い |
| `tools/fuse/xfstests_expected.json` | - | 自前の FUSE アダプター usbmem_fuse.cpp (GPL-2.0-or-later 表記。libfuse3 とリンク) と、xfstests の実行・期待値。公開版に FUSE の入口は無い |

### 公開版に入る「自前」のうち、カーネルへの言及があるもの (要レビュー: 3)

自前と書かれたファイルの中に、`fs/<名前>/`・「Linux カーネル」・「カーネルのソース」などの言及がある行数。読んだだけ (仕様の確認) か、写したかを人が判断する。

| ファイル | 行数 | 例 |
|---|--:|---|
| `android/app/src/main/java/org/usbmem/app/Edition.kt` | 1 | L7 * 公開版 (public) は GPL 系のコード (Linux カーネル由来のドライバなど) を一切含まないビルドで、扱えるファイルシステムがネイティブ層に |
| `core/compat/public/usbmem/f2fs_volume.h` | 1 | L2 // F2FS のリーダー + 書き込み (書き込みは USBMEM_NTFS_WRITE ビルドの mount_rw だけ。オンディスク形式からの独自実装。L |
| `core/compat/src/f2fs_volume.cpp` | 1 | L3 // Linux の fs/f2fs のコードは含まない (docs/f2fs-port.md)。ブロックサイズ 4 KiB、1 セグメント 512 ブロックの |

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
| CMake (`core/`) | `-DUSBMEM_EDITION=personal\|public` (既定 `personal`)。`public` は `core/compat/` に公開版の小さな CMakeLists.txt (overlay) を使い、`core/fuse.cmake` / `fuseshim.cmake` を読み込まない。`core/compat/f2fs.cmake` は `usbmem_kdecomp` (カーネルの展開器) があれば従来どおり、無ければ `f2fs_decomp_stub.c` と `usbmem_fsvol` に切り替わる。**個人版の ctest の一覧は main と同一** (書き込み OFF で 145、ON で 165。確認済み) |
| Gradle (`android/app`) | `-PusbmemEdition=personal\|public` (既定 `personal`)。`BuildConfig.EDITION`、公開版は `versionName` に `-public`、`-DUSBMEM_EDITION=public` を CMake へ |
| JNI (`android/app/src/main/cpp`) | `jni_edition.h` が FS ごとのマクロ `USBMEM_FS_*` を定める (個人版は全部 1、公開版は CMake が `USBMEM_FS_F2FS` だけ定義)。`jni_ntfs.cpp` の `mount_auto` は FS ごとに `#ifdef`。公開版で FS を判別できたがリンクしていないときは `EPROTONOSUPPORT` (93)、判別できなければ `EINVAL`。`nativeEditionInfo()` が `{"edition":..,"fs":[..]}` を返す |
| Kotlin / UI | `Edition.kt` (`isPublic`、`supports(fs)`。FS の一覧は実行時に `nativeEditionInfo()` で問い合わせる)。`UsbScreen` は、公開版で入っていない FS のパーティションに「この版では未対応です (対応: F2FS)」を出し、マウントのボタンを出さない。`MainActivity.fsRefusalNotice` は 93 を「この版では未対応のファイルシステム」の案内にする。暗号ボリュームの中の FS も同様 (`CryptoMount.innerRanges`)。個人版は `Edition.supports` が常に true |

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

1. [ ] 3.5 章の relicense 8 ファイルを Apache-2.0 で出してよいか確認する。3.4 章の除外 (Btrfs / XFS など) を戻すか決める。
2. [ ] 公開版のライセンスを確定する (既定 Apache-2.0)。`tools/public_edition/NOTICE` の著作権者の表記 (「android-usbmem contributors」の仮置き) と `README.md` を直す。
3. [ ] `tools/export_public.sh` を実行し、`check_public_tree.py` が OK であること。出力を目で見る (特に `docs/` の抜粋、機種名: F-51A・J9260・XQ-FS44・Ratoc などがコメントに残る。docs/release-audit.md 2.5 章)。
4. [ ] 公開版の履歴を、7 章の手順で新しく作る (個人版の履歴は公開しない)。
5. [ ] 公開先のアカウント名が決まったら、`docs` / `README` のリポジトリ参照を確認する (`<owner>` の伏せ字)。
6. [ ] gitleaks と `git grep` を公開用のクローンで再実行する (docs/release-audit.md 1 章)。
7. [ ] Play 配布を考えるなら、アプリ内の「オープンソースライセンス」画面 (AndroidX 等の Apache-2.0 の通知) と exFAT 特許・targetSdk の確認 (docs/licenses.md の未解決事項。公開版には exFAT ドライバが無いが、検査・mkfs はある)。
8. [ ] 公開版の APK の `applicationId` が個人版と同じ (`org.usbmem.app`)。同じ端末に両方を入れたい場合は `applicationIdSuffix` を足す (今は足していない。署名鍵が違うと上書きインストールできない)。
9. [ ] `main` だけを push する (他のブランチ・個人版の履歴は公開しない)。

## 10. 残る課題

- **F2FS の圧縮ファイルの読み取り** (公開版は未対応): lz4 は `core/src/backup_lz4.cpp` の自前の LZ4 デコーダで足りる可能性がある (F2FS の lz4 クラスタは lz4 のブロック形式)。lzo / zstd は寛容なライセンスの実装を自前で書くか、BSD 系の公開実装を取り込む (zstd は GPL-2.0 OR BSD-3-Clause の BSD 側。`third_party/kernel-lib` の個々のファイルのライセンスは精査が必要)。
- **Btrfs / XFS を公開版へ**: 3.4 章。Btrfs は自前の確認と展開の差し替え (zlib はシステムライブラリで足りる。lzo / zstd は上と同じ)。
- **公開版の計装テスト** (エミュレータ): F2FS・暗号・パーティション編集・UI の未対応 FS の案内。今は JVM 単体テストと、APK のシンボル検査だけ。
- **公開版の docs の整備**: 今は機能に関する docs の一部だけ。カーネル FS への言及が混ざる。
- 個人版の `core/compat/f2fs.cmake` の書き込みテストは `test_f2fs_write.cpp` が無いと入らない条件を足した (公開版が書き込み有効でも configure が通るように)。
- `docs/licenses.md` を実態に合わせた (別途)。`docs/release-audit.md` 3.2 章に「決定: エディション分離」を反映した。

## 11. CI の結果

(実行後に記入)
