# F2FS の読み取り専用リーダー

## 先に結論 (実用上の範囲)

- F2FS (Flash-Friendly File System、Android のデータ領域や一部の USB メモリ / SD カードで使われる) を**読み取り専用**でマウントできる。書き込みは常に拒否 (`EROFS`)。
- **Linux の `fs/f2fs` は取り込んでいない。** 他の移植 (ext4 / squashfs / erofs など) と違い、無改変インポートではなく、**オンディスク形式から書き起こした自前のリーダー** (`core/compat/src/f2fs_volume.cpp`、約 700 行、Apache-2.0) である。カーネルのソースは、レイアウト (構造体のオフセットと意味) を確かめる仕様書として読んだだけで、コードは写していない。
  - 理由: `fs/f2fs` はセグメント管理 (SIT / SSA の更新、GC スレッド、チェックポイント書き込み)、sysfs / debugfs、shrinker、per-CPU カウンタ、fscrypt / fsverity などと一体で、LinuxCompat に載せるには読み取りに要らない部分の大半を空実装にする必要があり、メモリと時間の見積もりが合わなかった。読み取りに必要な部分 (スーパーブロック、チェックポイント、NAT、ノード木、ディレクトリ、圧縮クラスタ) は小さい。
  - その代わり、**ドライバの出力そのものは検証にならない**ので、検証は外部のものに頼っている (後述: `mkfs.f2fs` / `sload.f2fs` / `fsck.f2fs` / `dump.f2fs`、そして QEMU 上の本物の Linux カーネルが書いたイメージ)。
- 圧縮 (lz4 / lzo / lzo-rle / zstd) の展開だけは、squashfs / erofs と共有する `usbmem_kdecomp` (Linux `lib/` の無改変) を使う。そのため、このリーダーを含む共有ライブラリは従来どおり GPL 扱い (docs/licenses.md)。
- 対応: インライン データ / インライン ディレクトリ / インライン xattr 領域 (固定 50 ワードと `flexible_inline_xattr`)、`extra_attr`、直接 / 間接 / 二重間接 / 三重間接のブロック木、圧縮ファイル (クラスタの大きさ 2〜64 ブロック)、シンボリックリンク、ハードリンク、特殊ファイル (一覧と stat のみ)、`inode_checksum` / `sb_checksum` / `inode_crtime` / `lost_found` / `quota` (読むだけ) 付きのイメージ、2 つのチェックポイント パックの新しい方の選択、NAT ジャーナル (compact / 通常)。
- マウントしない (理由を返す): 暗号化 / casefold / fs-verity / ブロック サイズが 4 KiB 以外 / セグメントあたりのブロック数が 512 以外 / zoned / multi-device / device alias / packed SSA / 未知の機能ビット → `EOPNOTSUPP` (95)。壊れている、またはチェックポイントの ERROR / FSCK / QUOTA_NEED_FSCK / RESIZEFS フラグ、または **fsync 済みで回復 (roll-forward) が必要な状態** → `EUCLEAN` (117)。
- `EUCLEAN` のうち「回復が必要」と「フラグ」の場合に限り、確認ダイアログのあとで `allow_noload` (ext4 のジャーナル無視と同じ形) により、**回復を無視して**チェックポイントの時点の内容を読み取り専用で開ける (`recovery_ignored()` が true)。古い内容が見えることがある。
- 実機 (USB ディスク) では未確認。ホストのイメージ ファイルと AVD のみ。

## 構成

| ファイル | 内容 |
|---|---|
| `core/compat/public/usbmem/f2fs_volume.h` | `F2fsVolume` (`FsVolume` の実装)、`F2fsMountOptions { allow_noload }` |
| `core/compat/src/f2fs_volume.cpp` | リーダー本体 |
| `core/compat/src/f2fs_decomp.c` | 圧縮クラスタの展開 (lzo / lz4 / zstd。`usbmem_kdecomp` を使う薄い C の層) |
| `core/compat/f2fs.cmake` | ターゲット `usbmem_f2fs` とテスト (ctest `f2fs_*`) |
| `core/compat/tests/test_f2fs.cpp` | テスト (読み取り / 破壊 / I/O エラー / ファジング / noload) |
| `tools/gen_f2fs_fixture.py` | `mkfs.f2fs` + `sload.f2fs` + `fsck.f2fs` でイメージを作る |
| `tools/f2fs_vm/` | QEMU 上の本物のカーネルに F2FS を書かせるイメージ生成 (`init.c` = VM の `/init`、`gen_k_fixtures.py`) |
| `tools/run_android_f2fs_test.sh`、`android/app/src/androidTest/.../F2fsInstrumentedTest.kt` | AVD での実行 |
| `android/app/src/main/cpp/jni_ntfs.cpp` ほか | `mount_auto` の分岐、UI (読み取り専用で公開) |

