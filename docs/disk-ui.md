# ディスクツールの画面 (検査 / フォーマット / パーティション編集 / バックアップ・復元 / 救出)

docs/disk-tools.md (検査・mkfs)、docs/partition-edit.md、docs/recovery-backup.md が作った処理を、アプリの画面から使えるようにしたもの。TODO 9 の「検査 / フォーマット / パーティション編集 / バックアップ / 救出の導線」の分。

## 1. 入口と構成

- 入口は 2 か所。USB 画面のデバイスのカード (`disk_tools_usb`、パーティション行の `disk_tools_part_<n>`) と、イメージ画面の「パーティション」のカード (`disk_tools_image`、同 `disk_tools_part_<n>`)。パーティション行から入ると、そのパーティションを選んだ状態で始まる。
- `ui/DiskToolsScreen.kt` の `DiskToolsHost` が、ツール一覧 (ハブ) と各ツールの画面を出す。`AppScaffold` は `DiskToolsNav.target` が立っている間、タブの中身の代わりにこれを出す。戻るボタン (システムの戻るも) でハブ → 元の画面。
- 画面は `ui/DiskToolsActions.kt` のインタフェースだけを呼ぶ。実処理は `DiskToolsController` (DiskTools / PartitionEditor / RecoveryBackup の Kotlin ラッパーを呼ぶ)。`MainActivity.diskTools` が実体 (計装テストも同じものを使う)。失敗は例外ではなく `Outcome.error` (利用者に見せる文言。ネイティブの理由コードは `diskCodeLabel` で文言にする) で返る。

| 画面 | ファイル | 性格 |
|---|---|---|
| ハブ | `DiskToolsScreen.kt` | 「読み取り専用」(緑) と「書き込みあり」(赤) の 2 群に分けて並べる |
| 検査 | `DiskCheckScreen.kt` | 読み取り専用。書き込まない・修復しないことを見出しと結果に明記。検査しない範囲 (限界) も FS 別に出す |
| フォーマット | `DiskFormatScreen.kt` | 書き込みあり。FS・ラベル・クラスタ (ブロック) サイズ・識別子の元 (seed) |
| パーティション編集 | `DiskPartitionScreen.kt` | 上半分は表示のみ (構成の帯グラフ・凡例・GPT の診断)、下半分は編集 (赤い縁) |
| バックアップ / 復元 | `DiskBackupScreen.kt` | バックアップは読み取り専用 (書くのは保存先のファイルだけ)、復元は書き込みあり |
| 救出 | `DiskRescueScreen.kt` | 読み取り元は読み取り専用。書くのは出力のイメージと進捗ファイルだけ |

共通部品は `DiskUi.kt` (見出し `ToolHeader`、赤い縁の `DangerCard`、`DangerButton`、`TypedConfirmDialog`、`PartitionBar`、`RangePicker`、ジョブの進捗)。

## 2. 破壊的 / 読み取り専用の区別

色だけに頼らず、**色・アイコン・見出し・手順**の 4 つで区別する。

| | 読み取り専用 | 書き込みあり |
|---|---|---|
| 色 | 緑 (成功色) | 赤 (エラー色)。実行ボタンも赤 |
| アイコン / バッジ | 鍵 + 「読み取り専用」 | 警告 + 「書き込みあり」 |
| 見出し (testTag) | `tool_header_readonly` | `tool_header_destructive` |
| 手順 | 押せばすぐ走る | 計画 (まだ何も書かない) → 確認文字列を打つ → 適用 |

書き込みあり操作は、**画面の無効化に頼らず**コントローラでも「すべて許可」(USB はセッションの書き込みモード、イメージは「書き込みを許可」) とマウント中でないことを確かめる (`requireWritable`)。マウント中 (OS・このアプリのどちらでも) は断る。書き込みモードの切り替えは既存の UI のとおり、USB を開く前に決める (この画面では切り替えない。読み取り専用で開いていれば `fmt_need_write` などの案内を出す)。

## 3. 確認 (TypedConfirmDialog)

デバイス名・対象 (パーティション)・容量・今の形式 / 作る形式を並べ、ネイティブが決めた確認文字列 (`ERASE xxxxxx` / `FORMAT xxxxxx` / `WRITE xxxxxx` / `RESTORE xxxxxx`) を**そのまま打たせる**。一致するまで実行ボタンは無効 (`disk_confirm_ok`)。キャンセル (`disk_confirm_cancel`) や外側のタップでは何も変えない。本文はスクロールでき、横向き・大きい文字でもボタンは見える。

### 計画と適用の間でロックを持たない

利用者が考えている間に USB セッションのロックを持ち続けないため、「計画 → 確認 → 適用」は次のようにしている。

