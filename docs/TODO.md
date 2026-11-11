# TODO (未解決・保留事項)

## 書き込み / NTFS
- [x] **LFS (LogFile) 2.0 の件は訂正済み (ご指摘どおり書き込める)** — Windows 11 は使用中の `$LogFile` を v2.0 で持つが、**クリーンにアンマウントすると restart page を v1.1 に戻す**
      (実測: 正常ディスマウント後は v1.1、ダーティのままデタッチしたイメージは v2.0)。クリーンな Windows 11 製ボリュームには `ntfs-3g` も自前ドライバも書き込めた。
      v2.0 が問題になるのは **ダーティ (未再生ログあり) のボリューム**だけで、そこは元々書かない (ログ再生が要る) ので現方針で正しい: 読み取り専用で開き、書くなら Windows で安全に取り外してもらう。
- [ ] ダーティ/未再生ログのボリュームをどう案内するか (UI: 「Windows で安全に取り外してください」)。ログ再生の自前実装はしない。
- [ ] **64KiB クラスタの `$MFTMirr` 偽陽性** — 上流は 1 クラスタ分 (64 レコード) を比較するが Windows が同期するのは先頭 4 つ。書き込み時に自動で読み取り専用になる。
      対処案: 比較を先頭 4 レコードに限るパッチ (ntfs-3g と同じ挙動)。`third_party/ntfs-resurrection/patches/` に追加するか判断。
- [ ] 書き込み段階 3 以降: サイズ拡張・新規作成・削除・rename・mkdir (`iomap_file_buffered_write`、setattr、create/unlink)。
- [ ] 電源断模擬 (書き込み途中で打ち切り) → `ntfsfix -n` / WinPE chkdsk で確認。
- [ ] 圧縮 (LZNT1/WOF)・疎ファイルへの書き込み。
- [ ] ダーティフラグ付き・未再生ログのボリュームの扱い (現状: ログ静止ならクリーンアンマウントでフラグを下ろす = 上流方針)。
- [ ] 実機 USB での読み書き検証 (USB メモリが用意できたら)。

## 互換層
- [ ] `iomap_readahead` 未実装 (読み取りは read_folio で足りている)。
- [ ] マウント失敗経路の inode 内部確保物のリーク (既知・許容)。
- [ ] 同期 writeback の ioend 結合 (性能)。`writeback_iter` の O(N) スナップショット。
- [ ] 残りのスタブ 55 個 (`tools/compat_stub_names.txt`)。書き込み拡張で必要になったものから実装。

## 検証・ツール
- [ ] `tools/winvhd/make_encrypted_fixtures.sh` の起動待ちループが雑 (約 12 分かかる)。DHCP 取得後すぐ進むよう直す。
- [ ] EFS: 復号は未実装 (読み出しは -EOPNOTSUPP で拒否)。BitLocker は Phase 4。
- [ ] 巨大 (TB 級) ボリューム上の Windows 製データ、未再生 `$LogFile` ボリューム。
- [ ] システムの Documents UI (SAF) の実機確認、arm64 実機。

## Phase 4 以降
- [ ] BitLocker / LUKS 復号、他 FS (exFAT, ext4 等) の統合、UAS (USB Attached SCSI) 対応。

## 電源断・書き込みの残課題 (2026-11-11 追加)
- [ ] `torn` で壊れた MFT システムレコード (0-3) を `$MFTMirr` のコピーで読み出す緩和 (読み取り専用) と、Windows `chkdsk` による回復可否の確認。
- [ ] 電源断模擬を Windows 製イメージ (gpt64k/4Kn) と、より長い操作列・大きなファイル書き込み (ランダム/シーケンシャル混在) に拡大。フル版 (`run_crash_tests.sh <bin> <dir> 40`) を定期実行。
- [ ] クラスタサイズ < 4096 のボリュームは書き込みマウントを拒否 (読み取り専用に固定)。
- [ ] 書き込みの `fsync` / SAF 側の flush の意味づけ、書き込み途中の USB 抜去 (I/O エラー) 時の挙動テスト。