## 設計の要点

オンディスクの要点 (すべて 4 KiB ブロック、1 セグメント = 512 ブロック):

- **スーパーブロック**: ブロック 0 のバイト 1024 とブロック 1 に 2 つ。磁気 0xF2F52010。`sb_checksum` があれば CRC32 を検証。領域 (チェックポイント → SIT → NAT → SSA → メイン) が隙間なく順に並ぶことを検査。
- **チェックポイント**: 2 つのパック (`cp_blkaddr` と `+512`)。それぞれ先頭と最終ブロックの CRC とバージョンが一致するものだけを有効とし、新しい方を使う。
- **NAT**: 1 ブロックに 455 エントリ (各 9 バイト)。チェックポイントのバージョン ビットマップ (MSB が先) で 2 つのコピーのどちらが最新かを選ぶ。カレント サマリ (NAT ジャーナル) の項目がブロックの内容を上書きする。
- **ノード**: フッタ (ブロック末尾 24 バイト) の nid / ino が、たどってきた先と一致することを毎回検査する (壊れた木で別のノードに飛ばされるのを防ぐ)。
- **inode**: `extra_isize` / `inline_xattr_size` から i_addr の開始位置とインライン領域を決める。直接アドレス数は inode 内 (extra と xattr を除く)、ノードあたり 1018、間接ノードあたり 1018 個の nid。
- **圧縮ファイル**: クラスタごとに先頭が `COMPRESS_ADDR` (0xFFFFFFFE) のとき圧縮済み。続く有効なブロック (`NEW_ADDR` / 0 で終わり) に、**24 バイトのヘッダ** (`clen` + `chksum` + 予約 16 バイト) + 圧縮データ。展開後は**ちょうどクラスタの大きさ**でなければ `EUCLEAN`。`chksum` (`compress_chksum` マウント オプションで有効になる) は検証していない (展開が成功し、長さが合うことで十分とした)。圧縮ファイルの 1 ノードあたりのアドレス数は、クラスタの大きさの倍数に切り下げる。
- **ディレクトリ**: 214 スロット / ブロック、ビットマップは LSB が先。インライン ディレクトリは `ビットマップ | 予約 | エントリ | 名前` の順。
- **roll-forward の検出**: チェックポイントの `cur_node_segno[1]` / `cur_node_blkoff[1]` (**メイン領域の先頭から**数えたセグメント番号) から暖かいノードのログをたどり、フッタのチェックポイント バージョン (`CRC_RECOVERY` のとき上位に CRC が入る) が一致し、`FSYNC` マークの付いたノードが 1 つでもあれば「回復が必要」とする。本物のカーネルが fsync 後に切られたイメージ (`k_unclean`) で確認済み。

安全のための方針:

- 書き込みは一切しない (`write` 系はすべて `EROFS`)。デバイスへの書き込みを発行するコードを持たない。
- すべてのブロック番号は、メイン領域 / 全体の範囲を検査してから読む。ループにはかならず上限を付けている (ノード木の深さ、ディレクトリのブロック数、roll-forward の歩数)。
- 読み取りは LRU キャッシュ (2048 ブロック)、ディレクトリのキャッシュ (8 件)、展開済みクラスタ 1 つ。メモリは大きさに比例しない (ファジングの最大 RSS は約 10 MiB)。

## テスト (`core/compat/tests/test_f2fs.cpp`、ctest `f2fs_*`)

イメージは 2 系統 (どちらも期待値は「イメージに入れる前のソース ツリー」で、ドライバの出力ではない):

