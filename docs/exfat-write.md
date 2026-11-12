# exFAT (Linux fs/exfat) の書き込み — 実験的

2026-11-12。読み取り専用で動いていた `fs/exfat` (**無改変**。`third_party/exfat` に差分なし) を、ext2 の書き込みで作った LinuxCompat の書き込み基盤の上で書き込み可能にした。
`USBMEM_NTFS_WRITE` を付けたビルドだけで有効 (CMake `-DUSBMEM_NTFS_WRITE=ON` / Gradle `-PusbmemNtfsWrite=true`)。無効なビルドで `read_only = 0` を要求すると `-EROFS`。実行時にも `mount_rw` (`read_only=0`) が必要。
**既定のビルドは今までどおり読み取り専用**。実機 (USB ディスク) での確認はまだ (ホストと AVD のみ)。

## 先に結論
- 作成・書き込み (拡張・上書き・穴・ValidDataLength をまたぐ書き込み)・truncate (縮小・拡張)・削除・rmdir・rename (同一ディレクトリ・跨ぎ・上書き・ディレクトリ・大文字小文字だけの変更) が動く。クラスタ 512B / 4KiB / 32KiB (セクタ 512 / 4096) で確認。
- 検証は**独立実装**で行った: exfatprogs の `fsck.exfat` と、relan の `exfat-fuse` (`mount.exfat-fuse`)。自前ドライバで書いたイメージが `fsck.exfat -n` で clean、かつ同じ操作を exfat-fuse でやった結果とツリー全体 (種別・サイズ・sha256) が一致。
  (Linux カーネルの exfat ドライバ自体は PC で使えなかったので、参照実装は exfat-fuse。置き換えた点は下の「参照実装について」。)
- 電源断の模擬 (320 試行/構成): 元ファイルの破損 0、ダーティ印なしで変更 0、`fsck.exfat -y` 後に clean にならない 0、修復前の不整合 0 (許容した例外は後述)。規則を外した突然変異テスト 4 種が全て検出された。
- exFAT にジャーナルは無い。電源断・抜去のあとはボリュームが「汚れた」(VolumeDirty) 状態で残り、`fsck.exfat` が必要。アプリは汚れたボリュームを書き込み可でマウントしない。これは仕様。
- `readlink` / `symlink` / `link` は exFAT に存在しない (`-95`)。

## 構成 (どこに何があるか)
書き込み基盤は ext2 のものを共用する。exFAT 固有なのは `rt_vfs_exfat.c`、`exfat_volume.cpp`、`exfat_c.h`。この作業で基盤に足したもの:

| 場所 | 内容 |
|---|---|
| `rt_pagecache.c` | `usbmem_bdev_writeback_ranges(bdev, ranges, n, inside)`: ダーティな 512B のうち、指定範囲に**入る(inside=1)/入らない(inside=0)**ものだけを書く。FAT・ビットマップ・新クラスタ・ディレクトリエントリを**段階に分けて**書くために必要 |
| `rt_write.c` | `iomap_truncate_page` (`rt_bh.c` のスタブを置換)、`simple_rename_timestamp` (スタブを実装)。`fill_via_read`: 書き込み前の folio 補充を `read_folio` で行う (後述) |
| `include/compat/fs.h` | `super_block.usbmem_fill_via_read_` |
| `rt_vfs_exfat.c` | ホスト入口。書き込み操作は `#ifdef USBMEM_NTFS_WRITE` の中。無効ビルドは `-EROFS` のスタブ |

### クラスタ < 4096 のとき (`fill_via_read`)
従来の `fill_folio` は「folio 全体が連続した 1 つのマッピング」と仮定していた。クラスタが 512B / 1KiB / 2KiB だと 1 つの 4KiB folio に**離れたクラスタ**が載りうる。
exFAT は `usbmem_fill_via_read_ = 1` を立て、書き込み前の folio 補充を上流の `read_folio` (iomap) に任せる。4KiB 以上のクラスタでは従来どおり。

