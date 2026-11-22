# 圧縮データの展開器 (core/include/usbmem/decomp.h)

Btrfs の圧縮 extent、F2FS の圧縮クラスタ、LZ4 バックアップ (`core/src/backup_lz4.cpp`) が使う、**展開だけ**の小さなライブラリ。
どれも公開版 (Apache-2.0) に入れられるように、GPL 系のカーネルのライブラリ (`third_party/kernel-lib`) に頼らない。

| 形式 | 使う場所 | 実装 | ライセンス |
|---|---|---|---|
| zlib | Btrfs (zlib) | システムの libz (`core/blockdev.cmake` が見つける。Android は NDK の libz) | zlib License |
| LZ4 ブロック | F2FS (lz4)、LZ4 バックアップ | 自前 (`core/src/decomp.cpp`)。バックアップが元々持っていた展開をここへ移した | Apache-2.0 |
| LZO1X (LZO-RLE を含む) | Btrfs (lzo)、F2FS (lzo / lzo-rle) | 自前 (`core/src/decomp.cpp`) | Apache-2.0 |
| Zstandard | Btrfs (zstd)、F2FS (zstd) | 自前 (`core/src/decomp_zstd.cpp`) | Apache-2.0 |

API は `usbmem_zlib_decompress` / `usbmem_lzo1x_decompress` / `usbmem_zstd_decompress` / `usbmem_lz4_block_decompress` (C リンケージ)。
戻り値は 0 (成功)、`-EIO` (入力が壊れている / 未対応)、`-ENOSPC` (出力が収まらない)、`-ENOMEM`。入力は信用せず、すべての長さ・距離・出力位置を検査する。

## 1. zstd を「自前の展開器」にした理由 (upstream の zstd を同梱しなかった理由)

選択肢は (a) upstream の zstd (BSD-3-Clause) を `external/zstd` に固定した版で同梱する、(b) 展開だけを自前で書く、の二つだった。(b) にした。

| 観点 | (a) upstream を同梱 | (b) 自前の展開器 |
|---|---|---|
| 量 | 展開側だけでも約 5,500 行 (参考: Linux 7.3 の `lib/zstd` の展開側 = decompress + common は 5,557 行で、upstream の展開側と同じ規模)。圧縮側も一緒に入るか、手で切り出すことになる | `core/src/decomp_zstd.cpp` 約 540 行 (展開だけ) |
| 保守 | 版・SHA-256 の固定、NOTICE、manifest の `external-permissive`、allow-list への BSD-3-Clause の追加、更新のたびの再確認が要る。upstream のファイルは GPL 系との二重ライセンスの表記を含む (`COPYING` / `LICENSE`) ので、ツリー検査に例外が増える | 自前のコードだけ。許可リストは増えない |
| 大きさ | 展開だけに絞っても数十〜百 KB 規模 (表と最適化した分岐が多い)。圧縮側を含むと数百 KB | x86-64 の `-Os` で 7.8 KB (zstd) + 2.1 KB (LZ4 / LZO / zlib の枠) |
| 検証のしやすさ | upstream は実績が大きい | 実際の圧縮器 (zstd コマンド、libzstd) と突き合わせられる (下記)。形式 (RFC 8878) は公開されていて、展開側は決定的 |
| 性能 | 速い (2.3 GB/s 前後) | 0.8〜1.1 GB/s (同じデータで libzstd の約 1/2〜1/3)。Btrfs の extent は最大 128 KiB、F2FS のクラスタは 16〜64 KiB なので、USB の転送速度 (数十〜数百 MB/s) に対して律速にならない |

辞書・ストリーミング (フレームをまたぐ状態)・コンテンツ チェックサムの検証・並列は持たない。Btrfs と F2FS の圧縮データは辞書なしの単一フレームで、チェックサムは FS 側 (Btrfs はデータの csum) にある。
辞書 ID が付いたフレームは `-EIO` で拒否する。フレームの後ろの 0 埋めは読まない。

## 2. 検証

- 実際の圧縮器の出力を展開して元データと比べる (`tools/gen_decomp_fixtures.py` が作り、`usbmem_decomp_tests <dir>` が検査する。ctest: `decomp_gen_fixture` → `decomp_real`)。
  - zstd: `zstd` コマンドのレベル (1〜22、`--ultra`)・戦略 (fast〜btultra2)・窓の大きさ・`--long`・チェックサムの有無・標準入力 / ファイルを組み合わせた 1,000 件余り。入力は、乱数 / 繰り返し / テキスト / 0 埋め / 混合など。
  - LZO: `lzop` コマンド (コンテナを剥がして LZO1X のブロックを取り出す。レベル 1 / 3 / 7 / 9)。
  - zlib: Python の zlib (レベル 0 / 1 / 6 / 9 と各戦略)。
  - LZ4: ブロック形式の手書きの参照圧縮器 (Python) と、既存のバックアップの試験。
- LZO-RLE は、カーネルの `lzorle1x_1_compress` をユーザー空間でビルドして作ったデータ (196 件) で確かめた (開発時に 1 度。ツリーには入れない)。そのうち 1 件は `--unit` に埋め込んである (先頭の 17, 1 と 0 の連続の命令を含む)。
- 壊した入力 (ビット反転・切り詰め・挿入・無作為) のファジング (`decomp_fuzz`、ASan / UBSan のビルドで意味がある。ctest は 4000 回、開発時に 6 万回)。
- 開発時の差分試験: 壊した zstd ストリームを libzstd と自前の展開器の両方に通し、**両方が成功した場合に出力が一致する**ことを確かめた (1 バイトを全値に変える網羅も含む)。
  自前の展開器だけが失敗するのは、Huffman のビット列を過不足なく使い切っていない入力 (規格どおりの圧縮器は必ず使い切る)。libzstd だけが失敗するのは、コンテンツ チェックサム・ウィンドウ超過のオフセット・余ったバイトなど (自前は検査しない)。
- Btrfs / F2FS の既存の試験 (mkfs.btrfs の `--compress` 付きイメージ、`sload.f2fs` の圧縮、実カーネルが書いた F2FS のイメージ) は、展開器を差し替えたあとも全部通る。

## 3. 個人版

個人版も同じ展開器を使う (展開器の二重化はしない)。`third_party/kernel-lib` は、カーネル由来の squashfs / erofs が引き続き使う (`usbmem_kdecomp`)。
個人版だけ、カーネルの展開ライブラリ (`usbmem_kdecomp`) を基準にした差分試験を持つ (`core/compat/tests/test_decomp_kernel.cpp`、ctest の `decomp_kernel` / `decomp_kernel_fuzz`。公開版には入らない)。
manifest の lzo / lz4 / zstd の全項目で、自前とカーネルがどちらも元データへ展開することと、壊した入力で両方が成功したときに出力が一致することを確かめる (片方だけ成功するのは許す。検査の厳しさが違ってよい)。
