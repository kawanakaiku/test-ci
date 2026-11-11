# Phase 2 spike 結果: fs/ntfs を無改変でビルドするために必要な互換面

方法: 取り込んだ `third_party/ntfs-resurrection/fs-ntfs/*.c`(無改変, commit 067c5c24cef2)を、**空のスタブヘッダ + 基本型(u8/__le16/loff_t 等)と
byteorder/min/max マクロだけ**で `gcc -fsyntax-only` した。残ったエラーが「互換レイヤーが最低限埋めるべき面」の下限になる。

## 実測(段階 1)
- 基本型だけで総エラー 17,085 → 5,030 件に減少(大半は型欠落の連鎖だった)。
- 未定義の **型 12**(pgoff_t, atomic_t/atomic64_t, wait_queue_head_t, kuid_t/kgid_t, errseq_t, rwlock_t, vm_fault_t, blk_opf_t 等)。
- 未宣言の **識別子 204**(定数・マクロ。`docs/ntfs-spike-missing-identifiers.txt`)。
- 未宣言の **関数/マクロ 436**(うち ntfs 自身のものは 2 のみ。`docs/ntfs-spike-missing-functions.txt`)。
  内訳の多い系統: inode_* 27, folio_* 26, iomap_* 14, bio_* 12, generic_* 10, d_* 9, filemap_* 8, bdev_* 7, atomic_* 7, fsparam_* 7 …
- **不完全型のまま使われている構造体**(メンバ参照エラーの多い順): inode 484, super_block 100, iomap 62, dentry 43, file 30, kstat 28, kiocb 23, folio 16, bio 16, kstatfs 13, iattr 13, fs_context 13。

## 解釈(事実と推定を分ける)
- 事実: 必要面は「約 450 の関数/マクロ + 約 200 の定数 + 十数個の構造体定義」で、これは**下限**(構造体を定義すると、メンバ・次の層のエラーが新たに現れる)。
- 事実: 数は多いが、大半は inode/folio/iomap/bio/dentry の 5 系統に集中している。ext4 など他のカーネル FS にも共通で、`LinuxCompat` を共通化する要件書の方針と整合する。
- 推定(未検証): iomap の buffered write/読み出しヘルパ(カーネル側の実装は fs/ntfs の外)と folio のロック/dirty/writeback の意味論を
  最小限で正しく実装するのが最大の工数・リスク。書き込み系は読み取り系より桁違いに重い。

## 判断(暫定)
方式 A(最小 VFS エミュレーション)は**技術的に閉じた問題**で、実測上も不可能ではない。ただし工数は大きい。段階的に進める案:
1. **読み取り専用の最小面**(super_block/inode/address_space/folio/bio→BlockDevice、iomap は read のみ)で、`$MFT`→ルート列挙→ファイル読み出しまで。
2. 通ったら SAF 公開(Phase 2 完了条件)。書き込みは Phase 3 で iomap write/writeback を追加。
3. 1 が想定より膨らむ場合のみ B(部分移植)へ切替。切替理由は本書に追記する。

再現: `tools/ntfs_compat_spike.sh`。

## 進捗ログ: 互換レイヤー実装 (方式 A, 読み取り専用優先)

`tools/ntfs_compat_build.sh` で `fs/ntfs/*.c` を `-fsyntax-only` した総エラー数(少ないほど互換面が埋まっている)。

| 段階 | 内容 | 総エラー |
|---|---|---|
| 0 | 空スタブのみ | 17,085 |
| 1 | 基本型・エンディアン・マクロ | 5,030 |
| 2 | `compat/kernel.h`(型・属性・byteorder) | 4,347 |
| 3 | `compat/sync.h`(atomic/mutex/rwsem/list/bitops/alloc/ERR_PTR)+ `compat/fs.h`(inode/super_block/folio/bio/iomap 等の構造体) | 2,267 |
| 4 | include 経路の修正(`-iquote`)、static_assert/wait_event | **1,488** |

内訳(段階 4): 未宣言識別子 361 / 暗黙関数宣言 575 / 不完全型 49 / メンバ不足 171。
残りの大半は関数プロトタイプ(`inode_*`, `folio_*`, `iomap_*`, `bio_*`, `d_*`, `generic_*`)と、`DECLARE_IOMAP_ITER_NEXT` など iomap 補助マクロ。

### 次にやること(未着手)
1. 残りの関数プロトタイプと定数(`docs/ntfs-spike-missing-*.txt` を参照)を宣言し、全 `.c` が構文通過する状態にする。
2. 実行時実装(`core/compat/src/`): ページキャッシュ(address_space→folio)、`bio`→`BlockDevice` ブリッジ、inode ハッシュ(`iget5_locked`)、`iomap_read_folio`。
3. 書き込み・xattr・DIO・swapfile・workqueue 系は読み取り専用段階ではスタブ(`-ENOSYS`)にする。
4. 最初のリンク可能な到達点: `runlist.c` `mst.c` `unistr.c` `upcase.c` `collate.c` と LZX/XPRESS を単体テスト付きでリンク。
5. その後 `$MFT` → ルート列挙 → ファイル読み出し。

