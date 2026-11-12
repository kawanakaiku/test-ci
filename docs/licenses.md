# ライセンス・依存関係の記録

| コンポーネント | 用途 | ライセンス | 状態 |
|---|---|---|---|
| 本プロジェクトの自前コード(core/, android/) | — | Apache-2.0(SPDX 表記。**暫定**: 下記 GPL 取り込み時に配布物全体の扱いを再判断) | Phase 1 |
| AndroidX / Compose / Material3 | UI | Apache-2.0 | Phase 1 |
| org.json (JVM 単体テストのみ) | テスト | JSON License(テスト依存のみ、配布物に含めない) | Phase 1 |
| Linux `fs/ntfs` | Phase 2 | GPL-2.0-or-later(super.c SPDX で確認。全ファイル確認は未了) | 未取り込み |
| zlib(libz) | QCOW2 / VMDK の圧縮クラスタ・グレインの展開(`core/src/blockdev_common.cpp`)。ホストは find_package、Android は NDK sysroot の `libz.so`(システムライブラリ。APK に同梱しない) | zlib License(寛容) | ディスクイメージ対応で追加 |
| QCOW2 / VHD / VHDX / VMDK / NBD の実装(`core/src/blockdev_*.cpp`) | ディスクイメージ・NBD クライアント | Apache-2.0(自前)。**他実装のコードは取り込んでいない**。公開仕様(QEMU docs/interop/qcow2.txt、Microsoft VHD / MS-VHDX、VMware Virtual Disk Format、NBD proto.md)から書き起こした。CRC-32C・UTF-16 変換・各パーサは自前 | 追加 |
| qemu-img / qemu-io / qemu-nbd、nbdkit、libnbd(nbdinfo / nbdcopy)、7-Zip | **テストの照合・フィクスチャ生成にだけ使う開発ツール**。配布物に含めない・リンクしない | 各プロジェクトのライセンス(GPL / BSD 等)。実行するだけなので本プロジェクトには波及しない | テスト専用 |

## 未解決の確認事項(推測で断定しない)
- GPL-2.0 コードをアプリに静的リンクした場合の配布物全体の扱い、Google Play 配布との両立(ソース提供方法)。Phase 7 前に法務的確認が必要。
- 自前コードのライセンスを GPL にするか、プロセス分離するか。
- exFAT 特許の現状、Play ポリシー最新版、targetSdk 要件(現在 36 を使用。最新要件は公開前に公式で再確認)。
- NTFS-3G / libbde / dislocker / libfdisk 等の各ライセンス(Phase 2/4/5 で調査)。
