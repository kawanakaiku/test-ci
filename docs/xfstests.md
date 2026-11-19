# xfstests (fstests) で自前実装を試す

自前実装の FS (XFS / Btrfs / F2FS / NTFS と、LinuxCompat 経由の ext4 / ext2 / FAT / exFAT) を、xfstests (fstests) の `generic/` で試す仕組みと、その結果の記録。

xfstests はカーネルがマウントした FS を VFS 越しに試す。自前実装は `core` の `FsVolume` API (inode 番号ベースの lookup / readdir / read / write / create / unlink / rename ...) で動くので、そのままは試せない。
そこで `core` の API を FUSE (libfuse3 の低水準 API) で見せるホスト用アダプター **`usbmem_fuse`** を作り、`FSTYP=fuse` + `FUSE_SUBTYP=.usbmem` で fstests を動かす。
`usbmem_fuse` は PC で自前実装をマウントして中身を見る / 書く道具としても使える (下の「PC でのマウント」)。

関連ファイル:

| ファイル | 役割 |
|---|---|
| `tools/fuse/usbmem_fuse.cpp` | アダプター本体 (libfuse3 低水準 API、単一スレッド) |
| `core/fuse.cmake` | ビルド定義 (`USBMEM_BUILD_FUSE=ON`、既定は OFF) |
| `tools/fuse/mount.fuse.usbmem` | `mount -t fuse.usbmem` 用のヘルパー (`/sbin/mount.fuse.usbmem` に置く) |
| `tools/fuse/run_xfstests.py` | fstests を 1 件ずつ実行して結果を集めるドライバー (mkfs / local.config / 分類 / 要約) |
| `tools/fuse/xfstests_expected.json` | 期待失敗リスト (分類つき) |
| `tools/fuse/fsck_ntfs.sh` | NTFS のアンマウント後の検査 (`ntfsfix -n` + `ntfs-3g.probe` + 自前の fscheck) |
| `tools/fuse/smoke.sh` | ビルド直後の簡単な動作確認 |
| `.github/workflows/xfstests.yml` | GitHub Actions (ブランチ名で FS とテストの範囲を決める) |

## 使い方

### PC でのマウント

```sh
cmake -S core -B build -DUSBMEM_BUILD_FUSE=ON -DUSBMEM_NTFS=ON -DUSBMEM_NTFS_WRITE=ON   # 書き込みには NTFS_WRITE=ON が要る
cmake --build build --target usbmem_fuse
build/usbmem_fuse -o fstype=ext4,ro IMAGE MNT          # 読み取り専用
build/usbmem_fuse -o fstype=xfs,quiet_setattr IMAGE MNT # 書き込み (core が書き込み不可と判断したら、黙って ro にせずマウントを失敗させる)
fusermount3 -u MNT
```

`-o` のオプション:

| オプション | 意味 |
|---|---|
| `fstype=NAME` | ext4 ext2 ext3 fat vfat exfat ntfs xfs btrfs f2fs iso9660 udf squashfs erofs。既定 `auto` (probe) |
| `ro` / `rw` | 既定 `rw`。`rw` を要求して core が書き込み不可 (`writable()==false`) と判断したらマウントを失敗させる |
| `quiet_setattr` | chmod / chown / utimes を黙って成功させる (値は変わらない近似)。FAT の `quiet` マウントと同じ。既定は `EOPNOTSUPP` |
| `uid=N` / `gid=N` | stat に見せる所有者 (既定 = 起動したユーザー) |
| `sync` / `dirsync` | 変更のたびに core の `sync()` を呼ぶ |
| `nosync_auto` | 既定の自動同期 (512 操作または 32 MiB ごと) を止める |
| それ以外 | libfuse に渡す (`allow_other` / `default_permissions` など) |