### 現時点の状態(正直な記録)
- **NTFS はまだマウントも読み取りもできない。** `core/compat` は CMake に未接続で、アプリ・テストには影響しない。
- 互換面の下限見積り(関数 約 430 / 定数 約 200 / 構造体 十数個)は変わらず、実装量は数千行規模。

### 更新 (互換ヘッダー完成)
- 段階 5〜: `compat/consts.h`, `vfs_api.h`, `misc.h`, `ops.h`(`tools/gen_ops_structs.py` が上流の演算テーブルから自動生成)を追加。
  iomap は上流 `include/linux/iomap.h`(commit 067c5c24cef2)の `iomap_iter_next` / `DEFINE_IOMAP_ITER_NEXT*` を同等に移植(GPL-2.0)。
- **結果: `fs/ntfs/*.c` と `lib/*.c` の全 31 ファイルが無改変のままコンパイル通過(構文エラー 0、`-c` でオブジェクト生成まで成功)。**
  総エラーの推移: 17,085 → 0。`tools/ntfs_compat_build.sh` で再現。
- ただしこれは**ヘッダーが埋まっただけ**で、動作する実装はまだ無い。オブジェクトをリンクするために実装が必要な外部シンボルは
  **224 個**(`docs/ntfs-compat-runtime-todo.txt`)。内訳の傾向: folio_* / inode・dentry 系 / bio・bdev 系 / iomap_* / generic_* / filemap_*。
  うち読み取り専用の最小到達点で本当に必要なのは一部(書き込み・VFS 公開・DIO・swapfile 系は `-ENOSYS` スタブで可)と見込むが、**未検証**。
- 次: `core/compat/src/` に最小実装(pagecache / inode hash / bio→BlockDevice / iomap_read_folio / iomap_iter_continue)を作り、
  CMake で `ntfs_fs` ターゲットとしてリンク → `runlist` 等の単体テスト → `$MFT` 読み出し。

### 更新 (最初の読み取り成功)
- `core/compat/src/` に実行時を実装: `rt_core.c`(メモリ/同期/inode キャッシュ/dentry/マウント)、`rt_pagecache.c`(folio・bio・iomap 読み取り・UTF 変換)、
  `rt_vfs.c`(ホスト向け mount/lookup/readdir/read)、`bdev_bridge.cpp`(BlockDevice ブリッジ)、`ntfs_volume.cpp`(C++ ラッパ)。
  未実装の 63 シンボルは `tools/gen_compat_stubs.py` が生成するスタブ(`rt_stubs.c`)。呼ばれると stderr に名前を出して -ENOSYS を返す。
- **結果: 無改変の `fs/ntfs` で、`mkntfs` + `ntfs-3g` が作った実 NTFS イメージ(32MiB, 512B セクタ, 4KiB クラスタ)を読み取り専用でマウントし、
  ルート列挙・入れ子ディレクトリ・日本語名・3MiB+777B ファイル・空ファイルを sha256 付きで検証して一致。** 失敗 0。
  通常ビルドと ASan/UBSan(リーク検出あり)の両方で `ctest` 通過(`ntfs_gen_fixture` + `ntfs_read`)。イメージは tmpfs (/dev/shm)。
- 読み取り中に実際に呼ばれた未実装 API: `super_set_uuid` のみ(影響なし)。
- ビルド上の注意: 互換シム(`<linux/errno.h>` 等)は **C ソース専用**にする(C++ に見せると glibc の `<errno.h>` が奪われ std ヘッダと衝突)。
  `project()` に C を含め忘れると C ソースが黙って無視される(一度ハマった)。

### 制約・未検証(正直な記録)
- 検証したのは **mkntfs/ntfs-3g が作った小さな正常イメージ 1 種**のみ。圧縮ファイル・暗号化・スパース・ADS・reparse・巨大ファイル・
  破損/敵対的イメージは未検証。Windows 作成イメージとの照合も未実施(Phase 2 完了条件)。
- folio は 4KiB 固定(large folio 未対応)。`iomap_readahead`、書き込み系、DIO、xattr/ACL は未実装(スタブ)。
- マウント/アンマウントの完全な後始末(inode 退避、メモリ解放)は未実装。読み取り専用固定。
- 単一スレッド前提(inode hash の I_NEW 待ちなし)。
- Android(NDK)ビルドへの接続と SAF DocumentsProvider への接続は未着手。

### Codex 2 回目レビューの反映 (2026-11-11)
修正済み: bdev ブリッジの戻り値を負の errno に統一 / UTF-8 の厳密検証 (overlong・サロゲート・U+10FFFF 超を拒否) /
マウント失敗時の sb・fc 解放 / lookup の dentry 破棄時に inode 参照を返す。
追加修正: アンマウント実装 (evict_inodes → put_super → 残り退避 → sb/dentry/bdev 解放)、folio の参照規約を Linux と同じに
(キャッシュ 1 + get 系で +1、最後の put で解放)。アンマウント時の ASan 検出で実際に folio UAF を再現して修正。
20 回のマウント/読み取り/アンマウント反復を ASan(リーク検出あり)で通過。
未対応 (記録): fill_super 失敗時の s_fs_info 解放(小規模リーク)。
