# ディスクイメージ形式 (QCOW2 / VHD / VHDX / VMDK) と COW オーバーレイ

`core/` に、仮想ディスクイメージを **BlockDevice として読む** 実装と、書き込みを別の場所に溜める **COW オーバーレイ** を追加した。
どれも `BlockDevice` なので、既存のパーティション解析 (`read_partition_table`) とファイルシステムのマウント (NTFS / ext2 / exFAT / FAT …) がそのまま使える。
NBD クライアントは [nbd.md](nbd.md)。

- 公開ヘッダ: `core/include/usbmem/disk_image.h` (C++)、`core/include/usbmem/c_api_blockdev.h` (C)
- 実装: `core/src/blockdev_{common,qcow2,vhd,vhdx,vmdk,open,overlay}.cpp`、C API は `core/src/c_api_blockdev.cpp`
- JNI: `android/app/src/main/cpp/jni_blockdev.cpp`、Kotlin: `android/app/src/main/java/org/usbmem/app/BlockDev.kt` (`BlockDevNative` / `BlockDeviceHandle` / `ImageResolver`)。**UI にはつないでいない** (統合は別担当)。
- 依存: zlib のみ (ホストは find_package、NDK は sysroot の libz)。ビルドは `core/blockdev.cmake` を `core/CMakeLists.txt` が include する。

## 使い方

```cpp
BlockDevicePtr file;                       // RawImageDevice など (SAF の fd でもよい)
RawImageDevice::open_path("disk.vhdx", true, 512, &file);
ImageOpenOptions opt;
opt.resolver = [&](const ImageResolveRequest& rq, BlockDevicePtr* out) { /* rq.name を呼び出し側が決めて開く */ };
BlockDevicePtr dev; ImageInfo info;
Status st = open_disk_image(file, opt, &dev, &info);   // 形式を自動判別。RAW ならそのまま返る
PartitionTable t; read_partition_table(*dev, &t);       // 以降は通常のデバイスと同じ
```

- **常に読み取り専用** (`is_read_only() == true`、`write_at` は `ReadOnly`)。書き込みを試したいときは `OverlayDevice` を重ねる (下記)。
- 判別 (`detect_image_format`): QCOW2 (`QFI\xfb`) / VHDX (`vhdxfile`) / VMDK (`KDMV` または `# Disk DescriptorFile`) / VHD (末尾 or 先頭 512 B の `conectix` フッタ + **チェックサム検証**) / それ以外は RAW。
  RAW を VHD と誤認しない (フッタのチェックサムが合わなければ RAW)。形式のマジックがあるのに壊れているファイルは、RAW 扱いにせず `Corrupt` / `Unsupported` で拒否する (中身の誤読を避ける)。
  **下位の I/O エラーは RAW 扱いにせず、そのまま返す** (故障注入テストで発見・修正済み)。
- 失敗時は `ImageInfo::detail` に人間向けの理由 (英語)。

### バッキング / 親 / エクステントの解決 (ImageResolver)

イメージが別ファイルを指す場合 (QCOW2 のバッキング、VHD/VHDX の親、VMDK の複数エクステントや親) は、`resolver` が**名前を受け取って呼び出し側が開く**。
イメージ内の名前は信用できない (絶対パスや `..` を含みうる) ので、ライブラリは勝手にファイルを開かない。resolver が無ければ `Unsupported` (黙ってゼロを返さない)。
`open_disk_image_path` (ホスト/テスト用) は、イメージと同じディレクトリからの相対名だけ解決し、絶対パス・`..`・ドライブ名は既定で拒否する。
Android (SAF) ではディレクトリを辿れないので、`ImageResolver` で兄弟ファイルをユーザーに選ばせる想定。

- チェーンの深さ上限 `max_backing_depth` (既定 8)。**循環**もここで `Corrupt` になる (テスト済み)。
- 親が見つからない/開けないときはそのエラーをそのまま返す。
- VHD は親の UUID、VMDK は親の CID が一致しなければ `Corrupt` (`allow_parent_mismatch` で緩和可)。

