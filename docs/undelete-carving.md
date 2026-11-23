# 削除ファイルの一覧と救出 / カービング

PhotoRec / TestDisk (ファイル復旧) 相当の読み取り専用の機能。core の C++ (`core/src/undelete*.cpp`、`core/src/carve*.cpp`)、C API (`core/include/usbmem/c_api_undelete.h`)、
JNI (`android/app/src/main/cpp/jni_undelete.cpp`)、Kotlin (`android/app/src/main/java/org/usbmem/app/Undelete.kt`) まで。画面は未接続。

- **自前コード**: オンディスク形式とファイル形式の公開仕様だけから書いた (GPL の PhotoRec / TestDisk / extundelete / ntfsundelete のコードは読んでいない・写していない)。
  FsVolume (FS の実装) を通さず、ディスク上の構造を直接読む。したがって公開版 (Apache-2.0 のみ) でも同じに動く (`docs/editions.md`)。
- **読み取り専用**: 検査するデバイスには 1 バイトも書かない。救出したファイルは**別の保存先** (ファイル記述子 / パス) へ書く。
  保存先が検査しているメディアと同じにならないようにするのは呼び出し側の責任 (画面を付けるときは、保存先がそのメディア上なら拒否する)。

## 1. 削除ファイルの一覧と救出 (`usbmem/undelete.h`)

`list_deleted(dev, options, &result)` は FS の種類を判別し (`probe_filesystem`)、割り当て情報 (`build_used_map`) を作り、削除済みの項目を解析して候補の一覧を返す。
`extract_deleted(dev, file, sink, cancel, &res)` は候補 1 件の内容を `sink` へ流し、SHA-256 を返す。

### 対応と限界

| FS | 見つけ方 | 内容の取り出し | 限界 |
|---|---|---|---|
| FAT12/16/32 | ディレクトリ項目の先頭バイト 0xE5。生きているディレクトリと、削除済みのディレクトリ (`.` `..` が正しく `.` が自分を指すもの) をたどる。LFN (長い名前) は削除済みの項目から復元し、チェックサムが合うよう先頭 1 文字を逆算する | 先頭クラスタから**連続**していると仮定してサイズ分を読む | 削除で FAT のチェーンが 0 になるので**断片化は復元できない** (仮定した配置を `layoutExact: false`、確度は上限 85。先頭の署名で裏付けられなければ 75 以下 = 「中」)。8.3 名だけの項目は先頭 1 文字が失われる (`namePartial`)。FAT の時刻はタイムゾーン不明なので UTC として扱う |
| exFAT | エントリセット (File 0x85→0x05、Stream 0xC0→0x40、Name 0xC1→0x41)。SetChecksum を検算 | NoFatChain (連続) なら正確。そうでなければ FAT のチェーン (削除時に消されないドライバもある) | FAT のチェーンが消えていれば連続を仮定する (`layoutExact: false`)。ValidDataLength より後ろはゼロの穴として扱う |
| ext2/3/4 | inode テーブルを走査し、削除済み (dtime が 0 でない・リンク数 0・inode ビットマップが空き) の通常ファイルで、サイズが残っているもの (inline data・暗号化・圧縮の inode は除く)。名前はディレクトリブロックの空き (slack) に残った項目から inode 番号で逆引きする (無ければ `inode_N`) | ext4: エクステント木 (深さ 5 まで、未初期化エクステントは穴)。ext2/3: ブロックポインタ (直接・間接・二重・三重) | **Linux カーネルは削除時にサイズとポインタ (エクステント) をゼロにする**ので、普通に削除したファイルは候補にならない (サイズ 0・配置なしは出さない)。候補になるのは、サイズとポインタが残っているもの (debugfs の `rm`、他の OS・古いドライバ、途中まで進んだ削除など)。**ジャーナル (jbd2) の過去の inode は使わない**。ゴーストエクステント (木の根の残骸) の探索も未実装 |
| NTFS | MFT レコード (`FILE`) の使用中フラグが落ちたもの。fixup を適用し、`$FILE_NAME` / `$DATA` / `$STANDARD_INFORMATION` を読む。パスは親の参照をたどって復元 (親の再利用はシーケンス番号で検出) | 非常駐 `$DATA` は run list、常駐なら MFT レコードの中身。スパースは穴 | 圧縮ファイル・暗号化ファイル・`$ATTRIBUTE_LIST` が必要なファイルは**一覧に出すが「復元不可 (none)」**。ntfs-3g の削除は MFT レコードを消すので候補にならない (Windows の削除は残る) |

