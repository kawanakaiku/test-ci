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
