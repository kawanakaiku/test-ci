# データ救出 (7 章) とバックアップ・復元 (8 章): RecoveryEngine / BackupEngine

`core/` に実装した。UI は未接続 (C API → JNI → Kotlin まで)。ホストのテストと、インストール済みの **ddrescue** / **partclone.\*** / e2fsck / fsck.fat / fsck.exfat / lz4 との照合で検証した。**実機の USB デバイスと AVD では未検証** (下の「限界」)。
範囲外: TestDisk 方式のパーティション探索・ファイル救出、PhotoRec 方式のカービング。

## 1. 構成

| ファイル | 役割 |
|---|---|
| `core/include/usbmem/recovery.h`, `src/recovery.cpp` | `Mapfile` (GNU ddrescue の mapfile を読み書き・編集)、`RescueEngine` (救出本体)、`rescue_preflight` (開始前の検査) |
| `core/include/usbmem/backup.h` | バックアップ/復元の公開 API (`backup_filesystem` / `read_image_info` / `verify_image` / `plan_restore` / `restore_token` / `apply_restore`) |
| `src/backup_internal.h` | 内部共有 (ブロックの選択・チェックサム区切り・LZ4 の入出力) |
| `src/backup_fs.cpp` | ファイルシステムの認識と「使用中ブロック」ビットマップの作成 (ext2/3/4・FAT12/16/32・exFAT・NTFS) |
| `src/backup_image.cpp` | Partclone イメージ形式の書き出し・読み取り・検査 |
| `src/backup_restore.cpp` | 復元の計画 → トークン → 適用 |
| `src/backup_lz4.cpp` | LZ4 フレーム形式 (自前実装。依存なし) |
| `include/usbmem/c_api_recovery.h`, `src/c_api_recovery.cpp` | C API `usbmem_rescue_*` / `usbmem_backup_*` / `usbmem_restore_*` と `usbmem_rjob` (取り消し・一時停止・進捗) |
| `android/app/src/main/cpp/jni_recovery.cpp`, `RecoveryBackup.kt` | JNI と Kotlin (`Rescue` / `Backup` / `RestorePlan` / `RecoveryJob`) |
| `core/recovery.cmake` | ビルド定義 (`usbmem_recovery`、テスト、CLI) |
| `core/tests/test_recovery.cpp`, `test_backup.cpp`, `recovery_test_util.h`, `recovery_cli.cpp` | 単体テストと検証用 CLI |
| `tools/check_recovery_tools.py` | ddrescue / partclone / lz4 / fsck との照合スクリプト |

## 2. データ救出 (RescueEngine)

読み取り元 `BlockDevice` から出力先 `BlockDevice` へ複製する。ddrescue と同じ考え方で、**まず健全な領域を大きな塊で読み、エラー領域は後回しにして細分化する**。

### 2.1 段階

| 段階 | 内容 |
|---|---|
| copy | 大きなクラスタ単位で順方向に読む。エラーに当たったら適応的に飛ばし (`*` = 未トリム)、飛ばした先から再開 |
| copy-2 | 飛ばした領域を戻って読む |
| trim | 未トリム領域を両端から 1 セクタずつ詰め、最初に失敗した所で止める (`/` = 未スクレイプ) |
| scrape | 残りを 1 セクタずつ読む |
| retry | 不良セクタ (`-`) を指定回数、向きを交互にして再読 |

読み取りは「範囲のどこかに不良セクタがあれば範囲全体が失敗する」デバイスを想定している (USB MSC と同じ)。`--no-trim` / `--no-scrape` で段階を省くと、`*` や `/` が残る (ddrescue と同じ)。

### 2.2 mapfile

GNU ddrescue の mapfile 形式 (コメントヘッダ、`現在位置 状態 パス` の行、`pos size status` の行。状態は `? * / - +`) をそのまま読み書きする。ddrescue が作った mapfile から続けられ、usbmem が作った mapfile を ddrescue が引き継げる (`tools/check_recovery_tools.py` で両方向を確認)。
- 保存は一時ファイル + fsync + rename で原子的。保存に失敗したら止まる (`MapfileError`。不良として記録しない)。
- 壊れた mapfile (範囲の重なり・隙間・不正な状態文字など) は読み込みで拒否し、出力には何も書かない。

### 2.3 安全