そのほかの FS (btrfs / f2fs / xfs / isofs ...) は `Errc::Unsupported`。

### 確度 (`confidence` 0-100 と `level`)

候補ごとに、**割り当て情報との突き合わせ**と**先頭の署名との整合**で確度を付ける。`level` は `high` (>= 80) / `medium` (>= 50) / `low` / `none`。

- 割り当て地図 (ビットマップ) で、ファイルのデータ領域が**別のファイルに再利用されていないか**を数える (`reusedBlocks` / `totalBlocks`)。
  1 ブロックでも再利用されていれば確度は 39 以下 (= low)。割合が大きいほど低い。**再利用された・壊れたファイルを「復元できる」と見せない**のが最優先。
- 地図が信用できない (未クリーン等で全ブロック使用中の扱いになる) ときは `allocationKnown: false` とし、確度の上限を 70 にする。
- 別の削除済みファイルとデータ領域が重なるもの (`overlapsOtherDeleted`) は上限 40。
- 先頭の数 KB を読んで、拡張子・中身の署名と照らす (`signatureCheck`: `match` / `mismatch` / `unknown`)。`mismatch` は -35。
  拡張子から期待される形式と違えば `mismatch`。テキストなら文字の割合、全部ゼロ / 全部 0xFF の先頭は `mismatch`。先頭がスパースの穴のものは `unknown` (ゼロが正しい)。
- 配置が仮定のもの (FAT の複数クラスタ、FAT チェーンが消えた exFAT) は上限 85。先頭の署名が `match` でなければ (`unknown` も) 75 以下 = 「中」。
- `none`: 解析側が復元不可と決めたもの。`extract` はエラー (`Errc::Unsupported`) を返す。

UI に出すときは、`low` と `none` を「復元できる」として見せない。`notes[]` に理由が入る (`clusters-reused`, `signature-mismatch`, `layout-assumed-contiguous`, `allocation-unknown`,
`overlaps-other-deleted`, `name-first-char-lost`, `parent-deleted`, `compressed`, `encrypted`, `attribute-list` ...)。

### JSON (`usbmem_undelete_result_json`)

```
{"fs","allocationKnown","truncated","cancelled","entriesSeen","skippedInvalid","warnings":[..],
 "files":[{"id","name","path","size","mtime","namePartial","inDeletedDir","node","blockBytes","totalBlocks","reusedBlocks","allocationKnown","layoutExact",
           "overlapsOtherDeleted","resident","signature","signatureCheck","confidence","level","recoverable","notes":[..]}]}
```

`id` は抽出に渡す番号。`mtime` は UNIX 秒 (不明なら 0)。抽出の結果は `{"ok","code","bytes","sha256"}`。

## 2. カービング (`usbmem/carve.h`)

ファイルシステムの管理情報に頼らず、全体 (または未割り当て領域だけ) を走査して、**先頭の署名 + 形式の構造**からファイルを切り出す。

### 走査の仕組み

- `align` (既定 512) ごとの位置で、署名の先頭バイトを素早く比べ、一致したら**形式ごとの検査器**が構造をたどって長さを決める。検査器が拒否すれば何も出さない (偽の検出を減らす)。
- 見つけたら、そのファイルの終わりまで飛ばして続ける (中に埋め込まれた別のファイル、例えば Exif のサムネイルは出さない)。
- 未割り当てのみ (`unallocatedOnly`): FS を判別して割り当てビットマップを作り、割り当て済みの領域を飛ばす。判別できなければ全体を走査して `allocationKnown: false`。
- 進捗は `scan` の done/total (バイト)。取り消しは次のチャンク境界・読み取りの間で効く。
- 再開 (mapfile 風): `state_path` に、デバイスの大きさ・設定のハッシュ (形式・align・最小サイズ・最大長・未割り当てのみ)・走査位置・見つけた一覧をテキストで保存する
  (一時ファイル + fsync + rename で原子的)。同じデバイス・同じ設定なら続きから行い、通しで走査した結果と同じ一覧になる。
  別の設定のものは `InvalidArgument`、壊れていれば `Corrupt` で拒否する。
