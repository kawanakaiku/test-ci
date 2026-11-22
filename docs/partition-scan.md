# パーティション探索・表の復元案・ブートセクタ修復 (TestDisk 相当)

`core/` に実装した (要件書 7 章の TestDisk 相当)。UI は未接続 (C API → JNI → Kotlin まで)。すべて自前コード (Apache-2.0) で、公開版にも入る (GPL 系・third_party・LinuxCompat に依存しない)。ホストのテスト・本物の `sfdisk` / `mkfs.*` / `fsck.*` / `testdisk` との照合・ASan/UBSan で検証した。**実機・AVD での書き込みは試していない**。

安全モデル: **探索と検査は絶対に書かない** (読み取り専用で開いたデバイスで動く)。書き込みは必ず 計画 → 確認トークン → 適用 の 3 段で、書き込み前にバックアップを取り、書いた後に読み戻して検証する。

## 1. 構成

| ファイル | 役割 |
|---|---|
| `include/usbmem/partition_scan.h`, `src/partition_scan.cpp`, `src/partition_scan_detect.cpp`, `src/partition_scan_internal.h` | 探索 (`scan_partitions`): 各 FS のシグネチャ → 開始位置の仮説 → 検証 → 候補 |
| `include/usbmem/partition_recover.h`, `src/partition_recover.cpp` | 表の復元案 (`propose_table_recovery`) と、案を `PartitionEditor` に積む `queue_recovery_proposal` |
| `include/usbmem/bootfix.h`, `src/bootfix.cpp` | ブートセクタ / スーパーブロックの検査 (`inspect_boot`)・修復の計画/トークン/適用・バックアップと復元 |
| `include/usbmem/c_api_partscan.h`, `src/c_api_partscan.cpp`, `src/c_api_partedit_internal.h` | C API (`usbmem_partscan_*` / `usbmem_bootfix_*` / `usbmem_pjob_*`) |
| `android/app/src/main/cpp/jni_partscan.cpp`, `.../PartitionScan.kt` | JNI と Kotlin (`PartitionScanner` / `BootFixSession` / `ScanJob`) |
| `core/partscan.cmake` | ライブラリ `usbmem_partscan` とテスト・CLI の定義 |
| `core/tests/test_partscan.cpp`, `test_bootfix.cpp`, `partscan_test_util.h`, `partscan_cli.cpp`, `tools/check_partscan_tools.py` | 単体テスト、手組みイメージの部品、検証用 CLI、外部ツールとの照合 |

## 2. パーティション探索 (読み取り専用)

```cpp
ScanOptions o;            // level = Quick / Full、min_confidence (既定 40)、cancel、progress(phase, done, total)
ScanResult r;
Status st = scan_partitions(dev, o, &r);   // Cancelled でも途中までの r が使える
```

### 方式

512 バイト単位のチャンクに見つかったシグネチャ (アンカー) から「パーティションの開始位置の仮説」を作り、FS ごとの検証 (ヘッダの整合・範囲・バックアップ構造・先頭の FAT/MFT など) で確かめる。同じ (開始, 系統) の仮説は 1 つにまとめる。

- **Quick**: セクタ 0〜127、1 MiB 境界 (最大 `max_positions` = 8192 か所)、旧 CHS のシリンダ境界 (k×16065 / +63)、既存の表と GPT エントリの開始位置、ディスク末尾の数セクタ、そして見つかった FS の終端の直後 (次の区画の候補) を調べる。バックアップ構造の探索 (ext の予備グループ、btrfs の 64 MiB ミラーなど) は 64 MiB 未満・表の開始位置・FS の終端直後だけに絞って速さを保つ。
- **Full**: 4 MiB ずつ順に読み、全セクタ境界を調べる (小さい区画・奇数位置の区画も見つかる)。`progress("scan", 読んだバイト数, 総バイト数)`。
- 読み取りエラーは数えてその位置を飛ばす (`read_errors`)。取り消しは `CancelToken`。