- **読み取り元には絶対に書かない** (`BlockDevice` の write は一度も呼ばない。テストで検証)。
- 読み取り元と出力先が同じ (同じファイル・シンボリックリンク経由・同じデバイスを含む) なら、何も開始せずに拒否する (`same-file`)。
- デバイスが抜かれた (`Disconnected` / `Closed`) 場合は不良として記録せず `InputGone` で止まる。出力側の書き込み失敗も不良とみなさない (`OutputError`)。いずれも同じ mapfile で再実行すれば続きから行う。
- USB デバイスを痛めないための**安全弁** (0 = 無効): 連続エラー数 `max_consecutive_errors`、総エラー数 `max_errors`、一定時間内のエラー率 (`error_window_ms` 中に `max_errors_in_window` 回)、進捗が無い時間 `no_progress_timeout_ms`、読み取りの間の休止 `pause_between_reads_ms`、読み取りのタイムアウト `read_timeout_ms`。時計は注入できる (テストは偽の時計)。止まった理由は `RescueStop::SafetyAbort` で、再開できる。
- 一時停止・取り消し・進捗: `RescueControl` (C API では `usbmem_rjob`)。取り消しは次の読み取りの前に効き、mapfile は保存済みの状態になる。

## 3. バックアップ・復元 (BackupEngine)

### 3.1 対応と形式

| FS | 使用中ブロックの求め方 | Partclone のブロック | 備考 |
|---|---|---|---|
| ext2/3/4 | グループ記述子 (meta_bg / 64bit / bigalloc を含む) のブロックビットマップ。`BLOCK_UNINIT` のグループは保守的に使用中 | FS ブロック | `needs_recovery` (ジャーナル未再生) や not-clean は拒否 |
| FAT12/16/32 | FAT のエントリ ≠ 0 + 予約領域・FAT・ルート領域 + 端数セクタ | セクタ | FAT32 は BPB の配置で判定 (クラスタ数が少なくても FAT32 として扱う。Linux・fsck.fat と同じ) |
| exFAT | ルートの 0x81 (Allocation Bitmap) エントリ | セクタ | |
| NTFS | MFT レコード 6 `$Bitmap` (fixup、runlist 解読) | クラスタ | `$Volume` の dirty フラグが立っていれば拒否。末尾のバックアップブートセクタは Partclone と同じく保存せず、復元時に再作成 |

暗号化ボリューム (BitLocker / LUKS) は `Unsupported` ("encrypted")。先に `BlockTransform` で復号したデバイスを渡す。
未クリーンの FS は `fs-not-clean` で拒否する。`full_if_unclean` を立てると、全ブロックを使用中として丸ごと保存する。

**画像形式は Partclone の image 0002** (partclone 0.3.49 の実出力から逆解析)。ヘッダ 0x6e バイト、ビットマップ + CRC、1 MiB (`ブロック数 = 1MiB / ブロックサイズ`) ごとの CRC32 (初期値 0xFFFFFFFF・最終 xor なし)。partclone の同じ FS・同じ入力に対し、**ヘッダ以降が 1 バイトも違わない画像**を作る (スクリプトで確認)。`partclone.chkimg` / `partclone.info` / `partclone.restore` が読め、partclone の画像を usbmem が復元できる。
圧縮は **LZ4 フレーム** (自前実装。1 MiB の独立ブロック)。`lz4` コマンドの出力 (`-1`〜`-9`、`-BD`、`-B4`〜`-B7`、フレーム CRC の有無、content size) を読め、usbmem の出力は `lz4 -d` で元の Partclone 画像に戻る。Partclone 自身には圧縮機能が無く外部の圧縮に任せるので、「画像全体を lz4 で包む」形になっている。
未対応: Partclone 画像形式 0001、バイト単位ビットマップ、`$Bitmap` が属性リストや複数のデータ run に分かれた NTFS (拒否する)、ビットマップが 1 GiB を超える大容量。

### 3.2 復元: 計画 → 確認トークン → 適用

パーティション編集 (`docs/partition-edit.md`) と同じ流れ。**画像と書き込み先を決めても、何も書かない**。

1. `plan_restore(画像, 書き込み先, 書き込み先が使用中か)`: 画像を検査し、`RestorePlan` (ok / code / message / warnings / 確認文字列 `RESTORE xxxxxx` / 計画のハッシュ) を返す。ok でなくても作る (UI が理由を見せられる)。
2. ユーザーに確認文字列を打たせる。`restore_token(計画, 打った文字列)` が `SHA-256("usbmem-restore-v1\0" + 計画ハッシュ + "\0" + 書き込み先 ID + "\0" + 打った文字列)` を返す。
3. `apply_restore(計画, 画像, 書き込み先, トークン)`: **計画を作り直してハッシュを比べ** (`plan-changed`)、トークンを確かめてから書く。

計画のハッシュは、画像のヘッダ・ビットマップ・全体の SHA-256、書き込み先の ID とサイズに結び付く。計画のあとに画像や書き込み先が変わると適用は拒否される。

