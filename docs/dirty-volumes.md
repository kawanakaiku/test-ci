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

## CI の結果 (確認ダイアログ、2026-11-17 UTC、ブランチ usbmem-ci-full-dirtyask、コミット c25de43)

- Host tests (run 44168855046): plain / san / wsan / wrel の 4 構成すべて成功 (`ntfs_state` は 4 構成とも Passed。所要 133 分、wsan が最長)。
- Android emulator tests (run 44168854969): 6 グループすべて成功。`ro-misc` の `ntfs_read` は 18 件 (`VolumeStateInstrumentedTest` + `DirtyConfirmUiTest` 9 件を含む) が skip 0・失敗 0。`crypt-ui` は 10 件成功 (既存の noload の確認フローは `CryptoPendingDialog` に置き換えても通る)。

## 残る課題・未検証

- **実機・実 Windows 製イメージでの確認は未** (Windows 11 の LFS 2.0 のダーティ、高速スタートアップの実イメージ)。`LOG_UNCLEAN` は LFS 1.1 の restart area の印だけを見る。LFS 2.0 の restart area の配置が 1.1 と同じかは未確認なので、2.0 の判定は上流 (読めず読み取り専用) に任せている。
- ダーティフラグだけ (ログ静止) の NTFS を書き込み可で開く方針 (上流の方針) は変えていない。確認ダイアログを足した (2026-11-18。上の節)。確認を取らずに書き込み可で開く経路は、C++ の `mount_rw` の既定 (`allow_dirty = true`。ctest 用) だけ。
- 確認ダイアログの暗号ボリューム版 (ダーティな NTFS が LUKS / BitLocker の中にあるとき) は、実画面の計装テストを足していない (`CryptoUiTest` のフィクスチャに、ダーティな NTFS を中に持つ暗号イメージが無い)。コード上は noload の保留と同じ経路を共有する。
- USB の確認は、実機 USB なしの偽のセッションで通した (実機は未確認)。
- 書き込みを止めた NTFS で、「ダーティフラグを下ろすだけ」の操作はしない (ログの再生をしない以上、フラグだけ下ろすと未再生の更新を無かったことにする)。
- 暗号ボリュームの中の NTFS / exFAT / FAT の通知は、中の FS ごとの最初の 1 つだけを出す (複数のパーティションがそれぞれ異常でも 1 件)。

## AVD での画面確認 (2026-11-19)

CI の計装テストは通っているが、**見た目・導線・文言を目で確かめる**ために、ローカルの AVD (sdk_gphone64_x86_64、API 37、1080x2400) で実画面を操作した。実機 USB は使っていない (イメージをアプリの「イメージ」画面から開く経路だけ)。
APK は main 22695e3 の GitHub Actions ビルド (run 44192340281、`-PusbmemNtfsWrite=true` の書き込み有効ビルド)。イメージは tmpfs (`/dev/shm`) 上に作り (すべて 16〜64 MiB。nbdfuse が要る大きさではなかった)、`adb push` で `/sdcard/Download/` に置いて、ピッカーから開いた。確認後に `adb pull` して、ダーティフラグ・md5 を PC で見た。
フィクスチャ: `tools/gen_ntfs_state_fixture.py` の 5 イメージ + (exFAT のダーティ / メディアエラー、FAT16 のダーティは mkfs 後にブートセクタのビットを立てただけの手作り)。画面のスクリーンショットはコミットしない。

### 1. ダーティな NTFS を書き込み可で開く前の確認 — 確認できた

「書き込みを許可」を入れて「NTFS イメージをマウント (書き込み可)」→ `ntfs_state_dirty.img` を選ぶ。

