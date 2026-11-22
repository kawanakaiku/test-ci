# Btrfs / XFS / F2FS の出所の調査 (2026-11-23)

自前の Btrfs・XFS・F2FS のリーダー / ライターと、新しい展開器 (docs/decomp.md) が、Linux カーネルのコード (GPL-2.0) の写しでないことを調べ、公開版 (Apache-2.0) へ入れてよいかを判断した記録。
決定 (ユーザー、2026-11-23): 自前の Btrfs / XFS を公開版に戻し、F2FS の圧縮も公開版で有効にする。

## 1. 結論

| 対象 | 判定 | 要点 |
|---|---|---|
| Btrfs (`btrfs_volume.cpp`、`btrfs_write.inc`、`btrfs_write_ops.inc`、ヘッダ) | **写し無し (書き直し 1 か所)** | 10 トークン以上の連続一致は 0.2〜1.0%。中身はボイラープレート (`for (int i = 0; i < n; i++)`)・形式の定数表・汎用の idiom だけ。XXH64 は仕様どおりの手順で、見た目が近かったので構成を書き直した |
| XFS (`xfs_volume.cpp`、`xfs_write*.inc`、ヘッダ) | **写し無し**。ただし**ユーザーの判断が要る点が 1 つある (5 章)** | 連続一致は 0.2〜0.6%。コメントの一致 0。エラー伝播の定型 (`rc = f(); if (rc) return rc;`) と形式の定数表だけ |
| F2FS (`f2fs_volume.cpp`、`f2fs_write*.inc`) | **書き直した (写しが 2 か所あった)** | ディレクトリのハッシュ (TEA を使う `tea_transform` / `str2hashbuf` / `dentry_hash`) と、ノードの種別の判定 (`is_dnode_ofs`) が、カーネルと同じ構造 (最長 92 トークンの一致) だった。形式の記述から書き直した。残りは 0〜0.6% |
| 展開器 (`decomp.cpp`、`decomp_zstd.cpp`、`f2fs_decomp.cpp`) | **写し無し** | `decomp_zstd.cpp` の 10% (最長 117 トークン) は RFC 8878 の表 (既定の確率分布・長さと距離の基数表) で、規格が決めた数値そのもの (表の形式でしか書けない) |
| 「要判断」で公開版から外したもの | **なし** | |

判定の基準 (ユーザーの指示): 形式の事実 (フィールドの配置・定数・ハッシュ関数・アルゴリズム) を自前の名前で書くのは問題なし。コード・コメント・制御構造の写しは書き直す。

## 2. 経歴 (git の履歴から)

| 対象 | 最初のコミット | 最後のコミット | 備考 |
|---|---|---|---|
| Btrfs (読み取り) | 2026-11-12 | 2026-11-19 | 最初のコミットのメッセージ: "own implementation, not a kernel port" |
| Btrfs (書き込み) | 2026-11-13 | | 自前の COW エンジン |
| XFS (読み取り) | 2026-11-12 | 2026-11-21 | "independent implementation of the on-disk format" |
| XFS (書き込み) | 2026-11-12 以降 | 2026-11-21 | ログ (バッファログ項目) 経由のコミット |
| F2FS (読み書き) | 2026-11-12 | 2026-11-13 | "own implementation from the on-disk format" |

コミットの作者表記はすべて "Claude Code" (AI のコーディング エージェントがユーザーの指示で書いた)。人手の写し (コピー & ペースト) の痕跡はない。
ただし、AI の学習データにカーネルのコードが含まれていた可能性は、履歴からは否定できない。**否定できる根拠は 3 章の機械照合だけ**である。

開発中に参照した資料 (docs と、コードの冒頭コメントの記述による):

- Btrfs: btrfs wiki "On-disk Format"。構造体の並びの確認に uapi の `btrfs_tree.h` を参照したと、以前のコメントにあった (今回、コメントから外した。形式の事実の確認という位置づけ)。
- XFS: 公開仕様 "XFS Algorithms & Data Structures"。**加えて、カーネルの形式定義ヘッダ (`xfs_format.h`・`xfs_da_format.h`・`xfs_log_format.h`) と `xfs_log_recover.c` を「仕様として読んだ」と、以前のコードの冒頭と docs/xfs-port.md に書いてあった**。
- F2FS: カーネルのソースを「構造体のレイアウトを確かめる仕様書として読んだ」(docs/f2fs-port.md)。