## 対応状況

| 形式 | 対応 | 明示的に拒否 (`Unsupported`) |
|---|---|---|
| QCOW2 | v2 / v3、クラスタ 512 B〜2 MiB、L1/L2、ゼロクラスタ (v3)、**deflate 圧縮クラスタ**、バッキングチェーン (qcow2 / raw、`backing_fmt` 拡張、仮想サイズがバッキングより大きい場合)、内部スナップショット入り (現在の状態を読む)、dirty ビット (lazy refcounts) | 暗号化 (AES / LUKS)、外部データファイル、拡張 L2、zstd 圧縮、未知の非互換機能ビット。corrupt ビット付きは `Corrupt` |
| VHD | 固定 / 動的 / 差分 (セクタビットマップ、親ロケータ W2ru/W2ku/Wi2r/Wi2k と親名 UTF-16BE)、フッタのチェックサム検証、先頭コピーへのフォールバック | — |
| VHDX | 固定 / 動的 / 差分 (BAT の状態 0/1/2/3/6/7、チャンクごとのセクタビットマップ、親ロケータ)、論理セクタ 512 / 4096、CRC-32C 検証 (ヘッダ・リージョンテーブル)、2 つのヘッダのうち新しい方 | **未再生のログ** (LogGuid ≠ 0) は既定で拒否 (`allow_unreplayed_log=true` で「古い内容かもしれない」と承知で読む)。未知の必須リージョン/メタデータ |
| VMDK | monolithicSparse、**streamOptimized** (zlib 圧縮グレイン、フッタのグレインディレクトリ)、monolithicFlat + descriptor、twoGbMaxExtentSparse / Flat (複数エクステントは resolver)、ZERO エクステント、descriptor 内のエクステント別オフセット、差分 (parentFileNameHint + CID) | VMFS / seSparse などの別形式、zlib 以外の圧縮 |

QCOW2 の圧縮クラスタは `csize_shift = 62 - (cluster_bits - 8)` の分割と生の deflate (wbits=-15)。VMDK の圧縮グレインは zlib 形式 (`[lba u64][size u32][データ]`、lba の整合も検証)。

## 安全設計 (ディスク上の構造を信用しない)

- すべてのテーブル参照・サイズ・オフセットに範囲検査と**桁あふれ検査**。ファイルの外を指すエントリは `Corrupt`。
- 確保量に上限: L1 ≤ 32 MiB、VHD BAT ≤ 64 MiB、VHDX BAT ≤ 256 MiB、VMDK グレインディレクトリ ≤ 32 MiB、descriptor ≤ 2 MiB、拡張/エクステント数の上限。テーブルキャッシュは 1 イメージ 16 MiB (`max_cache_bytes`) の LRU。
- 例外を外へ出さない (`bad_alloc` などは `NoMemory`/`Corrupt` の Status にする)。VMDK descriptor の数値パースも例外を投げない自前実装 (ファジングで `std::stoul` の例外を発見・修正済み)。
- 範囲外の読み取りは `OutOfRange`、close 後は `Closed`、キャンセルはユニット (クラスタ/ブロック/グレイン) ごとに確認して `Cancelled`。
- 1 つのイメージの全メソッドは 1 本のミューテックスで直列化 (キャッシュ共有のため)。複数スレッドから安全。

## COW オーバーレイ (`OverlayDevice`)

読み取り専用のベース (RAW でもイメージでも NBD でもよい) の上に、書き込みを `block_size` (既定 4096) 単位で溜める。**ベースには決して書かない** (テストでは読み取り専用ラッパーの書き込み回数 = 0 を確認)。

