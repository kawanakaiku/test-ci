# ダーティ / 未再生ログ / 休止状態のボリュームの扱いと案内 (2026-11-17)

Windows の「高速スタートアップ」「休止状態」「取り外し忘れ」(ダーティフラグ・未再生の `$LogFile`・`hiberfil.sys`) の状態で挿された NTFS / exFAT / FAT を、
**何が起きていて、何をすればよいか**を原因別に利用者へ伝える。ログの再生は実装しない。ダーティフラグを自動で書き換えて消すこともしない (既存の方針を広げない)。

## 現状 (この変更の前) の調査結果

「開き方」は core の挙動 (コードを読んだ結果。NTFS の未再生ログ + 書き込み要求だけは、下の「実測」で CI が確かめた)、「案内」は UI の文言。

| FS | 状態 | 読み取り専用で開く | 書き込み可を要求したとき | UI の案内 (変更前) |
|---|---|---|---|---|
| NTFS | ダーティフラグだけ (ログ静止) | 開ける | 書き込み可で開く (上流の既定が `errors=continue` なので、ダーティでも読み取り専用に落とさない)。クリーンなアンマウントでフラグを下ろす (上流の方針) | なし (読み取り専用の経路は通知も出ない) |
| NTFS | ダーティ + **未再生ログ** (LFS 1.1。Windows 10 以前・取り外し忘れ) | 開ける | **書き込み可で開き、マウント中に `$LogFile` を空にして書き始める** (上流は `ntfs_empty_logfile` を無条件に呼ぶ。未再生の更新を捨てる)。「元々書かない」という TODO の記述は、実際には Windows 11 の LFS 2.0 (上流が読めず `NVolErrors` で読み取り専用) でしか成り立っていなかった | なし |
| NTFS | ダーティ + 未再生ログ (LFS 2.0。Windows 11 の使用中) | 開ける | 上流が `$LogFile` を読めず (1.1 のみ対応) 読み取り専用に落ちる | 汎用「ビルドで無効、またはボリュームに異常/ダーティがあります」 |
| NTFS | 休止状態 (`hiberfil.sys` が `hibr`) | 開ける | 上流が読み取り専用に落とし、ダーティフラグも書かない (`NVolHibernated`) | 同上 |
| exFAT | VolumeDirty / MediaFailure | 開ける | `mount_rw` が読み取り専用でマウント (`writable() == false`)。何も書かない | 同上 |
| FAT12/16/32 | ブートセクタのダーティ bit / FAT[1] のクリーン bit / メディアエラー bit | 開ける | 同上 | 同上 |
| ext2/3/4・XFS・Btrfs・F2FS | ジャーナル / ログ未再生 | `EUCLEAN` で拒否 → 確認ダイアログ → noload で読み取り専用 | (ext4/ext2 は再生して書き込み可) | **十分** (専用の確認フロー・文言あり。変更なし) |

足りなかったのは NTFS / exFAT / FAT: (1) 書き込めない**本当の理由**が UI に届かず、一般的な「ビルドで無効 / 異常 / ダーティ」の 1 文だった、(2) 読み取り専用で開く経路は何の通知も出さず、休止状態のボリュームの「内容が古いかもしれない」ことも伝わらなかった、
(3) NTFS の未再生ログは、書き込みを要求されると**ログを捨てて書く**挙動だった (安全側の方針と食い違い)。

## 変更

### core: ボリュームの状態 (`FsVolume::state()`)
`core/compat/public/usbmem/vol_state.h` のビット (C / C++ 共通)。**読み取れた事実だけ**を立てる (推測しない)。