## 3. 機械照合

ツール: `tools/provenance_scan.py` (標準ライブラリのみ)。カーネルのソースは `tools/provenance_fetch.sh` が取得する (リポジトリには入れない)。

比べた相手 (GitHub のタグの tarball。取得時の SHA-256):

| タグ | tarball の SHA-256 |
|---|---|
| v7.3 | `fd00c1da5cb8d190679248673ca6424492336818e3874a4752144ded942ff68b` |
| v6.12 | `c148131ddf77ec5252eb1067cad8ed62b498c4bee79cc46b44659f2348cbd494` |

(GitHub が生成する tarball なので、将来は同じ内容でもハッシュが変わりうる。タグのコミットで確かめること。)

カーネル側の範囲: `fs/btrfs`・`fs/xfs`・`fs/f2fs`、`include/uapi/linux`、`include/linux`、`lib/{lzo,lz4,zstd,zlib_inflate}`、`lib/xxhash.c` と `lib/crc/*` (Btrfs のチェックサム)。

方法 (ファイルごと):

1. **トークン列の完全一致**: コメントを除いたトークン列 (識別子・数値・記号) で、10 トークン以上の連続一致。
2. **正規化一致**: 識別子 → I、数値 → N、文字列 → S に潰して 24 トークン以上の連続一致 (名前を変えただけの写しの検出)。
3. **コメントの一致**: 英語の語 6 語以上の連続一致。
4. **識別子の重なり**: 接頭辞つき (`xfs_` / `btrfs_` / `f2fs_`)・大文字マクロ・構造体のフィールド名など。
5. **コメント中のカーネルの識別子への言及** (読んだ証跡)。

結果 (修正後。カーネルとの完全一致 = 10 トークン以上):

| ファイル | トークン数 | 完全一致 | 最長 | 正規化 (24 以上) | 最長 | コメント一致 | 判定 |
|---|---|---|---|---|---|---|---|
| `btrfs_volume.cpp` | 11,829 | 115 (1.0%) | 12 | 335 (2.8%) | 39 | 0 | 写し無し |
| `btrfs_write.inc` | 16,100 | 131 (0.8%) | 12 | 304 (1.9%) | 55 | 0 | 写し無し |
| `btrfs_write_ops.inc` | 11,854 | 23 (0.2%) | 12 | 655 (5.5%) | 40 | 0 | 写し無し |
| `xfs_volume.cpp` | 10,862 | 23 (0.2%) | 12 | 208 (1.9%) | 52 | 0 | 写し無し |
| `xfs_write.inc` | 5,865 | 10 (0.2%) | 10 | 99 (1.7%) | 51 | 0 | 写し無し |
| `xfs_write_alloc.inc` | 10,163 | 32 (0.3%) | 11 | 927 (9.1%) | 36 | 0 | 写し無し |
| `xfs_write_attr.inc` | 3,639 | 21 (0.6%) | 11 | 106 (2.9%) | 29 | 0 | 写し無し |
| `xfs_write_full.inc` | 5,486 | 22 (0.4%) | 12 | 351 (6.4%) | 51 | 0 | 写し無し |
| `xfs_write_node.inc` | 2,735 | 10 (0.4%) | 10 | 173 (6.3%) | 34 | 0 | 写し無し |
| `xfs_write_ops.inc` | 10,875 | 46 (0.4%) | 12 | 250 (2.3%) | 41 | 0 | 写し無し |
| `xfs_write_rmap.inc` | 7,083 | 21 (0.3%) | 11 | 556 (7.8%) | 32 | 0 | 写し無し |
| `f2fs_volume.cpp` | 11,875 | 76 (0.6%) | 12 | 55 (0.5%) | 28 | 0 | 書き直した (下記) |
| `f2fs_write.inc` | 7,712 | 0 | 0 | 98 (1.3%) | 26 | 0 | 写し無し |
| `f2fs_write_cp.inc` | 4,624 | 11 (0.2%) | 11 | 0 | 0 | 0 | 書き直した (下記) |
| `f2fs_write_ops.inc` | 4,044 | 0 | 0 | 29 (0.7%) | 29 | 0 | 写し無し |
| `f2fs_decomp.cpp` | 173 | 0 | 0 | 0 | 0 | 0 | 写し無し |
| `decomp.cpp` | 1,832 | 0 | 0 | 0 | 0 | 0 | 写し無し |
| `decomp_zstd.cpp` | 6,534 | 682 (10.4%) | 117 | 713 (10.9%) | 120 | 0 | 写し無し (RFC 8878 の表) |

