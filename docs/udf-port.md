# UDF (fs/udf) の移植

Linux の `fs/udf` を LinuxCompat 上で**読み取り専用**に動かし、アプリから「マウント」できるようにした。ISO 9660 は docs/isofs-port.md (ブリッジ盤の優先順位などの共通事項もそちら)。

## 構成

| 場所 | 内容 |
|---|---|
| `third_party/udf/fs-udf/` | 上流 `fs/udf` (コミット 067c5c24)。**ファイルは無改変**。ビルドするのは balloc dir ialloc inode lowlevel namei partition super truncate symlink directory misc udftime unicode。`file.c` (write_iter / mmap / ioctl / setattr) は入れない |
| `core/compat/optical.cmake` | `usbmem_udf` とテスト |
| `core/compat/src/rt_vfs_udf.c` | ホスト入口 + `file.c` の代わりの空 ops + `crc_itu_t` |
| `core/compat/src/udf_volume.cpp` | `UdfVolume : FsVolume` (`fs_name()` = `"udf"`、`readlink` あり) |
| `core/compat/include/linux/crc-itu-t.h`, `udf_fs_i.h` | シム |
| `tools/gen_udf_fixture.py` | テスト用イメージの生成 |
| `core/compat/tests/test_udf.cpp` | ホストテスト |

共有ファイルへの追加 (どれも他の FS に影響しない): `linux/buffer_head.h` の `struct buffer_head` に `int b_count`、`src/rt_bh.c` の `__brelse` に参照数の判定 (UDF は 1 つの bh を複数の `extent_position` で共有し `get_bh`/`brelse` を対にして使う。`b_count` は追加の参照数で 0 = 参照 1 つなので、`get_bh` を使わない他の FS の挙動は同じ)、`compat/ops.h` の `address_space_operations` に `direct_IO`。

## 対応範囲

