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

### 最新 (ループデバイス = `--loop`、2026-11-19)

CI: **run 44461561567** (`usbmem-xfstests-all`、8 FS の quick グループ 652 件。既定が `--loop` になった workflow)。`generic/` と `shared/` のうち `_begin_fstest` に `quick` を持つもの。
同じコードで `-noloop` (イメージファイルをそのまま使う従来の動き) の run 44461564366 も取った (下の比較)。ext2 と FAT だけは期待リストを直した後に `-loopset` (5 分) で再確認した (run 44466208528 = ext2、44466211424 = FAT)。

| FS | pass | fail | hang | notrun | 期待外の失敗 | 後始末の fsck の失敗 (期待外) |
|---|---|---|---|---|---|---|
| ext4 | 144 | 24 | 0 | 484 | 0 | 0 |
| ext2 | 100 | 68 | 0 | 484 | 0 (再確認 run) | 0 |
| FAT (FAT32) | 108 | 27 | 2 | 515 | 0 (785 は期待リストへ) | 2 (hang で殺した 346 / 394 のみ。hang は fsck の失敗に数えないようにした) |
| exFAT | 91 | 46 | 0 | 515 | 0 | 0 |
| NTFS (書き込みビルド) | 113 | 22 | 0 | 517 | 0 | 0 (**検査あり**: 下の「NTFS の fsck」) |
| XFS (Full) | 144 | 24 | 0 | 484 | 0 | 1 (generic/411。下の「XFS の 1024 文字の symlink」。原因を直した。`fsck_ignore` は外した。確認の run は同節) |
| Btrfs | 142 | 26 | 0 | 484 | 0 | 0 |
| F2FS | 144 | 24 | 0 | 484 | 0 | 0 |

XFS・FAT・Btrfs・F2FS・exFAT の数字は、ランナーが落ちなかった run 44469300735 (同じ内容の再実行。`usbmem-xfstests-all`) のもの。run 44461561567 では XFS のジョブが「The hosted runner lost communication with the server」で落ち、44469300735 では ext4 / ext2 / NTFS が同じ理由で落ちた (**特定のテスト・FS に限らない**。ジョブのログもアーティファクトも残らない)。**原因は未確定** (ループを使わない `-noloop` の run 44461564366 でも F2FS のジョブが同じ理由で落ちたので、`--loop` / dm だけのせいとは言えない)。調べるために、ブランチ名の末尾に `-progress` を付けると 30 秒ごとに実行中のテスト名・メモリ・dmesg・dm の状態を `ci-progress` ブランチへ書く (`tools/ci/push_progress.sh`) ようにしたが、これを付けた run 44474332688 でも XFS・FAT・Btrfs が落ち、メモリは 10 GB 以上空き・dmesg に異常なしのまま止まっていて (書き込みも途中で止まる。sha の取り直しを直す前だった)、手がかりは得られていない。アップロード前に FUSE・dm・ループを外す step も足した (run 44480161924 は完走)。1 つの全体の run で 1〜3 ジョブが落ちる頻度なので、**結果は落ちなかったジョブの run から拾った**。ext4 / ext2 / NTFS の数字は 44474332688 のもの (期待外 0)。FAT は 44469300735 (hang 2 件を含む)。
(「期待外」は `xfstests_expected.json` に載っていない失敗。FAT の 785 と ext2 の 44 件は、この run で初めて動いたテストで、原因を調べて分類つきで期待リストに入れた。)
`--loop` にする前 (run 43747052912) は pass 92 (NTFS 85)・notrun 545 だった。**ブロックデバイス前提 / dm-flakey / dm-error のテストが動くようになり、pass が ext4 / F2FS で +52、NTFS で +28 になった。** 新しく動いたテストの中身は下の「ブロックデバイス (`--loop`)」。

fsck は各テストの後 (TEST と SCRATCH の両方) に独立ツール (e2fsck / fsck.fat / fsck.exfat / xfs_repair / btrfs check / fsck.f2fs。NTFS は `fsck_ntfs.sh`) で調べていて、pass したテストも含めて壊れたイメージは残らなかった。
NTFS は 1304 回 (652 件 × TEST / SCRATCH) の検査のうち、失敗したのは「テストが SCRATCH の中身を壊す」generic/108・740 の 2 件 (期待リストの `fsck_ignore`) だけで、3 つの検査 (下記) が一致して「NTFS ではない」と報告した。