ヘッダとテストも同じ手順で照合した (カーネルとの完全一致 / 最長):

| ファイル | トークン数 | 完全一致 | 最長 | 正規化の最長 | 判定 |
|---|---|---|---|---|---|
| `btrfs_volume.h`・`xfs_volume.h`・`f2fs_volume.h` | 419〜496 | 0 | 0 | 0 | 写し無し |
| `test_btrfs.cpp` | 10,776 | 65 (0.6%) | 14 | 0 | 写し無し (`for (int i = 0; ...)` と `int main(...)` の定型) |
| `test_btrfs_write.cpp` | 10,546 | 95 (0.9%) | 15 | 72 → 書き直した | 参照用の XXH64 が仕様どおりで、キー定数・手順がカーネルの `xxhash.c` と同じ構造だった。仕様の文章の順に書き直した |
| `test_xfs.cpp`・`xfs_dump.cpp` | 9,072・1,145 | 13 (0.1%)・13 (1.1%) | 13 | 0 | 写し無し |
| `test_xfs_write.cpp` | 12,547 | 56 (0.4%) | 13 | 38 | 写し無し (数値の並び = ファイルサイズの境界値の表と定型) |
| `test_f2fs.cpp` | 8,129 | 13 (0.2%) | 13 | 0 | 写し無し |


一致の中身を目で確かめた結果 (上位の一致):

- 完全一致の上位 (12 トークン前後): `for (int i = 0; i < n; i++)` の連続、`return -EIO; return 0; }`、`#else return false; #endif` など。構文が決める定型。
- 正規化一致の上位 (24〜55 トークン): ほとんどが `rc = f(...); if (rc) return rc; rc = g(...); if (rc) return rc;` の繰り返し (エラー伝播の定型)、`kXxx = 1, kYyy = 12, ...` の形式の定数表 (種別番号。値は形式が決める)、`st->ino = ino; st->size = ...` の構造体の詰め替え。
- コメントの一致: 0 (XFS の 1 件 (手元の旧版) は、コメント中の語の連続一致で、今回のコメントの書き直しでなくなった)。

## 4. 見つかった写しと、書き直したもの

| 場所 | 何が一致したか | 対処 |
|---|---|---|
| F2FS `f2fs_volume.cpp` のディレクトリ ハッシュ (旧 `tea_transform`・`str2hashbuf`・`dentry_hash`) | 最長 92 トークンの一致 | TEA の 1 周を `tea_mix`、詰め方を `hash_pack_word`、ハッシュ本体を `hash_mix_block` / `dentry_hash` として書き直した (形式の記述: 「名前を 16 バイトごとに 4 語へ詰め、足りない部分は残りの長さを 4 バイトに繰り返した値で埋め、TEA を 16 周回して 2 語を混ぜる」)。結果は実カーネルが作ったイメージ (tools/f2fs_vm) と mkfs.f2fs のイメージの全項目で一致 (ctest f2fs_read) |
| F2FS `f2fs_write_cp.inc` の `is_dnode_ofs` (旧コメント: 「カーネルの IS_DNODE」) | 11 トークン (+ コメントの対応づけ) | ノードのオフセットの並びの形式 (0 = inode、1・2 = 直接、3 = 間接、…) の記述から `ofs_is_indirect` として書き直した |
| F2FS の識別子 `dir_buckets`・`bucket_blocks`・`dir_block_index`・`crc32_le` | カーネルの関数名と同じだった | `buckets_at_level`・`blocks_per_bucket`・`first_block_of_bucket`・`crc32_seeded` に改名した |
| Btrfs `xxh64` (本体 `btrfs_volume.cpp` と、試験の参照実装 `test_btrfs_write.cpp`) | 正規化一致 93 / 72 トークン (XXH64 の手順そのもの) | アルゴリズムは公開仕様 (xxHash) なので写しではないが、構成が近かったので、4 レーンの配列・小さな補助関数に組み替えた (出力は変わらない。ctest で 4 種のチェックサムのイメージを確認) |
| コメントのカーネル関数名への言及 | `xfs_rmap_map`・`xfs_dir2_sf_addname_pick`・`xfs_attr_shortform_bytesfit`・`xfs_default_attroffset`・`XFS_DIR2_FREE_OFFSET`・`XFS_LI_BUF`・`XFS_BLF_CHUNK`・`BTRFS_MAX_EXTENT_ITEM_SIZE`・`btrfs_name_hash`・`btrfs_header` ほか | 説明の文に直した (名前を引いていた箇所の意味は形式の事実として書いた) |
| コードの冒頭コメント | 「`include/uapi/linux/btrfs_tree.h` の構造体の並び」・「`xfs_format.h` などを仕様として読み」 | 形式の事実の出所として書き直した (2 章・5 章) |
| `third_party/kernel-lib` のラッパ (`btrfs_decomp.c`・旧 `f2fs_decomp.c`・`f2fs_decomp_stub.c`) | GPL 系のライブラリへの依存 | 廃止し、`core/src/decomp*.cpp` に置き換えた (docs/decomp.md) |
| SPDX | `GPL-2.0-or-later` 表記 (カーネル lib・形式定義への依存の安全側) | Btrfs / XFS の全ファイルとテストを `Apache-2.0` に直した (manifest も `exclude own-gpl` から `include own` へ) |