- **ダイアログ (縦)**: タイトル「書き込み可で開きますか？」、本文は docs のとおり (「このボリュームは前回きれいに取り外されていません (Windows の取り外し忘れ・高速スタートアップなど)。書き込み可で開くと、アプリはダーティフラグを下ろして終了時に NTFS を更新します。Windows で chkdsk を実行してから使う方が安全です。」+「「読み取り専用で開く」を選ぶと、何も書き込みません。」)。ボタンは「読み取り専用で開く」(塗りつぶし)・「書き込み可で開く」(テキスト)・「キャンセル」(テキスト) の縦並び。全文が収まる。
- **回転**: 縦 → 横 → 縦で、ダイアログは出し直される (縦に戻して「読み取り専用で開く」を押せた)。MainActivity は `configChanges` に orientation を持つので Activity は作り直されず、Compose の状態がそのまま残る。
- **キャンセル**: 「キャンセル」ボタン、ダイアログの外側のタップのどちらも、画面下に情報の通知「開くのをやめました。ダーティフラグ付きのボリュームは何も開いていません。」。ファイルアプリのルートは増えない。イメージは md5 一致 (読み取り専用で開いたあとの pull でも一致、ダーティフラグ 1 のまま)。
- **「読み取り専用で開く」**: 警告の通知 (黄) 「前回、正しく取り外されていないボリュームです」/「NTFS のダーティフラグが付いています (Windows で「安全に取り外す」をせずに抜かれた、または高速スタートアップのまま終了した可能性があります)。 読み取り専用で開いています。 書き込むには、Windows に挿して、…。このアプリはログの再生も、ダーティフラグの書き換えもしません。」。ファイルアプリ (Documents UI) のルートは「NTFS (読み取り専用)」。イメージは md5 一致。
- **「書き込み可で開く」**: 警告の通知「ダーティフラグ付きのボリュームを書き込み可で開きました」/「NTFS のダーティフラグが付いています (…)。 ログに未再生の更新は無いため書き込み可で開きました。 閉じるときにダーティフラグを下ろします。Windows で chkdsk を実行したことがない場合は、念のため先にバックアップを取ってください。」。ルートは「NTFS (読み書き・実験的)」。マウント中にイメージを pull するとダーティフラグはまだ 1。
- **アンマウントでフラグが下りる**: 書き込み可で開いたあと、同じイメージを読み取り専用で共有し直す (同じ URI の登録を置き換える = クリーンなアンマウント) と、pull したイメージのダーティフラグは 0 (`$Volume` の flags 0x0001 → 0x0000)。
- **ログが未再生 / 休止状態のものは確認が出ず、止まる**: 「書き込みを許可」で `dirty_log` / `log` / `hiber` を開くと、ダイアログなしで読み取り専用で開き、通知は「書き込みを許可していましたが、データを守るため読み取り専用で開きました」(2 項参照)。4 イメージ (dirty_log / log / hiber / clean) とも pull して md5 一致。

見つけた点 (直していない。判断は親):

1. **横向きでダイアログの本文が切れる**。横 (高さ 1080 px) では本文が 3 行目で切れ、肝心の「書き込み可で開くと、アプリはダーティフラグを下ろして…」「何も書き込みません」が読めない。本文をスワイプしてもスクロールしなかった。ボタンは見えるので、警告を読まずに押せてしまう。縦向きでは問題なし。`DirtyWriteDialog` の `text` を `verticalScroll` で包む (`UnlockDialog` は包んでいる) か、横向きでの幅を広げる。同じ切れ方は `CryptoPendingDialog` (3 項) にもある。
2. **ダイアログに、どのイメージ・ボリュームの確認かが出ない** (ピッカーを閉じたあと、ファイル名が画面のどこにも無い)。暗号の中の確認は「暗号ボリューム「…」の解錠はできました」で名前が出るので、揃えるとよい。
3. **ボリュームの名前が `msf:19` のようになる**: ダウンロードのピッカーから選ぶ URI の `lastPathSegment` を名前にしているため、ファイルアプリのルート名・「ボリューム内のイメージをマウント」のボタン・暗号ボリュームの行・解錠ダイアログの見出しが `msf:19` / `msf:30` になるものがある (同じ一覧から選んでも `ntfs_state_clean.img` と出るものもある)。`OpenableColumns.DISPLAY_NAME` を使うと安定する。
4. **共有したイメージを閉じる導線が無い**。「イメージ」画面にも、ファイルアプリのルートにも、取り出し (eject) が無い。暗号ボリュームには「ロック (アンマウント)」があるが、平文のイメージは同じ URI をもう一度共有する (置き換わる) か、アプリのプロセスが終わるまで開いたまま。**ダーティフラグが下りるのは「クリーンなアンマウント」だけなので、書き込み可で開いたあと、利用者がフラグを下ろす方法を持たない** (プロセスが殺されるとフラグは残る)。docs の「クリーンなアンマウントでフラグを下ろす」を利用者に届ける導線が弱い。
5. 通知はイメージ画面の一番下のカード (スクロールの末尾) に出る。画面の下端に半分見える程度で、自動でスクロールしないので、見落としやすい (特に長い警告)。
6. ボタンの名前が「NTFS イメージをマウント (読み取り専用 / 書き込み可)」のまま、exFAT / FAT / ext4 のイメージも通る (下の 2 項のとおり)。

### 1 の見つけた点を直した (2026-11-19、CI + AVD で再確認)