- UDF 1.02 / 1.50 / 2.01 の通常 (Type 1) パーティション、Sparable パーティション (DVD-RAM / CD-RW)、ブロックサイズ 512/2048/4096 で検証 (1024 は未検証。上流がブロックサイズを順に試す。512 バイトのデバイス上の 2048 バイト UDF でも開く)、FE / EFE、ICB 内データ (小さなファイル)、短い/長い割り当て記述子、疎ファイル (穴)、断片化したファイル、ハードリンク、シンボリックリンク (`udf_get_link`)。
- 名前は UTF-8 (マウントオプション `utf8`)。255 バイトまでの名前、絵文字、日本語。
- **仮想パーティション (VAT)** と **Metadata パーティション (UDF 2.50 以降)** は上流が読むので拒否はしていない。ただし:
  - VAT: UDF 2.50 の空ボリューム (`mkudffs --media-type=bdr --vat`) だけ検証した。ホストの Linux は VAT のボリュームを書けないので、**ファイルの入った VAT 盤は作れず未検証**。UDF 1.50 の CD-R の VAT (`--media-type=cdr --vat`) は mkudffs 2.3 の出力をホストの Linux 自身も開けない (can't read superblock) ので、フィクスチャにしていない。
  - Metadata パーティション: mkudffs 2.3 では作れず、ホストの Linux でも作成できない。**未検証**。
  - どちらも ino の扱い (下記「複雑なボリューム」) をテスト用フックで通常のイメージに強制適用して、その経路 (表と readdir の lookup) だけは全イメージで検証した。
- 書き込みは全面的に未対応 (`mount(read_only=0)` は `-EROFS`)。

## inode 番号

udf の `i_ino` は ICB (File Entry) の**物理ブロック番号** (`udf_get_lb_pblock`)。ここから (パーティション参照番号, 論理ブロック) へ戻す:

- **単純なボリューム** (Type 1 / Sparable だけ): 物理ブロックを含むパーティションを探して `pblock - root`。状態を持たない。
- **複雑なボリューム** (VAT / Metadata を含む): ICB の物理位置からは論理アドレスに戻せない (VAT やメタデータファイル経由) ので、`lookup` / `stat` / `readdir` が返した ino → 論理アドレス (`UDF_I(inode)->i_location`) を表に覚える。表に無い ino は `-ESTALE`。`readdir` は各名前を `lookup` して表を作る (ディレクトリの長さに対して 2 乗の時間。正しさを優先)。ルートはマウント時に登録。
- `..` は ディレクトリの PARENT エントリを `lookup("..")` で引いて `file->f_pos` に入れる (`dir_emit_dotdot` は `optical.h`)。

## 互換層の穴埋めと、上流の参照漏れの回避

- `get_bh` を実際の参照数に (`optical.h`)、`put_bh` は `brelse`。
- `udf_read_folio` は `file_inode(file)` を使うので、`read` は inode だけ入った `struct file` を渡す。
- 書き込み側 (`__mpage_writepages` / `blockdev_direct_IO` / `inode_add_bytes` など) は読み取りでは呼ばれないので、コンパイルを通すための宣言/スタブ。
- **上流の参照漏れ**: `udf_fiiter_release` は `iter->epos.bh` (拡張割り当て記述子の続きブロック) を手放さない。ディレクトリが複数の割り当て拡張にまたがると、`lookup` のたびに bh が 1 つ漏れ、フォリオもアンマウント後まで残る (ASan で検出)。`directory.c` の中の名前だけをコンパイル定義 (`udf_fiiter_release=usbmem_real_fiiter_release`) で変え、他のファイルからの呼び出しは `rt_vfs_udf.c` の同名関数 (`epos.bh` を `brelse` してから本物を呼ぶ) に向けた。`directory.c` の内部の異常系の呼び出しは対象外 (壊れたボリュームでだけ起こる)。
- UBSan の `pointer-overflow` を UDF のソースだけ免除: `udf_get_pblock_virt15` が NULL の `s_vat_inode` に `UDF_I()` (container_of) を先に適用する (使う前に範囲検査で戻るので参照はしない)。

## マウント前の検査

`precheck`: デバイスが 34 KiB 以上で、32768 バイトから 2048 バイトごと 64 個の中に `BEA01` / `NSR02` / `NSR03` があること (ISO 9660 の記述子が先に並ぶブリッジ盤を通すため走査する)。無ければ `-EINVAL`。詳しい検査 (アンカー、ボリューム記述子列、ファイルセット) は上流。最終ブロックは `cdrom_get_last_written` が使えないのでデバイス容量から求める。

## 検証

テスト用イメージ (`tools/gen_udf_fixture.py`): `mkudffs` で空のイメージを作り、`udisksctl` でループデバイス + **ホストの Linux の UDF ドライバでマウントして木を書く** (書き込み側の独立した実装)。作成直後にアンマウントして読み取り専用で再マウントし、全ファイルの内容・mode・nlink・mtime・リンク先が manifest と一致することを確認してから manifest にする。7z (独立実装) による名前・サイズの照合は、7z が開けるときだけ行う (mkudffs / Linux が書いた UDF は 7z が "Unsupported feature" で開けず、ブリッジ盤だけ照合できた)。
`hd512` (UDF 2.01・512 バイトブロック) / `hd2048` / `hd4096` / `hd102` (UDF 1.02) / `spar150` (DVD-RAM・UDF 1.50・Sparable) / `cdrw150` (CD-RW) / `bridge` (genisoimage -udf: ISO 9660 + UDF 1.02 + Rock Ridge + Joliet) / `vat250_empty` (UDF 2.50 の VAT、空)。
内容: ISO と同じ木 (サイズ 0〜3 MiB、日本語・200 文字の名前・絵文字・1000 ファイルのディレクトリ・深さ 8・シンボリックリンク) に、UDF 固有の**ハードリンク**・**疎ファイル** (3 MiB で先頭と末尾 100 バイトだけ)・**断片化したファイル** (交互書き込み)・ICB 内の小さなファイルを追加。ブロックサイズが 512 だと長いシンボリックリンクは作れない (1 ブロックに収まらない) のでその 1 本だけ省く。

ホストテスト (`usbmem_udf_tests`): 全イメージで全ツリー比較、`--corrupt` (VRS・アンカー・主/予備 VDS・切り詰めなど 13 ケース。アンカーは末尾付近の全部を壊す)、`--ioerr`、`--fuzz` (VRS / アンカー / VDS / その後 / 末尾 / 全体の領域別)、`--coexist` (UDF + ext2 + ISO 9660 を同じプロセスで)、`udf_read_complex` (VAT / Metadata と同じ経路を強制)。

AVD (emulator-5556): `UdfInstrumentedTest` 6 件通過 (hd512 / hd2048 (ハードリンク同一 ino・シンボリックリンク含む) / spar150 / bridge (UDF を優先) / vat250_empty / ゼロ埋めの拒否)。

## 限界・未解決

- VAT / Metadata パーティションにファイルがある盤は未検証 (フィクスチャを作れない)。実機の光学ドライブ・実際の DVD/BD は未確認。
- 複雑なボリュームの `readdir` は名前ごとに `lookup` するため、非常に大きなディレクトリでは遅い。
- マルチセッション (最後のセッションの選択) は未対応 (`cdrom_multisession` は常に失敗)。
- 実機 USB での確認はしていない。
