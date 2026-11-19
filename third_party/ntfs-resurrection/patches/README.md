# ntfs-fix パッチ (~/Downloads/ntfs-fix から取り込み)

出所: ユーザー提供 (カーネル内 `ntfs` モジュール用の修正集)。**`fs-ntfs/` には適用していない** (無改変方針)。
`.ko` (ホストカーネル用ビルド済み) は Android で使えないため取り込まない。`host-kmod-build.sh.txt` は参考用 (実行不可の拡張子)。

対象: upstream 067c5c24cef2 の `fs-ntfs/` に対する `patch -p3 --dry-run` 結果 (2026-11-11)

| パッチ | 内容 | 状態 |
|---|---|---|
| 0001-ntfs-fix-mft-enospc | 割り当て時の `max_empty_bit_range` を zone_end までに制限 (偽 ENOSPC) | **適用可 (offset +5)**。不具合は現行ソースにも存在。書き込み経路のため、書き込みを有効化する Phase で適用・検証する |
| 0002-ntfs-windows_names-optional | `-o windows_names` 時のみ不正文字検査 | **不要**: 現行ソースは `ntfs_check_bad_windows_name()` 先頭で `NVolCheckWindowsNames` を判定済み (hunk 不一致) |
| 0003-ntfs-statfs-roomy-inodes | statfs の f_ffree を範囲内に丸め、空き容量分を加算 | **一部不要**: 丸めは upstream に取り込み済み。「空き分を加算」は表示上の調整のみで、読み取り専用の本プロジェクトでは不要 |

適用するときは `fs-ntfs/` を変更せず、ビルド時に作業コピーへパッチする方式にして UPSTREAM.md に記録すること。

## ビルド時に作業コピーへ当てているパッチ (2026-11-19〜)

`fs-ntfs/` は無改変のまま、`core/compat/ntfs_patch.cmake` が configure 時に `fs-ntfs/super.c` をビルドディレクトリ (`ntfs_patched/super.c`) へ複製して 1 行だけ置換し、そのコピーをコンパイルする
(`patch` コマンドに依存しない。置換元が見つからない / 一意でないと configure が失敗するので、上流の更新で前提が変わったら黙って未適用にならない)。

| パッチ | 内容 | 根拠・検証 |
|---|---|---|
| mftmirr-first4 (`super.c` の `check_mft_mirror()`) | `$MFTMirr` との比較を MFT レコード 0-3 (`$MFT` / `$MFTMirr` / `$LogFile` / `$Volume`) だけにする (`} while (++i < vol->mftmirr_size)` → 4 まで)。ミラーの実体 (`mftmirr_size`・書き込み時の同期範囲・ランリストの検査) は上流のまま | 64 KiB クラスタの Windows 製ボリュームが偽陽性で読み取り専用に落ちる件。docs/ntfs-mftmirr.md (ctest `ntfs_mirror`) |

更新時 (UPSTREAM.md のコミットを差し替えるとき) は、置換が今も必要か (上流が直していないか) を確認する。

