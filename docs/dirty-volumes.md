# ダーティ / 未再生ログ / 休止状態のボリュームの扱いと案内 (2026-11-17)

Windows の「高速スタートアップ」「休止状態」「取り外し忘れ」(ダーティフラグ・未再生の `$LogFile`・`hiberfil.sys`) の状態で挿された NTFS / exFAT / FAT を、
**何が起きていて、何をすればよいか**を原因別に利用者へ伝える。ログの再生は実装しない。ダーティフラグを自動で書き換えて消すこともしない (既存の方針を広げない)。

## 現状 (この変更の前) の調査結果

「開き方」は core の挙動 (コードを読んだ結果。NTFS の未再生ログ + 書き込み要求だけは、下の「実測」で CI が確かめた)、「案内」は UI の文言。

| FS | 状態 | 読み取り専用で開く | 書き込み可を要求したとき | UI の案内 (変更前) |
|---|---|---|---|---|
| NTFS | ダーティフラグだけ (ログ静止) | 開ける | 書き込み可で開く (上流の既定が `errors=continue` なので、ダーティでも読み取り専用に落とさない)。クリーンなアンマウントでフラグを下ろす (上流の方針)。**2026-11-18 から、UI は書き込み可で開く前に確認する** (下の「ダーティな NTFS を書き込み可で開く前の確認」) | なし (読み取り専用の経路は通知も出ない) |
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
- ダーティフラグだけでログが静止しているボリュームは、`allow_dirty = true` (既定。ctest などの C++ 呼び出し) なら**従来どおり**書き込み可で開き、クリーンなアンマウントでフラグを下ろす (上流の方針。広げていない)。`allow_dirty = false` (アプリの JNI) なら、書き込みマウントをせず読み取り専用のまま返す (下の「ダーティな NTFS を書き込み可で開く前の確認」)。
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

## ダーティな NTFS を書き込み可で開く前の確認 (2026-11-18)

ダーティフラグだけ (`LOG_UNCLEAN` も `HIBERNATED` もない) の NTFS に書き込み可が要求されたとき、UI は書き込み可で開く前に確認する。ext4 などの「ジャーナル未再生 → 読み取り専用 (noload) で開くか確認」と同じ型 (拒否 → 確認 → 別のモードで開き直す) で、何も保持せずに開き直す。

### core / JNI / Kotlin: 保留の持ち方
- `NtfsVolume::mount_rw(dev, err, allow_dirty = true)`。`allow_dirty = false` で、先に調べた読み取り専用のマウントが `DIRTY` (未再生ログ・休止状態は従来どおりその前で止まる) なら、書き込みマウントをせずにその読み取り専用のボリュームを返し、`state()` に `DIRTY | USBMEM_VOL_CONFIRM_WRITE` (16) を立てる。**書き込みマウントを一度も行わないので、イメージは 1 バイトも変わらない**。`CONFIRM_WRITE` はボリュームの事実ではなく、core が「確認待ちで止めた」と伝える合図 (`vol_state.h` に書いた)。
- JNI `nativeNtfsMountRw` / `nativeNtfsMountDeviceRw` に `allowDirty` を足した。Kotlin の `NtfsVolume.mountReadWrite` / `mountDeviceRangeReadWrite` は **`allowDirty = false` が既定**で、`CONFIRM_WRITE` が付いて返ったら、そのボリュームを**閉じて** `WriteConfirmRequired` (`NativeException` の派生。ジャーナル未再生の `EUCLEAN` と同じ「拒否」の形) を投げる。つまり**保留中に開いているものは何も無い** (ネイティブのハンドルもファイルディスクリプタも、デバイスのセッション側の登録も残らない)。確認が取れたら `allowDirty = true` で開き直す (マウント 1 回分の時間が増える)。
- exFAT / FAT はダーティだと上流が読み取り専用に落とす (書き込めない) ので、確認は出ない (今までどおり原因別の通知だけ)。

### app: どの経路に出したか

| 経路 | 確認 | 保留の持ち方 |
|---|---|---|
| USB のパーティション (`MainActivity.publishPartition`) | 出す | `UsbDeviceState.dirtyPrompt` (開始位置・長さ・ラベルだけ。プロセスで共有する `AppModel` にあるので、回転で Activity が作り直されても残る)。USB を閉じる・抜く (`resetAfterClose`) で消える。公開前なので SAF には何も出ていない。「読み取り専用で開く」= 読み取り専用で公開 (通知は従来の「読み取り専用で開いています」)、「書き込み可で開く」= `allowDirty = true` で開き直す |
| イメージを共有 (`shareImage`) | 出す | `ImageScreen` の `rememberSaveable` の URI だけ (回転で出し直される)。「読み取り専用で開く」= 既存の `shareImage(uri, writable = false)`、「書き込み可で開く」= `shareImageDirty` |
| 暗号ボリュームの中の NTFS (USB・イメージ) | 出す | ジャーナル未再生の保留 (`VolumeRegistry.pendingCrypto` の `PendingRecovery`。復号済みデバイスのハンドルだけ。鍵は持たない) に、`InnerRange.dirty = true` の範囲として相乗りする。確認 (`confirmCryptoNoload(.., writeDirty)`)・キャンセル・USB を閉じる・アプリの終了で必ず `release` する規律は、noload の保留と同じ。ほかに解錠できた FS はそのまま公開する |
| 入れ子マウント (`NestedMountSection`) | **出さない (不要)** | 入れ子は常に読み取り専用でマウントするので、書き込み可を要求しない |
| 書き込み可を要求しないとき (読み取り専用のセッション、イメージの「書き込みを許可」が切) | 出さない | 従来どおり、読み取り専用で開き、通知だけ |