### 対応する FS

FAT12/16/32、exFAT、NTFS、ext2/3/4、XFS、Btrfs、F2FS、squashfs、erofs、ISO 9660、UDF、LUKS1/2、BitLocker。既存の `probe` の判定は流用せず、位置が分からない状態でも使えるよう検出器を別に持つ (BPB / スーパーブロックの検査は `bootfix` と共有)。

**先頭が壊れていても見つける**: FAT32 (セクタ 6 のバックアップ)、exFAT (セクタ 12〜23)、NTFS (最終セクタ)、ext (予備スーパーブロック + 予備 GDT)、F2FS (2 つ目のスーパーブロック)、Btrfs (64 MiB のミラー)、LUKS2 (副ヘッダ "SKUL")。この場合は `primary_ok = false` / `backup_ok = true` で、信頼度は健全なものより低い (`primary-damaged` / `found-by-backup` の注記)。

**別の FS のバックアップを区画と取り違えない**: FAT32 のセクタ 6 の写し、exFAT のセクタ 12 の写し、F2FS の 2 つ目のスーパーブロック、NTFS の最終セクタの写し、XFS の AG コピー (AG 0 以外のスーパーブロック) は、候補から外す。

### 候補 (`ScanCandidate`)

開始 (セクタ・バイト)、大きさ (ヘッダから。`size_known` / `size_minimum`)、`fits_device` (ヘッダの大きさがデバイスに収まるか。切り詰められたイメージでは false で `exceeds-device`)、種別 (`fs` / `variant`)、信頼度 (0〜100)、`primary_ok` / `backup_ok`、注記、既存の表との関係、入れ子の親 (`nested_in`: 別の FS の内側にある FS = イメージファイルなど)。

信頼度の目安: 先頭の構造が整合し第 2 の証拠 (FAT の先頭エントリ、MFT、グループ記述子、バックアップの一致など) もある = 90 以上、先頭だけ = 60〜85、バックアップだけ = 50〜70。`min_confidence` 未満は出さない。

### 既存の表との関係 (`relation`)

| 値 | 意味 |
|---|---|
| `match` | 表のエントリと開始が一致し、大きさが収まる (FS 単体のデバイスも match) |
| `overlap` | 開始が違うが重なる、または FS がエントリより大きい |
| `contained` | エントリの内側にある (入れ子) |
| `not-in-table` | どのエントリにも対応しない = 失われた候補 |

表のエントリで FS が見つからなかったものは `table_entries_without_fs` に出る。

### JSON

`scan_result_to_json` (C API: `usbmem_partscan_result_json`)。64 ビット値は文字列。キーは `c_api_partscan.h` のコメントを参照。

## 3. 失われた表の復元案

```cpp
std::vector<RecoveryProposal> props;
propose_table_recovery(dev, scan, RecoveryOptions{}, &props);   // 信頼度の高い順
EditResult r = queue_recovery_proposal(editor, props[i]);       // PartitionEditor に操作を積むだけ (書かない)
// あとは editor.plan() → make_confirm_token → apply (docs/partition-edit.md。書き込み前に表のバックアップ USBMTBK1)
```

| `id` | 内容 | 信頼度 |
|---|---|---|
| `gpt-copy-backup-to-primary` / `gpt-copy-primary-to-backup` / `gpt-move-backup-to-end` / `gpt-rebuild-protective-mbr` | GPT の片方 (予備ヘッダ + 予備配列を含む) が生きているときの修復。エディタが提案する修復をそのまま案にする。GUID・名前・属性までそのまま | 90〜95 |
| `add-missing` | 既存の表を残し、表に無い候補 (`not-in-table`) だけを足す | 既存エントリ = 100、追加分 = 候補の信頼度の平均 |
| `rebuild-gpt` | 見つけた FS から GPT を作り直す (新しいディスク GUID) | 候補の信頼度の平均 × 0.85 + 加点 (表の残骸がある +12、無いとき GPT +1) |
| `rebuild-mbr` | 同じく MBR。4 つまでは主区画、それ以上は主 3 + 拡張 + 論理 (EBR は開始の 1 セクタ前)。2 TiB を超える / 512 バイト以外のセクタでは信頼度 10 以下 | 同上 (手がかりが無ければ MBR +3 = USB メモリの標準を優先) |