### XFS の 1024 文字の symlink (原因を特定して修正、2026-11-19)

全体の run (44469300735) で一度だけ、generic/411 の後の SCRATCH を `xfs_repair -n` が「symlink in inode N too long (1024 chars)」と報告して fsck が失敗した。411 自身は symlink を作らない (二重マウントで失敗するテスト) ので、SCRATCH に残っていた直前までのテスト (fsstress の symlink) の産物で、単独 (`-t411`、run 44480161924) では出ない。

**原因**: xfs_repair (xfsprogs 7.2.0) は **1024 文字の symlink を不正とする** (許すのは 1023 文字まで)。手元で `mkfs.xfs -p` の protofile に 1023 / 1024 / 1025 文字の symlink を入れて試すと、1023 は clean、1024 は「symlink in inode N too long (1024 chars)」、1025 も同じ。mkfs.xfs 自身 (libxfs) も 1024 文字以上の symlink は「corrupt buffer」で書けない。一方 core の `ino_symlink_body` は `kMaxSymlink = 1024` まで許し、1024 文字を remote (XSLM) ブロックに書いていた。ヘッダは `kSymlinkHdr = 56`、NUL は書かない (サイズは文字数そのもの) ので、書き方自体は正しく、**上限の判定が 1 文字多かっただけ**。fsstress が 1024 文字の symlink を作ったと推測する (未確認。直前のテストの産物)。

**修正**: 作るときの上限を `kMaxSymlinkCreate = 1023` にした (超えると `ENAMETOOLONG`。`core/compat/src/xfs_volume.cpp` / `xfs_write_ops.inc`)。読む側 (`read_symlink`) の上限は 1024 のまま (他の実装が作ったものを読む寛容さは残す)。

**検査**: `usbmem_xfs_write_tests --symlinks` (`tools/run_xfs_write_tests.sh` の 4b。既定と isize=1024 の 2 構成)。1 .. 1023 文字 (inode 内 / 別ブロックの境目 335 / 336 / 337 / 847 / 848 / 849 を含む 12 通り) は作れて `readlink` / サイズが一致し再マウント後も読める、1024 / 1025 / 1500 / 4095 / 4096 / 5000 は `ENAMETOOLONG` で何も残らない、のあとに `xfs_repair -n` が clean でログが CLEAN。ctest `xfs_write` と `usbmem-ci-xfs` (`xfs-write.yml`) に入る。

**確認 (2026-11-19、コミット 7f268f5)**: `usbmem-ci-xfs` の xfs-write.yml (run 44483373110、xfs_repair 76 回・カーネル相互確認 30 回、`symlinks default` / `symlinks isize1024` とも 12 作成・6 拒否で ok、all passed、突然変異 7/7 も成功)、同じブランチの Host tests (run 44483372958、plain / wrel 成功。ctest `xfs_write` / `xfs_crash` 通過)。xfstests (`usbmem-xfstests-xfs`、期待リストから 411 の `fsck_ignore` を外した版) は run 44483382774 の最初の試行がランナー切断 (「The hosted runner lost communication」、既知の件) で落ちたが、再試行で成功: 652 件中 pass=144 / fail=24 (全部期待どおり、期待外 0) / notrun=484、fsck の失敗は 108 (期待リストの `fsck_ignore`) のみで 411 の後の fsck は clean。ただし 411 の後の失敗は元々 1 回しか出ていないので、この run の成功だけでは再発しないことの証明にはならない (確かな根拠は 1024 文字が xfs_repair で不正になる手元の再現と上の境界検査)。

### 初回 (ループなし、2026-11-15)

run 43665002548 / 43747052912 (`usbmem-xfstests-all`、所要 32 分)。ext4 だけ `-strict` (`quiet_setattr` なし) の run 43665047439 も取った。途中の run (43659182212、43662498331) の失敗は、下の「見つけた不具合」で直したもの。

| FS | pass | fail | hang | notrun | 後始末の fsck の失敗 |
|---|---|---|---|---|---|
| ext4 | 92 | 15 | 0 | 545 | 0 |
| ext2 | 92 | 15 | 0 | 545 | 0 |
| FAT (FAT32) | 81 | 16 | 2 | 553 | 2 (hang で殺した 2 件のみ) |
| exFAT | 84 | 15 | 0 | 553 | 0 |
| NTFS (書き込みビルド) | 85 | 13 | 0 | 554 | (検査なし) |
| XFS (Full) | 92 | 15 | 0 | 545 | 0 |
| Btrfs | 92 | 15 | 0 | 545 | 0 |
| F2FS | 92 | 15 | 0 | 545 | 0 |

