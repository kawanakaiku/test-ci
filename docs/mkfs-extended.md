# mkfs の拡張 (ext3 / ext4 / FAT12 / FAT16 / NTFS)

TODO 6 (DiskTools) の mkfs 拡張。ext2 / exFAT / FAT32 は `docs/disk-tools.md` 3 章。ここでは増えた 5 種類と、容量・機能の問い合わせ、検証、既存ツールとの差を書く。
**UI はまだ無い** (core の API・JNI・Kotlin ラッパーまで)。実機 (USB メモリ) への書き込みはしていない (イメージファイルと `MemoryDevice` だけ)。

## 1. 方針

- すべて **自前の実装** (`core/src/mkfs_ext.cpp` / `mkfs_fat16.cpp` / `mkfs_ntfs.cpp`、Apache-2.0)。公開されているオンディスク形式の仕様 (ext4 は kernel.org の "ext4 Data Structures and Algorithms"、
  FAT は Microsoft の fatgen103、NTFS は Linux-NTFS の形式文書) から書いた。mke2fs / mkfs.fat / mkntfs / ntfs-3g / カーネルのコードは写していない。
  NTFS だけは、mkntfs が作ったイメージの「構造」(どのレコードにどの属性が並ぶか) を外から見て、自前の出力と突き合わせて形を確かめた。
- 公開版に入る (`tools/public_manifest.txt` の `core/src/**` に含まれる。GPL 系のドライバには依存しない)。
- 計画 → 確認トークン → 適用 → 読み戻し + 自前の検査の安全モデルは `docs/disk-tools.md` 3 章のまま。この拡張で足したのは領域の「埋め値」(`MkfsRegion::fill`。NTFS の `$LogFile` を 0xFF で埋める) だけ。
- 検査器 (`check_filesystem`) がメモリ上限だけで `incomplete` になった (誤り・警告は 0) ときは、読み戻しが一致していれば適用を通し、計画の結果に Info `check-partial` を付ける
  (十数 TiB の疎ボリュームの検査が上限に当たるため。`Incomplete` で誤りが 1 つでもあれば従来どおり失敗)。

## 2. 作れるものと容量の範囲

`usbmem_mkfs_capabilities_json` / `MkfsSession.capabilities()` が返す (何も書かない)。長さ (`length`) を渡すと、その容量で作れるかも返す。

| 形式 | 最小 | 最大 | ブロック / クラスタ | ラベル |
|---|---|---|---|---|
| ext2 | 256 KiB | 2^32 ブロック (4 KiB で 16 TiB) | 1 / 2 / 4 KiB | 16 バイト (UTF-8) |
| ext3 | 4 MiB | 同上 (ブロックマップのため) | 1 / 2 / 4 KiB | 16 バイト |
| ext4 | 4 MiB | 2^48 ブロック (実質 EiB 級。64bit は自動) | 1 / 2 / 4 KiB | 16 バイト |
| FAT12 | 64 KiB (セクタ 512) | 4084 クラスタ x 64 KiB = 約 255 MiB | セクタ〜64 KiB | ASCII 大文字 11 |
| FAT16 | 約 2.1 MiB | 65524 クラスタ x 64 KiB = 約 4 GiB | セクタ〜64 KiB | ASCII 大文字 11 |
| FAT32 | 約 32.5 MiB | 約 2 TiB (セクタ 512) | 〜64 KiB | ASCII 大文字 11 |
| exFAT | 1 MiB | 実質無制限 | 〜32 MiB | UTF-16 11 文字 |
| NTFS (実験的) | 2 MiB | 2^32 - 2 クラスタ (64 KiB で約 256 TiB) | 512 B〜64 KiB | UTF-16 32 文字 |

小さすぎる・大きすぎる指定は、計画が不可 (`ok = false`) になり、`problems` に理由コード (`too-small` / `too-large` / `bad-cluster-size` / `bad-inode-size` / `bad-features` / `bad-uuid` /
`bad-label` / `bad-journal-size` ...) と英語の説明が入る。`capabilities(length)` の `sizeOk` / `reasonCode` / `reason` も同じ判定 (同じコードで計画を組み立てて確かめる)。
FAT の種別は自動選択しない (FAT12 / FAT16 / FAT32 を呼び出し側が選ぶ。UI は `capabilities(length)` で `sizeOk` が true の形式だけ出せばよい)。