| 項 | 対応 |
|---|---|
| 1 横向きで本文が切れる | 確認ダイアログ共通の `DialogBody` (Components.kt) を `verticalScroll` にした (`ConfirmDialog` / `DirtyWriteDialog` / `CryptoPendingDialog` / ネストの noload が共通)。ボタンは AlertDialog が下に固定するので常に見える。AVD (横 2400x1080) で、本文は 3 行ぶんの窓だが、スワイプで最後の「何も書き込みません」まで読めた。縦 → 横 → 縦の回転でダイアログは出し直される |
| 2 どのイメージの確認か出ない | 本文の先頭に対象の名前 (太字) を出す。USB のパーティションは既存のラベル、イメージは下の 3 と同じ表示名。AVD: 「ntfs_dirty.img」「ext_needs_recovery.img」「nl.img」(暗号の中の確認も) |
| 3 `msf:19` のような名前 | `UriNames.displayName` が `OpenableColumns.DISPLAY_NAME` を ContentResolver に問い合わせ、取れなければ URI の末尾 (`raw:/…/x.img` → `x.img`)。公開ボリュームのラベル・パーティション一覧の見出し・暗号ボリュームの行・解錠ダイアログの見出しが同じ名前になる |
| 4 閉じる導線が無い | イメージ画面に「マウント中のイメージ」の節を足した (名前・FS・読み取り専用 / 書き込み可 + 「閉じる (アンマウント)」)。確認なしでクリーンにアンマウントし (`VolumeRegistry.closeByUser`)、再起動後の復元の一覧からも外す。通知は書き込み可だったとき「閉じました。書き込みを確定しました」、読み取り専用なら「閉じました (アンマウントしました)」。AVD: 書き込み可で開いた `ntfs_dirty.img` の `$Volume` flags が、マウント中は 0x1、閉じたあと pull したイメージは 0x0 |
| 5 通知が下端で見落とす | イメージ画面の通知は、出たら見える所まで自動でスクロールする (`BringIntoViewRequester`)。AVD: 書き込み可で開いた直後に警告カードの全文が画面内に入った |
| 6 ボタン名 | 「イメージをマウント (読み取り専用 / 書き込み可)」に変えた |

計装テスト (CI): `DirtyConfirmUiTest` に `image_dialogNamesTheImage_andScrollsInLandscape` (横向き固定で本文が末尾までスクロールでき、ボタンが見える・対象名の表示)、`image_closeButton_unmountsCleanlyAndLowersDirtyFlag` (閉じる → ダーティフラグが下りる)、`image_closeButton_readOnly` を足した。ro-misc (`usbmem-ci-only-ro-misc` の run 44423733365) で `ntfs_read` の 21 件が成功 (従来 18 件 + 3 件)。

### 2. ダーティ / 未再生ログ / 休止状態の原因別の案内 — 確認できた (すべて見た文言のとおり、docs の表と一致)

通知はすべて、イメージ画面の下に出る警告カード。タイトルと詳細 (要点)。

| イメージ | 開き方 | タイトル | 結果の文 |
|---|---|---|---|
| NTFS `dirty_log` (ダーティ + 未再生ログ) | 読み取り専用 | Windows の書き込みが途中のままのボリュームです | 「NTFS の $LogFile に、Windows が反映し終えていない更新の記録 (未再生のログ) が残っています。…このアプリはログを再生しないので、読み取りはできますが、内容が最新でない・一部が欠けている可能性があります。 読み取り専用で開いています。 書き込むには、Windows に挿して、…」 |
| 同上 | 書き込みを許可 | 同上 | 同上 + 「書き込みを許可していましたが、データを守るため読み取り専用で開きました。」(確認ダイアログは出ない) |
| NTFS `log` (未再生ログだけ) | 読み取り専用 / 書き込み | 同上 | `dirty_log` と同じ文 (ダーティフラグの言及なし) |
| NTFS `hiber` | 読み取り専用 / 書き込み | Windows が休止状態のままのボリュームです | 「Windows が休止状態 (高速スタートアップを含む) のままシャットダウンされ、hiberfil.sys が残っています。…いま読むと内容が古いことがあり、書き込むと Windows が再開したときにボリュームが壊れます。」+ 結果の文 +「…Shift キーを押しながら「シャットダウン」を選ぶか、「高速スタートアップを有効にする」をオフにすると、休止状態になりません。」 |
| NTFS `clean` | 読み取り専用 | マウントしました: ntfs_state_clean.img | 「読み取り専用。ファイルアプリの一覧に表示されます。」(情報) |
| exFAT (VolumeDirty) | 読み取り専用 / 書き込み | 前回、正しく取り外されていないボリュームです | 「exFAT のダーティフラグ (VolumeDirty) が付いています (「安全に取り外す」をせずに抜かれた、または書き込み途中だった可能性があります)。」+ 結果の文 +「…このアプリはログの再生も、ダーティフラグの書き換えもしません。」 (書き込みを許可 → 「読み取り専用で開きました」の文) |
| FAT16 (ブートセクタのダーティ bit) | 読み取り専用 / 書き込み | 同上 | 「FAT のダーティビット (前回きれいに取り外されなかった印) が付いています。」+ 同上 |
| exFAT (MediaFailure) | 書き込み | メディアエラーの印が付いたボリュームです | 「「メディアエラーあり (表面検査が必要)」の印が付いています。読み書きできない場所がある可能性があります。」+ 結果の文 +「Windows の chkdsk /r (不良セクターの検査) などで確認し、大事なデータは先にコピーしてください。」 |