環境変数: `USBMEM_FUSE_TRACE=1` (全操作を `[pid 秒.ミリ秒]` つきで stderr に出す)、`USBMEM_FUSE_LOG` (出力先)、`USBMEM_FUSE_FSCK="コマンド"` (アンマウントでボリュームを閉じた後に `sh -c コマンド sh IMAGE` を実行し、失敗したら `USBMEM_FUSE_FSCK_LOG` に `FSCK-FAIL IMAGE` を書く。fstests は FUSE の整合性検査を持たないので、その代わり)。

### ローカルで fstests を動かす (root)

```sh
git clone --depth 1 --branch v2026.11.10 https://git.kernel.org/pub/scm/fs/xfs/xfstests-dev.git fstests && make -C fstests -j
sudo python3 tools/fuse/run_xfstests.py --fs ext4 --fstests ./fstests --out /tmp/out --group quick     # または --tests generic/001,generic/002
```

ドライバーは FS ごとに mkfs したイメージ (TEST / SCRATCH) を作り、`/sbin/mount.fuse.usbmem` と `/etc/usbmem_fuse.env`、fstests の `local.config` を置いて、テストを **1 件ずつ** `./check` で実行する (アダプターがハング・クラッシュしても全体が止まらないように、タイムアウトつき)。
結果は `out/results.json` と `out/summary.md`。イメージは既定で 1 GiB (FAT / exFAT は疎ファイルが無いので 6 GiB)。

### GitHub Actions

`.github/workflows/xfstests.yml`。`usbmem-xfstests*` ブランチへの push で動く (`usbmem-ci*` は重い Host tests / emulator が動くので使わない)。ブランチ名で範囲を決める:

| ブランチ | 動くもの |
|---|---|
| `usbmem-xfstests-<fs>` | その FS の quick グループ全部 (fs = ext4 ext2 fat exfat ntfs xfs btrfs f2fs) |
| `usbmem-xfstests-<fs>-smoke` | 14 件 (001 002 005 006 007 010 011 013 023 024 035 075 091 112) |
| `usbmem-xfstests-all[-smoke]` | 8 FS 全部 |
| `...-trace` | テスト 001 002 003 005 を `USBMEM_FUSE_TRACE=1` で |
| `...-t<番号>[_<番号>]` | 指定した generic/ のテストだけ、トレースつき (例 `usbmem-xfstests-xfs-t023_024`) |
| `...-noremkfs` | テストごとの mkfs をしない |
| `...-noloop` | TEST / SCRATCH をループデバイスにしない (既定は `--loop` = ループデバイス。下の「ブロックデバイス」) |
| `...-loopset` | `--loop` でだけ動くテスト (dm-flakey / dm-error / ブロックデバイス前提。約 60 件) だけ。例 `usbmem-xfstests-exfat-loopset` (5 分) |
| `...-strict` | `quiet_setattr` を付けない |
| `workflow_dispatch` | fs / group / tests / mount_opts / timeout / loop を入力 |

fstests は kernel.org が繋がらないことがあるので、3 回 × (kernel.org, googlesource) を各 120 秒で試し、だめなら GitHub の kdave ミラーの既定ブランチを使う (警告を出す)。

## アダプターの設計

### 正直に返す (対応しているふりをしない)

core に無い操作は、そのとおりのエラーで返す。近似するものは近似であると明記する。

| 操作 | 返し方 |
|---|---|
| chmod / chown / utimes (setattr の mode / uid / gid / atime / mtime) | 既定は `EOPNOTSUPP`。要求が「今の値と同じ」なら成功。`quiet_setattr` なら黙って成功 (値は変わらない) |
| xattr / ACL | 設定は `ENOSYS` / `EOPNOTSUPP`。一覧は空、取得は `ENODATA` (「どのファイルも xattr を持たない」と答える。登録しないと fssum のように一覧を読むツールが `EOPNOTSUPP` で失敗する: generic/547, 785) |
| fallocate / ioctl / `lseek(SEEK_DATA, SEEK_HOLE)` / copy_file_range / reflink | `ENOSYS` / `EOPNOTSUPP` |
| mknod (通常ファイル以外)、rename の `EXCHANGE` / `WHITEOUT` | `EOPNOTSUPP` |
| statfs | core に空き容量の API が無い。総容量 = イメージの大きさ、空き = 総容量の 95% からツリーのファイル大きさの合計を引いた見積もり (疎ファイル・メタデータは見ない近似) |
| 所有者・時刻 | core は mode / nlink / size / mtime だけ返す。uid / gid は固定、atime / ctime は mtime と同じ |