| ビット | 意味 | 取得元 |
|---|---|---|
| `USBMEM_VOL_DIRTY` (1) | ダーティフラグ (前回きれいに取り外されていない) | NTFS: `$Volume` の `VOLUME_IS_DIRTY` (マウント直後の値) / exFAT: ブートセクタの VolumeFlags / FAT: ブートセクタのダーティ bit・FAT[1] のクリーン bit (どの面でも) |
| `USBMEM_VOL_LOG_UNCLEAN` (2) | NTFS の `$LogFile` に未再生のログ | `$LogFile` の先頭 2 ページ (restart page) の restart area に「クリーン」の印も「クライアントなし」もない。判定は `core/include/usbmem/ntfs_logstate.h` (自前の検査 `fscheck_ntfs.cpp` と共通) |
| `USBMEM_VOL_HIBERNATED` (4) | NTFS: Windows が休止状態 (高速スタートアップを含む) | 上流の `check_windows_hibernation_status` の結果 (`NVolHibernated`。`hiberfil.sys` が `hibr`、または 4 KiB 未満・判定不能) |
| `USBMEM_VOL_MEDIA_ERROR` (8) | exFAT MediaFailure / FAT のメディアエラー bit | ブートセクタ |

読み取り専用で開いたときも返す。`FsVolume::state()` (既定 0) → `NtfsVolume` / `ExfatVolume` / `FatVolume` → `LockedFsVolume` (入れ子・暗号の中も同じ) → JNI `nativeFsState` → Kotlin `NtfsVolume.state` (`VolState`)。

### core: NTFS の `mount_rw` が未再生ログ・休止状態を止める
`NtfsVolume::mount_rw` は、書き込み可を要求されたとき**先に読み取り専用でマウントして状態を調べ**、`LOG_UNCLEAN` / `HIBERNATED` なら**その読み取り専用のボリュームをそのまま返す** (`writable() == false`、`state()` に理由)。
書き込みマウントを一度も行わないので、**イメージは 1 バイトも変わらない** (ログも空にしない、ダーティフラグも触らない)。問題なければ、いったん閉じて書き込み可でマウントし直す (マウント 1 回分の時間が増える)。
- ダーティフラグだけでログが静止しているボリュームは**従来どおり**書き込み可で開き、クリーンなアンマウントでフラグを下ろす (上流の方針。広げていない)。
- LFS 2.0 などログを読めないものは、上流が従来どおり読み取り専用に落とす (state には `DIRTY` が出る)。`LOG_UNCLEAN` は「restart page を読めて、クリーンの印がない」ときだけ。
- ビルドで書き込みが無効なときは従来どおり `EROFS` で失敗する。

### app: 原因別の通知 (`VolumeStateNotice`)
`android/app/src/main/java/org/usbmem/app/VolumeStateNotice.kt`。state・FS の種類・「書き込みを要求したか」「実際に書き込み可か」から、Warning の通知 (タイトル + 詳細) を組み立てる。**経路ごとに文言を持たない**: 次の 4 か所がこれを通す。
USB (`publishPartition`: 読み取り専用で開く / 書き込み可で開く / 書き込みを要求したが落ちた、の 3 通り)、イメージ (`shareImage`)、暗号ボリュームの中 (`cryptoSuccessNotice`。中の FS ごと)、入れ子マウント (`nestedMount`)。
state が 0 (何も取れていない) のときは null を返し、従来の汎用の通知を出す (汎用の文言は「ビルドで書き込みが無効か、ボリュームが書き込みの条件を満たしていません」に直した)。

| 原因 | タイトル | 詳細 (要点) |
|---|---|---|
| 休止状態 | Windows が休止状態のままのボリュームです | hiberfil.sys が残っている。いま読むと内容が古いことがあり、書き込むと Windows の再開でボリュームが壊れる。Windows に挿して「安全に取り外し」。Shift+シャットダウン / 高速スタートアップをオフ |
| 未再生ログ | Windows の書き込みが途中のままのボリュームです | `$LogFile` に未反映の更新。このアプリはログを再生しない (内容が最新でない・欠ける可能性)。Windows で安全に取り外す / chkdsk |
| ダーティだけ | 前回、正しく取り外されていないボリュームです | NTFS / exFAT (VolumeDirty) / FAT (ダーティビット) 別の説明。ログもダーティフラグの書き換えもしない |
| メディアエラー | メディアエラーの印が付いたボリュームです | chkdsk /r |
| ダーティだけ + 書き込み可で開いた (NTFS) | ダーティフラグ付きのボリュームを書き込み可で開きました | ログに未再生の更新は無いので書き込み可。閉じるときにフラグを下ろす |