### 書き込み可でマウントする条件 (`writable_precheck`)
次のどれかなら**読み取り専用に落ちる** (`writable() == false`、書き込みは `-EROFS`/30)。イメージは 1 バイトも変えない。
- `VolumeFlags` に VolumeDirty (0x2) が立っている (前回きれいに取り外されていない)。
- `VolumeFlags` に MediaFailure (0x4) が立っている。
- ブートセクタが exFAT でない / セクタサイズが 512〜4096 でない / FAT が 2 面 (TexFAT。上流のマウントが `num_fats != 1` を拒否するので実際にはマウント自体が失敗する)。
常に**読み取り専用でマウント**する。最初の書き込みの直前に `SB_RDONLY` を外して `exfat_set_volume_dirty` を呼ぶ (ブートセクタに VolumeDirty を書いて FUA/flush)。マウントしただけ・読んだだけでは何も書かない (`--noop` テストで、rw を要求して書かずに閉じたイメージが**バイト一致**)。
クリーンな unmount で `exfat_put_super` が VolumeDirty を消す。

## 同期の順序と安全の規則
ジャーナルが無いので、順序で安全を作る。`usbmem_exfat_sync` (`sync_all`) の手順:

1. **データ**: 各ファイルの page cache をデバイスへ書き、flush。ここで途切れても、どのファイルのメタデータもまだ変わっていない。
2. **inode メタデータ**: `write_inode` でディレクトリエントリセットを更新 (bd の dirty になるだけ)。
3. **メタデータを種類別の順序で**。各段階の間に flush を入れる。
   - **確保系** (作成・書き込み・拡張・名前の追加): 新クラスタの中身 → ビットマップ → FAT → ディレクトリエントリ
   - **解放系** (削除・縮小・上書き rename): ディレクトリエントリ → FAT → ビットマップ
4. **確保系と解放系を同じ同期の中で混ぜない**。操作の入口 (`begin_op`) で種類が変わるときは先に同期する。

理由と、それぞれの規則を外すと何が起きるか (すべて突然変異テストで確認):
- **ダーティ印は最初の変更より前に確定**: 外すと、電源断のあとボリュームが clean に見えるのに中身が変わっている (B が失敗: 312/320)。
- **解放したクラスタを同期の前に再利用しない** (種類切り替えでの同期): 外すと、解放したクラスタに新しいデータが先に書かれ、電源断のあと「まだ生きている古いファイル」の内容が新しいデータに入れ替わる (A が失敗: 152/320)。
- **新クラスタの中身を、それを参照するものより先に書く**: 新しいディレクトリ (ゼロ埋め) や伸びたディレクトリの新しい部分を、エントリより先に書かないと、電源断のあとディレクトリの中に古いクラスタの残骸が見える (D が失敗: 4/320)。
  新クラスタは、前回の同期時点のビットマップの写し (`bm_snap`) と現在のビットマップの差で見つける。
- **ビットマップ → FAT → エントリ** (確保系): ルートディレクトリはエントリを持たず「FAT のチェーン」だけが範囲を決める。FAT を先に書くと、ゼロ埋め前のクラスタがルートに繋がって見える。逆順にすると (ビットマップが後) 「クラスタが free なのにチェーンにある」不整合が出る (D が失敗: 48/320)。
- **エントリ → FAT → ビットマップ** (解放系): 先に参照を消す。

### ピン (ino → inode の表)
exFAT の ino は `iunique` で実行時に採番され、ディスク上の値ではない。`lookup` / `readdir` / `create` が返した inode を**参照を保持**して ino から引く (`unmount` まで有効)。
削除・rmdir・rename で上書きされたものはピンを外す (これで inode が追い出され、`exfat_evict_inode` がクラスタを解放する)。
ピンには**親の ino**を持たせ、ディレクトリを自分の中へ移す rename (`-EINVAL`) の検査に使う (exFAT に `..` は無く、inode の `dir.dir` は親チェーンの途中のクラスタを指すことがあり祖先をたどれない)。