(この表の時点の NTFS は検査ツールを動かしていない。結果は `--tests` を絞らない quick グループの範囲で、fsstress を使う generic/013 などを含む。)

### NTFS の fsck

NTFS には e2fsck / xfs_repair のような独立した検査専用ツールが無い (`ntfsfix` は修復ツールで、`-n` でも見る範囲が狭い)。そこで `tools/fuse/fsck_ntfs.sh` で、アンマウント後の TEST / SCRATCH に次の 3 つを読み取り専用で当て、どれかが失敗したら FSCK-FAIL にする (他の FS の `fsck.<fs>` と同じ枠。`run_xfstests.py` の `FS["ntfs"]["fsck"]`)。

| 検査 | 見るもの | 失敗にする条件 |
|---|---|---|
| `ntfsfix -n` | ブートセクタとバックアップ・`$MFT` / `$MFTMirr`・ジャーナル・ダーティ (独立実装の ntfs-3g) | 終了コード != 0 |
| `ntfs-3g.probe --readonly` | ntfs-3g が読み書きでマウントできる状態か | 終了コード != 0 |
| `usbmem_fscheck check IMG --fs ntfs` | 自前の整合性検査 (docs/disk-tools.md。MFT 全レコード・`$Bitmap` とクラスタ使用・親ディレクトリ・ダーティ・`$LogFile` など) | errors (2) / unsupported (3) / incomplete (4) / I/O 失敗 (10)。**warnings (1) は報告するが失敗にしない** (誤検出の恐れがあるものを fsck の失敗にしないため。ログに `NTFS-FSCK-WARN`) |

`usbmem_fscheck` は `usbmem_disktools_cli` と同じソース (`core/tests/disktools_cli.cpp`) を xfstests のビルド (`core/fuse.cmake`) でも作ったもの。
結果: 8 FS の run (44461561567) で NTFS の 1304 回の検査のうち、warnings は 0、errors は SCRATCH を壊すテスト 2 件 (generic/108 の LVM、generic/740 の他 FS への mkfs) だけで、**3 つの検査が一致**した (`usbmem_fscheck` は「boot sector の OEM ID が NTFS ではない」、`ntfsfix` は "Volume is corrupt"、`ntfs-3g.probe` は "NTFS signature is missing")。
つまり (a) 自前の fscheck は xfstests の全テスト後の NTFS で誤検出しなかった、(b) 壊れたイメージはきちんと 3 つとも検出する。限界: ディレクトリのインデックス (B+ ツリー) の中身などは自前の fscheck が見ない (docs/disk-tools.md)。ntfs-3g の `ntfsfix` / `probe` もそこまでは見ないので、NTFS のインデックスの整合性は検査されていない。

### ブロックデバイス (`--loop`)

fstests は SCRATCH_DEV / TEST_DEV が**ブロックデバイス**であることを前提にするテストが多い (`_require_block_device`)。`FSTYP=fuse` + イメージファイルのままだと、これらは "require ... to be valid block disk" で notrun になる (初回の ext4 で 75 件)。
`run_xfstests.py --loop` (workflow の既定) は、TEST / SCRATCH のイメージファイルに `losetup` でループデバイスを付け、`TEST_DEV=/dev/loopN` として fstests に渡す。アダプターはブロックデバイスをそのまま開ける (`RawImageDevice` は `lseek(SEEK_END)` でサイズを取る)。
これで新しく動いたもの (ext4 で 61 件: pass 52、期待リストの失敗 9):

| グループ | 件数 | 何を試すか |
|---|---|---|
| dm-flakey (`_require_dm_target flakey` + `_require_metadata_journaling`) | 約 45 (pass) | **電源断のシミュレーション**: fsync した内容が、書き込みを捨てて (`drop_writes`) 再マウントした後に残っているか (generic/034 039 040 041 056 057 065 073 090 101 104 106 107 321 322 325 335 336 341 342 343 348 376 480 481 498 502 510 526 527 534 538 547 552 557 640 690 741 764 771 779 782 784 785 789 790 ほか)。core の `sync()` / `flush()` が本当にデバイスまで届いていることの試験になる |
| dm-error | 3 | generic/338 (EIO を起こして mount / umount が固まらないか) は pass。441 / 484 は期待リスト (adapter_limit。下) |
| `_require_block_device` だけ | 2 | generic/067 (pass)、740 (他 FS の mkfs。通るが SCRATCH が壊れる = `fsck_ignore`)。108・563・704・731 は mkfs し直す前提で `premise_mismatch`。361 は疎ファイル、466 / 081 は `_scratch_mkfs_sized` で、別の理由の notrun に変わった |