Btrfs / XFS / F2FS のコードが `#include` するのは、自前のヘッダ (`usbmem/*.h`) と標準ライブラリだけ。カーネルのヘッダ・uapi ヘッダの取り込み・コピーはない (`grep -rn '<linux/' core/compat/src/{btrfs,xfs,f2fs}*` が空であることを確認した。F2FS / XFS / Btrfs の CMake も `-I` でカーネルのヘッダを足さない)。

## 5. 残る点 (ユーザーの判断が要るもの)

1. **XFS の形式定義の出所**。「公開仕様から書いた」と言い切れず、**カーネルの形式定義ヘッダを読んで数値を確かめた**ことがある (2 章)。機械照合では写しは見つからなかった (コメントの一致 0、10 トークン以上の一致は定型のみ) ので、ユーザーの指示 (形式の事実を自前の名前で書くのは問題なし) に沿って公開版に入れた。
   ただし、フィールドの配置や定数を GPL のヘッダから取ったことの法的な評価は、著作権者であるユーザーが決めること。気になるなら、`tools/public_manifest.txt` の XFS の行 (`core/compat/src/xfs_*.inc`・`xfs_volume.cpp`・ヘッダ・`xfs.cmake`・試験) を `exclude` に戻せば公開版から外れる (JNI は `USBMEM_FS_XFS` の条件コンパイルなので、CMake の定義とリンクも外す)。
2. **照合の限界**。カーネル (v7.3 / v6.12) とだけ照合した。btrfs-progs / xfsprogs / f2fs-tools (ユーザー空間のコード、GPL-2.0) とは照合していない。形式を実装するコードは、カーネルとこれらの間で似ていることが多い。
3. AI が書いたコードなので、学習データ由来の類似は 3 章の機械照合以外では否定できない (2 章)。

## 6. 展開器の出所

docs/decomp.md。zlib はシステムの libz。LZO1X は LZO の公開された形式の説明から自前で書いた (minilzo・カーネルの lzo は使わない)。Zstandard は RFC 8878 から自前で書いた (upstream の同梱はしない。理由は docs/decomp.md の 1 章)。
LZ4 のブロックは、以前から `core/src/backup_lz4.cpp` にあった自前の展開器を移したもの。カーネルの `lib/{lzo,lz4,zstd}` との機械照合の結果は 3 章の最後の 3 行。

## 7. 再現手順

```sh
tools/provenance_fetch.sh /dev/shm/prov            # カーネルを取得 (終わったら rm -rf)
python3 -I tools/provenance_scan.py --kernel /dev/shm/prov/k-v7.3 --kernel /dev/shm/prov/k-v6.12 --group btrfs --show 8 \
  --ours core/compat/src/btrfs_volume.cpp core/compat/src/btrfs_write.inc core/compat/src/btrfs_write_ops.inc
# --group xfs / f2fs / decomp も同様
```
