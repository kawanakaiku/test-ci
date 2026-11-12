# ライセンス・依存関係の記録

| コンポーネント | 用途 | ライセンス | 状態 |
|---|---|---|---|
| 本プロジェクトの自前コード(core/, android/) | — | Apache-2.0(SPDX 表記。**暫定**: 下記 GPL 取り込み時に配布物全体の扱いを再判断) | Phase 1 |
| AndroidX / Compose / Material3 | UI | Apache-2.0 | Phase 1 |
| org.json (JVM 単体テストのみ) | テスト | JSON License(テスト依存のみ、配布物に含めない) | Phase 1 |
| libfdisk (util-linux) | パーティション編集 | LGPL-2.1+ | **採用しない** (自前実装。理由は docs/partition-edit.md 9 章) |
| Linux `fs/ntfs` | Phase 2 | GPL-2.0-or-later(super.c SPDX で確認。全ファイル確認は未了) | 未取り込み |
| Linux `fs/squashfs` (`third_party/squashfs`、無改変) | squashfs の読み取り専用 | GPL-2.0-only / GPL-2.0-or-later (各ファイルの SPDX。COPYING は GPL-2.0 全文) | 取り込み済み (docs/squashfs-port.md) |
| Linux `fs/erofs` (`third_party/erofs`、無改変) | erofs の読み取り専用 | GPL-2.0-only / GPL-2.0-or-later (各ファイルの SPDX。COPYING は GPL-2.0 全文) | 取り込み済み (docs/erofs-port.md) |
| Btrfs の読み取りリーダー (`core/compat/src/btrfs_volume.cpp`、自前実装。ディスク形式は公開仕様) と展開ラッパ (`btrfs_decomp.c`) | Btrfs の読み取り専用 | SPDX は GPL-2.0-or-later (展開に上記のカーネル lib をリンクするため。リーダー自体はカーネルのコードを含まない) | 実装済み (docs/btrfs-port.md) |
| Linux `lib/{zlib_inflate,lz4,lzo,xz,zstd}`、`lib/xxhash.c` (`third_party/kernel-lib`、無改変) | squashfs / erofs の展開 | ファイルごとに異なる (GPL-2.0、BSD-2/3-Clause、zstd は GPL-2.0 OR BSD-3-Clause、xz は 0BSD/パブリックドメイン、zlib は zlib ライセンス)。各ファイルの SPDX / 冒頭コメントを参照。`include/linux/crc32.h` だけは本プロジェクトの自前シム | 取り込み済み。**各ファイルのライセンスの精査は未了** (配布前に確認が必要) |
| 暗号プリミティブ一式 (AES・SHA-1/256/512・HMAC・PBKDF2・AES-CBC/XTS/CCM・BLAKE2b・Argon2) と BitLocker / LUKS の解析 (`core/src/crypto_*.cpp`, `transform_*.cpp`, `mini_json.cpp`) | 暗号化ボリュームの読み取り (docs/crypto-volumes.md) | 自前実装 Apache-2.0。**新しい外部依存なし** (OpenSSL・mbedTLS・libbde・libcryptsetup はリンクしない) | 実装済み |
| libbde-utils (bdeinfo / bdemount) | BitLocker の復号結果を突き合わせる**テスト時の外部ツール** | LGPL-3.0-or-later。配布物に含めない・リンクしない | テストのみ |
| cryptsetup / nbdkit (luks フィルタ) / qemu-img | LUKS フィクスチャの生成と独立検証の**テスト時の外部ツール** | GPL-2.0-or-later / BSD-3-Clause / GPL-2.0-or-later。配布物に含めない | テストのみ |
| Python `cryptography` / `argon2-cffi` / `hashlib` | 既知値テストのベクタ生成・合成フィクスチャ生成 (`tools/*.py`) | Apache-2.0 または BSD / MIT。配布物に含めない | テストのみ |
| zlib(libz) | QCOW2 / VMDK の圧縮クラスタ・グレインの展開(`core/src/blockdev_common.cpp`)。ホストは find_package、Android は NDK sysroot の `libz.so`(システムライブラリ。APK に同梱しない) | zlib License(寛容) | ディスクイメージ対応で追加 |
| QCOW2 / VHD / VHDX / VMDK / NBD の実装(`core/src/blockdev_*.cpp`) | ディスクイメージ・NBD クライアント | Apache-2.0(自前)。**他実装のコードは取り込んでいない**。公開仕様(QEMU docs/interop/qcow2.txt、Microsoft VHD / MS-VHDX、VMware Virtual Disk Format、NBD proto.md)から書き起こした。CRC-32C・UTF-16 変換・各パーサは自前 | 追加 |
| qemu-img / qemu-io / qemu-nbd、nbdkit、libnbd(nbdinfo / nbdcopy)、7-Zip | **テストの照合・フィクスチャ生成にだけ使う開発ツール**。配布物に含めない・リンクしない | 各プロジェクトのライセンス(GPL / BSD 等)。実行するだけなので本プロジェクトには波及しない | テスト専用 |
| DiskTools (`core/src/fscheck*.cpp`, `mkfs*.cpp`) | 読み取り専用の整合性検査と mkfs (docs/disk-tools.md) | 自前実装 Apache-2.0。**新しい外部依存なし** (e2fsprogs / exfatprogs / dosfstools / ntfs-3g のコードは取り込まない)。exFAT のアップケース表は Unicode の大文字写像から生成 | 実装済み |
| fsck.fat / fsck.exfat / e2fsck / ntfsfix / mkfs.* / mtools / debugfs | DiskTools の独立検証の**テスト時の外部ツール** (検査のみの引数で呼ぶ) | GPL-2.0-or-later 他。配布物に含めない・リンクしない | テストのみ |

## 未解決の確認事項(推測で断定しない)
- GPL-2.0 コードをアプリに静的リンクした場合の配布物全体の扱い、Google Play 配布との両立(ソース提供方法)。Phase 7 前に法務的確認が必要。
- 自前コードのライセンスを GPL にするか、プロセス分離するか。
- exFAT 特許の現状、Play ポリシー最新版、targetSdk 要件(現在 36 を使用。最新要件は公開前に公式で再確認)。
- NTFS-3G / libbde / dislocker / libfdisk 等の各ライセンス(Phase 2/4/5 で調査)。