### アダプター側の補完 (core の機能ではない)

* **開いたまま unlink / rename で上書きされたファイル**: 最後に閉じるまで隠し名 `.usbmem_hidden_N` で残し、最後の release で unlink する (nlink は 0 で見せる)。libfuse の `hard_remove` 無しと同じ。
* **nodeid と実 inode 番号の分離**: FUSE の nodeid は core の inode 番号とは別に振る。core の番号は消した後に別のオブジェクトへ再利用される (FAT / exFAT はディレクトリエントリの位置、ext / XFS / Btrfs / F2FS は解放した番号) ので、同じ nodeid を使い回すと、カーネルが持つ古い inode (誰かの cwd や開いたファイル) と型が食い違って `EISDIR` / `ELOOP` / `EIO` になる。消えたオブジェクトの対応を切り、同じ実 inode 番号が次に現れたら新しい nodeid を振る。`forget` / `batch_forget` で参照数を数えて掃除する。
* **消されたディレクトリが cwd のまま**: `rmdir` された後も、誰かの cwd や開いたディレクトリへの `getattr` / `chdir("..")` が通る (Linux の通常の FS と同じ)。`default_permissions` を付けるとカーネルが権限検査のため `getattr` を投げるので、最後の属性を nlink 0 で返す (generic/011 が CI でだけ落ちた原因)。
* **マウントの直列化 (flock)**: fstests は umount の直後に再 mount する。FUSE の `umount(2)` はデーモンの後始末 (sync / クローズ / fsck) が終わる前に戻るので、次の mount と古いデーモンが競合する (ext2 が "opened read-only" になった)。イメージを `flock` (排他、ro は共有) で待ち、fsck が終わるまで保持する。ドライバーも各テストの後でデーモンの終了を待つ。
* **自動同期**: 512 操作または 32 MiB の書き込みごとに core の `sync()`。

### 単一スレッド

core の `FsVolume` はスレッドセーフではないので、libfuse の単一スレッドのループで動かす (`-s` 相当)。fsstress の並列度が高くても、操作はデーモン内で直列になる。性能の試験ではなく意味論の試験。

## 結果

最終の CI: **run 43665002548** (`usbmem-xfstests-all`、8 FS の quick グループ 652 件、所要 32 分)。`generic/` と `shared/` のうち `_begin_fstest` に `quick` を持つもの。
ext4 だけ `-strict` (`quiet_setattr` なし) の run 43665047439 も取った。途中の run (43659182212、43662498331) の失敗は、下の「見つけた不具合」で直したもの。

| FS | pass | fail | hang | notrun | 後始末の fsck の失敗 |
|---|---|---|---|---|---|
| ext4 | 92 | 15 | 0 | 545 | 0 |
| ext2 | 92 | 15 | 0 | 545 | 0 |
| FAT (FAT32) | 81 | 16 | 2 | 553 | 2 (hang で殺した 2 件のみ) |
| exFAT | 84 | 15 | 0 | 553 | 0 |
| NTFS (書き込みビルド) | 85 | 13 | 0 | 554 | (検査なし: ntfsfix 相当が無い) |
| XFS (Full) | 92 | 15 | 0 | 545 | 0 |
| Btrfs | 92 | 15 | 0 | 545 | 0 |
| F2FS | 92 | 15 | 0 | 545 | 0 |

