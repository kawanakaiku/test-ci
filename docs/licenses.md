# ライセンス・依存関係の記録

| コンポーネント | 用途 | ライセンス | 状態 |
|---|---|---|---|
| 本プロジェクトの自前コード(core/src, core/include, core/tests, android/ ほか) | — | Apache-2.0(SPDX 表記。公開版はこれで出す。個人版は GPL 系と同居するので配布しない) | **エディション分離 (docs/editions.md)**: 公開版は GPL 系を含まない |
| AndroidX / Compose / Material3 | UI | Apache-2.0 | Phase 1 |
| SMBJ 0.15.0 (`com.hierynomus:smbj`) と、その依存: `org.bouncycastle:bcprov-jdk18on` 1.85.2、`org.slf4j:slf4j-api` 2.0.18、`net.engio:mbassador` 1.3.2、`com.hierynomus:asn-one` 0.6.0 | SMB (ネットワーク共有) のクライアント (docs/smb.md)。Maven Central から解決、リポジトリにバイナリを置かない | SMBJ = Apache-2.0、asn-one = Apache-2.0、bcprov = MIT (POM の名前は "Bouncy Castle Licence"、jar の `META-INF/LICENSE.md` の本文は MIT)、slf4j-api = MIT、mbassador = MIT。**GPL / LGPL は無い** (POM と jar の LICENSE で確認、2026-11-23)。LGPL の libsmb2 / jcifs-ng は採用しない | 追加 (公開版に含める。NOTICE に記載。SMB1 は話せない・認証は NTLM のみ) |
| org.json (JVM 単体テストのみ) | テスト | JSON License(テスト依存のみ、配布物に含めない) | Phase 1 |
| libfdisk (util-linux) | パーティション編集 | LGPL-2.1+ | **採用しない** (自前実装。理由は docs/partition-edit.md 9 章) |
| Linux `fs/ntfs` (NTFS Resurrection。`third_party/ntfs-resurrection`、無改変。`$MFTMirr` のパッチはビルド時の作業コピーに当てる) | NTFS の読み書き | GPL-2.0-or-later (各ファイルの SPDX。一部 MIT / GPL-2.0-only) | 取り込み済み。**公開版には入れない** (docs/editions.md) |
| Linux `fs/exfat` (`third_party/exfat`、無改変) | exFAT の読み書き | GPL-2.0-or-later (一部 GPL-2.0 WITH Linux-syscall-note) | 取り込み済み。公開版に入れない |
| Linux `fs/ext2` (`third_party/ext2`、無改変) | ext2 の読み書き | GPL-2.0 | 取り込み済み。公開版に入れない |
| Linux `fs/ext4` + `fs/jbd2` (`third_party/ext4`、無改変) | ext3 / ext4 の読み書き | GPL-2.0 (一部 LGPL-2.1) | 取り込み済み。公開版に入れない |
| Linux `fs/fat` + `fs/nls` (`third_party/fat`、無改変) | FAT12/16/32 (vfat) の読み書き | GPL-2.0-only / GPL-2.0 | 取り込み済み。公開版に入れない |
| Linux `fs/isofs` / `fs/udf` (`third_party/isofs`, `third_party/udf`、無改変) | ISO 9660 / UDF の読み取り専用 | GPL-2.0 / GPL-2.0-only (UDF に LGPL-2.0+ が 1 ファイル) | 取り込み済み。公開版に入れない |
| LinuxCompat 層 (`core/compat/src/rt_*.c`, `core/compat/include*/`) | カーネルの内部 API (inode / dentry / ページキャッシュ / bio / buffer_head / jbd2 / VFS) の再実装。上のカーネル FS を動かすため | GPL-2.0-or-later 表記 (SPDX の無いヘッダ約 90 ファイルを含む)。**カーネルのヘッダ・実装に倣って書かれた派生物として扱う** | **公開版に入れない** (docs/editions.md) |
| Linux `fs/squashfs` (`third_party/squashfs`、無改変) | squashfs の読み取り専用 | GPL-2.0-only / GPL-2.0-or-later (各ファイルの SPDX。COPYING は GPL-2.0 全文) | 取り込み済み (docs/squashfs-port.md) |
| XFS リーダ / ライタ (`core/compat/src/xfs_volume.cpp` と `xfs_write*.inc`。公開仕様から**独立に実装**。形式の数値の確認にカーネルの形式定義ヘッダを読んだ。カーネルのコードは取り込んでいない・機械照合で写しは無し) | XFS の読み書き (書き込みは実験的) | Apache-2.0 (2026-11-23。出所の調査: docs/provenance-btrfs-xfs.md。形式の定義を GPL のヘッダから取ったことの評価は著作権者の判断) | 実装済み (docs/xfs-port.md) |
| Linux `fs/erofs` (`third_party/erofs`、無改変) | erofs の読み取り専用 | GPL-2.0-only / GPL-2.0-or-later (各ファイルの SPDX。COPYING は GPL-2.0 全文) | 取り込み済み (docs/erofs-port.md) |
| Btrfs のリーダー / ライター (`core/compat/src/btrfs_volume.cpp` と `btrfs_write*.inc`、自前実装。ディスク形式は公開仕様)。展開は core の展開器 (`core/src/decomp*.cpp`。zlib = システムの libz、LZO1X・zstd = 自前: docs/decomp.md) | Btrfs の読み書き (書き込みは実験的) | Apache-2.0 (2026-11-23。以前は展開にカーネル lib をリンクしたため GPL 表記だった。出所の調査: docs/provenance-btrfs-xfs.md) | 実装済み (docs/btrfs-port.md) |
| F2FS リーダー (`core/compat/src/f2fs_volume.cpp`、`f2fs_decomp.cpp`。Linux `fs/f2fs` は**取り込んでいない**: オンディスク形式から書き起こした自前実装、カーネルのコードは写していない) | F2FS の読み書き | Apache-2.0 (自前)。圧縮 (lz4 / lzo / lzo-rle / zstd) の展開は core の自前の展開器 (2026-11-23 に、以前の `usbmem_kdecomp` (GPL 系) へのリンクをやめた) | 取り込み済み (docs/f2fs-port.md) |
| Linux `lib/{zlib_inflate,lz4,lzo,xz,zstd}`、`lib/xxhash.c` (`third_party/kernel-lib`、無改変) | squashfs / erofs の展開 (個人版のみ。Btrfs / F2FS の展開は 2026-11-23 から自前の展開器) | ファイルごとに異なる (GPL-2.0、BSD-2/3-Clause、zstd は GPL-2.0 OR BSD-3-Clause、xz は 0BSD/パブリックドメイン、zlib は zlib ライセンス)。各ファイルの SPDX / 冒頭コメントを参照。`include/linux/crc32.h` だけは本プロジェクトの自前シム | 取り込み済み。**各ファイルのライセンスの精査は未了** (配布前に確認が必要) |
| 暗号プリミティブ一式 (AES・SHA-1/256/512・HMAC・PBKDF2・AES-CBC/XTS/CCM・BLAKE2b・Argon2) と BitLocker / LUKS の解析 (`core/src/crypto_*.cpp`, `transform_*.cpp`, `mini_json.cpp`) | 暗号化ボリュームの読み取り (docs/crypto-volumes.md) | 自前実装 Apache-2.0。**新しい外部依存なし** (OpenSSL・mbedTLS・libbde・libcryptsetup はリンクしない) | 実装済み |
| libbde-utils (bdeinfo / bdemount) | BitLocker の復号結果を突き合わせる**テスト時の外部ツール** | LGPL-3.0-or-later。配布物に含めない・リンクしない | テストのみ |
| cryptsetup / nbdkit (luks フィルタ) / qemu-img | LUKS フィクスチャの生成と独立検証の**テスト時の外部ツール** | GPL-2.0-or-later / BSD-3-Clause / GPL-2.0-or-later。配布物に含めない | テストのみ |
| Python `cryptography` / `argon2-cffi` / `hashlib` | 既知値テストのベクタ生成・合成フィクスチャ生成 (`tools/*.py`) | Apache-2.0 または BSD / MIT。配布物に含めない | テストのみ |
| zlib(libz) | QCOW2 / VMDK の圧縮クラスタ・グレインの展開(`core/src/blockdev_common.cpp`)。ホストは find_package、Android は NDK sysroot の `libz.so`(システムライブラリ。APK に同梱しない) | zlib License(寛容) | ディスクイメージ対応で追加 |
| QCOW2 / VHD / VHDX / VMDK / NBD の実装(`core/src/blockdev_*.cpp`) | ディスクイメージ・NBD クライアント | Apache-2.0(自前)。**他実装のコードは取り込んでいない**。公開仕様(QEMU docs/interop/qcow2.txt、Microsoft VHD / MS-VHDX、VMware Virtual Disk Format、NBD proto.md)から書き起こした。CRC-32C・UTF-16 変換・各パーサは自前 | 追加 |
| qemu-img / qemu-io / qemu-nbd、nbdkit、libnbd(nbdinfo / nbdcopy)、7-Zip | **テストの照合・フィクスチャ生成にだけ使う開発ツール**。配布物に含めない・リンクしない | 各プロジェクトのライセンス(GPL / BSD 等)。実行するだけなので本プロジェクトには波及しない | テスト専用 |
| DiskTools (`core/src/fscheck*.cpp`, `mkfs*.cpp`) | 読み取り専用の整合性検査と mkfs (docs/disk-tools.md) | 自前実装 Apache-2.0。**新しい外部依存なし** (e2fsprogs / exfatprogs / dosfstools / ntfs-3g のコードは取り込まない)。exFAT のアップケース表は Unicode の大文字写像から生成 | 実装済み |
| fsck.fat / fsck.exfat / e2fsck / ntfsfix / mkfs.* / mtools / debugfs | DiskTools の独立検証の**テスト時の外部ツール** (検査のみの引数で呼ぶ) | GPL-2.0-or-later 他。配布物に含めない・リンクしない | テストのみ |
| ddrescue / partclone.* / lz4 コマンド / e2fsprogs / dosfstools / exfatprogs / ntfs-3g (mkntfs) (ホストの検証専用) | 救出・バックアップの出力との照合 (`tools/check_recovery_tools.py`)。コードは取り込まず、アプリにも含めない | GPL-2.0+ 等 (実行のみ。成果物に入らない) | 検証用のみ |
| Partclone イメージ形式 / ddrescue mapfile 形式 | 互換のため形式を再実装 (partclone 0.3.49 の実出力の解析から。partclone のソースコードは取り込んでいない)。LZ4 フレーム形式も自前実装 | 形式の仕様。自前コード (Apache-2.0) | 実装済み |
| fuseshim の試作 (`core/fuseshim/`、docs/fuse-layer.md): ntfs-3g 2022.10.3 (libntfs-3g + src/ntfs-3g.c) と wimlib 1.14.4 (libwim。`--without-ntfs-3g`) を、libfuse 互換の shim の上で無改変で動かす | NTFS / WIM を読む実験と測定 (CI の専用ジョブ `fuseshim.yml` のみ) | ntfs-3g: GPL-2.0-or-later (内蔵 fuse-lite のヘッダ / ソースは LGPLv2)。wimlib: LGPL-3.0-or-later と GPL-3.0-or-later のデュアル (libntfs-3g にリンクすると LGPL は選べない = 配布時は `--without-ntfs-3g`)。shim・アダプターは GPL-2.0-or-later (SPDX) | **試作のみ。配布物に含めない・ソースもリポジトリに置かない** (CI が固定バージョンを sha256 検証つきで取得)。製品に入れる場合は「配布する依存」の行に移し、ライセンス全文・ソースの入手先の表示・別プロセス / 別 .so の方式を決める (docs/fuse-layer.md 4 章) |

