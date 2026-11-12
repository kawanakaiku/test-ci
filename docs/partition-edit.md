# パーティション管理 (要件書 4 章): 情報取得・編集・バックアップ/復元

`core/` に実装した。UI は未接続 (C API → JNI → Kotlin まで)。ホストのテストと外部ツール (sfdisk / sgdisk / parted / fdisk / blkid) との照合で検証した。**実機・AVD での書き込みは試していない** (テストはすべてファイルイメージ。実機で壊してよいディスクを用意できたら別途行う)。

## 1. 構成

| ファイル | 役割 |
|---|---|
| `core/include/usbmem/partition.h`, `src/partition.cpp` | 既存のパーサの拡張 (追記のみ): PARTUUID、GPT 属性、種別名、FS ラベル/UUID/シリアル |
| `fs_label.h`, `src/fs_label.cpp` | FS ラベル/UUID/シリアルの軽量リーダ (マウントしない・読み取り専用) |
| `sha256.h`, `src/sha256.cpp` | 自前の SHA-256 (確認トークン・バックアップの完全性) |
| `partition_edit.h`, `src/partition_edit.cpp` | `PartitionEditor` (操作 → 計画 → 適用)、GPT/MBR のシリアライザ、復元の計画 |
| `src/gpt_inspect.cpp` | GPT の詳細検査 (`inspect_gpt`)、修復の提案 |
| `src/partition_backup.cpp` | バックアップファイルの形式、`sfdisk --dump` 互換の出力 |
| `src/partition_edit_json.cpp` | 計画・検査の JSON |
| `c_api_partedit.h`, `src/c_api_partedit.cpp` | C API `usbmem_partedit_*` |
| `android/app/src/main/cpp/jni_partedit.cpp`, `PartitionEditor.kt` | JNI と Kotlin (`object PartitionEditor` + `PartitionEditSession`) |
| `core/tests/partedit_cli.cpp`, `tools/check_partedit_tools.py` | 検証用 CLI と、外部ツールとの照合スクリプト |

## 2. 取得できる情報

`partition_table_to_json` に次のキーを追加した (既存のキーは変えていない)。

- パーティションごと: `partUuid` (GPT = ユニーク GUID の小文字、MBR = `<ディスク署名 8 桁>-<番号 2 桁>`。Linux の PARTUUID と同じ)、`gptAttributes` (64 ビット、文字列)、`typeName` (GPT 型 GUID / MBR 種別バイトの名前。表を拡充)、`bootable` (MBR の active)、`fsLabel` / `fsUuid` / `fsSerial` / `fsGuid` / `fsDetected`。
- テーブル: `mbrDiskId` (ディスク署名)。
- `ParseOptions::read_labels = false` でラベルの読み取りを止められる。

### FS ラベル/UUID (`read_fs_label`)

NTFS (`$Volume` の `$VOLUME_NAME`、BPB のシリアル)、FAT12/16/32 (ブートセクタ + ルートのボリュームラベルエントリ。ルートのエントリが優先)、exFAT (ラベル 0x83、GUID 0xA0、シリアル)、ext2/3/4、ISO9660 (PVD)、UDF (AVDP → PVD / LVD の dstring)、LUKS 1/2、btrfs、xfs。`FsType::Unknown` のときは magic で erofs / squashfs / LUKS を判別する。

- マウントしない。範囲 (パーティション) の外は読まない。すべての長さ・オフセットを検査し、壊れた入力でも例外・無限ループ・範囲外アクセスを起こさない (テストでコーパスを壊して fuzz)。
- 文字列は整形する: 正しい UTF-8 はそのまま、そうでなければ非 ASCII は `?`、制御文字は空白。
- UUID の表記は blkid に合わせた (FAT / exFAT / NTFS は `XXXX-XXXX` / 16 桁 16 進、ext / btrfs / xfs / LUKS / erofs は小文字の 8-4-4-4-12)。exFAT のボリューム GUID は Microsoft 形式 (先頭 3 フィールドがリトルエンディアン) で `fsGuid` に出す。UDF の UUID は blkid と同じ 16 桁。

