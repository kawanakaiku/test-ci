# DiskTools: 整合性検査 (fsck 相当) と mkfs

要件書 6 章。`core` に実装 (Android 非依存・PC でテスト可能)。ライブラリは `usbmem_disktools` (STATIC、`usbmem_core` にだけ依存。`core/disktools.cmake`)。
自前実装のみ (e2fsprogs / exfatprogs / dosfstools / ntfs-3g のコードは取り込まない・リンクしない。テスト時の外部ツールとして使うだけ)。

## 1. 読み取り専用の整合性検査

`check_filesystem(dev, offset, length, fs, options, &report)` (`core/include/usbmem/fscheck.h`)。C API は `usbmem_fscheck_run`
(`c_api_disktools.h`)、Kotlin は `DiskCheck.check` (`DiskTools.kt`)。

- **書き込まない**: `write_at` / `flush` を一切呼ばない。読み取り専用で開いたデバイスで動く (単体テストで書き込み回数 0 を確認)。
- **修復しない**: 報告するだけ。NTFS の `ntfsfix` のような自動修復は呼ばない (要件)。
- **ディスク上の構造を信用しない**: すべてのオフセット・長さを境界検査し、ループ・巨大な値でも有限のメモリと時間で終わる。
  `memory_limit` (既定 256 MiB)、`max_objects`、`max_findings` (既定 500。超えた分も件数は数える)、キャンセル、進捗コールバックを持つ。
  上限に当たると verdict は `incomplete` (見つかった誤りがあれば `errors` を優先)。
- 結果: `verdict` = `clean | warnings | errors | unsupported | incomplete`、`findings[]` = `{severity, code, message, offset?, object?}`、
  `unclean` (ダーティ / 未クリーンアンマウント / 未再生ジャーナル)、`needsJournalRecovery` (ext3/4 の未再生ジャーナル。読み取り専用マウントは noload が必要)。
  CLI の終了コード: 0 clean / 1 warnings / 2 errors / 3 unsupported / 4 incomplete。

### 検査内容

| FS | 内容 |
|----|------|
| FAT12/16/32 | BPB の妥当性・種別 (クラスタ数)・FAT 同士の一致・チェーン (ループ・範囲外・クロスリンク)・サイズとチェーン長・LFN (チェックサム・序数)・`.` `..`・重複/不正な名前・孤立クラスタ・ダーティビット・FSInfo・バックアップブート |
| exFAT | ブート領域のチェックサム (106,107,112 を除く)・バックアップ領域・FAT・割り当てビットマップと実使用の差・アップケーステーブル (チェックサム・展開)・エントリセットのチェックサム・名前ハッシュ・NoFatChain・有効データ長 ≤ データ長・クロスリンク |
| ext2/3/4 | スーパーブロック・グループ記述子 (crc16 / crc32c)・パス 1 (iノード・ブロックマップ/extent 木/インライン・EA ブロック・重複ブロック・i_blocks/i_size)・パス 2 (ディレクトリブロック・rec_len・名前・filetype・重複名・末尾チェックサム)・パス 3 (接続性)・パス 4 (リンク数)・パス 5 (ビットマップ・空き数)・ジャーナル状態 |
| NTFS | ブートセクタとバックアップ・`$MFT` / `$MFTMirr` の一致・全 MFT レコード (マルチセクタ保護・ヘッダ・属性列・ランリスト)・MFT ビットマップ・`$Bitmap` とクラスタ使用 (二重割り当て・未登録・取りこぼし)・親ディレクトリの整合・ダーティフラグ・`$LogFile` の状態 (未再生) |

### 検査しないもの (限界)

- ext: htree のハッシュ順・bigalloc・meta_bg・外部ジャーナル・quota・ジャーナル内容の再生可否は検査しない。未対応の機能を持つイメージは `incomplete` として知らせる。
- NTFS: ディレクトリのインデックス (B+ ツリー) の中身・`$Secure`・`$UpCase`・`$AttrDef`・`$LogFile` の再生可否・圧縮データの解凍は検査しない (ログは「空か / 未クリーンか」だけ)。
- 外部ツールが「問題」と言わない状態を過検出として報告しうる箇所は意図的に Warning にしている (例: ext の空き数の不一致、未クリーンのフラグ)。