1. 計画: デバイスを開いて計画を作って閉じる。画面は確認文字列と変更内容を表示する。
2. 適用: もう一度開いて、**同じ指定で計画を作り直し**、確認文字列が画面に見せたものと一致することを確かめてから、打たれた文字列で適用する。食い違えば「計画のあとに状態が変わりました」で中止する。

計画が開き直しで同じになるよう、mkfs は seed (ボリューム UUID / シリアル) と時刻、パーティション編集は GUID とディスク ID を画面が最初の計画で決めて持ち回る。mkfs の計画の識別子は `raw-image(rw|ro, size)` の説明文字列も含むので、計画と適用は同じモード (rw / ro) で開く。

## 4. 各画面の要点

- **検査**: パーティションまたはディスク全体を選んで実行。進捗・取り消し (`check_cancel`)。結果は判定 (`check_verdict_<clean|warnings|errors|unsupported|incomplete>`)・件数・findings の一覧・ジャーナル再生が必要かどうか。対応していない形式は選択肢に「検査に対応していません」と出す。
- **フォーマット**: 選択肢は `MkfsSession.capabilities` (docs/mkfs-extended.md) の結果だけで作る (後述の 4.1)。実行前に、書き換える領域の元の内容をアプリ内 (`files/disktools/backups`) に自動保存する。結果は読み戻しの検証の有無と「検査で確かめる」(検査画面へ)。
- **パーティション編集**: GPT / MBR の帯グラフ (幅は容量に比例、空き領域は枠線だけ、暗号化は鍵アイコン。TalkBack には帯全体を 1 つの説明として読ませる)。操作 (追加・削除・サイズ変更・名前・種類・新しい表・GPT の修復・バックアップからの表の復元) を積み、計画で「適用前 / 適用後」の帯と表、変更の一覧、破壊的か・強い確認が要るかを見せる。「表のバックアップ」で今の表を SAF のファイルへ保存できる。適用前にもアプリ内へ自動保存する。
- **バックアップ / 復元**: 保存先 / 復元元は SAF のファイル。バックアップは進捗・取り消し (取り消すと途中のファイルは消す)・保存したファイルの検証 (`bak_verify`)。復元は計画の画面で、**画像と書き込み先の形式・サイズ・ラベルを左右に並べ**、形式やサイズが違えば警告を出す (取り違え防止)。
- **救出**: 読み取り元 (読み取り専用) → 出力イメージ (新しいファイル / 既存のファイル)。進捗ファイル (mapfile。ddrescue 互換) はアプリ内に残り、同じ指定でもう一度実行すれば続きから。開始前に `rescue_preflight` の結果 (出力の大きさ・進捗ファイルの状態・警告) を必ず見せ、拒否 (読み取り元と出力が同じファイルなど) なら始められない。進捗は帯 (救出済み / 読めなかった / 読み直し待ち / 未試行)。一時停止・取り消し・安全弁 (連続エラー数・進みがない秒数)。

### 4.1 フォーマットの選択肢 (`DiskFormatScreen`)

- 形式のチップは `DiskToolsActions.formatCaps(範囲, セクタ)` (= `MkfsSession.capabilities`) の `supported` なものすべて。チップに最小〜最大容量・ラベルの上限と文字種を添える。ネイティブが `experimental` とした形式 (NTFS) には「実験的」の帯 (`fmt_ntfs_experimental`) を出す。
- 範囲が小さすぎる / 大きすぎる形式は、選べるが計画のボタンが止まり、ネイティブの `reason` と容量の範囲 (`fmt_cap_reason` / `fmt_cap_range`) を見せる。どの形式も作れない範囲なら `fmt_none_fits`。
- 「FAT (自動)」(`fmt_fs_fat_auto`): 範囲の容量から FAT12 / 16 / 32 を選ぶ。純 Kotlin (`FormatChoices.autoFat`。8 MiB 未満は 12 → 16 → 32、512 MiB 未満は 16 → 32 → 12、以上は 32 → 16 → 12 の順で、ネイティブが `sizeOk` とした最初のもの)。選んだ結果は `fmt_fat_auto_pick` に出す。コアの mkfs の公開 API は変えていない。
- クラスタ (ブロック) サイズは `clusterSizes` のうち、その範囲で `sizeOk` なものだけ (先頭は「自動 (既定 ...)」)。ラベルは上限 (バイト数) と文字種 (`ascii-upper` は大文字の ASCII、`utf8` / `utf16`) に合わせて入力のたびに整える。
- ext3 / ext4 の機能は「詳細設定」(`fmt_adv_toggle` で開く。`fmt_adv`) の中のチェック (`fmt_feat_<名前>`)。既定からの差分だけを `features` ("a,^b" 形式) にして `MkfsSession.openEx` へ渡す。何も変えなければ従来の `open` を使う。
- 計画と適用で同じ計画になるよう、画面は乱数の種と時刻を最初の計画で決めて持ち回る (`MkfsSession.openEx` の `seed` / `timestampSec`)。