## 3. GPT の検査と修復計画

`inspect_gpt` は primary / backup それぞれについて、署名・ヘッダ CRC・リビジョン・ヘッダサイズ・自己参照 LBA・使用可能範囲・エントリ配列の位置/CRC を調べ、`copiesAgree` (GUID・範囲・配列が一致)、バックアップが末尾にあるか、ディスクが大きくなったか (`diskGrew`)、イメージが縮んだか (`backupBeyondDisk`)、保護 MBR の状態 (有無・範囲・ハイブリッド) を返す。診断メッセージと、提案する修復 (`suggestedRepairs`) を付ける。

| 修復 | いつ | 動作 |
|---|---|---|
| `copy-primary-to-backup` | primary が正しい | backup を作り直す (ディスク末尾) |
| `copy-backup-to-primary` | backup が正しい | primary を作り直す |
| `move-backup-to-end` | ディスクが大きくなった / イメージが縮んだ | 使用可能範囲の末尾・backup の位置・保護 MBR を更新して backup を末尾へ |
| `rebuild-protective-mbr` | 保護 MBR が無い / 範囲が違う | 保護 MBR を作り直す (ハイブリッド MBR は破棄される) |

修復も通常の編集と同じ `plan → apply` を通る (勝手に直さない)。縮んだイメージは既存パーサだと「使えない」扱いなので、`truncated` を別に持ち、`move-backup-to-end` だけは受け付ける。

## 4. 編集 (`PartitionEditor`)

```
open(dev) ─→ 操作を積む (create / remove / resize / move / type / name / attrs / boot / uuid / diskid / new_table / repair_gpt)
          ─→ plan()      ※ 何も書かない: 検証・差分・書き込み予定 (セクタ単位)・確認文字列
          ─→ make_confirm_token(plan, ユーザーが入力した文字列)
          ─→ apply(plan, token)
```

- **ハンドル**: 既存のパーティションは `index` (GPT はスロット+1、MBR は表示順)。`create` の戻り値が新規分のハンドル。
- **plan()** は作業中のモデルを GPT/MBR(+EBR) のセクタ列に直し、現在のディスクの内容と比べて同一のセクタを落とし、**書き込みを重ねたオーバーレイを実際にパーサで読み直して `after` を作る** (モデルと読み戻しが食い違えば内部不整合として拒否)。書き込み先がテーブル用セクタ以外 (= データ領域) に当たっていないかも検査する。
- **検証**: 長さ 0 / 範囲外 / 使用可能範囲外 / 32 ビット LBA 超過 / オーバーフロー / 重なり / 主パーティション数 (MBR は 4 まで、拡張は 1 つ) / 論理が拡張の外 / GPT エントリ数超過 / 名前が 36 UTF-16 単位を超える、などを Error にする。整列していない配置、削除・縮小・移動は Warning / `destructive`。
- **MBR の論理パーティション**: EBR チェーンを作り直す。最初の EBR は拡張の先頭、以降の EBR は論理パーティションの直前 (`start-1`) に置く。拡張内の論理は `[start-1, start+count)` を占有するとして重なりを検査する。CHS は fdisk と同じく `FE FF FF` に丸める。
- **新規テーブル** (`new_table`): パーティション無しのファイルシステム (スーパーフロッピー) の上に作るときは `overwrite_filesystem = true` を明示しない限り拒否する (ブートセクタを壊すため)。

### 4.1 確認トークン

```
confirmPhrase = "WRITE xxxxxx"  (復元でサイズ不一致などを強制するときは "FORCE xxxxxx"; xxxxxx は plan_hash の先頭 6 文字)
token = SHA-256("usbmem-confirm-v1" \0 plan_hash \0 device_id \0 ユーザーが打った文字列)
plan_hash = SHA-256(device_id と全書き込みの内容)
```