- 設定: 形式の選択、最小サイズ、最大サイズ、ブロック境界 (align)、最大件数、進捗、取り消し、見つけ次第の書き出し (`out_dir`)。

### 対応する形式と構造の検査

| 形式 (id) | 長さの決め方・構造の検査 |
|---|---|
| jpeg | マーカーを SOI から順にたどり (APPn / DQT / SOF / DHT / SOS ...)、SOS 後のエントロピー符号 (0xFF00 と RSTn) を読んで EOI まで。SOF と SOS が無いもの・マーカーの長さが不正なものは拒否。入れ子のサムネイルは外側の 1 つとして切り出す |
| png | 全チャンクの長さと **CRC を検算**。IHDR 先頭・IEND 終端 |
| gif | ヘッダ・論理画面・色テーブル・ブロック (拡張・画像) の連なり・`;` 終端 |
| bmp | ヘッダの大きさと、幅 × 高さ × 画素深度から計算した大きさが**一致**するもの |
| pdf | `%PDF-1.x` から `%%EOF` を探し、直前の `startxref` の値が **`xref` か `N G obj` を厳密に指す**ことを確かめる。直後に `N G obj` が続けば増分更新として続ける |
| zip 系 | ローカルヘッダ (データ記述子 bit 3 を含む) をたどり、セントラルディレクトリと EOCD (zip64 も) の整合を確かめる。内容で docx / xlsx / pptx / odt / ods / odp / epub / jar / apk に分類 |
| mp4 / mov | atom (box) の連なり。先頭は `ftyp`、`moov`/`mvhd` が必要。64 ビット長に対応。brand で mp4 / mov / m4a / heic ... の拡張子 |
| mp3 | ID3v2 + MPEG フレームの同期の連なり (ID3 付きは 8 フレーム以上、無しは 24 フレーム以上)。ID3v1 の `TAG` 128 バイトを含める |
| riff | wav / avi / webp。チャンクの長さの連なりが RIFF の大きさと一致 (奇数長のパディングを許す) |
| 7z | 署名ヘッダの CRC と、次ヘッダの CRC を検算し、全体の長さを決める |
| gzip | ヘッダを読み、**zlib の raw inflate** で最後まで展開して CRC32 と ISIZE を検算 |
| sqlite | ヘッダ (ページサイズ・ページ数・テキストエンコーディング) の整合、変更カウンタと version-valid-for の一致 |
| pe (exe/dll) | DOS ヘッダ → PE ヘッダ → セクションテーブルとセキュリティ (証明書) テーブルから末尾を計算。拡張子は DLL フラグで分ける |

`hits[].confidence` は構造がどこまで確かめられたか (CRC まで確かめた形式は高い)。長さと拡張子は `length` / `ext` / `name` (`f<16進オフセット>.<拡張子>`)。

### 限界

- **断片化したファイルは扱わない** (先頭から連続しているものだけ)。複数の断片に分かれたファイルは、最初の断片で終わるか、構造の検査に失敗して出ない。
- 途中で切れたファイルや、長さがヘッダに書いてあるだけの形式 (riff / bmp / mp4 の最後の atom / pe) で、後ろに別のデータが続くものは、**切れていること自体は分からない**
  (長さは宣言どおりに出す)。CRC のある形式 (png / gzip / 7z の署名ヘッダ・次ヘッダ / zip の構造) は壊れを拒否するが、圧縮されたデータの途中の 1 バイトの壊れ (7z のパック済みデータ) は展開しないと分からない。
- 暗号化・圧縮されたコンテナの中身の検証はしない (zip は構造まで)。
- 最小の長さに満たない・最大長を超える候補は出さない。未割り当て領域のみの走査は、FS が判別でき、割り当て情報が信用できる場合だけ意味がある。

### JSON (`usbmem_carve_result_json`)

```
{"fs","allocationKnown","cancelled","finished","pos","devSize","bytesScanned","readErrors","rejected","warnings":[..],
 "hits":[{"offset","length","format","ext","detail","confidence","name"}]}
```