確認ダイアログ (`DirtyWriteDialog`、USB・イメージ・暗号で共通。暗号の中は `CryptoPendingDialog` が noload と dirty を出し分ける): 文言は「このボリュームは前回きれいに取り外されていません (Windows の取り外し忘れ・高速スタートアップなど)。書き込み可で開くと、アプリはダーティフラグを下ろして終了時に NTFS を更新します。Windows で chkdsk を実行してから使う方が安全です。」。選択肢は「読み取り専用で開く」(既定・強調のボタン) / 「書き込み可で開く」(押し間違えにくいテキストボタン) / キャンセル (何も開かない)。

LFS 2.0 (Windows 11 の使用中) のダーティは、`state()` に `DIRTY` だけが出るので確認が出る。「書き込み可で開く」を選んでも、上流がログを読めず読み取り専用に落ちる (そのときは従来どおり、書けなかった原因の通知が出る)。確認が無駄になるが、読み取り専用で開くだけなら害はない。

## 検証

- ctest `ntfs_state` (`tools/run_ntfs_state_tests.sh` + `tools/gen_ntfs_state_fixture.py` + `usbmem_ntfs_tests --state`): mkntfs + ntfs-3g で 16 MiB のクリーンなボリュームを作り、その写しの `$Volume` のフラグと `$LogFile` の restart page (Windows 10 流の LFS 1.1。上流の検査を通る形) を書き換えて、
  `clean` / `dirty` / `dirty_log` / `log` / `hiber` の 5 イメージを作る。読み取り専用・rw 要求の両方で `state()` が期待どおり、止めるべきものは `writable() == false` で**イメージ不変**、`dirty` は書き込み可 + アンマウント後にフラグが下りる (ダーティフラグの方針の回帰)。
  参考として、上流のドライバだけで rw マウントしたときのログの扱いを `info:` 行に出す (判定には使わない)。
- ctest `ntfs_state` に、確認待ち (`mount_rw(.., allow_dirty = false)`) の検査を追加: `dirty` は読み取り専用のまま `DIRTY | CONFIRM_WRITE`・`writable() == false`・書き込みは `EROFS`・読める・**イメージ不変**。`clean` は書き込み可で `CONFIRM_WRITE` なし。
- 計装テスト `VolumeStateInstrumentedTest.dirtyFlagOnlyAsksBeforeWriting` (`allowDirty = false` で `WriteConfirmRequired`・イメージ不変 → `allowDirty = true` で書き込み可・アンマウントでフラグが下りる)・`cleanVolumeDoesNotAsk`。
- 計装テスト `DirtyConfirmUiTest` (`ro-misc` の `ntfs_read`。実画面を操作): イメージ画面 (書き込みを許可 → 共有 → 確認が出る・まだ公開されない・イメージ不変 / 「読み取り専用で開く」→ 読み取り専用で公開・イメージ不変 / 「書き込み可で開く」→ 書き込み可・書き込み・アンマウントでフラグが下りる / キャンセル → 何も公開されない / 回転 (`recreate`) でダイアログが出し直される)。USB 画面は偽のセッション (マウントは実イメージ。`UsbSessions.testOverride`) で、同じ 4 つ + 抜去 (`resetAfterClose`) で何も残らない + 読み取り専用のセッションは確認なし。
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
- ダーティフラグだけ (ログ静止) の NTFS を書き込み可で開く方針 (上流の方針) は変えていない。確認ダイアログを足した (2026-11-18。上の節)。確認を取らずに書き込み可で開く経路は、C++ の `mount_rw` の既定 (`allow_dirty = true`。ctest 用) だけ。
- 確認ダイアログの暗号ボリューム版 (ダーティな NTFS が LUKS / BitLocker の中にあるとき) は、実画面の計装テストを足していない (`CryptoUiTest` のフィクスチャに、ダーティな NTFS を中に持つ暗号イメージが無い)。コード上は noload の保留と同じ経路を共有する。
- USB の確認は、実機 USB なしの偽のセッションで通した (実機は未確認)。
- 書き込みを止めた NTFS で、「ダーティフラグを下ろすだけ」の操作はしない (ログの再生をしない以上、フラグだけ下ろすと未再生の更新を無かったことにする)。
- 暗号ボリュームの中の NTFS / exFAT / FAT の通知は、中の FS ごとの最初の 1 つだけを出す (複数のパーティションがそれぞれ異常でも 1 件)。