- 表に載せる区画は信頼度・入れ子・重なりで選ぶ (入れ子の FS は載せない、先頭が壊れている FS は載せるが信頼度を下げる)。
- 大きさ: ヘッダから分かるものはその大きさ (1 MiB 境界へ切り上げ、次の区画にぶつかるなら手前まで)。**分からないもの (LUKS1・UDF など) は次の区画の手前 / ディスクの終わりまで**広げる (候補側に `size-unknown` の注記が付く)。
- 種別: FS から GPT の型 GUID / MBR の種別へ対応づける (NTFS・exFAT・BitLocker・ISO・UDF = MS basic data / 0x07、FAT32 = 0x0C、FAT16 = 0x04 / 0x06、LUKS = LUKS 型 / 0x83、ext・XFS・Btrfs・F2FS など = Linux filesystem / 0x83)。
- どの案も、読み取り専用のビューで本物の `PartitionEditor` に積んで `plan()` を作り、重なり・範囲・MBR の本数制限などを検査してから出す (`plan_ok`, `problems`, `confirm_phrase`)。`plan_ok = false` の案は適用できない。
- FS のデータ領域は書かない。書くのは表 (GPT の先頭 34 セクタと末尾 33 セクタ / MBR のセクタ 0 と EBR) だけで、その前に表のバックアップを取る。

## 4. ブートセクタの検査と修復

```cpp
BootInspection ins;
inspect_boot(dev, offset, length /*0=終わりまで*/, FsType::Unknown /*自動*/, {}, &ins);   // 読み取り専用
BootRepairPlan plan;
plan_boot_repair(dev, offset, length, fs, ins.repairs[0].id, ins.repairs[0].source, &plan);  // 書かない
std::string token = make_boot_confirm_token(plan, typed_phrase);                              // typed_phrase == plan.confirm_phrase ("REPAIR xxxxxx")
BootApplyResult r = apply_boot_repair(dev, plan, token, {backup_sink, ...});
```

### 検査 (`verdict`)

`ok` / `warnings` / `repairable` (壊れていて直せる) / `damaged` (壊れていて直せない) / `unrecognized`。`findings[]` に安定したコード (`fat32-boot-invalid` など) と英語のメッセージ、`repairs[]` に今の状態で計画できる修復、`details` に読み取れた値が入る。先頭が壊れていても、バックアップ構造から FS を判別する。

| FS | 検査 | 修復 (`id`) |
|---|---|---|
| FAT32 | セクタ 0 (BPB)・FSInfo・バックアップ (セクタ 6〜8) | `fat32-restore-primary-from-backup` (6〜8 → 0〜2)、`fat32-refresh-backup-from-primary`、`fat32-rebuild-fsinfo` (署名を直し、空き数・次の空きを「不明」0xFFFFFFFF に) |
| FAT12/16 | BPB・FAT の先頭エントリ・2 本の FAT の一致 | **なし** (バックアップが無い)。壊れていれば FAT のレイアウトから BPB の再構築を `proposedBpb` で**提案するだけ** (書かない)。仮定 (512 バイトセクタ・FAT 2 本・ルート 512 エントリ) を `assumptions` に示す |
| NTFS | セクタ 0・最終セクタのバックアップ・$MFT / $MFTMirr の位置 | `ntfs-restore-primary-from-backup`、`ntfs-refresh-backup-from-primary`。**ntfsfix の自動修復は使わない**。バックアップが指す $MFT が無いときは「書き戻しても意味がない」ので勧めない (`ntfs-no-mft`) |
| exFAT | ブート領域 0〜11 とチェックサム (セクタ 11)・バックアップ 12〜23 | `exfat-restore-main-from-backup`、`exfat-refresh-backup-from-main`、`exfat-recompute-checksums` (セクタ 0〜10 が正しいとき) |
| ext2/3/4 | 先頭のスーパーブロック (チェックサム含む)・予備スーパーブロック (e2fsck -b の位置 = ブロック 8193 など)・先頭と予備の GDT の一致 | `ext-restore-superblock-from-backup` (`source` に予備のブロック番号。必要なら GDT も)、`ext-restore-gdt-from-backup` |