## 3. API の使い方

- C: `c_api_undelete.h`。`usbmem_ujob` で進捗 (`usbmem_ujob_progress`) と取り消し (`usbmem_ujob_cancel`)。長い処理は呼んだスレッドをふさぐので、別スレッドから進捗・取り消しを呼ぶ。
  取り消されると `Errc::Cancelled` を返すが、それまでの結果を持つハンドルも返る (carve は `state_path` から再開できる)。
- Kotlin: `UndeleteScanner.scan(deviceHandle)` / `Carver.run(deviceHandle, CarveConfig(...), stateFile, outDir, job)` と `UndeleteJob`。`PartitionScan.kt` と同じ作法 (戻り値は `-(Errc)`、ハンドルは > 4095)。
- デバイスはパーティションなら `usbmem_device_sub_range` で切り出して渡す。必ずデバイスを閉じる**前**にハンドルを閉じる。

## 4. 検証

ホスト (ctest):

- `undelete_unit`: 手で組んだ最小の構造・形式ごとの検査・偽の検出が 0 であること・壊れた入力 2000 件の fuzz (wsan = ASan/UBSan)・C API と取り消し・状態の保存と読み込み・読み取り専用であること (書き込みが 1 度も起きない)。
- `undelete_tools` (`tools/check_undelete_tools.py`): 本物の `mkfs.fat` (12/16/32)・`mkfs.exfat`・`mke2fs` (ext2/3/4)・`mkntfs` が作った FS に、自前の最小の書き手 (`tools/ud_fslib.py`、
  FAT / exFAT。`fsck.fat -n` / `fsck.exfat -n` で前後を検証) か `mke2fs -d` + `debugfs -w rm`、`ntfs-3g` で書いたファイルを削除し (NTFS は Windows の削除と同じ変更を MFT と `$Bitmap` に施す)、
  **削除ファイルが一覧に出て、抽出した内容の SHA-256 が元と一致する**こと、上書きしたものが**確度 低で `reusedBlocks > 0`** であること、
  「内容が元と違うのに 高/中」というものが無いこと、生きているファイルが出ないことを確かめる。`ntfsundelete -s` があれば候補数を info として比べる。
- `carve_tools` (`tools/check_carve_tools.py`): 各形式の見本 (`tools/carve_samples.py`。標準ライブラリで作った構造と、PIL / ffmpeg / 7z があれば本物のエンコーダの出力) を
  ばらばらの位置に置いた 96 MiB のイメージを走査し、**全部が正しい位置・長さ・拡張子で切り出され SHA-256 が一致**すること、署名だけの偽物・途中で切れたもの・壊れたものが出ないこと、
  ランダム / ゼロ / テキスト / 0xFF / 署名だけを散らしたランダムで**偽の検出が 0** であること、形式の選択・最小サイズ・align・取り消し→再開 (通しの結果と一致)・未割り当てのみ
  (FAT16 / ext4 で、削除したファイルだけが出る)・性能 (GiB/分) を確かめる。`photorec` があれば同じイメージで比べ、**自前が見落としたものは info として出す** (失敗にはしない)。
- どれも道具 (`mkfs.*` / `debugfs` / `ntfs-3g` / `fusermount3` など) が無ければその部分を飛ばす (すべて無ければ SKIP = 終了コード 77)。イメージは tmpfs (`/dev/shm`) に作って終わったら消す。

公開版: `tools/public_manifest.txt` に自前コード (`core/undelete.cmake`、`tools/ud_fslib.py`、2 つの検証スクリプト、この文書) を載せてあり、
`python3 tools/license_inventory.py --check` と `tools/export_public.sh` が通る。

## 5. 判断と今後

- 画面は未接続。足すときは、`level` が `low` / `none` のものを「復元できる」と表示しない、保存先が検査対象のメディア上なら拒否する、の 2 点を守る。
- ext のジャーナルからの復元、ゴーストエクステント、NTFS の `$ATTRIBUTE_LIST`・圧縮・暗号化、断片化したファイルのカービング (PhotoRec の「断片の再構成」相当) は未実装。
- FAT / exFAT の時刻はタイムゾーンを持たないので UTC として扱う。