仕組みの注意 (`run_xfstests.py` / `mount.fuse.usbmem` / アダプターに入れたもの):

* **`wipefs` の無効化**: SCRATCH がブロックデバイスだと、`check` がテストごとに `wipefs -a $SCRATCH_DEV` する。`FSTYP=fuse` では `_scratch_mkfs` が何もしない (作り直せない) ので、シグネチャが消えたまま SCRATCH のマウントが失敗する。ドライバーがテストごとに mkfs し直すので、`local.config` で `WIPEFS_PROG=/bin/true` にした。
* **mkfs し直し**: ループデバイスを付けたまま (ファイルを作り直すとループが古い inode を指す)、`blkdiscard` で穴を開けてからデバイスに mkfs する。fsck もループデバイスに当てる (ファイルに直接当てるとページキャッシュと食い違う)。
* **`fsname` は指定のまま**: `/dev/mapper/NAME` を `realpath` すると `/dev/dm-0` になり、fstests の `umount $SCRATCH_DEV` (= `/dev/mapper/NAME`) が FUSE のマウントと一致せず "not mounted" になって、デーモンが残り、次の mount が排他ロックで 120 秒待って失敗した。絶対パスで渡されたら、`fsname` はそのままにした。
* **`mount -t fuse` (サブタイプなし)**: 一部の fstests (dm-error 系、二重マウントを試すもの) は `FSTYP` をそのまま `mount -t` に渡すので、fuse3 の `/sbin/mount.fuse` が呼ばれ、デバイスをコマンドとして実行しようとして失敗する ("Permission denied" / "Unknown error 5005")。`--loop` のとき、`mount.fuse.usbmem` を `/sbin/mount.fuse` としても置く (元は `mount.fuse.orig` に退避)。このとき `fstype=` が無ければ既定のオプションを足す。すでにマウント中のデバイスをもう一度マウントしようとするもの (generic/409 410 411) は、120 秒待たずにすぐ失敗させる。
* **xattr の一覧**: 登録しないと `listxattr` が `EOPNOTSUPP` になり、fssum (generic/547, 785) が失敗する。「どのファイルも xattr を持たない」と答えるようにした (設定は従来どおり `EOPNOTSUPP`)。
* **CI の確認**: GitHub Actions の ubuntu-latest (kernel 7.0-azure) は `dm-flakey` / `dm-error` / `dm-snapshot` / `dm-log-writes` のモジュールを持ち、`losetup` / `blkdiscard` も使える (workflow の "Probe loop / device-mapper support" ステップで確認)。

**電源断の試験で見えたこと** (core の挙動。バグではなく方針): 書き込みを捨てた後の再マウントで、**exFAT (ダーティフラグ) と ext2 (not-clean)** は core が書き込み可でマウントしない (読み取り専用に落とす。docs/dirty-volumes.md)。カーネルの exfat / ext2 は書き込み可でマウントして続行するので、このクラッシュ後の整合性を見るテストは exFAT で 21 件、ext2 で 44 件が先へ進まない (期待リストに `core_unsupported` で記載)。ext4 / XFS / Btrfs / F2FS / NTFS / FAT は、fsync した内容が残った (期待外の失敗 0)。

### notrun が多い理由

652 件中 484〜517 件が notrun (`--loop`)。ほとんどは fstests が `FSTYP=fuse` に対して機能が無いと判断したため (reflink・xattr・chmod・falloc など)。理由の分類 (`results.json` の `notrun_cat`、summary.md に理由ごとの件数):

