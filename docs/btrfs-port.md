# Btrfs の読み取り専用対応

2026-11-12。**単一デバイスの Btrfs を、読み取り専用で**マウントできるようにした。ホスト (4 構成の ctest) と Android (AVD の instrumented test) で
「マウント → 一覧 → stat → 読み取り → シンボリックリンク → サブボリューム」まで動かした。実機 (F-51A + USB) では未確認。

## 先に結論: カーネルの fs/btrfs は載せていない (他の FS と違う点)
squashfs / erofs / ext4 などは Linux の `fs/*` を無改変で LinuxCompat に載せた。Btrfs は**載せず、ディスク上の形式を直接読む自前のリーダー**にした。理由:
- `fs/btrfs` は約 16 万行 (`inode.c` 1 万行、`volumes.c` 9 千行、`tree-log.c` / `send.c` / `relocation.c` / `qgroup.c` …)。トランザクション、delayed ref、
  extent_io (folio ごとの状態)、bio チェーン (チェックサム検証を完了コールバックで行う)、ワークキュー、kthread、ブロックグループ、予約領域 (space_info) などに強く結びつき、
  「読み取りに必要な部分だけ」を切り出しても、`open_ctree` が全部を初期化する。互換層に数百の API を足す作業になり、読み取り専用でも実効が薄い。
- 読み取りに必要な形式は小さい: スーパーブロック → chunk (論理 → 物理) → root tree → fs tree の B 木 → inode / dir item / file extent。
  自前で約 1000 行 (`core/compat/src/btrfs_volume.cpp`)。ディスク上の値を一切信用しない書き方 (範囲・桁あふれ・ループの検査) を最初から入れられる。
- 展開だけは既存のカーネルの展開ライブラリ (`third_party/kernel-lib`: `lib/zlib_inflate`・`lib/lzo`・`lib/zstd`、無改変) を使う (`btrfs_decomp.c` の薄いラッパ)。
- 副作用: 書き込み・tree-log 再生・修復の経路が**コードとして存在しない**。読み取り専用であることが構造で保証される。

## 対応範囲
- **デバイス**: 単一デバイス。複数デバイス (`num_devices != 1`)・RAID0 / RAID10 / RAID5 / RAID6 は `EOPNOTSUPP(95)` で拒否。プロファイルは single / DUP が読める
  (DUP / RAID1 系の別コピーは、チェックサム不一致のときのフォールバックにも使う)。
- **チェックサム**: crc32c / xxhash64 / sha256 / blake2b-256。**スーパーブロックとメタデータ (全ノード) は常に検証**し、不一致は別コピー (DUP) を試して、無ければ `EIO`。
  **データも既定で検証** (csum ツリーを引く。NODATASUM / NODATACOW のファイルと csum の無いセクタは検証しない)。不一致は `EIO` (壊れたデータを黙って返さない)。`verify_data=false` で止められる。
- **圧縮**: zlib / lzo (btrfs 独自のセクタ区切り形式を解釈) / zstd。inline extent の圧縮も同じ。展開後の上限は 2 MiB (カーネルは 128 KiB)。
- **ノード / セクタ**: ノードサイズ 4 KiB〜64 KiB、セクタサイズ 512 B〜64 KiB (4 KiB 以外もテスト済み)。
- **extent**: inline / regular / prealloc / 穴 (NO_HOLES あり・なし)、疎ファイル、reflink で共有された extent (offset 付きの参照) は形式どおり読む。
- **サブボリューム**: 既定のサブボリューム (`default` の DIR_ITEM が指すもの。通常は ID 5) を root にする。サブボリューム (ネスト含む) は親から普通のディレクトリとして辿れる。
  `subvol_id` で別のサブボリュームを root にもできる。`list_subvolumes()` で ID・親・名前・読み取り専用フラグの一覧 (スナップショットも同じ扱い)。UI からの選択は未実装。
  ino は `(サブボリューム番号 << 48) | オブジェクト ID` の合成値 (オブジェクト ID が 2^48 以上のものは `EOVERFLOW`)。
- **特殊ファイル**: シンボリックリンク (`readlink`、`read` でもリンク先)、FIFO / ソケット / デバイスは一覧と stat だけ (read は `EINVAL`)。ハードリンクは同じ inode。
- **機能フラグ**: incompat は mixed_backref / default_subvol / mixed_groups / compress_lzo / compress_zstd / big_metadata / extended_iref / skinny_metadata / no_holes / metadata_uuid / raid1c34 / simple_quota を許可。
  RAID56 と未知のビット (zoned・extent_tree_v2・raid_stripe_tree など) は `EOPNOTSUPP`。compat_ro (free-space-tree・block-group-tree・verity) は読み取りに影響しないので許可。
- **スーパーブロック**: 主 (64 KiB) と 2 つのミラー (64 MiB・256 GiB) のうち、チェックサムが正しく**世代が最大**のものを使う。

## tree-log (未再生のログ)
- スーパーブロックの `log_root != 0` は「fsync した更新がメインの木にまだ反映されていない」状態。このアプリは**再生しない** (書き込みになるため)。
- 既定は `EUCLEAN(117)` で拒否。UI は ext3/ext4 のジャーナルと同じ確認ダイアログ (「読み取り専用で開く」) を出し、承諾されたときだけ `allow_noload` で開く。
  開くと `log_ignored()` が真になり、「読み取り専用 (ジャーナル未再生)」と表示する。直近の fsync 済みの更新が見えないことがある。**書き込みは一切しない**。
- 破損は `EIO(5)` で返す (`EUCLEAN` にしない: 確認ダイアログが無意味に出るのを避ける)。