失敗はすべて期待失敗リスト (`tools/fuse/xfstests_expected.json`) に載っていて、リストにない失敗 (unexpected) は 0、リストにあるのに通ったもの (now pass) も 0。
fsck は各テストの後 (TEST と SCRATCH の両方) に独立ツール (e2fsck / fsck.fat / fsck.exfat / xfs_repair / btrfs check / fsck.f2fs) で調べていて、pass したテストも含めて壊れたイメージは残らなかった。
(NTFS は検査ツールを動かしていない。結果は `--tests` を絞らない quick グループの範囲で、fsstress を使う generic/013 などを含む。)

### notrun が多い理由

652 件中 545〜554 件が notrun。ほとんどは fstests が `FSTYP=fuse` に対して機能が無いと判断したため (ブロックデバイスが要る・reflink・xattr・chmod・falloc など)。理由の分類 (`results.json` の `notrun_cat`、summary.md に理由ごとの件数):

| 分類 | 意味 | ext4 の件数 |
|---|---|---|
| `saf_unusable` | SAF (Android のストレージアクセスフレームワーク) 越しには使えない / FUSE ではカーネルに任せられない操作 (reflink / dedupe / fallocate / punch / mknod / swapfile / O_TMPFILE / exchangerange ...) | 251 |
| `core_unsupported` | core が持たない機能 (chmod / chown / xattr / ACL / 暗号 / クォータ / chattr / 疎ファイルの探索 / 時刻の細部 ...) | 121 |
| `environment` | fstests の環境の都合 (最大の理由は「SCRATCH が valid block disk でない」75 件、shutdown / freeze / dax / logwrites / ツールが無い ...)。自前実装と関係ない | 170 |
| `other` | 上のどれにも当てはまらないもの (正規表現が粗い。FAT / exFAT / NTFS は 45 件でほぼ同じ種類の理由) | 3 |

つまり「pass 92 件」は、ブロックデバイスや特殊機能を要求しない、`generic/` の基本的な試験 (create / read / write / rename / unlink / truncate / mkdir / readdir / 巨大ディレクトリ / fsstress / 並行 append / O_TRUNC / mmap など) の範囲。notrun の `environment` を減らす (ループデバイスを使えば `valid block disk` の 75 件は動く) のは未了。

### 期待失敗リスト

`tools/fuse/xfstests_expected.json` (全 FS 共通 `*` と FS ごとのキー)。分類は次の 6 つ。**「SAF から使えない操作」と「core が持たない機能」は分けてある**。

**SAF から使えない操作 (saf_unusable)**: 失敗ではなく notrun として現れる (fstests が機能の有無を先に調べるため)。reflink / dedupe / fallocate / punch / zero-range / collapse / insert / exchangerange / mknod / mkfifo / swapfile / O_TMPFILE。symlink / hardlink は FS によって core にはあるが SAF からは作れない (NTFS は fstests が "No hardlink support" / "Require symlinks support" で notrun にした)。失敗リストには入らない。

**core が持たない機能 (core_unsupported)**:

| テスト | FS | 理由 |
|---|---|---|
| generic/003 | 全部 | core は atime / ctime を返さない (mtime と同じ値)。atime の更新規則 (relatime) を調べる |
| generic/120 | 全部 | atime の更新 (noatime / relatime) |
| generic/221 | 全部 | ctime / mtime の更新規則 |
| generic/236 | ext4 ext2 xfs btrfs f2fs | link による ctime の更新 |
| generic/755 | 同上 | unlink で対象の ctime が変わること |
| generic/258 | 全部 | 負の時刻 (1970 年より前) の mtime を保持しない |
| generic/087 | 全部 | chmod / setuid / setgid (core に権限の変更 API が無い。`-strict` なしでも "quiet_setattr" は値を変えない近似) |
| generic/088 | FAT / exFAT | 実行ビットで access(X_OK) が変わる前提 (権限ビットを持たない FS) |
| generic/490 | exFAT | `lseek(SEEK_DATA / SEEK_HOLE)` (アダプターは全体をデータとして返す) |

**アダプターでは実現できない (adapter_limit)**:

| テスト | FS | 理由 |
|---|---|---|
| generic/426, 467, 477, 756, 777 | 全部 | `open_by_handle_at`: FUSE の nodeid はアダプターが振る (永続しない) ので、再マウントをまたぐ / キャッシュから消えたハンドルは ESTALE (116)。core にも inode 番号から開く手段が無い |

**fstests の前提とこの環境・FS の性質が合わない (premise_mismatch)**:

| テスト | FS | 理由 |
|---|---|---|
| generic/732 | 全部 | 同じイメージを 2 か所へマウントして同じスーパーブロックを共有させる前提。アダプターは同じイメージを二重に開かない (flock で 120 秒待って失敗する) |
| generic/634 | 全部 | `touch` が `Dec 31 23:59:59 UTC 2147483647` を解釈できない (この環境の coreutils)。FS と無関係 |
| generic/798 | 全部 | ページキャッシュの Dirty 数を見る。FUSE は書き込みをすぐデーモンへ送るので dirty ページが残らない |
| generic/309, 313 | FAT | FAT の時刻の粒度は 2 秒。1 秒以内の mtime / ctime の更新を見る |
| generic/346, 394 | FAT | 疎ファイルが無いので、巨大な穴 / 1 GiB の truncate がゼロ埋めの書き込みになり、300 秒の制限で hang 扱いになる (殺した後の fsck は 2 件失敗と数えられる) |

### 見つけた不具合 (すべて修正済み・再現と回帰試験つき)

**アダプターの不具合**

1. **O_TRUNC の open で切り詰めていなかった** (generic/647, 729 で発見): libfuse3 は `FUSE_CAP_ATOMIC_O_TRUNC` を既定で有効にするので、カーネルは setattr(size=0) を送らず、`open(O_TRUNC)` で切り詰める責任はデーモンにある。`> file` で既存の内容が残っていた。`smoke.sh` に確認を追加。
2. **nodeid を実 inode 番号と同一にしていた** (fsstress など): FAT / exFAT の inode 番号はディレクトリエントリの位置、ext / XFS などは解放後に再利用される。消えたオブジェクトの nodeid が別物に再利用され、カーネルが持つ古い inode と型が食い違って EISDIR / ELOOP / EIO になった。nodeid を別に振り、`forget` で掃除する設計にした。
3. **消されたディレクトリが cwd のとき `getattr` が ENOENT** (generic/011。CI でだけ落ちた): `default_permissions` ではカーネルが権限検査のために `getattr` を投げる。最後の属性を nlink 0 で返す。
4. **unmount 直後の再 mount との競合**: `umount(2)` はデーモンの後始末の完了前に戻るので、次の mount と競合した (ext2 の "opened read-only"、ext4 のジャーナル)。イメージの flock と、ドライバーでのデーモン終了待ち。
5. **remount ro (generic/452)**: ヘルパーが新しいデーモンを起動しようとして flock で 120 秒待っていた。`mount -i -o remount,ro` でカーネル側の ro / rw だけ切り替える。
6. **fsck が "canceled"**: fstests はテストの後に systemd のスコープを止める (スコープ内の全プロセスへ SIGTERM)。デーモンの後始末として走らせた `e2fsck` などがそれで殺されていた (ブロックしたシグナルのマスクはシェルを経由すると引き継がれない)。fsck はドライバーが各テストの後に走らせる方式にした (デーモンの `USBMEM_FUSE_FSCK` は PC 用として残してある)。
7. 1 件の上限を 180 秒から 300 秒に (generic/069 が 165〜180 秒かかり、境目で hang 扱いになった)。

**core の不具合 (最小限の修正 + 回帰試験)**