- 結果の文は、書き込みを許可していないとき「読み取り専用で開いています。」、許可していたのに落ちたとき「書き込みを許可していましたが、データを守るため読み取り専用で開きました。」と出し分けられている。
- 違和感: (a) exFAT / FAT が「前回、正しく取り外されていない」と同じ見出しなのは良いが、Android で作ったイメージにも「Windows に挿して…」を勧める文面になる (想定どおりだが、ファイルの種類によらず Windows 前提)。(b) ボタンが「NTFS イメージをマウント」のまま exFAT / FAT / ext4 も開ける (1 項 6)。(c) 詳細が 8 行近くなり、通知カードがスクロールの下端に出る (1 項 5)。
- 未確認: USB 経路 (`publishPartition`)・入れ子経路・通知ドロワーの Android 通知 (この通知は画面内のカードで、`dumpsys notification` には何も出ない。アプリはシステム通知を出していない)。

## AVD での画面確認の残りを直した (2026-11-19、権限の検査と同じコミット列)

- **(a) 横向きの確認ダイアログ**: `DirtyWriteDialog` の 3 ボタンを、横向き (または幅 600dp 以上) では横 1 列 (「キャンセル」「書き込み可で開く」「読み取り専用で開く」(強調) の順に右寄せ)、縦向きの狭い画面は従来どおり縦に並べる。**AVD で、ボタンを横に並べるだけでは潰れることが分かった**: ダイアログの幅がプラットフォームの既定 (横向きで約 320dp) のままだと、強調ボタンが細い棒になり本文も出なかった (CI の `DirtyConfirmUiTest` の横向きの試験が同じ原因で失敗して気づいた)。`ConfirmDialog` / `DirtyWriteDialog` に `DialogProperties(usePlatformDefaultWidth = false)` と左右 24dp の余白を足して、幅を Material の上限 (560dp) まで広げた。横向きで本文 (警告の 2 つの段落) が全部読め、ボタンは 1 行、縦向きは幅が広がって行数が減った (AVD の実画面、API 37)。試験: `DirtyConfirmUiTest.image_dialogNamesTheImage_andScrollsInLandscape` に、縦向きは縦並び (`dirty_buttons_column`)・横向きは横 1 列 (`dirty_buttons_row`、3 つが同じ高さで重ならない) を足した。
- **(b) USB 画面の通知の自動スクロール**: イメージ画面にあった `BringIntoViewRequester` の仕組みを `Banner(scrollIntoView = true)` (`Modifier.bringIntoViewWhenChanged`、`ui/Components.kt`) に共通化し、イメージ画面の通知・USB 画面のデバイスカードの通知 (`usb_device_notice`)・USB 全体の通知 (`usb_notice`) が使う。通知が出た (内容が変わった) ときに、見える所までスクロールする。AVD で、イメージ画面の通知 (「開くのをやめました…」) がスクロールなしで全部見えることを確認 (共通化後)。**USB 画面そのものは、AVD に USB が無いので実画面では未確認** (同じ部品。偽のセッションの試験で確かめるのは未了)。
- **(c) ジャーナル未再生の確認の本文**: 検出した FS 名が分かる経路 (USB のパーティション: パーティション表の `fs`、イメージ: 開けなかったイメージを調べた FS、ネストしたイメージ: そのパーティション表) は、「この EXT4 ボリュームには、まだ反映されていない更新 (ジャーナル) が残っています。…」の短い文言 (`noload_dialog_text_fs`。ext3/ext4 = ジャーナル、XFS = ログ、Btrfs = tree-log、F2FS = 直近の fsync の記録)。分からない・暗号ボリュームの中 (複数の FS がありうる) は従来の全 FS を列挙した文言 (`noload_dialog_text`)。AVD で ext4 (needs_recovery) のイメージの確認が短い文言になることを確認。試験: `DirtyConfirmUiTest.image_noloadDialogNamesOnlyTheDetectedFs` (ro-misc の `ntfs_read` が `ext4_e4_needs_recovery.img` も端末へ送る)。
- **(d) 解錠ダイアログの「下の『記憶する』を…」**: 「記憶する」のチェックボックスはパスワードの入力欄の下、この文より**上**にあり、回復キー / BEK ファイルの入力のときは出ない。文言を「上の「このボリュームのパスワードを記憶する」を選ばない限り保存しません」に直し、パスワード以外の方式のときは「記憶する」に触れない文 (`unlock_not_saved_plain`) にした。AVD (PIN 設定済み) で確認: パスワード = 上の文、BEK ファイル = 記憶に触れない文。