`apply()` は渡された計画の内容から `plan_hash` を**再計算**して、一致しなければ `plan-tampered` で拒否する。打った文字列が `confirmPhrase` と違えば `bad-token`。計画後にデバイスの該当セクタが変わっていたら `device-changed` (書き込み前の再読み取りで検出)。

### 4.2 適用の安全ゲート (どれかに当たると何も書かない)

| code | 条件 |
|---|---|
| `device-read-only` | デバイスが読み取り専用 |
| `in-use` | 呼び出し側が「使用中 (マウント中など)」と宣言 |
| `busy` | 同じデバイスに書き込み可のエディタが既にある (open で拒否) |
| `bad-token` / `plan-tampered` / `device-changed` | 上記 4.1 |
| `backup-required` / `backup-failed` | 書き込み前の自動バックアップが無い / 保存に失敗 (`ApplyOptions::require_backup` が既定 true) |
| `force-required` | 復元でディスクサイズ/セクタサイズが違うのに FORCE 確認文字列でない |

### 4.3 書き込み順序

GPT: **backup entries → backup header → primary entries → primary header → 保護 MBR**。各段階の後に `flush`。どの時点で止まっても primary か backup のどちらかは有効に残り、表は「古い」か「新しい」のどちらかとして読める (電源断シミュレーションで全書き込み位置について確認した)。MBR: EBR を末尾側から先頭側へ、最後に MBR (セクタ 0)。

書いた後は**読み戻して**計画と一致するか (再パースした表が `after` と同じか) を検証する (`verified`)。

書き込み中に失敗したら、書いた分を**逆順に元の内容へ戻す** (`rolledBack`)。戻す書き込みも失敗したときは報告する (バックアップファイルから復元できる)。

### 4.4 自動バックアップ

`apply()` は書き込みの直前に `backup_sink` へ「これから書き換えるセクタの元の内容 + テーブル関連セクタ」をバックアップ形式で渡す。C API の `usbmem_partedit_apply(..., backup_path, ...)` は渡されたパスへ**書いて fsync してから**書き込みを始める。保存できなければ書かない。

## 5. バックアップ / 復元

### ファイル形式 (`USBMTBK1`、リトルエンディアン)

```
0   magic "USBMTBK1"
8   u32 version (=1)        12 u32 ヘッダ長 (=64)
16  u32 セクタサイズ         20 u32 テーブル種別 (0 none / 1 mbr / 2 gpt)
24  u64 ディスクのバイト数    32 u64 作成時刻 (UNIX 秒)
40  16B ディスク ID (MBR 署名 / GPT ディスク GUID)
56  u32 レコード数
64  レコード × N (LBA 昇順): u64 lba, u8 kind, pad3, u32 len, data[len]
    u32 CRC-32 (ここまで全部)   32B SHA-256 (ここまで全部。CRC を含む)
```

保存するのは「テーブルに関わるセクタ」: セクタ 0 (MBR/保護 MBR)、EBR チェーン、GPT の primary/backup のヘッダとエントリ配列。パーティションのデータは含まない。壊れたファイルは CRC / SHA-256 / 範囲の検査で拒否する (全バイトを 1 ビットずつ反転して検出を確認)。

### 復元

`plan_restore` が計画を作る。ディスクサイズまたはセクタサイズが違うときは `requiresForce` (確認文字列が `FORCE ...` になる)。ディスク ID が違うだけなら警告。ファイルの LBA がディスクの外なら強制してもエラー。書き込み順は GPT の backup entries → backup header → primary entries → primary header の後に ebr → mbr、最後にバックアップに無い古い GPT ヘッダの消去。**復元も `apply` を通る**ので、現在の状態の自動バックアップを取り、読み戻しで検証する。

### `sfdisk --dump` 互換