| 分類 | 意味 | ext4 の件数 (`--loop`) | (`--loop` なし) |
|---|---|---|---|
| `saf_unusable` | SAF (Android のストレージアクセスフレームワーク) 越しには使えない / FUSE ではカーネルに任せられない操作 (reflink / dedupe / fallocate / punch / mknod / swapfile / O_TMPFILE / exchangerange ...) | 262 | 251 |
| `core_unsupported` | core が持たない機能 (chmod / chown / xattr の設定 / ACL / 暗号 / クォータ / chattr / 疎ファイルの探索 / 時刻の細部 / FAT・exFAT・NTFS のハードリンクとシンボリックリンク ...) | 127 | 121 |
| `environment` | fstests の環境の都合 (shutdown / freeze / dax / logwrites / ツールが無い ...)。自前実装と関係ない | 92 | 170 (うち 75 が valid block disk) |
| `other` | 上のどれにも当てはまらないもの。分類の正規表現を直して 0 にした | 0 | 3 (FAT 系は 45) |

(`saf_unusable` が増えたのは、`environment` だった「SCRATCH が valid block disk でない」で止まっていたテストが、次の条件 (reflink / falloc / fpunch など) で止まるようになったため。)

#### 精査: アダプター側・環境側の工夫で動かせるもの (2026-11-19)

`environment` と `other` の理由を 1 件ずつ見直した結果。

| 理由 (件数, ext4 / XFS) | 動かせるか | 結論 |
|---|---|---|
| valid block disk / local device (75 件) | ループデバイスで動く | **実装した** (上の「ブロックデバイス」。dm-flakey の 48 件が電源断の試験になる。効果が最大) |
| fuse does not support shutdown (17 件) | しない | FS の shutdown ioctl (`XFS_IOC_GOINGDOWN` など) は FUSE が中継しない。電源断は dm-flakey で代用 (動くようにした) |
| Quota user tools not installed (21 件) | 意味が無い | `quota` を入れても、クォータ機能は core に無く別の理由 (core_unsupported) で notrun になる |
| fsverity utility required (10 件) | 意味が無い | `fsverity` を入れても FS が verity を持たない (core_unsupported) |
| Filesystem fuse not supported in `_scratch_mkfs_sized` (7 件) | しない | fstests 本体が FSTYP=fuse で小さい FS を作る手段を持たない (mkfs を呼ぶコードが無い)。fstests の改造が要る |
| fuse does not support freezing (3 件) | しない | FS の freeze (`FIFREEZE`) は FUSE が中継しない |
| dax / FITRIM / FSTRIM / logwrites / extsize / delayed allocation / huge file / casefold / inlinecrypt / idmapped / setdeleg (各 1〜3 件) | しない | カーネル・ブロック層の機能で、FUSE の FS では持たない |
| write atomic / multi-block atomic writes (数件) | しない | ブロックデバイスの atomic write (scsi_debug 等) の機能 |
| 他: LVM / dm-snapshot を使うもの (generic/081, 108) | 動かない | runner に LVM はあるが、論理ボリュームに mkfs し直す前提 (FSTYP=fuse の mkfs は何もしない)。108 は mount に失敗して SCRATCH も壊す (`premise_mismatch` + `fsck_ignore`)、081 は `_scratch_mkfs_sized` で notrun |

#### 残る課題 (見積もり)

* 「mkfs を呼ぶテスト」(`_scratch_mkfs_sized` 7 件、generic/108・563・704・731 など): アダプター側で、`_scratch_mkfs` の代わりに usbmem 側で mkfs するフックを置けば動く可能性がある (fstests の `_scratch_mkfs` は FSTYP=fuse で `_scratch_cleanup_files` に固定)。fstests 本体の改造 (または `MKFS_PROG` の差し替えの仕組みの追加) が要るので、今回は見送った。見積もり: 半日〜1 日。
* アダプターの fsync のエラー報告 (errseq 相当、generic/441): 1 時間程度。

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
| generic/321 | FAT | ファイルの mode (rw-r--r--) を保つ前提。FAT は権限ビットを持たず全部 rwxr-xr-x に見える |
| generic/040, 041 | Btrfs | 同じディレクトリの同じファイルへの 3000 個のハードリンク。core は INODE_REF の 1 項目 (約 350 件) にしか入れられず (INODE_EXTREF は未実装)、EMLINK |
| dm-flakey の 21 件 (generic/034 073 101 321 322 325 335 341 342 376 481 510 534 547 552 557 640 764 771 784 785) | exFAT | 電源断の後の再マウントで、core がダーティフラグのボリュームを書き込み可でマウントしない (docs/dirty-volumes.md)。カーネルの exfat は書き込み可で続行する |
| dm-flakey の 44 件 (034 039 040 041 056 057 065 073 090 101 104 106 107 321 322 325 335 336 341 342 343 348 376 480 481 498 502 510 526 527 534 547 552 557 640 690 764 771 779 782 784 785 789 790) | ext2 | 同上 (not-clean の ext2 は書き込み可でマウントしない。ジャーナルが無く fsck が要るため) |