## エディション (2026-11-21)

| 版 | 内容 | ライセンス |
|---|---|---|
| 公開版 (public) | `core/src` の自前コード (展開器を含む) + F2FS / Btrfs / XFS (自前) + 共通 IF・ネストマウント + Android アプリ。カーネル由来の FS ドライバ・LinuxCompat・fuseshim を含まない | Apache-2.0 (既定。ユーザーが変更可) |
| 個人版 (personal) | 全部入り。**配布しない** (公開義務が生じない) | 表の各行のとおり (配布しないので全体のライセンスは決めない) |

kfsd (docs/kfsd.md): GPL 系のドライバは別プロセスの実行ファイル `kfsd` (GPL-2.0) に載せ、公開版の本体 (Apache-2.0) とは RPC だけで通信する。ライセンスの境界の一覧は docs/kfsd.md 3 章。

ファイル単位の棚卸し・判断の根拠・書き出し手順・公開前チェックリストは **docs/editions.md**、機械可読な結果は `tools/license_inventory.py`。この表の GPL / LGPL の行は、すべて個人版だけのもの (公開版のツリーには `third_party/` が無い)。

## 未解決の確認事項(推測で断定しない)
- (解消: エディション分離で、個人版は配布せず、公開版は GPL 系を含めない。docs/editions.md) 個人版を第三者へ渡す場合は、GPL-2.0 の義務 (配布物全体のソース提供) が生じる。
- (公開版は Apache-2.0 の既定。決定済みの方針は docs/editions.md。) 自前コードのライセンスを GPL にするか、プロセス分離するか (個人版を配布する場合の話)。(FUSE プログラム (ntfs-3g / wimlib) の層は別プロセスを主案にするが、native 層の .so にはカーネル由来の GPL-2.0+ コードが既に同居しているので、アプリ全体の方針はこれとは別に決める必要がある。docs/fuse-layer.md 4 章)
- exFAT 特許の現状、Play ポリシー最新版、targetSdk 要件(現在 36 を使用。最新要件は公開前に公式で再確認)。
- NTFS-3G / libbde / dislocker / libfdisk 等の各ライセンス(Phase 2/4/5 で調査)。