1. **exFAT: NO_FAT_CHAIN のディレクトリの探索で、ヒントを使うとクラスタ数が合わない** (generic/006): `exfat_find_dir_entry` で、ヒント位置から始めるとき `clu.size` が元のディレクトリ全体のままで、ディレクトリの終端を越えて読んだ (EIO)。`third_party/exfat/fs-exfat/dir.c` に最小の修正 (コメントは "usbmem:" から始まる)。
2. **exFAT: 同じ関数の「巻き戻し」で `clu.size` を戻していなかった** (generic/006): 先頭に戻るとき同上。回帰試験 `test_exfat_write --dirfull` (名前 384 / 4096 個の作成・unlink・rmdir) を `tools/run_exfat_write_tests.sh` に追加。
3. **XFS: 空き領域の B+木 (bnobt / cntbt) の削除で、葉の最小レコード数を割る** (generic/013 の後の xfs_repair): 兄弟との併合・再分配 (`bt_rebalance`) を追加。回帰試験 `test_xfs_write --frag` (6000 ファイルの作成と無作為な削除) を `tools/run_xfs_write_tests.sh` に追加。
4. **XFS: ショートフォームのディレクトリでエントリのオフセットが増え続ける** (generic/028 の後の xfs_repair "directory offsets too high"): 名前の変更を繰り返すと、追加位置が常に「最後のエントリの後ろ」でオフセットが伸びた。カーネルの `xfs_dir2_sf_addname_pick` と同じ「入る最も低い隙間」にし、エントリをオフセット順に並べる。`--frag` に 3000 回の rename を追加 (修正前は xfs_repair -n が失敗することを確認)。

### 使い方の注意・制限

* fstests は `FSTYP=fuse` では mkfs も fsck もしないので、ドライバーがテストごとに mkfs し直し (`--no-remkfs` で止められる)、テストの後に fsck を走らせる。前のテストの壊れ方が次へ持ち越されないようにするため。
* 総容量 / 空き容量の statfs は見積もり (core に API が無い)。空き領域を見るテスト (`_scratch_mkfs_sized`、ENOSPC を狙うもの) は notrun か不正確。
* 単一スレッド。性能試験ではなく意味論の試験。FAT / exFAT の疎ファイル無しで時間がかかるテストがある。
* イメージは 1 GiB (FAT / exFAT は fsstress が埋めるので 6 GiB)。
* `-strict` (quiet_setattr なし) の ext4 (run 43665047439): pass 90 / fail 15 / notrun 547。generic/087 が notrun (chmod 不可) に変わり、generic/005 が失敗 (symlink への utimes が EOPNOTSUPP で、ELOOP が出ない)。それ以外は同じ。

## 未了・注意

* **`--no-remkfs` での ext4 の再確認**: 未実施 (テストごとの mkfs を止めたとき、前のテストの影響でどう崩れるか)。
* **notrun の `environment` 75 件** (`valid block disk`): ループデバイスをテスト用に用意すれば動くものがあるはずだが、`FSTYP=fuse` では `_require_block_dev` が通らない。未了。
* **notrun の分類の正規表現は粗い**: `other` が FAT / exFAT / NTFS で 45 件ある。
* **時刻・権限**: core が atime / ctime / 負の時刻 / 権限を持たない限り、generic/003・087・120・221・236・258・755 は落ちたまま (期待失敗)。core に API を足すのは別の話 (docs/TODO.md)。
* **ファイルハンドル (open_by_handle_at)**: nodeid を永続にする (inode 番号 + 世代を nodeid にする) 設計に変えない限り通らない。FS ごとに inode 番号の安定性が違う (FAT / exFAT はエントリ位置) ので、通すなら FS ごとの確認が要る。
* **同じイメージの二重マウント** (generic/732): 現状は拒否 (排他ロック)。core が複数のボリュームを同じデバイス上で共有する設計ではない。
* **FAT の generic/346, 394**: 1 GiB のゼロ埋めに 300 秒では足りない。`--timeout` を伸ばせば通るかもしれないが、試していない。
* **btrfs / f2fs / ntfs / xfs の「pass 92」は ext4 と同じ集合**: fstests が FUSE に対して notrun にするので、各 FS の固有の機能 (btrfs のスナップショット、f2fs の gc など) は試されない。FS 固有の試験は、既存の `tools/run_*_write_tests.sh` が担う。