## 3. ext3 / ext4

### 構成

- ブロックグループ・GDT (32 バイト。2^32 ブロック以上は 64bit で 64 バイト)・iノードテーブル・スーパーブロックと GDT の控え (sparse_super: 0, 1, 3/5/7 のべき乗)。
- **ext4 の既定**: `has_journal` / `extent` / `huge_file` / `flex_bg` / `metadata_csum` / `dir_nlink` / `extra_isize` + `resize_inode` / `dir_index` / `filetype` / `sparse_super` / `large_file` / `ext_attr`。
  2^32 ブロック以上では `64bit` を自動で付け、`resize_inode` を外す (mke2fs と同じ。resize inode のブロック番号が 32 ビットのため)。iノード 256 バイト。
- **ext3**: ブロックマップ (IND / DIND) + ジャーナル。extent・flex_bg・metadata_csum・64bit・huge_file・dir_nlink・extra_isize は付かない。`^has_journal` で ext2 相当にもできる (ジャーナル無し)。
- `flex_bg`: 16 グループ分のビットマップと iノードテーブルを先頭グループに並べる。1 グループに収まらないとき (1 KiB ブロックで数百 MiB など) はまとまりを 8 / 4 / 2 / 1 に減らす (`s_log_groups_per_flex` に記録)。
- `metadata_csum`: crc32c (シード = UUID)。iノード・ディレクトリブロックの末尾・extent ブロックの末尾・ビットマップ・GDT・スーパーブロックのチェックサムを付ける。
  2 つ目以降のグループは `BLOCK_UNINIT` / `INODE_UNINIT`、iノードテーブルは遅延ゼロ化 (`ITABLE_ZEROED` を立てない = カーネルの `lazy_itable_init` と同じ状態)。`uninit_bg` だけ (`^metadata_csum,uninit_bg`) なら crc16。
- `resize_inode` (iノード 7): 予約 GDT ブロック (1024 倍まで拡張できる分。上限はブロックサイズ / 4) と、各控えグループの複製を指す。
- ジャーナル (jbd2 v2): 大きさは mke2fs の表と同じ (4 MiB 未満のブロック数で 1024 ブロック、〜16 MiB で 4096 ...、最大 262144 ブロック)。`journal_size` で指定可 (1024 ブロック以上、全体の半分まで)。
  置き場所は mke2fs と同じく中ほどのグループ。ext4 は extent、ext3 はブロックマップ。`s_jnl_blocks` の控えも書く。ジャーナルの本体はゼロ埋め (`lazy_journal` の指定は計画に残すだけで、書く内容は同じ)。
- ルート (iノード 2)・`lost+found` (iノード 11。ブロックサイズに応じて 12〜16 KiB)・予約済み iノード。
- ブロックサイズの既定は 512 MiB 未満 1 KiB、それ以上 4 KiB。iノード比は mke2fs の `small`(<512 MiB は 4096、3 MiB 未満は 8192) / `default` (16384) / `big` (4 TiB 以上 32768) / `huge` (16 TiB 以上 65536) と同じ。
- 指定できる機能 (`features`): mke2fs 風 (`^resize_inode,64bit` ...)。ext4 では `extent` を外せない (外すなら ext3 を選ぶ)、ext3 に ext4 の機能は付けられない、`extra_isize` と `metadata_csum` は 256 バイトのiノードが前提。
- アプリの既存ドライバ (`Ext4Volume`) が読み書きできる範囲 (許可リストの機能) に収めてある。`orphan_file` / `metadata_csum_seed` / `encrypt` / `casefold` / `quota` / `bigalloc` / `mmp` は作らない。

### mke2fs との差 (この環境の e2fsprogs 1.47.4、`mke2fs -t ext4` との `dumpe2fs -h` の比較。64 MiB と 1 GiB)