## 2. 検証 (`tools/check_disktools.py`)

`tools/check_disktools.py <usbmem_disktools_cli> [fat|exfat|ext|ntfs|mkfs|fuzz|all] [--quick]`。外部ツールは**検査のみの引数**でしか呼ばない
(`fsck.fat -n` / `fsck.exfat -n` / `e2fsck -fn` / `ntfsfix -n`)。実イメージ (mkfs.fat+mcopy / mkfs.exfat 由来の生成器 / `mke2fs -d` / mkntfs+ntfs-3g) のバイトを壊し、
「外部ツールが問題と言う」と「自前が警告/エラーを出す」を比べる。**見逃し (FN) を最重視**、過検出 (FP) も数える。外部ツールが無ければ SKIP (終了 77)。

| 対象 | ケース数 | 見逃し | 過検出 | 備考 |
|------|---------|--------|--------|------|
| FAT (12/16/32) | 149 | 0 | 0 | 許容した差は `allow` に理由つきで列挙 |
| exFAT | 144 | 0 | 0 | |
| ext2/3/4 | 339 | 0 | 0 | ext2/ext3/ext4 (extent・metadata_csum・inline_data・小 iノード) |
| NTFS | 76 | 0 | 0 | `ntfsfix -n` は見る範囲が狭い。見えない破壊は「壊した事実 (構成上の正解)」を基準にした |
| mkfs | 35 | 0 | 0 | 下記 |
| fuzz | 8 種 x 250 | 落ちた 0 | | ASan/UBSan 付きの CLI で無作為な破損 2000 件 |

許容した差 (`allow`): ext の `sb_error_state` など、e2fsck が「問題」とは言わないが `-fn` で状態として表示されるもの (自前は Warning で報告)。
NTFS は `ntfsfix -n` が dirty / ログ未クリーンを報告しないため、これらは構成上の正解で見ている。

`ASan` 付きの実行: `cmake -S core -B build-asan -DUSBMEM_BUILD_TESTS=ON -DUSBMEM_SANITIZE=ON` → `ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=0 tools/check_disktools.py build-asan/usbmem_disktools_cli all`
(環境によっては `verify_asan_link_order=0` が要る)。単体テスト (`usbmem_disktools_tests`) も同じ設定で通る。

## 3. mkfs (ext2 / exFAT / FAT32)

`Mkfs::plan` → `Mkfs::make_confirm_token` → `Mkfs::apply` (`core/include/usbmem/mkfs.h`)。C API は `usbmem_mkfs_*`、Kotlin は `MkfsSession`。
NTFS の mkfs は対象外。FAT12/16 も対象外 (FAT32 が小さすぎる容量は理由を示して拒否する)。

安全モデル (パーティション編集 `docs/partition-edit.md` と同じ流儀):

1. **計画は書かない**。範囲・サイズ・ラベルを検証し、書く領域 (ブート領域・FAT・ビットマップ・iノードテーブル・ルート等) の一覧を作る。データ領域には書かない。
2. **確認トークン** = SHA-256("usbmem-confirm-v1", 計画ハッシュ, デバイス識別, ユーザーが打った確認文字列)。計画ハッシュは書く領域の中身と事前内容のダイジェストを含む。
   確認文字列は、既存のファイルシステム・パーティション表・何か分からない内容を壊すなら `ERASE xxxxxx`、完全に空なら `FORMAT xxxxxx`。
3. **適用の拒否条件**: デバイスが読み取り専用 (`device-read-only`)、マウント中 (`in-use`)、計画が不可 (`plan-invalid`)、計画の改ざん (`plan-tampered`)、トークン不一致 (`bad-token`)、
   別デバイス/範囲 (`wrong-device`)、計画後にディスクが変わった (`device-changed`)、同じデバイスで別の適用が進行中 (`busy`)、バックアップが取れない (`backup-required` / `backup-failed`)。拒否時は 1 バイトも書かない。
