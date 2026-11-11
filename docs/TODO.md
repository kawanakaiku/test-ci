# TODO (未解決・保留事項)

## 書き込み / NTFS
- [ ] **LFS (LogFile) 2.0 対応の検討** — Windows 11 本体で作った NTFS は `$LogFile` が v2.0 で、上流は 1.1 のみ対応のため書き込みマウントが自動で読み取り専用になる
      (読み取りは問題なし)。現方針は「案 A: LFS 2.0 は読み取り専用のまま。書き込みは LFS 1.1 のみ」。後で再判断:
      案 B = LFS 2.0 を調べて対応 / 案 C = ログ静止を確認してクリーンなボリュームに限り opt-in で書く。最近の Windows 11 でフォーマットした USB に書けない点が Phase 3 完了条件に影響する。
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