- メモリ上、または追記ログファイル (`Options::file_path`)。ログは `ヘッダ(64 B, CRC) + レコード[REC1 | ブロック番号 | データ | CRC32]` の追記のみ。
  **クラッシュ耐性**: 再オープン時に、完全で CRC が合うレコードまでを採用し、壊れた/途切れた末尾は捨てる (ファイルも切り詰める)。ログを任意の長さで切った全ケースで「完全なレコードまでの状態」に戻ることをテスト。ヘッダの不一致 (別のベース/ブロックサイズ) は `InvalidArgument`、ヘッダ破損は `Corrupt`。
- `commit(target, clear_after)`: 溜めた書き込みを書き込み可能な target へ昇順に書き戻して flush (明示操作。途中で失敗してもオーバーレイは残る)。`discard()` で全部捨てる。
- `max_overlay_bytes` を超えると `Io(ENOSPC)`。
- 用途: 「このイメージに書き込んだらどうなるか」を元ファイルを触らずに試す、書き込み機能の安全な検証。

## 検証

`tools/gen_blockdev_fixtures.py` が (1) `qemu-img` / `qemu-io` で各形式を作り、**`qemu-img compare` で元の RAW と一致**することを毎回確認し、(2) qemu-img で作れない VHD 差分 / VHDX 差分 (論理セクタ 4096 を含む) / 複数エクステント VMDK / 壊れたイメージを仕様から直接書く (C++ 実装とは独立の Python)。
C++ テスト (`core/tests/test_blockdev.cpp`) はすべてのイメージを元の RAW とバイト単位で照合する (連続読み・ランダム読み 1500 回・境界読み・全体 1 回)。

| 検証 | 内容 |
|---|---|
| 内容照合 | 43 フィクスチャ。QCOW2 20 / VHD 6 / VHDX 7 / VMDK 8 / RAW 2 (拒否されるべき 5 件と壊れた 3 件を含む) がすべて期待どおり |
| パーティション + FS | MBR / GPT の元ディスク (ext2 パーティション入り) を全形式で開き、`read_partition_table` の JSON が RAW と同一、パーティション 1 (ext2) の内容が RAW と完全一致、`probe_filesystem` = ext2 |
| ファジング | 14 種のシード × (ヘッダ領域の全バイトを 4 通りに変異 + ランダム複数バイト変異 250 回 + 切り詰め) ≈ 19.6 万回。クラッシュ・ハング・想定外のエラーコードなし (ASan/UBSan 配下でも) |
| 故障注入 | 下位デバイスの N 回目の読み取りで Io / Disconnected / Timeout を起こす (バッキング含む)。エラーは別の種類に化けず・握りつぶされず伝わり、成功した読み取りの内容は常に正しい |
| 並行読み | 4 スレッド × 300 回のランダム読み (4 形式) |
| 方針 | resolver 無し → `Unsupported`、深さ上限、循環バッキング、親 ID 不一致、キャンセル、close 後 |

## 制限・未解決

- **書き込み非対応** (形式への書き込み)。オーバーレイで代用する。
- VHDX の**ログ再生は未実装** (未再生ログは既定で拒否)。Windows で正常にマウント解除されたイメージには通常ログは無い。
- QCOW2: 暗号化・zstd・外部データファイル・拡張 L2 は未対応。refcount は見ない (読み取りには不要)。
- VMDK: VMFS / seSparse 未対応。descriptor の `ddb.*` (ジオメトリなど) は無視。
- VHDX のセクタビットマップのビット順 (LSB が若いセクタ) は仕様の記述から実装した。qemu-img は VHDX 差分を読めないため、**差分 VHDX は実機 (Windows 作成) のイメージで未検証**で、自前ライタ (同じ解釈) とのみ照合している。Windows 上で作った差分 VHDX での確認が望ましい。
- VHD の差分も同様に自前ライタとの照合 (qemu-img は VHD 差分を作れない)。
- 容量の大きなイメージ (数十 GiB 以上) での実測性能は未計測。
- ネストしたマウント (SAF 上のイメージファイルを開く) と UI は未統合。