1. `tools/gen_f2fs_fixture.py`: `mkfs.f2fs` の機能の組み合わせ (`plain` = 機能なし、`ext`、`quota`、`large` = 6 GiB の疎イメージで NAT / SIT が複数セグメント) に `sload.f2fs` でツリーを入れ、**`fsck.f2fs` が全項目 OK** のものだけを使う。
2. `tools/f2fs_vm/gen_k_fixtures.py`: 本物の Linux カーネル (cachyos 7.1.8 の汎用 `vmlinuz` と `f2fs.ko`) を QEMU (TCG、KVM 不要) で起動し、`init.c` がマウントしてツリーをコピーする。`mkfs.f2fs` / `sload.f2fs` では作れないものを作る:
   - `k_basic`: `flexible_inline_xattr`、ハードリンク、シンボリックリンク、FIFO / ソケット / キャラクタ / ブロック デバイス、**9 GiB の疎ファイル** (直接 / 単間接 / 二重間接 / 三重間接のノード木をすべて通る)
   - `k_nofeat` / `k_ext`: 機能なしの古い形式 / `extra_attr` など
   - `k_comp_lz4_c2` / `k_comp_lz4_c4` / `k_comp_lz4_chk` / `k_comp_lzo_c2` / `k_comp_lzorle_c3` / `k_comp_zstd_c2`: 圧縮 (クラスタ 4 / 8 / 16 ブロック、`compress_chksum` 付き)
   - `k_dirty`: チェックポイントのあと fsync も sync もせずに電源断 (回復は不要。チェックポイントの内容がそのまま見える)
   - `k_unclean`: チェックポイントのあと fsync だけして電源断 (**roll-forward が必要**)
   - きれいにアンマウントしたものは、こちらも `fsck.f2fs` が全項目 OK。

- `f2fs_read`: 全イメージの全ファイル / ディレクトリ / リンクを、manifest (パス、サイズ、mtime、nlink、内容 = パターン / ゼロ / 参照ファイル) と突き合わせる。ブロック境界の前後のサイズ、1000 エントリのディレクトリ、日本語名、長い名前を含む。
- `f2fs_corrupt`: メタデータの領域ごとに 16 KiB を破壊したイメージ (507 ケース)。クラッシュ / ハングなし。
- `f2fs_ioerr`: n 回目の読み取りで失敗させる (全イメージ)。マウント失敗 / 読み取り失敗がエラー コードで返る。
- `f2fs_fuzz`: ランダムなバイト破壊 (全イメージ × 600 回、seed 1)。クラッシュ / ハングなし、最大 RSS 約 10 MiB。
- `f2fs_noload`: `k_unclean` は既定で `EUCLEAN`、`allow_noload` なら開けて `recovery_ignored()` が true。

ctest は `f2fs_gen_fixture` と `f2fs_gen_kfixture` (カーネルや qemu が無ければ SKIP) の後に上のテストを走らせる。

## 実装中に見つけて直したこと

- 圧縮ヘッダは 20 バイトではなく **24 バイト**。最初に 20 で作り、`sload.f2fs` が圧縮に対応していないこのマシンでは検出できなかった。本物のカーネルが書いた圧縮イメージで初めて失敗し、修正した (`kCompHdr`)。
- roll-forward の検出が、ノード ログの位置を `seg0` 基準で計算していた (正しくはメイン領域基準)。`k_unclean` を本物のカーネルで作って初めて分かった。それまでは「回復が必要な状態」を一度も検出できていなかった。
- `sload.f2fs` (1.16.0) は、`flexible_inline_xattr` をつけたイメージで、自分の `fsck.f2fs` が NG とするものを作ることがある (SIT / `valid_block_count` の不整合)。そのため機能なし / `extra_attr` 系のみ `sload.f2fs` で作り、`flexible_inline_xattr` は本物のカーネルで作る。

## 限界・未解決

- 実機 USB での確認なし。
- 読み取り専用のみ。書き込み、回復 (roll-forward の適用)、`fsck` は未対応。
- ブロック サイズ 4 KiB / セグメント 512 ブロックのみ (Android の標準)。大きなページ サイズのカーネルで作ったイメージは拒否される。
- 暗号化 (fscrypt)、casefold、fs-verity、zoned、multi-device は拒否。
- 圧縮ファイルの `chksum` は検証しない。`compress_released` (`F2FS_IOC_RELEASE_COMPRESS_BLOCKS` で読み取り専用にして領域を解放したファイル) は `EOPNOTSUPP`。
- xattr、ACL、プロジェクト ID、project quota は読まない (quota 付きイメージの読み取りは可)。
- アクセス時刻は inode の `i_atime` をそのまま返す (relatime の補正なし)。
- 特殊ファイルは一覧と stat のみ。内容は読めない。
- 圧縮アルゴリズム `lz4` は `LZ4_decompress_safe`、`zstd` は ワークスペースを都度確保 (性能は未調整)。

## 検証の結果

(ホスト 4 構成と AVD の結果は、実行後にここへ追記する)