| 項目 | mke2fs | 自前 | 理由・影響 |
|---|---|---|---|
| `orphan_file` | あり | なし | 新しい機能。古い Linux (6.x 未満) では読み取り専用になる。アプリのドライバも対象外 |
| `metadata_csum_seed` | あり | なし | 不要 (UUID 変更時に再計算が要らなくなるだけ) |
| `64bit` | あり (この環境の mke2fs.conf) | 2^32 ブロック以上だけ | 要件どおり。小さいボリュームでは GDT が 32 バイトで済む |
| 予約 GDT ブロック数 | 64 bit の GDT 前提で計算 (1 GiB で 127) | 32 バイトの GDT 前提 (同 63) | 1024 倍まで拡張できる点は同じ。64 MiB (1 KiB ブロック) では 256 と 255 |
| iノード数・ブロックサイズ・ジャーナルの大きさ・ラベル・UUID | 同じ | 同じ | 64 MiB と 1 GiB で `dumpe2fs -h` の該当行が一致 |
| flex_bg のまとまり | 16 | 16 (入らなければ 8 / 4 / 2 / 1) | mke2fs は 1 KiB ブロックの数百 MiB でテーブルが次のグループにはみ出す。自前ははみ出さないよう縮める |
| ジャーナル本体 | 確保のみ (ゼロは lazy_journal_init 次第) | 常にゼロ埋め | 内容の差だけ |
| 乱数・時刻 | 実行ごとに違う | `seed` / `timestamp` で固定可 | 再現テスト用 |

## 4. FAT16 / FAT12

- 配置: 予約領域 (ブートセクタ + データ領域をクラスタ境界に揃える余白) → FAT x2 → 固定のルートディレクトリ (512 エントリ。1 MiB 未満は 224) → データ領域。ブートセクタは起動しない (停止するだけ)。
- **FAT16 のクラスタサイズ**は Microsoft の表 (fatgen103): 〜16 MiB は 1 KiB、〜128 MiB は 2 KiB、〜256 MiB は 4 KiB、〜512 MiB は 8 KiB、〜1 GiB は 16 KiB、〜2 GiB は 32 KiB、それ以上 64 KiB。
  クラスタ数が上限 (65524) を超えるときは大きく、FAT16 の下限 (4085) を割るときは小さくする。
- **FAT12**: クラスタ数が 4084 以下になる最小のクラスタサイズ (セクタ 512 なら 1 MiB 以下は 1 セクタ〜)。フロッピー (1440 KiB) も作れる。
- 指定したクラスタサイズでクラスタ数が範囲外なら `too-small` / `too-large` で理由を示して拒否する。セクタ 512 / 1024 / 2048 / 4096 (4Kn)。FAT32 と同じ `fat_label` (ラベル検査) とボリュームシリアルの作り方を共有。
- mkfs.fat との差: クラスタサイズは Microsoft の表のとおりにするため、境界付近 (たとえば 2 GiB ちょうど) で mkfs.fat と 1 段違うことがある (mkfs.fat は 64 MiB で 2 KiB、600 MiB で 16 KiB、2 GiB で 64 KiB。
  自前は 64 MiB で 2 KiB、600 MiB で 16 KiB)。FAT の本数は 2、ルートのエントリ数は 512、メディア記述子 0xF8、ボリュームラベルは `NO NAME` (指定があればルートのラベルエントリにも書く)。

## 5. NTFS (実験的)

`core/src/mkfs_ntfs.cpp`。**計画に Warning `experimental` が必ず付く**。ntfs-3g / アプリのドライバでは確かめたが、**Windows の chkdsk では未確認** (次節)。

### 作る構造

- MFT レコード 0〜26 (1 レコード 1 KiB。4Kn のときは 4 KiB): `$MFT` `$MFTMirr` `$LogFile` `$Volume` `$AttrDef` `.` (ルート) `$Bitmap` `$Boot` `$BadClus` `$Secure` `$UpCase` `$Extend`、
  12〜15 は「使用中で中身が空」、16〜23 は未使用 (mkntfs と同じ)、24〜26 は `$Extend` の下の `$Quota` / `$ObjId` / `$Reparse`。`$MFT` の確保領域に入る残りのレコードは未使用のレコードとして初期化する。