`export_sfdisk` は `sfdisk -d` と同じ書式を出す (sfdisk 2.42.4 の出力と項目単位で一致することをテストで確認)。注意点: GPT の属性は `attrs="RequiredPartition LegacyBIOSBootable GUID:60"` で、`GUID:N` は**絶対ビット番号** (48..63)。dos は `type=83` (小文字 16 進)・`bootable`・`label-id: 0x....`、論理は 5 から。デバイス名が数字で終わるときは `p` を付ける。出力を `sfdisk` に流し込んで同じ表を再現できる。

## 6. C API / JNI / Kotlin

`c_api_partedit.h` に宣言。ハンドル式で、戻り値は 0 または `usbmem::Errc`、機械可読な詳細コードは `usbmem_partedit_last_error` (`h == NULL` なら直近の open の失敗)。JSON / テキストを返すものは `usbmem_free` で解放する。操作を積むと保持している計画は無効になる (apply は `no-plan`)。

JNI は `Java_org_usbmem_app_PartitionEditor_nativeXxx` (jni_bridge.cpp と同じ規約: UTF-8 の byte[]、失敗は戻り値か `errorOut[0]`)。Kotlin は `object PartitionEditor` (external 宣言のみ) と、その上の `PartitionEditSession` (`create` / `remove` / `plan` / `apply` / `backup` / `exportSfdisk` ...)。`NativeBridge.kt` には触れていない。デバイスを閉じる**前**にセッションを閉じること。

## 7. 検証

| 方法 | 内容 |
|---|---|
| 単体テスト (`usbmem_tests`) | SHA-256 の既知値、PARTUUID・種別名、GPT 検査、GPT/MBR/論理の編集、ゲートとトークン、書き込み順序と flush、バックアップ形式 (全バイト反転)、復元、sfdisk 出力、修復、FS ラベル (手組みイメージ + 壊れた入力)、C API |
| 故障注入 | 各書き込み・各 flush でエラーを起こす → ロールバックされる / ロールバック不能でも primary か backup が有効で、**データ領域への書き込みはゼロ** |
| 電源断シミュレーション | 全書き込み位置で止める (クリーン / セクタ途中で破れる) → 表は古いか新しいか。MBR の EBR チェーンと新規テーブルの種別切り替えで「どちらでもない」状態は検出されなかった |
| fuzz | ランダムな操作列 2500 回 (拒否された計画は書かず、通った計画は適用して検証)、バックアップファイルの破壊 |
| ASan / UBSan | 上記すべて |
| `tools/check_partedit_tools.py` | 外部ツールとの双方向の照合 (下) |

### 外部ツールとの照合 (`tools/check_partedit_tools.py`、286 項目)

- **外部が作った表を自前が読む**: sgdisk (GPT、属性・日本語名・GUID 指定)、sfdisk (dos、論理 3 つ)、parted (gpt / msdos) が作った表を `sfdisk --json` と比較 (範囲・種別・GUID・名前・active・ディスク ID・PARTUUID)。
- **自前が作った表を外部が検証する**: 新規 GPT / MBR (論理あり)、sgdisk が作った表を編集、sfdisk が作った dos を編集。`sgdisk -v` / `sgdisk -p` / `sgdisk -i`、`sfdisk --verify` / `--json`、`parted print`、`fdisk -l` が問題なしと判定し内容が一致する。GPT の CRC・ヘッダ位置・配列・保護 MBR は Python で独立に再計算 (外部ツールを使わない)。
- **修復**: ディスク拡大 / primary 破損 / backup 破損 / イメージ縮小 / 保護 MBR の破損を、`sgdisk -v` の指摘と照合し、修復後に `sgdisk -v` が通る。primary・backup・保護 MBR の修復は元のイメージとバイト単位で一致する。
- **sgdisk のバックアップとの相互運用**: `sgdisk --backup` → 自前で編集 → `sgdisk --load-backup` で元の表に戻る。自前のバックアップから、sgdisk で壊した表を復元するとイメージがバイト一致で元通りになる。
- **バックアップファイル**: zlib の CRC-32 と hashlib の SHA-256 を Python で再計算し、各レコードがイメージのセクタと一致することを確認。
- **sfdisk 互換出力**: 自前の出力が `sfdisk -d` と一致し、`sfdisk` に流して同じ表を再現できる。
- **FS ラベル**: mkfs.fat (12/16/32)、mkfs.exfat、mke2fs (ext2/ext4)、mkntfs、genisoimage、mkudffs、mksquashfs、mkfs.erofs、mkfs.btrfs、mkfs.xfs、cryptsetup (LUKS2) が作ったイメージを GPT のパーティションの中に置き、`blkid -p` のラベル・UUID と比較。