結果の文は「書き込みを許可していましたが、データを守るため読み取り専用で開きました」/「読み取り専用で開いています」を使い分ける。

## 検証

- ctest `ntfs_state` (`tools/run_ntfs_state_tests.sh` + `tools/gen_ntfs_state_fixture.py` + `usbmem_ntfs_tests --state`): mkntfs + ntfs-3g で 16 MiB のクリーンなボリュームを作り、その写しの `$Volume` のフラグと `$LogFile` の restart page (Windows 10 流の LFS 1.1。上流の検査を通る形) を書き換えて、
  `clean` / `dirty` / `dirty_log` / `log` / `hiber` の 5 イメージを作る。読み取り専用・rw 要求の両方で `state()` が期待どおり、止めるべきものは `writable() == false` で**イメージ不変**、`dirty` は書き込み可 + アンマウント後にフラグが下りる (ダーティフラグの方針の回帰)。
  参考として、上流のドライバだけで rw マウントしたときのログの扱いを `info:` 行に出す (判定には使わない)。
- ctest `fat_write` / `exfat_write` の拒否テストに `state()` の検査 (boot-dirty → DIRTY、boot-media → MEDIA_ERROR、FAT[1] → DIRTY、exFAT の 1 / 8 / 9)。
- 自前の検査 (`disktools`) は `ntfs_logstate.h` を共有したので、既存の `logfile_*` の変異テストで回帰を見る。
- 計装テスト `VolumeStateInstrumentedTest` (ro-misc の `ntfs_read`): 通知の文言が原因別に出ること (休止 / 未再生ログ / ダーティ + 読み取り専用 / ダーティ + 書き込み可 / exFAT / FAT / メディアエラー / state 0 は null)、NTFS の 5 イメージで state・書き込みの扱い・通知・イメージ不変。
  `FatWriteTest` / `ExfatWriteTest` のダーティのイメージに state と通知の検査を追加。

## CI の結果 (2026-11-17、ブランチ usbmem-ci-full-dirtyui、コミット 8ffcb86)

- Host tests (run 44096388040): plain / san / wsan / wrel の 4 構成すべて成功 (`ntfs_state` は 4 構成とも Passed。skip ではない)。
- Android emulator tests (run 44096388238): 6 グループすべて成功。`ro-misc` の `ntfs_read` で `VolumeStateInstrumentedTest` を含む 8 件が skip 0 で成功 (書き込み有効のビルドなので、rw 要求の検査も実行された)。`ext-fat` の FAT / exFAT のダーティの検査も成功。
- ctest は成功時に出力を出さないため、上流だけの rw マウントでログがどうなるかの `info:` 行は CI の記録では見ていない (判定には使っていない)。

## 残る課題・未検証

- **実機・実 Windows 製イメージでの確認は未** (Windows 11 の LFS 2.0 のダーティ、高速スタートアップの実イメージ)。`LOG_UNCLEAN` は LFS 1.1 の restart area の印だけを見る。LFS 2.0 の restart area の配置が 1.1 と同じかは未確認なので、2.0 の判定は上流 (読めず読み取り専用) に任せている。
- ダーティフラグだけ (ログ静止) のボリュームを書き込み可で開く方針 (上流の方針) は変えていない。Windows が chkdsk を要求する状態で書くことになるので、将来「確認ダイアログ」にする案がある。
- 書き込みを止めた NTFS で、「ダーティフラグを下ろすだけ」の操作はしない (ログの再生をしない以上、フラグだけ下ろすと未再生の更新を無かったことにする)。
- 暗号ボリュームの中の NTFS / exFAT / FAT の通知は、中の FS ごとの最初の 1 つだけを出す (複数のパーティションがそれぞれ異常でも 1 件)。