- 更新シーケンス配列 (512 バイトごとの末尾のすげ替え)、属性 (標準情報 72 バイト / ファイル名 / データ / ボリューム名・情報 / インデックスルート・アロケーション・ビットマップ)、マッピングペア (符号つき最小バイト数)。
- ルートの `$I30`: INDEX_ROOT + 1 つの INDX ブロック (4 KiB) + `$BITMAP`。`$Extend` の `$I30` に 3 つの子。
- `$Secure`: セキュリティ記述子 3 つ (0x100 = システムファイル、0x101 = `$Secure` と `$Extend`、0x102 = ルート) を `$SDS` (256 KiB 先のミラーも書く) と `$SDH` / `$SII` に入れる。ハッシュは公開されている回転和。
- `$UpCase`: 65536 エントリ。`mkfs_upcase.inc` (exFAT と共有する圧縮表) から展開。
- `$LogFile`: **0xFF で埋める** (リセット済みのログ。ntfs-3g と `core/include/usbmem/ntfs_logstate.h` は「きれいなログ」と見なし、Windows は最初のマウントで作り直す)。大きさは容量の 1/32 (256 KiB〜64 MiB)。
- `$Boot`: 先頭の 8 KiB (BPB + ブートコード領域)。バックアップのブートセクタは**最後のセクタ**。BPB の総セクタ数は最後のセクタを含まない値 (mkntfs と同じ)。`$MFTMirr` はボリュームの中ほど (先頭 4 レコード)。
- `$Bitmap` は使用中のクラスタ + 末尾の余白ビット。`$BadClus` の `$Bad` は全体が疎。
- クラスタサイズ: 既定は 4 KiB (7 MiB 未満は 512 B、16 TiB 超は 8 KiB、32 / 64 / 128 TiB 超でそれぞれ 16 / 32 / 64 KiB)。512 B〜64 KiB を指定可 (セクタサイズ以上)。セクタ 512 / 1024 / 2048 / 4096。
- ボリュームシリアルは `uuid` (先頭 8 バイト) または乱数。NTFS 3.1。ラベルは UTF-8 → UTF-16 で 32 文字まで (`\ / " * ? < > | :` と制御文字は拒否)。

### 作らないもの・制限

- `$UsnJrnl` と `$Extend\$RmMetadata` (トランザクション NTFS)。Windows が必要なときに作る。
- `$UpCase` の `$Info` ストリーム・`$Volume` の `$OBJECT_ID`。
- ブートコードは起動しない (停止するだけ)。起動可能な Windows ボリュームは作らない。
- 大きいボリュームでは `$Bitmap` がゼロの大きな塊になる (10 TiB の 4 KiB クラスタで約 320 MiB)。Warning `slow` を出す。クラスタを大きくすれば短くなる。
- 検査 (`check_filesystem`) は十数 TiB で資源上限に達して `incomplete` になる (`check-partial`)。
- **Windows の chkdsk は未実施** (tools/winvhd の重い検証は別途)。ntfs-3g の `ntfsfix -n`・`ntfsinfo`・`ntfscp` / `ntfscat`・読み書きマウントと、アプリのドライバ (個人版) で確認。Windows で開く前に必ず chkdsk の結果を確かめること。

## 6. API

### core (C++)

`MkfsOptions` に `uuid` / `features` / `journal_size` / `lazy_journal_init` を追加。`Mkfs::capabilities(length, sector_size, cluster_size)` が `MkfsCapability` の配列を返し、
`mkfs_capabilities_to_json` が JSON にする (`core/include/usbmem/mkfs.h`)。`Mkfs::supported` / `min_size` は ext3 / ext4 / FAT12 / FAT16 / NTFS を含む。

### C API / JNI / Kotlin

```c
int usbmem_mkfs_open_ex(dev, fs_name, offset, length, const char* options, in_use, seed, timestamp, &h);   // 既存の usbmem_mkfs_open は変えていない
int usbmem_mkfs_capabilities_json(length, sector_size, cluster_size, char** out_json);
```