## 構成
- `core/compat/public/usbmem/btrfs_volume.h` : `usbmem::BtrfsVolume` (`FsVolume` 実装)、`BtrfsMountOptions {allow_noload, subvol_id, verify_data}`、`BtrfsSubvolume`。
- `core/compat/src/btrfs_volume.cpp` : リーダー本体。全 API は 1 つの mutex で直列化し、例外は外へ出さない。ノードは検証してから LRU 的にキャッシュ (予算 8 MiB)。展開済み extent は 4 つだけキャッシュ。
- `core/compat/src/btrfs_decomp.c` : zlib / lzo1x / zstd の展開 (カーネルの lib、GPL-2.0)。これを通して `usbmem_kdecomp` にリンクするので、`usbmem_btrfs` をリンクする JNI は GPL 扱い (他の FS と同じ。docs/licenses.md)。
- `core/compat/btrfs.cmake` : ターゲットとテストの登録 (`zfs.cmake` を触らない)。
- JNI: `jni_ntfs.cpp` の `mount_auto` に `FsType::Btrfs` の分岐 (rw を要求されても読み取り専用)。Kotlin: `NtfsVolume.fsName == "btrfs"`、`journalIgnored`、UI のボタン (読み取り専用でマウント)、`LinkResolver` の対象。
- 判別は `fs_probe` (オフセット 0x10040 の `_BHRfS_M`) が既にある。

## 検証
- **イメージの生成** (`tools/gen_btrfs_fixture.py`): `mkfs.btrfs --rootdir` で、チェックサム 4 種 × 圧縮 (zlib・zlib:9・lzo・zstd・zstd:15) × ノードサイズ (4K・8K・16K・32K・64K) × セクタサイズ (4K・8K・16K・64K) ×
  機能 (no-holes・holes・extref・block-group-tree・free-space-tree なし・mixed) × プロファイル (single・dup) の 30 イメージ弱、サブボリューム 2 種、NODATASUM / NODATACOW、複数デバイス (拒否用)。
  ツリーは ext2/squashfs/erofs と同じ (1000 ファイルのディレクトリ・疎ファイル・日本語名・255 バイト名・ハードリンク・リンク・FIFO・ソケット)。
- **独立した参照との比較** (リーダー自身の manifest ではない):
  - 生成時に全イメージを `btrfs check --readonly` で検査し、`btrfs restore` で展開し直した結果が元ツリーとバイト一致することを確認。
  - テスト `btrfs_restore_xcheck`: リーダー (`usbmem_btrfs_tests tree`) の出力 (全ファイルの SHA-256・サイズ・リンク先・ディレクトリ) を、`btrfs restore` の展開結果と全イメージで突き合わせる (23 イメージ・各 1062 項目が一致)。
  - 例外: セクタサイズが CPU のページサイズと違うイメージは `btrfs restore` (btrfs-progs 7.1) が abort するので、`btrfs check` と manifest 照合のみ。
- **テスト** (`core/compat/tests/test_btrfs.cpp`、`ctest -R btrfs`):
  - `btrfs_read`: manifest (サイズ・mtime・nlink・内容・リンク・特殊ファイル・ディレクトリの全項目)、lookup / readdir の整合、不正な ino、書き込みの拒否 (デバイスへの write / flush が 0 回)、
    サブボリューム (トップレベル / 既定 / ネスト / 読み取り専用フラグ / 存在しない ID)、NODATASUM と csum 付きファイルで**データを 1 バイト壊した**ときの挙動 (csum あり → `EIO`、なし → そのまま返る)、
    複数デバイスの拒否、tree-log の `EUCLEAN` と `allow_noload`、未知の incompat の拒否、スーパーブロックのミラーへのフォールバック。
  - `btrfs_corrupt`: スーパーブロックの全フィールド × 数種の値 (チェックサムは直す) で、クラッシュせず失敗するか読めること (3456 通り)。sys_chunk_array の破損 400 通り。
  - `btrfs_ioerr`: N 回目の読み取りから EIO にして、マウントと走査がクラッシュ・ハングしないこと (3 イメージ)。
  - `btrfs_fuzz`: メタデータのノードを探して数バイト変え、**チェックサムを直して** (検査の中身に届かせて) 走査する (12 イメージ × 150 回)。
  - 4 構成 (plain / san / wsan / wrel) で実行。
- **AVD** (`BtrfsInstrumentedTest`、`tools/run_android_btrfs_test.sh`): 16 イメージ・18 テストが通った。イメージは gzip で送って端末で展開する (疎で 100 MB 前後あるため)。

## 制限・未了
- 複数デバイス・RAID0/1/10/5/6・シード / シードされたデバイス・zoned・extent_tree_v2・raid_stripe_tree・暗号化 extent は読めない (理由付きで拒否)。
- tree-log は再生しない (確認のうえ無視して開くだけ)。スナップショット・サブボリュームの UI 選択は未実装 (API はある)。
- xattr・ACL・暗号化 (fscrypt)・fs-verity は読まない (`FsVolume` に xattr の口が無い)。ファイルの「圧縮済みか」などの属性も出さない。
- 大きなファイルの先頭への `read` は、1 回ごとに B 木を引く (キャッシュはノードのみ)。大きな連続読み出しの速度は未測定。
- 4Kn のデバイス上の Btrfs・ARM 実機での速度・実機の USB での動作は未確認。reflink で共有された extent の実イメージは、tmpfs では `mkfs.btrfs --reflink` が使えず作れなかった (形式は読むが実イメージでは未検証)。
- 書き込みは永久に別問題 (今回の対象外)。