4. **書く前のバックアップ**: 上書きする領域の元の内容 (領域ごとに 4 MiB 以下、合計 64 MiB まで) を `backup_sink` に渡す。保存できなければ書かない。
   大きなゼロ埋め領域 (FAT 本体・iノードテーブル) は含まれない (計画に Warning `partly-unbacked` で明示)。`mkfs_restore_backup` で書き戻せる。
5. **書き込み順**: 旧ファイルシステムの識別子を先に消す (`invalidate-old`) → 控え・FAT・ビットマップ・ルート等 → 最後に主ブートセクタ/主スーパーブロック (これが「できあがり」の印)。各段階の後に flush。
6. **失敗・キャンセル**: バックアップした領域を元に戻そうとし、戻せたかを `rolledBack` で報告する。
7. **検証**: 書いた領域をすべて読み戻して比べ、さらに自前の検査 (`check_filesystem`) が `clean` であることを確認する。両方通ったときだけ `verified = true`。

作る構造:

- **FAT32**: 予約 32 セクタ (0=ブート, 1=FSInfo, 2=拡張, 6〜8=控え)・FAT x2・クラスタ 2=ルート。クラスタ数 65525 以上 (未満は FAT16 と判別されるため拒否)。
  クラスタサイズは容量に応じて自動 (512B / 4K / 8K / 16K / 32K)。64 KiB まで指定可。データ領域の先頭はクラスタ境界に揃える。ラベルは ASCII 大文字 11 文字まで。
- **exFAT**: ブート領域 12 セクタ + 控え・FAT 1 本・ビットマップ・アップケーステーブル (Unicode の大文字写像から生成した圧縮表)・ルート。ラベルは UTF-16 11 文字まで。
- **ext2**: ジャーナル無し・flex_bg 無し・リサイズ予約無し (compat=dir_index / incompat=filetype / ro_compat=sparse_super+large_file)。ブロック 1/2/4 KiB、
  スーパーブロックと GDT の控えは 0, 1, 3/5/7 のべき乗。lost+found 付き。iノードテーブルは全部ゼロ埋めする (大きいデバイスでは時間がかかる。Warning `slow`)。ラベルは 16 バイトまで。

mkfs の検証 (`tools/dt_mkfs.py`): 0xA5 で埋めたイメージに作って、`fsck.fat -n` / `fsck.exfat -n` / `e2fsck -fn` がクリーンと言うこと、
さらに実際に書き込める (FAT32: `mcopy`・`mmd` で書いてから `fsck.fat -n` と読み戻し / ext2: `debugfs -w` で書いてから `e2fsck -fn` と読み戻し) ことを確認。
サイズ・クラスタ・セクタ 4096・パーティション内オフセット・端数のあるサイズ・5 GiB の 64 KiB クラスタなど 35 ケース。

### 既知の制限

- ext2 以外の ext (ext3/4 のジャーナル・extent・メタデータチェックサム) は作れない (要件は ext2 まで)。
- exFAT のアップケーステーブルは Windows 標準のものと完全には一致しない (BMP の単純な大文字写像)。Linux・Android・Windows とも表のチェックサムを検証して使う。
- FAT32 のブートコードは起動しない (停止するだけ)。起動可能なディスクは作らない。
- 実機 (USB メモリ) への書き込みは行っていない (イメージファイルと `MemoryDevice` のみ)。実機での確認は未了。
- 実際の Windows / macOS での読み取りは未確認 (Linux の `fsck.*` と mtools / debugfs のみ)。

## 4. API 一覧

| 層 | 検査 | mkfs |
|----|------|------|
| C++ | `check_filesystem`, `fscheck_report_to_json` | `Mkfs::plan / make_confirm_token / apply`, `mkfs_plan_to_json`, `parse_mkfs_backup`, `mkfs_restore_backup` |
| C | `usbmem_fscheck_run`, `usbmem_fscheck_supported`, `usbmem_job_*` | `usbmem_mkfs_open / plan_json / make_token / apply / last_backup / backup_info_json / restore_backup / min_size` |
| JNI | `jni_disktools.cpp` (`DiskTools.native*`) | 同左 |
| Kotlin | `DiskCheck.check`, `DiskJob` | `MkfsSession.open / plan / apply`, `MkfsSession.restore` |

UI は未接続。