`fs_name`: `ext2` `ext3` `ext4` `fat12` `fat16` `fat32` `exfat` `ntfs`。`options` は `key=value` を改行で区切った UTF-8:
`label` / `cluster_size` / `sector_size` / `inode_ratio` / `inode_size` / `reserved_percent` / `uuid` / `features` / `journal_size` (バイト) / `lazy_journal` (0|1)。
未知のキー・数値でない値は計画の `problems` に Error `bad-option`。Kotlin: `MkfsSession.openEx(deviceHandle, fsName, offset, length, options = mapOf("label" to "X"))` と `MkfsSession.capabilities(length, sectorSize, clusterSize)` (JSON 配列)。

`capabilities` の要素: `fs` / `supported` / `experimental` / `minBytes` / `maxBytes` (文字列) / `clusterSizes` / `defaultCluster` (length 指定時) / `labelMax` / `labelCharset` / `features` (ext。`^` つきは既定で無効) /
`sizeOk` / `reasonCode` / `reason` / `note`。

## 7. 検証

| 何を | どう | どこで |
|---|---|---|
| ext3 / ext4 | `e2fsck -fn` が終了 0 かつ `WARNING` なし。`dumpe2fs -h` の機能が期待どおり。`debugfs -w` でファイル・ディレクトリ・シンボリックリンクを書いて再度 `e2fsck -fn` と読み戻し | `tools/dt_mkfs2.py` (ctest `disktools_tools`) |
| FAT12 / FAT16 | `fsck.fat -n`、`7z l`、mtools (`mdir` / `mmd` / `mcopy` で書いて `fsck.fat -n`。mtools が無ければ省略) | 同上 |
| NTFS | `ntfsfix -n`、`ntfsinfo -m`、`ntfscp` で書いて `ntfscat` で読み戻し、もう一度 `ntfsfix -n`。手元ではさらに ntfs-3g の読み書きマウント (FUSE) | 同上 |
| 容量の範囲 | `caps` の一覧・範囲・容量ごとの `sizeOk` / `reasonCode`。拒否の理由コード (小さすぎ・大きすぎ・不正なクラスタ / 機能 / UUID) | 同上 |
| 範囲 | ext4 4 MiB〜20 TiB (疎)、ext3 4 MiB〜6 GiB、FAT16 3 MiB〜3 GiB、FAT12 64 KiB〜200 MiB、NTFS 2 MiB〜2 TiB (疎)。ブロック / クラスタ 512 B〜64 KiB、セクタ 4096、ラベル・UUID・機能の組み合わせ | `tools/check_disktools.py <cli> mkfs2` (全件 91 ケース。`--quick` は 40 弱) |
| 乱数 | `USBMEM_MKFS_RANDOM=N` (既定 12 / quick 4) 件の容量・クラスタ・セクタ・ラベル・機能の組み合わせ。クラッシュ・適用失敗・外部ツールの指摘は失敗。拒否は理由コードが妥当なら可 | 同上 |
| ASan / UBSan | 乱数 500 件 (`USBMEM_MKFS_ONLY_RANDOM=1 USBMEM_MKFS_RANDOM=500`、ASan + UBSan 付きの CLI)。落ちない・fsck がきれい | 手元 (結果は 8 章) |
| アプリのドライバで往復 (個人版) | 自前 mkfs → 読み書きでマウント (ext4 の書き込みは `USBMEM_NTFS_WRITE` ビルド) → ディレクトリ・ファイル作成・追記・改名・削除 → 再マウントして全内容を突き合わせ → 自前の検査が clean → 外部の fsck | `usbmem_mkfs_roundtrip` / `tools/run_mkfs_roundtrip.sh` (ctest `mkfs_roundtrip`。書き込み無効のビルドでは SKIP) |
| 既存の試験を壊していない | `tools/check_mkfs_defaults.py`、`tools/fs_feature_matrix.json` は変更なし | 既存 |

イメージは 0xA5 で埋めてから作る (ゼロ前提に頼らない)。疎イメージ・一時ファイルは tmpfs (`/dev/shm`) に作り、終わったら消す。外部ツールは検査用の引数だけで呼ぶ (書き込みはイメージファイルに対してだけ)。

## 8. 結果

手元 (ホスト、2026-11-23。e2fsprogs 1.47.4 / dosfstools / ntfs-3g):