### 4.2 失われたパーティションを探す / ブートセクタを検査・修復 (TestDisk 相当)

どちらも `DiskRecoveryFeatures.registerAll()` (`MainActivity.onCreate`) が 5 章の口へ登録する。**探索・検査 (読み取り専用) と、適用 (書き込む) は、見出し・色・カードを分ける**: 上のカードは `ToolHeader` の「読み取り専用」(`lost_header_readonly` / `boot_header_readonly`)、書き込みは `DangerCard` (`lost_write_card` / `boot_repair_card`) の中だけ。

- **失われたパーティションを探す** (`DiskLostScreen.kt`): 「速い探索 / 全セクタ」を選んで実行 (`lost_run`)。進捗 (調べた位置・読んだ量) と取り消し (`lost_cancel`、取り消すと途中までの結果だけ出し、復元案は出さない)。結果は候補の一覧 (`lost_cand_<i>`: 開始・大きさ・FS・信頼度・今の表との関係 `match / overlap / contained / not-in-table`・注記) と、信頼度順の復元案 (`lost_prop_<n>`)。復元案ごとに、適用後の表を `PartitionBar` (`lost_prop_<n>_bar`) と行で描く。案を選ぶ (`lost_prop_pick_<n>`) と書き込みカードに計画 (変更の一覧・確認の文言) が出て、適用 (`lost_apply`) → `TypedConfirmDialog` → 適用。適用前に表の領域を `files/disktools/backups/scan-<時刻>.bak` へ自動保存する旨を `lost_backup_guide` に出す。書き込みモードでない (`lost_need_write`)・マウント中 (`lost_in_use`) は適用できない。適用後は表を読み直す。
- **ブートセクタを検査 / 修復** (`DiskBootScreen.kt`): パーティションごと (パーティションが無い / ディスク全体が 1 つの FS ならその範囲) に検査 (`boot_run`)。結果は判定 (`boot_verdict_<番号>_<ok|warnings|repairable|damaged|unrecognized>`)・異常の一覧・BPB の要点 (`boot_findings_<番号>`、詳細は折りたたみ)。修復の候補 (`boot_plan_<番号>_<修復 ID>`) を選ぶと、書き込む領域 (`boot_plan_writes`) と確認の文言 ("REPAIR xxxxxx") が出る。**ntfsfix のような自動修復はしない** (`boot_no_auto`): 控えのブートセクタなど、検査で根拠が確かめられたものを書き戻す修復だけ。適用前に書き換える領域を `boot-<時刻>-<番号>.bak` へ保存し、適用後に同じ範囲を検査し直して結果を差し替える。
- 計画と適用の間でロックを持たない (3 章)。適用は装置を開き直して、同じ入力 (復元案の番号・ID・乱数の種) で計画を作り直し、確認の文言が画面に出したものと同じかを確かめてから書く。計画の識別子には開き方 (rw / ro) が入るので、計画も適用と同じ開き方で作る。復元案の計画を決定的にするため、`PartitionEditSession.open` に乱数の種 (`rngSeed`、既定 0 = 乱数) を渡せる。

## 5. 他の担当の機能の口 (機能の問い合わせ)

パーティション編集の画面には、TestDisk 相当の機能の口が 2 つある (`DiskSlot.LostPartitions` = 失われたパーティションを探す、`DiskSlot.BootSector` = ブートセクタの検査 / 修復)。**API ができるまで画面には何も出さない** (「準備中」も出さない)。API を作った担当は次のように登録する。

```kotlin
DiskToolsFeatures.register(DiskSlot.LostPartitions) { ctx: ToolContext ->
    // ctx.a (DiskToolsActions)、ctx.target、ctx.table、ctx.writeAllowed、ctx.reload() が使える
    MyLostPartitionsCard(ctx)
}
```

登録すると、その口のカード (見出しと説明は `strings_disktools.xml` の `dt_slot_*`、testTag は `disk_slot_lost_partitions` / `disk_slot_boot_sector`) が出る。`DiskToolsFeatures.available(slot)` で有無を問い合わせられる。登録の場所は、機能を持つ Kotlin のオブジェクトの初期化 (`MainActivity.onCreate` など、画面が出る前) でよい。書き込みを伴う機能は、このファイルの `TypedConfirmDialog` と `DangerCard` を使って、他の破壊的操作と同じ見た目・手順にそろえる。

## 6. 既知の制約・実機で確かめること