### 大文字小文字
exFAT は名前の大文字小文字を区別しない (lookup も重複判定も)。大文字小文字だけが違う名前への rename は、同じエントリの綴りの変更として扱う (移動先を負の dentry に置き換えて上流に渡す)。完全に同じ名前への rename は何もしない。

## テスト
| ツール | 内容 |
|---|---|
| `tools/run_exfat_write_tests.sh <bin>` | 決まった操作列 (`gen_exfat_ops.py`。クラスタ 512B / 4KiB / 32KiB のフィクスチャ)→ `fsck.exfat -n` + exfat-fuse での同一操作列とのツリー比較。乱数操作 + メモリ上のモデル (クラスタ 512B / 4KiB / 32KiB、複数シード、操作ごとのチェックポイント比較、再マウント後比較) → `fsck.exfat` + exfat-fuse のツリーとモデルのダンプの比較。数千ファイルの作成・削除・rename。noop / 書き込み拒否 (VolumeDirty / MediaFailure) / 満杯とディレクトリ伸長不能 / 名前の規則 / クリーン unmount |
| `tools/run_exfat_crash_tests.sh <bin> x [N] [cluster]` | 電源断の模擬 (`crash_device.h`): 順序保持 (prefix) とキャッシュ (順序保証なし) × 全セクタ/途中切断 (torn) × 同期方式 2 通り (最後に 1 回 / 操作ごと)。ボリュームは小さく (256 クラスタ) して割り当てが一周するようにし、解放クラスタの再利用を起こす。元のボリュームは**独立実装 (exfat-fuse) で作る** |
| `tools/run_android_exfat_write_test.sh` | AVD: 計装テスト `ExfatWriteTest` → 結果イメージを `adb pull` → `fsck.exfat` + exfat-fuse と比較 |
| ctest `exfat_write` / `exfat_crash` | 上の 2 つ (軽量版)。`run_host_tests.sh` の wsan / wrel で実行される |

電源断テストの検査:
- (A) `fsck.exfat -y` で直したあと、元からあった全ファイルのツリーが元と一致 (操作列が消す元ファイルは「消えている」か「元のまま」)。
- (B) ブートセクタ以外が 1 バイトでも変わっているなら VolumeDirty が立っている。
- (C) `fsck.exfat -y` のあと `fsck.exfat -n` が clean。
- (D) 修復**前**の `fsck.exfat -n` がエラーなし (書き込み順序の検査)。torn=1 は複数セクタにまたがる構造の書き込みが途中で切れた場合の「ファイルのチェックサム不一致」「ルートのクラスタチェーン破損」だけ許す。exFAT 自体に複数セクタ更新の原子性が無いため (fsck が直す)。

突然変異テスト (`-DCMAKE_C_FLAGS=-DUSBMEM_MUTATE_...` で別ビルド。通常ビルドには入らない):
| 突然変異 | 結果 (320 試行) |
|---|---|
| `USBMEM_MUTATE_NO_DIRTY` (ダーティ印をメモリ上でだけ立てる) | B: 312 失敗 |
| `USBMEM_MUTATE_NO_KIND_SYNC` (確保系/解放系の切り替えで同期しない) | A: 152 失敗 |
| `USBMEM_MUTATE_ORDER` (書き込み順序を逆にする) | D: 48 失敗 |
| `USBMEM_MUTATE_NO_NEWCLU_FIRST` (新クラスタの中身を先に書く規則を外す) | D: 4 失敗 |
正しいビルドは全部 0 (クラスタ 4KiB / 512B)。