**アダプターでは実現できない (adapter_limit)**:

| テスト | FS | 理由 |
|---|---|---|
| generic/426, 467, 477, 756, 777 | 全部 | `open_by_handle_at`: FUSE の nodeid はアダプターが振る (永続しない) ので、再マウントをまたぐ / キャッシュから消えたハンドルは ESTALE (116)。core にも inode 番号から開く手段が無い |
| generic/441 | 全部 (`--loop`) | dm-error で EIO を起こした後、同じファイルを開いている別の fd の fsync にも EIO が 1 回ずつ報告される (errseq) 前提。アダプターは fsync を core の `sync()` に写すだけで、失敗の後の 2 回目の fsync は成功を返す (エラーが fd ごとに残らない)。実装すれば通る見込み (1 時間程度) |
| generic/484 | 全部 (`--loop`) | `syncfs(2)` の EIO。FUSE のカーネルは syncfs をデーモンへ送らない (virtiofs だけ)。EIO を注入した直後のイメージなので fsck の失敗は想定内 (`fsck_ignore`) |

期待リストの `fsck_ignore: true` は「テストが SCRATCH の中身を壊すので、終わった後の fsck の失敗は想定内」(generic/108, 484, 740)。fsck の失敗として数えず、summary に別の節で出す。

**fstests の前提とこの環境・FS の性質が合わない (premise_mismatch)**:

| テスト | FS | 理由 |
|---|---|---|
| generic/732 | 全部 | 同じイメージを 2 か所へマウントして同じスーパーブロックを共有させる前提。アダプターは同じイメージを二重に開かない (flock で 120 秒待って失敗する) |
| generic/634 | 全部 | `touch` が `Dec 31 23:59:59 UTC 2147483647` を解釈できない (この環境の coreutils)。FS と無関係 |
| generic/798 | 全部 | ページキャッシュの Dirty 数を見る。FUSE は書き込みをすぐデーモンへ送るので dirty ページが残らない |
| generic/309, 313 | FAT | FAT の時刻の粒度は 2 秒。1 秒以内の mtime / ctime の更新を見る |
| generic/108 | 全部 (`--loop`) | SCRATCH を LVM の物理ボリュームにして `_scratch_mkfs` で作り直す前提。FSTYP=fuse の mkfs は何もしないので空の論理ボリュームをマウントできず、SCRATCH のシグネチャも壊れる (`fsck_ignore`) |
| generic/563 | 全部 (`--loop`) | FS の上のファイルを loop デバイスにして、その中に mkfs + mount する前提 (同上) |
| generic/704, 731 | 全部 (`--loop`) | scsi_debug のデバイスに mkfs して mount する前提 (同上) |
| generic/740 | 全部 (`--loop`) | SCRATCH に ext 以外の他 FS を mkfs して mkfs の検出を見る。通るが、終わった後の SCRATCH は別の FS (fsck の失敗は想定内、`fsck_ignore`) |
| generic/409, 410, 411 | 全部 (`--loop`) | TEST_DEV を TEST_DIR の下へもう一度マウント (bind マウントの共有・伝播) する前提。同じイメージを二重に開かない (generic/732 と同じ) |
| generic/620 | FAT / exFAT | dm の巨大デバイスに mkfs して mount する前提 (同上)。他の FS はイメージが小さく notrun |
| generic/785 | FAT | fssum -A が再マウント前後でメタデータを比べる。FAT の時刻の粒度 2 秒 (309 / 313 と同じ) が原因と推定 (未確認) |
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
8. **`/dev/mapper/NAME` の `fsname` を `realpath` で `/dev/dm-0` にしていた** (`--loop` の dm-flakey 系で発見): fstests の `umount $SCRATCH_DEV` が FUSE のマウントと一致せず "not mounted" になり、デーモンが残って次の mount が排他ロックで失敗した。絶対パスなら `fsname` は指定のまま。
9. **xattr の一覧・取得を登録していなかった** (generic/547, 785): 一覧が `EOPNOTSUPP` になり fssum が失敗した。一覧は空・取得は `ENODATA` で答える。
10. **`mount -t fuse` (サブタイプなし) を受けられなかった** (dm-error 系): `/sbin/mount.fuse` を `mount.fuse.usbmem` に差し替え (`--loop` のとき)。