`ctest` の `partedit_tools` として登録 (sfdisk が見つかったときのみ。無いツールの項目は SKIP)。

## 8. 制限

- **MBR の論理パーティション (EBR チェーン) の編集は複数セクタで、一般には原子的でない。** 試したシナリオでは「古い」「新しい」以外の途中状態は出なかったが、保証はしない。バックアップからの復元で戻せる。
- **ハイブリッド MBR** は `rebuild-protective-mbr` で破棄される。GPT の編集で保護 MBR を書き直すときも、ハイブリッドの 0xEE 以外のエントリは保持しない。
- **排他はデバイスオブジェクト単位** (ベストエフォート)。別プロセス・別のデバイスオブジェクトが同じディスクを開いていても検出できない。「使用中」は呼び出し側が `in_use` で宣言する (OS のマウント状態は見ない)。
- MBR のセクタ 0 のブートコードは `0x55AA` 署名があるときだけ引き継ぐ。
- **リサイズ・移動はテーブルのエントリだけ。** ファイルシステムの中身は動かさず、縮小で中身を切ることもありうる (Warning を出す)。
- 縮んだ GPT イメージの修復は明示的な `move-backup-to-end` のみ。
- 4 Kn (4096 バイトセクタ) は GPT の故障注入テストで確認した (MBR の 4 Kn と外部ツールとの照合は未実施)。実デバイスでは未確認。
- sgdisk の `GUID:N` 属性の番号は絶対ビット番号 (上記)。他のツールの表記とは必ずしも一致しない。
- **実機・AVD での書き込みは未実施。**

## 9. なぜ libfdisk を使わないか

libfdisk (util-linux) は LGPL-2.1+ で、アプリの native 層 (すでに GPL-2.0-or-later の NTFS / exFAT 等を取り込んだ共有ライブラリ) とライセンス上は共存できるが、次の理由で自前の最小実装にした。

1. **FS アクセスの形が合わない**: libfdisk はファイルディスクリプタ/デバイスパスを自分で開き、`ioctl` (BLKGETSIZE64、BLKRRPART、ジオメトリ) とカーネルの再読み込みを前提にしている。ここでは `BlockDevice` (USB MSC を直接叩く、fd を持たない場合もある) を介して、全 I/O を故障注入・電源断シミュレーション・オーバーレイ読み込みできる形にする必要がある。
2. **計画・差分・書き込み順序の制御**: 確認トークン、セクタ単位の書き込み予定、段階ごとの flush、ロールバック、読み戻し検証を厳密に制御したい。libfdisk は `fdisk_write_disklabel` で一括して書き、順序を呼び出し側で決められない。
3. **サイズと依存**: libfdisk は libuuid / libblkid / libsmartcols などに依存し、Android (bionic、NDK) 向けの移植とビルドの手間が大きい。必要な機能 (MBR / EBR / GPT の読み書きとバックアップ) は数千行で収まる。
4. **検証の独立性**: 実装を libfdisk にすると、参照にしている sfdisk / fdisk と同じコードで書いて同じコードで確認することになり、独立検証にならない。自前実装を sfdisk / sgdisk / parted で照合する方が検証として意味がある。

`sha256` も自前 (外部の暗号ライブラリを入れない)。新しい外部依存・同梱コードは無いので `docs/licenses.md` の更新は不要 (libfdisk の項目は「採用しない」と記録した)。