拒否する場合 (どれも書き込み前): `target-read-only`、`target-in-use` (マウント中など。呼び出し側が申告)、`target-too-small`、`bad-token`、`plan-changed`、画像の検査失敗 (`image-corrupt`)。

適用の手順:
1. **先頭 4096 バイトをゼロにする**(途中で止まっても「もっともらしい FS」が残らないよう、まずブートセクタを無効にする)。
2. 使用中ブロックを順に書く。ブートセクタ・スーパーブロックを含む先頭領域は**保留**し、最後に 1 回の書き込みでまとめて書く。
3. NTFS はバックアップブートセクタを末尾に書く。
4. **読み戻して検査**: 書いたブロックを読み直し、チャンクごとの CRC と比べる。不一致なら失敗 (`verify-failed`)。
5. 書き込み前に画像全体を検査するので(チャンク CRC・ビットマップ CRC・全体 SHA-256)、壊れた画像では書き込み先に一切触れない。

### 3.3 失敗時の挙動

復元は原子的ではない。途中で電源が落ちると書き込み先は壊れた状態になるが、上の順序のため**ブートセクタが無効のまま**になり、マウントできる「中途半端な FS」にはならない (テスト `restore_power_loss_never_leaves_a_plausible_filesystem` が、書き込みを任意の回数で打ち切って確認する)。

## 4. C API / JNI / Kotlin

C API は `core/include/usbmem/c_api_recovery.h`。結果はすべて UTF-8 JSON の文字列 (`usbmem_free` で解放)。長い処理は呼び出したスレッドをふさぐので、`usbmem_rjob` を作って別スレッドから `usbmem_rjob_cancel` / `usbmem_rjob_pause` / `usbmem_rjob_progress_json` を呼ぶ。

Kotlin (`RecoveryBackup.kt`):

```kotlin
val job = RecoveryJob()
val r = Rescue.run(inHandle, outHandle, mapPath, RescueOptions(maxConsecutiveErrors = 200, noProgressTimeoutMs = 60_000), job)
val b = Backup.run(srcHandle, imagePath, lz4 = true, job = job)
RestorePlan.create(imagePath, targetHandle).use { plan ->
    if (plan.ok) {
        val token = plan.token(userTyped)          // plan.confirmPhrase と一致していなければ apply が拒否
        plan.apply(targetHandle, token, targetInUse = false, job = job)
    }
}
```

デバイスは `NativeBridge` で得たハンドル (Long)。UI から使うときは必ず Dispatchers.IO など別スレッドで呼ぶ。

## 5. 検証

### 5.1 単体テスト (`usbmem_recovery_tests`)
mapfile (参照実装との一致・不正入力・パーサのファジング)、救出 (不良セクタの記録が正確、段階の順序、取り消し→再開で同じ結果、デバイス消失・出力失敗・mapfile 保存失敗、一時停止、ランダムシナリオの不変条件)、バックアップ/復元(実 FS フィクスチャの往復、画像の全ビット反転・切り詰めの検出、不正ヘッダ・バイト変異のファジング、LZ4 の壊れたストリーム、ゲート、電源断)。

### 5.2 外部ツールとの照合 (`tools/check_recovery_tools.py <usbmem_recovery_cli>`)
- ddrescue: `LD_PRELOAD` で「不良セクタを含む読み取りは EIO」にした同じ入力に対し、ddrescue と usbmem の**最終 mapfile と出力が一致**(既定・`-r1`)。途中まで usbmem → ddrescue、ddrescue → usbmem の引き継ぎも同じ結果。
- partclone: mke2fs (ext2/3/4)、mkfs.fat (12/16/32)、mkfs.exfat、mkntfs、実フィクスチャに対し、使用中ビットマップが partclone と一致し、画像がバイト単位で一致、`partclone.chkimg/info/restore` で読め、双方向の復元後に e2fsck / fsck.fat / fsck.exfat が通る。lz4 コマンドとの往復、改ざんの検出が partclone と一致。

## 6. 限界

- 実機の USB デバイス・AVD では未検証 (ホストのファイルとエラー注入のみ)。
- copy の 2 周目は順方向のみ (ddrescue は逆方向にも読む)。結果の mapfile は同じになるが、読む順序は完全には同じではない。
- Partclone 形式 0001、バイト単位ビットマップ、NTFS の属性リスト/断片化した `$Bitmap`、1 GiB 超のビットマップは未対応。
- 読み戻し検査はページキャッシュを経由することがある(`BlockDevice` が flush/キャッシュ回避を保証する場合のみ実デバイスの内容を確かめる)。
- 復元は原子的ではない (3.3)。
- LZ4 自体には整合性検査が無い。Partclone のチャンク CRC と全体 SHA-256 が守る。
