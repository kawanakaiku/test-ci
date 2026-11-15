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
| `...-strict` | `quiet_setattr` を付けない |
| `workflow_dispatch` | fs / group / tests / mount_opts / timeout を入力 |

fstests は kernel.org が繋がらないことがあるので、3 回 × (kernel.org, googlesource) を各 120 秒で試し、だめなら GitHub の kdave ミラーの既定ブランチを使う (警告を出す)。

## アダプターの設計

### 正直に返す (対応しているふりをしない)

core に無い操作は、そのとおりのエラーで返す。近似するものは近似であると明記する。

| 操作 | 返し方 |
|---|---|
| chmod / chown / utimes (setattr の mode / uid / gid / atime / mtime) | 既定は `EOPNOTSUPP`。要求が「今の値と同じ」なら成功。`quiet_setattr` なら黙って成功 (値は変わらない) |
| xattr / ACL | `ENOSYS` / `EOPNOTSUPP` |
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

(下の「結果」節を CI の結果で埋める)