**core の不具合 (最小限の修正 + 回帰試験)**

1. **exFAT: NO_FAT_CHAIN のディレクトリの探索で、ヒントを使うとクラスタ数が合わない** (generic/006): `exfat_find_dir_entry` で、ヒント位置から始めるとき `clu.size` が元のディレクトリ全体のままで、ディレクトリの終端を越えて読んだ (EIO)。`third_party/exfat/fs-exfat/dir.c` に最小の修正 (コメントは "usbmem:" から始まる)。
2. **exFAT: 同じ関数の「巻き戻し」で `clu.size` を戻していなかった** (generic/006): 先頭に戻るとき同上。回帰試験 `test_exfat_write --dirfull` (名前 384 / 4096 個の作成・unlink・rmdir) を `tools/run_exfat_write_tests.sh` に追加。
3. **XFS: 空き領域の B+木 (bnobt / cntbt) の削除で、葉の最小レコード数を割る** (generic/013 の後の xfs_repair): 兄弟との併合・再分配 (`bt_rebalance`) を追加。回帰試験 `test_xfs_write --frag` (6000 ファイルの作成と無作為な削除) を `tools/run_xfs_write_tests.sh` に追加。
4. **XFS: ショートフォームのディレクトリでエントリのオフセットが増え続ける** (generic/028 の後の xfs_repair "directory offsets too high"): 名前の変更を繰り返すと、追加位置が常に「最後のエントリの後ろ」でオフセットが伸びた。カーネルの `xfs_dir2_sf_addname_pick` と同じ「入る最も低い隙間」にし、エントリをオフセット順に並べる。`--frag` に 3000 回の rename を追加 (修正前は xfs_repair -n が失敗することを確認)。

5. **Btrfs: ハードリンクが INODE_REF の 1 項目に収まらないとき、変更の途中で EMLINK になりトランザクションを中止する** (generic/040, 041。`--loop` で動くようになって発見): `op_link` は `add_entry` の中で dir_item を足した後に `inref_add` が溢れ (`EMLINK`)、変更が始まっているのでボリュームを異常終了にしていた。以後はすべて失敗する (ENOENT など)。`inref_room` で変更の前に調べるようにし、EMLINK だけ返す (同じディレクトリに約 350 個を超えるハードリンクは INODE_EXTREF が無いので依然 EMLINK。generic/040, 041 は期待リストの core_unsupported)。回帰試験は generic/040, 041 自体 (EMLINK の後も ln が EMLINK を返し続け、ENOENT にならない。run 44454416850 / 44461561567)。

### 使い方の注意・制限

* fstests は `FSTYP=fuse` では mkfs も fsck もしないので、ドライバーがテストごとに mkfs し直し (`--no-remkfs` で止められる)、テストの後に fsck を走らせる。前のテストの壊れ方が次へ持ち越されないようにするため。
* 総容量 / 空き容量の statfs は見積もり (core に API が無い)。空き領域を見るテスト (`_scratch_mkfs_sized`、ENOSPC を狙うもの) は notrun か不正確。
* 単一スレッド。性能試験ではなく意味論の試験。FAT / exFAT の疎ファイル無しで時間がかかるテストがある。
* イメージは 1 GiB (FAT / exFAT は fsstress が埋めるので 6 GiB)。
* `-strict` (quiet_setattr なし) の ext4 (run 43665047439): pass 90 / fail 15 / notrun 547。generic/087 が notrun (chmod 不可) に変わり、generic/005 が失敗 (symlink への utimes が EOPNOTSUPP で、ELOOP が出ない)。それ以外は同じ。

## 未了・注意