### 適用の関門 (`apply_boot_repair`)

1. トークン (SHA-256 に plan_hash・device_id・入力文字列を混ぜる) が合わない → `bad-token`。計画の書き込み内容・位置が計画時と違う → `plan-tampered`。
2. デバイスが読み取り専用 → `device-read-only`。マウント中 (`in_use`) → `in-use`。同じデバイスへの適用が同時に走っている → `busy`。
3. 書く場所の現在の内容が計画時の `before` と違う → `device-changed`。
4. バックアップ: 書き込み前に上書きする領域の元の内容を `backup_sink` に渡す。無い / 失敗 → **何も書かずに** `backup-required` / `backup-failed`。
5. 書く → 読み戻して計画どおりか確認 → もう一度検査してブート構造の Error が消えたか確認。失敗は `write-failed` / `verify-failed` で、**書いた分を元の内容に戻す** (`rolled_back`)。キャンセルも同様。
6. 書くのは計画に載ったブート構造 (セクタ・領域・スーパーブロック + GDT) だけ。FS のデータには触れない。

### バックアップ (`USBMBFX1`)

形式: マジック・版・件数・時刻・offset・length・FS・デバイス ID のハッシュ・レコード {位置, 長さ, 元の内容}・CRC-32・SHA-256。`parse_boot_backup` で検査、`restore_boot_backup` で書き戻す (壊れていれば拒否、デバイスが小さいときも拒否)。書き戻すと修復前のバイト列に完全に戻ることをテストしている。

### ext の注意

予備スーパーブロック / 予備 GDT から戻したあとの空きブロック数・空き inode 数・ディレクトリ数 (集計) は古い値になる。これは `e2fsck -b` で戻した場合と同じで、次に書き込みありの `e2fsck -f` を通せば直る。`e2fsck -fn` は集計だけを指摘する (スーパーブロック・GDT・ビットマップの指摘は無くなる)。

## 5. C API / JNI / Kotlin

`c_api_partscan.h` (コメントが仕様)。進捗とキャンセルは他の API と同じ作法 (`usbmem_pjob`、別スレッドから `cancel` / `progress`)。探索のキャンセルは `Errc::Cancelled` を返すが、途中までの結果のハンドルも返す。

Kotlin: `PartitionScanner.run(deviceHandle, full, ..., job)` → `resultJson()` / `proposalsJson()` / `queueProposal(index, PartitionEditSession)`。`BootFixSession.open(...)` → `inspection()` → `plan(id, source)` → `apply(typedPhrase, backupPath, ...)`。書き込みを伴う操作の確認文字列の入力は UI の責務 (読み取り専用の検査と区別して見せること)。

## 6. 検証

