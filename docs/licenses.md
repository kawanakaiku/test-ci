# ライセンス・依存関係の記録

| コンポーネント | 用途 | ライセンス | 状態 |
|---|---|---|---|
| 本プロジェクトの自前コード(core/, android/) | — | Apache-2.0(SPDX 表記。**暫定**: 下記 GPL 取り込み時に配布物全体の扱いを再判断) | Phase 1 |
| AndroidX / Compose / Material3 | UI | Apache-2.0 | Phase 1 |
| org.json (JVM 単体テストのみ) | テスト | JSON License(テスト依存のみ、配布物に含めない) | Phase 1 |
| Linux `fs/ntfs` | Phase 2 | GPL-2.0-or-later(super.c SPDX で確認。全ファイル確認は未了) | 未取り込み |
| Linux `fs/squashfs` (`third_party/squashfs`、無改変) | squashfs の読み取り専用 | GPL-2.0-only / GPL-2.0-or-later (各ファイルの SPDX。COPYING は GPL-2.0 全文) | 取り込み済み (docs/squashfs-port.md) |
| Linux `fs/erofs` (`third_party/erofs`、無改変) | erofs の読み取り専用 | GPL-2.0-only / GPL-2.0-or-later (各ファイルの SPDX。COPYING は GPL-2.0 全文) | 取り込み済み (docs/erofs-port.md) |
| Linux `lib/{zlib_inflate,lz4,lzo,xz,zstd}`、`lib/xxhash.c` (`third_party/kernel-lib`、無改変) | squashfs / erofs の展開 | ファイルごとに異なる (GPL-2.0、BSD-2/3-Clause、zstd は GPL-2.0 OR BSD-3-Clause、xz は 0BSD/パブリックドメイン、zlib は zlib ライセンス)。各ファイルの SPDX / 冒頭コメントを参照。`include/linux/crc32.h` だけは本プロジェクトの自前シム | 取り込み済み。**各ファイルのライセンスの精査は未了** (配布前に確認が必要) |

## 未解決の確認事項(推測で断定しない)
- GPL-2.0 コードをアプリに静的リンクした場合の配布物全体の扱い、Google Play 配布との両立(ソース提供方法)。Phase 7 前に法務的確認が必要。
- 自前コードのライセンスを GPL にするか、プロセス分離するか。
- exFAT 特許の現状、Play ポリシー最新版、targetSdk 要件(現在 36 を使用。最新要件は公開前に公式で再確認)。
- NTFS-3G / libbde / dislocker / libfdisk 等の各ライセンス(Phase 2/4/5 で調査)。