* **`--no-remkfs` での ext4 の再確認**: 未実施 (テストごとの mkfs を止めたとき、前のテストの影響でどう崩れるか)。
* **notrun の `environment`**: 最大の「valid block disk」は `--loop` で解消した (上の「精査」)。残りは shutdown / freeze / dax / ツールなどで、FUSE では動かせない。mkfs を呼ぶテスト (`_scratch_mkfs_sized` など) は fstests 側の仕組みが要る (「残る課題」)。
* **ext2 / exFAT の電源断の試験**: core が not-clean / ダーティのボリュームを書き込み可で開かないので、クラッシュ後の整合性は試せていない (exFAT 21 件・ext2 44 件)。core の方針 (docs/dirty-volumes.md) なので、試すなら「アダプターのオプションで、ダーティを承知で書き込み可で開く」(PC 専用の `-o force_rw` 相当) を core に足す必要がある。見積もり: 半日。
* **FAT の generic/346, 394**: 346 は `--loop` で通ることも hang になることもあった (時間のばらつき)。
* **時刻・権限**: core が atime / ctime / 負の時刻 / 権限を持たない限り、generic/003・087・120・221・236・258・755 は落ちたまま (期待失敗)。core に API を足すのは別の話 (docs/TODO.md)。
* **ファイルハンドル (open_by_handle_at、generic/426 467 477 756 777 × 8 FS = 40 件)**: nodeid を永続にする (inode 番号 + 世代を nodeid にする) 設計に変えない限り通らない。FS ごとに inode 番号の安定性が違う (FAT / exFAT はエントリ位置) ので、通すなら FS ごとの確認が要る。**見積もり (2026-11-19、実装は見送り): 4〜6 時間 + CI の往復**。必要なもの: (1) `FUSE_CAP_EXPORT_SUPPORT` を `op_init` で有効にする、(2) nodeid = 実 inode 番号にして、実 inode 番号ごとの世代 (generation) を `reply_entry` で返す (現在の「別の nodeid を振る」設計をやめ、消えた inode の再利用は世代の加算で表す。カーネルは nodeid + generation が違えば別の inode として作り直す)、(3) `lookup(nodeid, ".")` (カーネルがハンドルから dentry を作るときに送る) に、nodeid 自身の属性で答える (core で inode 番号から stat できるか FS ごとに確認)、(4) 再マウントをまたぐ (477 / 777) には世代がメモリだけでは足りない (FAT / exFAT は削除後に番号が再利用されるので、ハンドルが別のファイルを指す)。リスクは、上の「見つけた不具合」アダプター 2 (nodeid の再利用で EISDIR / ELOOP / EIO) の回帰。今回は 5 件 × 8 FS のためだけにそのリスクを取らなかった。
* **同じイメージの二重マウント** (generic/732): 現状は拒否 (排他ロック)。core が複数のボリュームを同じデバイス上で共有する設計ではない。
* **FAT の generic/346, 394**: 1 GiB のゼロ埋めに 300 秒では足りない。`--timeout` を伸ばせば通るかもしれないが、試していない。
* **btrfs / f2fs / ntfs / xfs の「pass 92」は ext4 と同じ集合**: fstests が FUSE に対して notrun にするので、各 FS の固有の機能 (btrfs のスナップショット、f2fs の gc など) は試されない。FS 固有の試験は、既存の `tools/run_*_write_tests.sh` が担う。

## XFS の rmapbt / 親ポインタのあるボリュームでの結果 (2026-11-21)

XFS の書き込みが rmapbt (mkfs.xfs 7.x の既定) と親ポインタ (同、`-n parent=1`) に対応した (docs/xfs-write.md)。`run_xfstests.py --xfs-features base|rmapbt|parent|all` で XFS の mkfs の機能を選べる (既定 `base` = `-m rmapbt=0 -n parent=0`、従来どおり)。workflow はブランチ名の末尾 `-xfsrmapbt` / `-xfsparent` / `-xfsall` で選ぶ (例 `usbmem-xfstests-xfs-xfsparent`)。

- `usbmem-xfstests-xfs` (base): run 44907405127、quick グループ 652 件、pass 144 / fail 24 (すべて期待内、期待外 0) / notrun 484、アンマウント後の fsck 失敗 0。従来と同じ。
- `usbmem-xfstests-xfs-xfsparent` (rmapbt=1 parent=1): run 44907406907、pass 142 / fail 26 (期待外 2: generic/040 / 041) / notrun 484、アンマウント後の fsck 失敗 0 (`xfs_repair -n` はテストごとの TEST / SCRATCH とも clean)。
  - generic/040 / 041: 同じファイルへの 3000 個のハードリンク。親ポインタ付きでは、リンクごとに子の属性フォークへ親ポインタが増え、自前は属性の leaf 1 ブロック (約 110 個) までしか入れられない (node 形式の属性は未実装) ので `EMLINK`。`xfstests_expected.json` の `xfs` に `core_unsupported` として載せた (親ポインタのあるボリュームだけ。既定では通る)。