### 参照実装について (置き換えた点)
- Linux カーネルの exfat ドライバは PC のこの環境で使えなかった。参照には exfat-fuse 1.4.0 (relan) を使った。fusermount3 3.x が exfat-fuse の付ける `user=` / `blkdev` / `blksize=` を拒否するので、起動直前にそれらを取り除く `LD_PRELOAD` (`tools/exfat_fuse_shim.c`、テスト専用) を挟んでいる。
- exfat-fuse は **ValidDataLength の規則を守らない**: VDL より先への書き込みで、VDL と書き込み位置の間をゼロ埋めせず、ディスク上の古い値が見える。フィクスチャの `partial_valid.bin` の操作だけは参照との比較から外し、規則から計算した期待値 (`tools/exfat_vdl_expect.py`) と比べる。自前ドライバは規則どおりゼロで読める。
- 大文字小文字だけの rename、上書き rename、ディレクトリ rename は exfat-fuse でも同じ結果になった。

## 既知の限界・未実装
- **実機 (USB ディスク) では未確認**。ホスト (ASan/UBSan を含む 4 構成) と AVD のみ。
- 電源断の模擬の限界: 書き込みが途中で切れる単位は 512B のセクタ。実デバイスの挙動 (キャッシュの順序、書き込みの原子性、FUA/flush の尊重) は模擬しきれない。flush を無視する安価な USB ブリッジでは順序の保証が効かない。
- 複数セクタにまたがる更新 (セクタ境界をまたぐエントリセット、ルートの FAT チェーンの付け替え) の途中で電源が切れると、そのファイル 1 件が壊れうる。fsck が直す (そのファイルは消える/縮む)。exFAT の仕様上の弱点で、ここでは軽減していない。
- 確保系と解放系の混在: 「上書き rename で親ディレクトリも伸びる」場合は 1 回の同期に両方が入る。順序は確保系を優先する。まれな組み合わせで、電源断のあと孤児クラスタ (fsck が回収) 以上の害が出るかは未検証。
- 電源断のあと、未参照クラスタ (孤児) は `fsck.exfat` が回収するまで残る (容量が減って見える)。
- ディレクトリの作成・削除が多い場合の同期は、ディレクトリエントリの書き出しがまとまって 1 回の flush になる。1 操作ごとに同期する使い方 (SAF) は flush が増えて遅い。
- ビットマップの写し (`bm_snap`) の取得と比較が同期ごとに全ビットマップを走査する (数 TiB・小クラスタでは重い)。差分追跡への最適化は未実装。
- タイムスタンプの時差 (`time_offset`) は上流の既定のまま。
- `readlink` / `symlink` / `link`: exFAT に無い (`-95`)。
- UI (書き込み許可スイッチ) はパレント側の担当。JNI は `mount_auto` で rw 要求なら `ExfatVolume::mount_rw`。

## 結果 (2026-11-12)
- ホスト 4 構成 (`tools/run_host_tests.sh`): plain 25/25、san (ASan+UBSan) 25/25、wsan (書き込み有効+ASan+UBSan) 33/33、wrel (書き込み有効リリース) 33/33。既存テストは壊れていない (`test_exfat.cpp` の「rw を要求したら -EROFS」は、ext2 と同じく書き込み有効ビルドでは成功してもよいことに更新)。
- 電源断 (`run_exfat_crash_tests.sh`、40 サンプル x {prefix,cache} x {torn 0/1} x {最後に 1 回 sync / 操作ごとに sync} = 320 試行): クラスタ 4KiB は A/B/C/D とも 0。クラスタ 512B も 0 (torn=1 でルートの FAT チェーン付け替えが切れる例を許容)。
- AVD (x86_64, emulator-5556): `ExfatWriteTest` 2 件 OK (操作列 + VolumeDirty イメージが読み取り専用に落ちる)。結果イメージは `fsck.exfat -n` clean、VolumeDirty なし、exfat-fuse で同じ操作列を実行した結果と 434 エントリが一致 (partial_valid.bin は VDL の規則の期待値と一致)。
- `usbmem_exfat_write_tests` の乱数モデル: クラスタ 512B / 4KiB / 32KiB のシード各 2 以上 x 1200 操作、不一致 0、予期しない結果 0。