- `tools/check_disktools.py <cli> mkfs2` 全件 90 ケース: 見逃し (FN) 0、過検出 (FP) 0。ext4 は 4 MiB〜20 TiB (疎)、NTFS は 2 MiB〜17 TiB (疎。17 TiB は検査が `check-partial`)。
  開発中に見つかった不具合: 1 KiB ブロックで block 0 を使用中にしていなかった (最小のグループでクラッシュ)、予約 GDT ブロックの参照先を書いていなかった (e2fsck: Inode 7 has illegal block)、
  2^32 ブロック以上で resize_inode を外していなかった、flex_bg のまとまりが 1 グループに収まらない (1 KiB ブロックの 300 MiB)、`$MFT` の確保領域がクラスタ境界で端数のとき ntfs-3g が読めない
  (`$MFT` のサイズが確保領域より小さく、残りのレコードが無かった) → いずれも修正済みで、回帰として上のケースに入っている。
- ASan + UBSan 付きの CLI で乱数 500 件 (ext3 / ext4 / FAT12 / FAT16 / NTFS、容量 64 KiB〜4 GiB、クラスタ・セクタ 4096・ラベル・機能の組み合わせ。うち 83 件は理由コードつきで拒否): クラッシュ・サニタイザの指摘・fsck の指摘 0 (約 7 分)。
- NTFS: `ntfsfix -n` / `ntfsinfo -m` / `ntfscp` + `ntfscat` / ntfs-3g の読み書きマウント (ファイル 3 MB の複製・200 ファイル・削除) が、クラスタ 512 B〜64 KiB、セクタ 512 / 4096、2 MiB〜17 TiB で通る。書いたあとも `ntfsfix -n` と自前の検査がきれい。
- アプリのドライバでの往復 (個人版、`USBMEM_NTFS_WRITE=ON`、`tools/run_mkfs_roundtrip.sh`): ext2 / ext3 / ext4 (1 KiB・2 KiB・4 KiB ブロック) / FAT12 (1440 KiB 含む) / FAT16 / FAT32 / exFAT / NTFS (512 B〜64 KiB・セクタ 4096) で 16 構成すべて通る
  (書いた内容を再マウントして突き合わせ、自前の検査 clean、`e2fsck -fn` / `fsck.fat -n` / `fsck.exfat -n` / `ntfsfix -n` もきれい)。ジャーナル無しの ext4 はドライバが読み取り専用にするので往復の対象外 (作るのは問題ない)。
- 単体テスト (`usbmem_disktools_tests`、16 件): 新形式の往復と再現性 (seed / timestamp)、UUID・ラベルがスーパーブロックに入ること、不正な機能・UUID・ジャーナルの大きさの拒否、`capabilities` と計画の可否の一致、`open_ex`。
- 公開版: `python3 tools/license_inventory.py --check` と `tools/export_public.sh` が通り、書き出したツリー (`USBMEM_EDITION=public`) で `usbmem_disktools_cli` / `usbmem_disktools_tests` がビルドできて通る。
- GitHub Actions: このファイルを書いた時点では未実施 (push の手順は 9 章)。

## 9. 未解決・判断が必要な点

- **CI (GitHub Actions) は未実行**: この作業のエージェントからは `git -c credential.helper=...` の push が環境の制限で通らなかった。`usbmem-ci-full-mkfs-ext` への push で host-tests (plain / san / wsan / wrel。`disktools_tools` の `mkfs2` と `mkfs_roundtrip`) と公開版の CI が動く。Kotlin / JNI の変更は Android のビルド (`usbmem-ci-*` の Android ジョブ) でコンパイルされる。

- NTFS を Windows の chkdsk で確かめていない。`tools/winvhd` で VHD にして `chkdsk` を走らせる検証は重いので未実施。実験的の表示は、これが済むまで外さない。
- ext4 に `orphan_file` / `metadata_csum_seed` を付けない (アプリのドライバが対象外)。新しい Linux でそれらが必須な場面は無い。
- FAT の種別を容量から自動で選ぶ機能は作っていない (UI が `capabilities` を見て選ぶ前提)。
- 検査器が十数 TiB で資源上限に達して `incomplete` になる (`check-partial`)。必要なら検査器側の上限を上げる。