- バックアップ / 復元の C API はパスを取る。SAF のファイルは `ParcelFileDescriptor` を開いて `/proc/self/fd/<n>` で渡している (fd を開いている間だけ有効)。ネイティブの側がファイルを開き直す (`open`) ので、fd がシーク可能な通常ファイルでないと失敗しうる (Google ドライブ等のストリーム型の provider は不可)。AVD の `file://` と端末内ストレージでは確認する。実機の外部ストレージ provider は未確認。
- 救出の出力は、足りなければ `ftruncate` で疎に広げる。広げられない provider は、出力できない旨を出す (`dt_err_rescue_extend`)。
- 救出の取り消し・再開は `pauseBetweenReadsMs` で遅くして試験している。実際に読めないセクタを持つ実機での動作 (安全弁が働くか) は、この試験では確かめていない (別途の実機の試験が要る)。
- フォーマットの選択肢は、ネイティブの `capabilities` が返す形式だけ (ext2 / ext3 / ext4 / FAT12 / FAT16 / FAT32 / exFAT / NTFS)。画面は問い合わせの結果だけを見る。
- 暗号化 (LUKS / BitLocker) のパーティションは、帯グラフと一覧に「暗号化」と鍵アイコンを出し、パーティション編集の画面にサイズ変更・削除で中身ごと失われる旨を表示する。中の FS の検査・バックアップは解錠が要るので、この画面は暗号の外側 (暗号化されたまま) のパーティションとして扱う。

## 7. 試験

`DiskUiTest` (計装テスト。CI の `disk-ui` グループ。docs/ci.md)。イメージ画面で開いたイメージ (ファイル選択だけ espresso-intents で差し替え) と、偽の USB セッション (実イメージをネイティブのデバイスとして渡す) に対して実画面を操作する。

- 検査: クリーン / 壊れたイメージの判定と結果の表示、書かないこと (SHA-256 が変わらない)。
- フォーマット: 空のイメージに ext2 / exFAT / FAT32 → 結果を `e2fsck -fn` / `fsck.exfat -n` / `fsck.fat -n` とラベルで外から検証。確認のキャンセルで何も変わらない・読み取り専用で実行できない。
- パーティション編集: 構成の表示、口が登録まで出ないこと、パーティション削除 → 計画 (適用前後) → 適用 → `sgdisk -v` / `-p` と残った ext2 の `e2fsck`。キャンセル・読み取り専用で変わらない。
- バックアップ / 復元: バックアップ → 検証、復元の左右比較・キャンセル・実行 → `e2fsck` とラベルの復元。
- 救出: 開始前の検査の表示 → 取り消し → 進捗ファイルが残り「続きから」→ 完了 → 出力と元の `cmp`。同じファイルを出力にすると拒否。
- USB: 読み取り専用では検査だけ動き書き込みツールは止まる、「すべて許可」ならフォーマットできる。
- フォーマットの選択肢: 40 MiB で「FAT (自動)」→ FAT16・ラベルは大文字 (`fsck.fat -n`、ブートセクタの種別とラベル)、6 MiB で FAT32 を選ぶと `reason` が出て計画が止まり自動なら FAT12、ext4 の機能 (`resize_inode`) を詳細設定で外す (`e2fsck -fn` と `dumpe2fs`)、NTFS の「実験的」と `ntfsfix -n`。
- 失われたパーティションを探す (`lost_mbr.img` / `lost_gpt.img`): 読み取り専用で探索 → 候補 2 つ・復元案・適用できない旨・イメージ不変、MBR / GPT の復元 (`sfdisk -V` / `sgdisk -v`、開始位置が 2048 と 16384、ext2 と FAT16 の中身が健全)、確認のキャンセル (入力違い → キャンセルで不変)、OS がマウント中の USB は適用不可、取り消し済みのジョブは途中の結果だけ。
- ブートセクタ (`boot_multi.img`: FAT32 / NTFS / exFAT を 1 つの MBR に置き、3 つとも先頭のセクタだけ壊したもの): 読み取り専用で 3 つとも「修復できる」と出ること、それぞれの修復 → 適用後の検査が「正常」、`fsck.fat -n` / `ntfsfix -n` / `fsck.exfat -n` で外から確認、ほかのパーティションと表が元のまま (`cmp`)、確認のキャンセルで不変、マウント中の USB は適用不可。
- 横向き・大きい文字 (font_scale 2.0) で確認ダイアログが使える (フォーマット・探索の適用・ブートセクタの修復)。

書き込んだイメージは `cache/dui_out` に残り、CI が `run-as` で取り出して、ホストの `e2fsck` / `fsck.fat` / `fsck.exfat` / `ntfsfix` / `sfdisk` / `sgdisk` / `cmp` で検証する (`tools/ci_android_tests.sh` の `disk_ui_verify`)。フィクスチャは `tools/gen_disk_ui_fixture.py`。