| 層 | 内容 |
|---|---|
| `partscan_unit` (host ctest) | 手組みの合成イメージで全 FS を Quick / Full で検出 (サイズ・種別・信頼度・`fits_device`)、先頭だけ壊した FS をバックアップから検出、ゼロ・乱数・テキスト・ブートコード風の領域で**偽の候補 0**、入れ子、4K セクタ、切り詰め、既存の表との関係、復元案 (GPT 全消去・片方生存・MBR 拡張/論理・追加のみ) を実際に適用して真の表と比較、取り消し/進捗/読み取りエラー、**2000 件の破損入力で落ちない**、C API。ブートセクタ: 全損傷の種類・計画/トークン/適用の各関門・書き込み失敗のロールバック・バックアップの往復・2000 件の破損ヘッダ・C API |
| `partscan_tools` (`tools/check_partscan_tools.py`) | 本物の `sfdisk` で GPT / MBR (拡張 + 論理 3) を作り、本物の `mkfs.*` (ext2/4・FAT16/32・exFAT・NTFS・XFS・Btrfs・F2FS・squashfs・ISO 9660・UDF・LUKS1/2・erofs。無いツールの分は SKIP) で FS を置く。表の消去 / 先頭のゼロ埋め / 表だけの破損 / 末尾の切り詰めのたびに Quick と Full で探索し、全区画の発見・偽の候補 0・大きさを確認。復元案を適用して `sfdisk --json` / `sfdisk -V` で真の表と比べ、表以外のバイトが変わっていないことを確認。ブートセクタは FS ごと・損傷ごとに検査 → 修復 → `fsck.fat -n` / `fsck.exfat -n` / `e2fsck -fn` / `ntfsfix -n` が通ることを確認し、バックアップの書き戻しが修復前のバイト列に戻ることを確認 |
| ASan / UBSan | CI の `san` / `wsan` 構成で上のすべてを実行。ローカルでも通した |

### TestDisk との照合 (差分テスト)

`testdisk /log /cmd <img> analyse,search` を同じイメージに対して実行し (CI の apt に `testdisk` を追加)、ログの検出行 (種別 + 開始 CHS + 終了 CHS + セクタ数) から開始 LBA を計算して、こちらの結果と比べる。

- **出力は決定的** (時刻以外は同じ)。ただし testdisk は CHS で位置を出すので 1〜2 セクタずれた報告 (例: ext4 の予備スーパーブロックからの検出 = 開始の 2 セクタ手前) があり、完全一致は期待できない。表が正常なディスクでは testdisk の `search` は検出を出さない。
- そのため、**こちらの結果と真の表との照合を主**にし、testdisk との比較は「testdisk が真の開始位置に見つけたものをこちらが見落としたら失敗」だけを検査して、差は `info:` として出力する (XFS・Btrfs・F2FS・LUKS・erofs など testdisk が探さない FS はこちらだけが見つける)。
- testdisk が無い・動かない・240 秒を超える場合はその部分だけ SKIP (`skipped:` に出る)。

## 7. 制限・未検証

- **実機・AVD での書き込みは未検証**。テストはすべてファイルイメージ。
- BitLocker のヘッダの総セクタ数 (0x28) の意味が NTFS と同じか (総数 - 1) を本物の BitLocker ボリュームで確かめていないので、大きさは「下限」(`size_minimum`) として扱う。LUKS1 は大きさがヘッダに無いので大きさ不明 (下限 = ヘッダ + 鍵素材)。
- UDF の大きさは AVDP / パーティション記述子から取るが、`mkfs.udf` のイメージでは取れない場合がある (大きさ不明として扱い、復元案では次の区画まで広げる)。
- 探索は区画の開始位置を 512 バイトの倍数で探す (Full) か典型位置 (Quick)。Quick は「1 MiB 未満・旧 CHS 境界・1 MiB 境界・直前の FS の直後」にある区画だけを見つける。
- 4Kn のデバイスは `sector_size` を渡す (自動判別は PartitionEditor と同じ)。
- 拡張区画の中の論理区画の復元は EBR を 1 セクタ前に置く標準的な配置を作る。元の配置と違うことがある (FS の位置は変わらないので読めるが、`sfdisk` の出力は元と違う)。
- ファイル救出 (削除ファイルの一覧・PhotoRec 相当のカービング) は未着手。
