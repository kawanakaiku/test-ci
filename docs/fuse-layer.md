# FUSE プログラムを載せる層 (fuseshim) の試作と設計

ユーザー空間の FS 実装 (ntfs-3g、wimlib の wimmount など) を、無改変のソースで、**プロセス内の libfuse 互換の層 (shim)** の上で動かし、
`FsVolume` (core の読み取り API。`core/compat/public/usbmem/fs_volume.h`。依頼文・TODO の「VolumeOps」はこれ) に変換して SAF へ出すための検討と、小さな試作の記録。
TODO.md の「FUSE プログラムを載せる層 (第 3 の方式) の検討」(2026-11-16) への答え。**試作は製品に組み込んでいない** (UI・SAF・既存の core は変えていない。既定のビルドにも入らない)。

- 試作のコード: `core/fuseshim/` (shim 本体・BlockDevice 接続・FsVolume アダプター・RPC・測定用の実行体)、`core/fuseshim.cmake` (既定 OFF のオプション)、`tools/fuseshim/` (ソース取得)、`tools/run_fuseshim_tests.sh` (試験)、`.github/workflows/fuseshim.yml` (専用ジョブ。`usbmem-fuseshim*` ブランチへの push)
- 方針: **サードパーティのソースはリポジトリに置かない**。CI が固定バージョンを sha256 検証つきで取得してビルドする。
- 逆向きの先行実装: `tools/fuse/usbmem_fuse.cpp` (FsVolume → FUSE。xfstests 用)。操作の対応表 (lookup / getattr / readdir / read / readlink / statfs) は同じ。

## 0. 結論 (詳細は各章)

| 項目 | 結論 |
|---|---|
| 動いたか | ntfs-3g 2022.10.3 (src/ntfs-3g.c と libntfs-3g は**無改変**) が shim 越しに動き、自前の NTFS 実装と全ツリー (名前・種別・サイズ・mtime・sha256) が一致した。wimlib 1.14.4 の wimmount (`--without-ntfs-3g`) も libfuse 3.x 系の shim で動き、LZX / 無圧縮 / LZMS の WIM が元のツリーと一致した (6 章)。別プロセス (socketpair RPC) 越しでも一致 |
| shim の大きさ | ntfs-3g を載せる C は合計 558 行 (論理 約 410 行): libfuse 2.x 系 shim 292、ブロックデバイス接続 201、C API ヘッダ 65。libfuse 3.x 系 (wimlib 用) 215 行。FsVolume アダプター + セッション 約 350 行、RPC 約 200 行 (5.3 章) |
| 本番の方針 | **別プロセス (RPC) を主案** (ユーザー決定)。試作は同一プロセスで測り、socketpair 越しの形も測った: 連続読み 約 8 → 3.3 GB/s、メタデータ 1 往復 約 9 µs (5.4 章)。理由と設計は 3 章 |
| 性能の発見 | ntfs-3g は**パスを引くたびに索引ブロックを読み直す**ので、ページキャッシュの無い実機 USB では下位に読み取りキャッシュ層が必須 (64 MiB で解消)。逆に自前の NTFS は下位への読みが 4 KiB 単位で、大きな連続読みは約 3.5 倍遅い (5.4 章) |
| 製品化 (個人版) | **wimlib の WIM を別プロセス (`:wimfuse` Service + exec) で読む形を Android (AVD) で実測済み** (10 章)。ntfs-3g は製品の経路に載せない |
| 載せる価値のある FS | ntfs-3g は「独立した参照実装 (NTFS 書き込みの突き合わせ)」としての価値が大きい。製品の経路に載せる価値は、既に自前実装がある NTFS / exFAT / ext では低い。**自前実装のない FS (wimlib の WIM、将来の squashfuse 系や暗号ボリューム系)** が候補 (7 章) |

## 1. shim が提供する libfuse の範囲

ソースを読んで数えた (ntfs-3g 2022.10.3、wimlib 1.14.4)。

### 1.1 ntfs-3g (高水準 API・パス単位) — 対象にした

`src/ntfs-3g.c` が呼ぶ libfuse の関数は **11 個だけ**: `fuse_opt_add_arg` / `fuse_opt_free_args` / `fuse_version` / `fuse_mount` / `fuse_unmount` / `fuse_new` /
`fuse_destroy` / `fuse_loop` / `fuse_get_session` / `fuse_set_signal_handlers` / `fuse_get_context`。内蔵 FUSE (`--with-fuse=internal`、`FUSE_INTERNAL`) でビルドすると、
さらに `drop_privs` / `restore_privs` (libfuse-lite の fusermount.c にある特権の出し入れ) が要る。`fuse_main` は使わず、main が自分でこの順に呼ぶ
(`fuse_mount` → `fuse_new` → `fuse_set_signal_handlers` → `fuse_loop` → `fuse_unmount` → `fuse_destroy`)。
登録する `fuse_operations` は getattr / readlink / readdir / open / release / read / write / truncate / statfs / chmod / chown / create / mknod / symlink / link / unlink / rename / mkdir / rmdir /
utimens / fsync / bmap / ioctl / access / opendir / releasedir / getxattr / setxattr / listxattr / removexattr / init / destroy。読み取り専用の試作で呼ぶのは
getattr / readdir / open / read / release / readlink / statfs / getxattr / opendir / releasedir と、init / destroy。

ヘッダ (`fuse.h` など) は、ntfs-3g のソースに含まれる `include/fuse-lite/` (libfuse 2.x 系の API のヘッダ。LGPLv2) を、ビルド時にそのまま使う
(`struct fuse_operations` の並びが ntfs-3g.c と shim で自動的に揃う)。本番で自前のヘッダにするなら、使うフィールドだけの小さな `fuse.h` を書く (数百行)。

### 1.2 lowntfs-3g (低水準 API・ino 単位) — 今回は対象にしない

`src/lowntfs-3g.c` は低水準 API: `fuse_lowlevel_new` / `fuse_session_*` / `fuse_req_ctx` と、応答の `fuse_reply_*` (err 43 回・buf 6・entry 5・xattr 3・open 2・attr 2・write / statfs / readlink / create / ioctl / bmap 各 1)、
`fuse_add_direntry`、`fuse_lowlevel_notify_inval_inode` など約 25 種で、操作は 32 本 (`ntfs_fuse_*` の `static void`)。低水準は要求ごとに `fuse_req_t` を渡し、応答を後から返す
(= shim は要求オブジェクトと応答バッファを実装する必要がある)。**ino 単位は `FsVolume` と素直に対応する**ので、本番で lowntfs-3g を載せる利点は、パス → ino の表 (下記 1.4) が要らないこと。
ただし shim が 3〜4 倍大きくなる (見積もり)。試作では高水準 (ntfs-3g) を選んだ: shim が小さく、ntfs-3g は両方とも同じ `ntfs_*` 関数に処理を寄せているので、確かめたい性質 (BlockDevice 差し替え・グローバル状態・性能) は変わらない。

### 1.3 wimlib の wimmount (libfuse **3.x** 系の高水準 API)

`src/mount_image.c` は `FUSE_USE_VERSION 30`。ライブラリ関数として `wimlib_mount_image(wim, image, dir, flags, staging_dir)` があり、中で `fuse_main` (= `fuse_main_real_versioned`) を 1 回呼ぶ。
libfuse の関数は **`fuse_main` / `fuse_get_context` (6 箇所) / `fuse_exit` の 3 つだけ**。ただし 2.x 系とは API が違う: `getattr(path, stat, fi)`、`readdir(path, buf, filler, off, fi, flags)` (filler は `flags` つき)、
`init(conn, cfg)` (`struct fuse_config` に `use_ino` / `kernel_cache` / `nullpath_ok` / `hard_remove` / `entry_timeout` ... を書く)。`nullpath_ok` なので read / readdir は `path` が NULL になりうる設定で、
`fi->fh` (自前の `wimfs_fd`) に頼る → **shim は open / opendir が返した `fi` を read / readdir / release に渡さなければならない** (実装済み)。
readdir は **stat を付けずに名前だけ** `filler` に渡す (`filler(buf, name, NULL, 0, 0)`)。→ アダプターが ino / 種別を getattr で補う (実装済み)。
アンマウントは、特別な xattr 経由で `unmount_wimfs()` が `fuse_exit()` を呼ぶ仕組み (別プロセスの `wimlib_unmount_image` が mqueue + xattr で頼む)。shim ではホストが `stop()` で fuse の本体ループを返させるので使わない。
2.x 系と 3.x 系の `fuse_operations` は構造が違うので、**shim は 2 ファイル** (`fuseshim_fuse2.c` / `fuseshim_fuse3.c`)。公開 API (`fuseshim.h`) は共通。1 つの実行体にはどちらか一方だけ入る。

### 1.4 パス単位 → ino 単位 (アダプター)

高水準 API はパス単位、`FsVolume` は ino 単位。`FuseShimVolume` が「ino → 最初に見えたパス」の表を持つ: lookup / readdir で見えた子を登録し、stat / read / readlink は表のパスで呼ぶ。
ino は getattr / filler の `st_ino` (ntfs-3g は `use_ino` で MFT 番号 = 自前実装と同じ番号。wimlib は `use_ino` で連番)。ハードリンクは別パスでも中身が同じなので 1 つでよい。
**読み取り専用なら表は変わらない**。書き込みを足すなら rename / unlink / rmdir で表を更新する必要がある (未実装)。表の大きさはディレクトリを見た数に比例する (20000 ファイルで約 3 MB。本番は上限つき LRU にする)。

## 2. ブロックデバイスの I/O の差し替え

ntfs-3g は、libntfs-3g の `struct ntfs_device_operations` (open / close / seek / read / write / pread / pwrite / sync / stat / ioctl の 10 関数) 越しにだけデバイスへ触る。
既定の実装 `ntfs_device_unix_io_ops` (= `ntfs_device_default_io_ops`。`unix_io.c`) が `/dev` ノードやイメージを open(2) / pread(2) / ioctl(2) する。

| 候補 | 内容 | 評価 |
|---|---|---|
| **A. `unix_io.c` を外し、同じシンボル `ntfs_device_unix_io_ops` を自前の .c で定義する (採用)** | ソースは無改変。ビルドの対象ファイルを 1 つ差し替えるだけ。`ntfs3g_io.c` (約 200 行) が 10 関数を BlockDevice の `read_at` / `write_at` / `flush` に直結。seek 位置の管理・EOF の切り詰め (BlockDevice は範囲外を断る)・`BLKGETSIZE64` / `BLKSSZGET` の ioctl だけ実装 | 型が公開ヘッダ (`device.h`) に載っている拡張点なので壊れにくい。バージョンが上がっても、構造体が変わらない限り追従不要。**余計なコピー 0** (ntfs-3g の呼び出し側のバッファに BlockDevice が直接読む) |
| B. `--wrap=open,pread,fstat,ioctl` や `LD_PRELOAD` で libc の関数を差し替える | fd を仮想化し、パス (または特別な名前) の open を横取り | 無改変だが、プロセス全体の open / pread に効いてしまう (他のコードに影響する)。fd の番号管理・fcntl ロック・fstat の偽装が要る。wimlib のように「パスを open して pread」だけの相手には使えるが、同一プロセスでは副作用が大きい |
| C. ntfs-3g のソースに小さなパッチを当てる | `unix_io.c` の中身を書き換える | 動くが、取得したソースへのパッチの保守が要る。A で足りるので不要 |
| D. libfuse-lite (内蔵 FUSE) のまま /dev/fuse を使う | 差し替えなし | Android アプリは /dev/fuse を開けない。目的に反する |

- デバイス名: ntfs-3g は `realpath` を通すので、**存在するパス**を渡す必要がある (試作は `/dev/null`)。名前は I/O 側で無視する (結び付けるデバイスは `fuseshim_ntfs3g_io_bind` で渡した 1 つ)。
  複数ボリュームを同時に扱うなら名前でレジストリを引く (別プロセス方式なら 1 プロセス 1 ボリュームなので不要)。
- 書き込み: `write_at` / `flush` も結んである (読み取り専用のマウントでは呼ばれない)。ダーティフラグ・`$LogFile` の扱いは ntfs-3g 任せなので、自前実装 (docs/dirty-volumes.md の「未再生ログ / 休止状態は書かない」) の方針と一致しない (後述 8 章)。
- wimlib: `wimlib_open_wim(path)` が open(2) + pread(2) で読むので、**WIM ファイルを実在のパスで渡す必要がある**。WIM が USB の NTFS の中にあるなら、(a) 一時ファイルにコピー (「一時コピーしない」方針に反する)、(b) 上の B の方式 (open / pread / fstat の差し替え) で
  `FsVolume::read` に繋ぐ、(c) wimlib のファイル I/O 層 (`src/file_io.c` の `filedes_*`) に小さなパッチを当てる、のどれかが要る。**試作では未対応** (実在するファイルで試験)。

## 3. グローバル状態と exit() を持つ main の隔離 — 同一プロセスの危険と、本番は別プロセス

ソースで確かめた「同一プロセスで ntfs-3g の main を呼ぶと何が危ないか」:

| 危険 | 場所 | 試作での扱い |
|---|---|---|
| `daemon(3)` で fork して親を exit する | `ntfs-3g.c` の `setup_logging` (`no_detach` が無いとき) | `-o no_detach` を渡す + `-Ddaemon=fuseshim_daemon_stub` (何もしない関数) でビルドして二重に防ぐ |
| `exit()` | `ntfs-3g_common.c` の `-h` (exit 9) / `-V` (exit 0)、libntfs-3g の `unistr.c` (DEBUG ビルドのみ) | `-h` / `-V` を渡さない。本番の同一プロセスなら `exit` を longjmp / スレッド終了に差し替えるリンカ操作が要る |
| `static` のグローバル (`ctx`、`opts`、`ntfs_3g_ops`)、`main` の終了時に解放済みのポインタが残る | `ntfs-3g.c` | **1 プロセスで 1 回だけ**。2 回目は未定義 (未検証)。ボリュームを開いて閉じてまた開く使い方は不可 |
| `getopt_long` の内部状態 (`optind`) | `ntfs_parse_options` | 呼ぶ前に `optind = 0` (実装済み)。アプリの他のコードが getopt を使うと干渉する |
| `setlocale(LC_ALL, "")` | `ntfs_set_locale()` | プロセスのロケールが変わる (アプリ全体に効く) |
| `/proc/filesystems` の読み、root なら `modprobe fuse` の fork + exec、`mknod /dev/fuse` | `get_fuse_fstype` / `load_fuse_module` / `create_dev_fuse` (Linux ブランチ) | 非 root では EPERM になるだけで無害。root で走る環境 (`su` など) では副作用がある。**同一プロセスでは避けたい** |
| `/proc/mounts` の読み (`ntfs_check_if_mounted`) | libntfs-3g | 無害 (Android でも読める) |
| シグナル | `fuse_set_signal_handlers` (libfuse の SIGINT / SIGTERM / SIGHUP / SIGPIPE の奪取) | shim は何もしない (アプリのハンドラを奪わない) |
| ログ | `ntfs_log_handler_stderr` (stderr へ。`daemon` 後は syslog) | stderr のまま。Android では logcat に出ない → 本番は `ntfs_log_set_handler` で差し替え |
| スレッド | ntfs-3g は単一スレッド前提 (ロックなし)。FUSE の単一スレッドループが直列化していた | shim が全操作を 1 つのミューテックスで直列化 (性能の上限になる) |
| メモリの上限・OOM | ntfs-3g は malloc を直接呼び、失敗でも落ちない箇所がある | 同一プロセスだとアプリごと落ちる |

**本番の方針 (ユーザー決定): 別プロセス (プロセス分離) を主案にする。** 理由は 2 つ: (1) ライセンスの波及を抑える考え方 (GPL / LGPL のコードを別実行ファイルに分け、本体とは操作の送受信だけで通信する。4 章)、(2) 上の表の exit() / グローバル状態 / 1 回限りの main / OOM を、プロセスごと使い捨てにして隔離できる (クラッシュしてもアプリは落ちない。ボリュームを閉じたらプロセスごと捨てるので「2 回目は未定義」が問題にならない)。

### 3.1 構成

```
[アプリ本体のプロセス]                                  [FUSE プログラムのプロセス (使い捨て、1 ボリューム 1 プロセス)]
 SAF / DocumentsProvider → FsVolume (RpcVolume) ──socketpair──→ rpc_serve → FuseShimVolume → shim → ntfs-3g の fuse_operations
                                                                            ↑ BlockDevice 接続 (ntfs3g_io.c)
                                                                 (A) 同じプロセスで USB の fd から BlockDevice を作る  または
                                                                 (B) block 読みを本体へ RPC で戻す
```

- **試作で作ったのは `core/fuseshim/rpc_volume.{h,cpp}`**: `rpc_serve(fd, FsVolume&)` (FUSE プログラムの側) と `RpcVolume : FsVolume` (本体側)。`usbmem_fsdump_* --rpc` が fork した子プロセスに shim + ntfs-3g (または wimlib) を載せ、親が `RpcVolume` で読む。
  shim の下側 (`FuseShimVolume`) が `FsVolume` そのものなので、RPC の境界は「FsVolume を話す 2 つの端」として挟めた (試作の同一プロセス版は RpcVolume を挟まないだけ)。
- メッセージ形式 (試作): 固定長の要求 `{op, name_len, a, b, c}` (40 バイト) + 名前、応答 `{rc, payload_len}` (16 バイト) + 本体。op は LOOKUP / STAT / READDIR / READ / READLINK / STATS。READDIR は 1 ディレクトリ全部を 1 応答にまとめる (`{ino, dtype, name_len, name}` の列)。
  本番で足すもの: statfs / getxattr、要求 ID (パイプライン化)、1 応答の上限と継続 (巨大ディレクトリ)、エラーの区別 (FUSE プログラムの死亡 = EPIPE → ボリューム切断として扱う)、書き込み系。
- **ゼロコピーできない分のコスト**: READ は FUSE プログラム側のバッファ → socket (コピー 1) → 本体側のバッファ (コピー 2)。本体側は呼び出し元のバッファへ直接受ける (試作済み) ので、**同一プロセスより 2 回の memcpy とコンテキストスイッチ 2 回が増える**。測定は 5 章。
  USB ストレージの帯域 (USB 2.0 で約 40 MB/s、USB 3.x で数百 MB/s) に比べて、socketpair は数 GB/s 出るので、READ の増分は支配的でない見込み。大きな読み (1 MiB) にまとめれば往復回数も少ない。
  メタデータ (lookup / stat) は往復 1 回あたり数十 µs のコストで、20000 ファイルの列挙 + stat で数百 ms の増加 (5 章)。READDIR にエントリごとの stat を同梱すれば往復を減らせる (FsVolume の API の変更が要る。要検討)。
- ブロックデバイス: (A) USB の fd (`UsbDeviceConnection` の fd) を子プロセスに渡し、子が自分で `usbmem_core` の BlockDevice を作る — 往復が無く速いが、子プロセスに USB ドライバ (core) も入る。
  (B) 子 → 本体への block 読み RPC — 本体の BlockDevice (暗号・パーティション・ネスト・キャッシュの層) をそのまま使えるが、ntfs-3g の 4 KiB の索引ブロック読みごとに往復が要る (5 章の「索引ブロックの再読み込み」の問題があり、本体側にキャッシュ層が必須)。
  暗号ボリューム / ネストの上の NTFS を載せるなら (B)、USB 直なら (A)、が自然。**要検討**: 両対応の `BlockDevice` 抽象を子プロセスに持たせる (ntfs3g_io.c の `fuseshim_blkdev` は既に関数ポインタ 3 つ + サイズなので、RPC クライアントでも実装できる)。

### 3.2 Android での別プロセスの起こし方 (調査。**AVD で検証済み (10 章)**: a と c の組み合わせ = `:wimfuse` の Service が `nativeLibraryDir` の ELF を exec。実機は未検証)

| 方式 | 内容 | 長所 / 短所・要確認 |
|---|---|---|
| **a. 別の `:service` プロセス (`android:process=":fuse"` の Service)** | OS が zygote から起こす通常のアプリプロセス。同じ UID。JNI の .so (shim + ntfs-3g) をそのプロセスだけで `System.loadLibrary`。本体とは `bindService` + `ParcelFileDescriptor` (socketpair の片端) で結ぶ | exec の制限 (W^X) を受けない。クラッシュ隔離・メモリ上限の分離・exit() の影響隔離ができる。**GPL / LGPL のコードを入れた .so は別 .so (別ファイル) にできる**が、同じ APK に入る。プロセス起動 ~100 ms、メモリ (ART + 空のアプリ) 数十 MB が増える |
| **b. `isolatedProcess="true"` の Service** | 権限なし・ファイルアクセスなし・別 UID の隔離プロセス。fd は binder で受け取れる | 最も強い隔離。ただし isolated_app の SELinux ドメインで、渡された USB fd への ioctl (usbdevfs) や socket が許されるかは**未確認** (「要検証」)。(B) の block RPC 方式なら USB に触らないので成立しやすい。スレッド / メモリ制限、ネットワーク不可 (CIFS には使えない)。`useAppZygote` 経由で .so を事前ロードできる |
| **c. 実行ファイルを fork/exec** | APK に ELF の実行ファイルを同梱し、`ProcessBuilder` で起こす。stdin/stdout か `LocalSocket` (SCM_RIGHTS で fd を渡す) で RPC | **W^X の制約**: targetSdk 29 以降、アプリのデータディレクトリ (`filesDir` など) に置いたファイルの exec は SELinux が拒否する。実行できるのは `nativeLibraryDir` (APK から展開された `lib*.so`) の中のファイルだけ。実行ファイルを `libfuse_server.so` の名前で `jniLibs/<abi>/` に入れ、`extractNativeLibs=true` (`packaging.jniLibs.useLegacyPackaging = true`) にして `nativeLibraryDir/libfuse_server.so` を exec する、というのが一般的な回避。**Google は推奨していない** (将来の制限の可能性)。`ProcessBuilder` は fd 0,1,2 しか引き継がないので、USB の fd は UNIX ソケット + SCM_RIGHTS で渡す。**exec が成立するかは AVD で 1 回確かめる必要がある (未確認)** |
| d. アプリ本体を fork() する | JNI から fork し、子で FUSE プログラムを動かす | **不可**: 本体は ART (多数のスレッド・GC) なので、fork 後の子は async-signal-safe な関数しか安全に呼べない。zygote の仕組みの外で fork すると ART が壊れる |

推奨: **a (`:service` プロセス) を最初の選択**にする。exec の W^X 問題がなく、AVD でそのまま試せ、本体との結合は ParcelFileDescriptor で足りる。ライセンス上の「別の実行ファイル」としての性質 (4 章) を重視するなら c を追加で検討する (a は同じ APK の別プロセスで、c は別の ELF)。b は将来の強化 (隔離の強さと USB fd の扱いの検証が要る)。

## 4. ライセンス

(各ディレクトリの現状: `core/` は Apache-2.0、`core/compat/` と `third_party/` (カーネル由来) は GPL-2.0-or-later (docs/licenses.md)、`tools/fuse/usbmem_fuse.cpp` は GPL-2.0-or-later。配布物全体の扱いは docs/licenses.md の「未解決の確認事項」にある**保留項目**: 「自前コードのライセンスを GPL にするか、プロセス分離するか」「GPL-2.0 コードを静的リンクした場合の配布物全体の扱い」。)

**以下は法的な断定ではなく、確認が要る点の整理**。

### 4.1 取り込む対象

| 対象 | ライセンス (ソースで確認) | 備考 |
|---|---|---|
| ntfs-3g (src/ntfs-3g.c、libntfs-3g) | GPL-2.0-or-later (ファイル冒頭の表記。unix_io.c も "GNU General Public License ... either version 2 ... or any later version"。`COPYING`)。`COPYING.LIB` (LGPL) は README に「**fuse-lite ライブラリ**に適用」とあり、libntfs-3g ではない | 配布するなら実質 GPL。「LGPL 系の部分あり」。依頼文の「libntfs-3g は LGPL 系の部分あり」は、COPYING.LIB が fuse-lite 用であることによる (libntfs-3g 本体は GPL-2.0+)。**ファイルごとの精査は未了** |
| ntfs-3g の `include/fuse-lite/*.h` (shim のコンパイルに使う) と `libfuse-lite/` | LGPLv2 (README と各ファイルの表記。libfuse 由来)。shim は libfuse-lite の .c を使わず (置き換える)、ヘッダだけをビルド時に使う | ビルド時にだけ使う。本番で自前ヘッダにすれば取り込まない |
| wimlib (libwim) | LGPL-3.0-or-later と GPL-3.0-or-later の**デュアル** (`COPYING`: 「libntfs-3g などサードパーティのライセンスが禁じない場合に限り LGPL を選べる」) | 下 4.3 |
| shim・アダプター (`core/fuseshim/`) | 本試作は GPL-2.0-or-later (SPDX)。`fs_volume.h` が GPL-2.0-or-later のため。`fuseshim.h` と shim 本体は libfuse の API を再実装した小さなコードで、Apache-2.0 にもできる (未決) | ntfs-3g にリンクした実行体全体は GPL になる |

### 4.2 取り込み方 (提案)

- **ソースは同梱しない**: CI が固定バージョンを取得し sha256 を検証する (`tools/fuseshim/fetch_ntfs3g.sh`、`fetch_wimlib.sh`)。ntfs-3g は配布元 (tuxera.com) が GitHub runner の既定 User-Agent に 403 を返したので、ブラウザ風の User-Agent を付けた。Debian の orig tarball (中身は同じソースで configure が無い。autoreconf 必要) も予備にしてある。**取得に失敗したら skip ではなく失敗にする** (見逃し防止)。
- **製品に入れる場合のソース提供**: ビルドに使ったバージョン・ソースの入手先 (固定 URL と sha256)・自前の差分 (shim と io 差し替えとビルドフラグ。**ソース自体は無改変**) を、アプリのライセンス表示 (アプリ内の「ライセンス」画面 + 配布ページ) に載せる。ライセンス全文 (GPL-2.0 / GPL-3.0 / LGPL-3.0) の同梱も要る。

### 4.3 wimlib は ntfs-3g に依存するか (ソースで確認、ユーザーの質問への答え)

- **依存は任意で、wimmount (WIM のマウント) には要らない**。`configure.ac` の `--without-ntfs-3g` (`WITH_NTFS_3G`) で libntfs-3g への依存が外れる。libntfs-3g を使うのは `src/ntfs-3g_apply.c` と `src/ntfs-3g_capture.c` (NTFS ボリュームへの直接の capture / apply。セキュリティ記述子や代替データストリームの保存のため) だけで、`blob_table.c` / `resource.c` / `extract.c` / `update_image.c` の関連箇所も `#ifdef WITH_NTFS_3G` で囲まれている。
  `src/mount_image.c` (wimmount) には ntfs-3g への参照が**一つも無い** (grep で確認)。FUSE 部分 (`WITH_FUSE`、libfuse3) は別のオプションで、wimlib 自前の `wimfs_*` 操作で動く。
- `--without-ntfs-3g --with-fuse` で `libwim.a` が作れることは **CI で確認した** (`tools/fuseshim/fetch_wimlib.sh`。結果は 6 章)。ビルド後に `nm -u libwim.a | grep ' U ntfs_'` が空であることも確認している。
- wimlib の `COPYING` は、「libwim は LGPL-3.0+ でも使えるが、libntfs-3g (GPL) にリンクする場合はそのオプションを選べない。Windows ビルドと `--without-ntfs-3g` の UNIX ビルドは LGPL を選べる」と明記している。
  → **wimmount だけ使うなら `--without-ntfs-3g` でビルドし、LGPL-3.0+ (または GPL-3.0+) として扱える**。

### 4.4 ライセンスの結合 (確認が要る点)

- GPL-2.0-or-later (ntfs-3g、カーネル由来コード) と GPL-3.0-or-later (wimlib の GPL オプション) を**同じ実行体 / .so に結合**した配布物は、両方を満たせるのは GPL-3.0 (以降) の側、という整理が一般的 (GPL-2.0-only は GPL-3.0 と非互換だが、"or later" を持つ部分は 3.0 を選べる)。カーネル由来のコードに **GPL-2.0-only** のファイルがある (docs/licenses.md の squashfs / erofs の行: 「GPL-2.0-only / GPL-2.0-or-later (各ファイルの SPDX)」) ので、**それと wimlib (GPL-3.0+) を 1 つの実行体に結合すると互換にならない可能性**がある。確認が要る。
- **LGPL-3.0 で wimlib を使う**なら、(a) `--without-ntfs-3g` でビルドする、(b) **動的リンクで差し替え可能な別の .so** にする (LGPL は利用者がライブラリを差し替えて再リンクできることを求める。静的リンクの場合は再リンク可能なオブジェクトの提供が要る)。APK に入れる場合、`libwim.so` を単独の共有ライブラリとして同梱し、アプリ本体の .so と別ファイルにするのが素直。差し替えの現実性 (APK の署名、`nativeLibraryDir` は読み取り専用) は**要確認**。別プロセス (3.2 の a / c) にして、本体との接点を RPC だけにすれば、本体は LGPL のコードを「使う別プログラム」として扱える (ただし shim が libfuse の互換層として wimlib と一緒に別プロセス側に入る)。
- **プロセス分離で防げるのは「本体側への波及」という考え方**であって、法的な保証ではない。分離しても:
  - 配布する GPL / LGPL コード**自体**の義務 (ライセンス全文の同梱、対応するソースの入手先の表示 (書面での申し出または URL)、改変部分の公開) は残る。
  - すでに**アプリの native 層の .so には、カーネル由来の GPL-2.0+ のコードが同居している** (ntfs / exfat / ext2 / ext4 / fat / xfs / btrfs / isofs / udf / squashfs / erofs の各ドライバ)。つまり FUSE 層の別プロセス化を決めても、アプリ全体のライセンス方針 (docs/licenses.md の保留項目「自前コードを GPL にするか、プロセス分離するか」「Google Play 配布との両立」) は**別途決める必要がある**。この層の分離は、その保留項目の一部 (本体の `core/` と GPL の `core/compat/` の関係) を解決しない。
- 2 つ目の考慮: 別プロセス方式でも、同じ APK に入れて配布する以上、「集積物 (aggregate)」として扱えるかは、通信の密接さ (RPC の粒度、データ構造の共有) で議論が分かれる。**ソース提供の義務は変わらない**。

### 4.5 配布する場合の義務と docs/licenses.md の更新 (確認が要る点)

- ライセンス全文 (GPL-2.0、GPL-3.0、LGPL-3.0 の該当するもの) をアプリ内 (「ライセンス」画面) と配布物に含める。
- 対応するソースの入手先を表示する (ntfs-3g / wimlib のバージョンと取得 URL・sha256、自前のビルド差分 (shim・io 差し替え・ビルドフラグ・config.h の生成手順))。ビルドに使ったタグ / コミットを固定する。
- 著作権表示・保証なしの表示を残す。
- docs/licenses.md では、ntfs-3g と wimlib は現在「テスト専用 (配布物に含めない・リンクしない)」の扱い (ntfs-3g (mkntfs) / libbde-utils などの行)。**製品に入れる場合は「配布する依存」の行へ移す**: コンポーネント名・用途・ライセンス (上の表)・取り込み方 (CI で取得、無改変、別プロセスか別 .so)・状態 (試作のみ)。この試作の段階ではテスト・測定用 (CI の専用ジョブ) で、**配布物には含めていない**ので、licenses.md には「試作 (fuseshim)」の行を足すに留めた。
- Google Play 配布との両立 (GPL のソース提供方法) は licenses.md の未解決事項のまま。

## 5. 試作の実装・検証・測定 (ntfs-3g)

### 5.1 実装した範囲

```
core/fuseshim/
  fuseshim.h             C API (libfuse のヘッダに依存しない)。BlockDevice 接続・fuse_loop のフック・fuse_operations を呼ぶ関数
  fuseshim_fuse2.c       libfuse 2.x 系の shim (ntfs-3g 用): fuse_mount / fuse_new / fuse_loop / fuse_get_context ...
  ntfs3g_io.c            libntfs-3g の unix_io.c の代わり (BlockDevice へ直結)
  fuseshim_volume.{h,cpp} FuseShimVolume (FsVolume の実装) と FuseShimSession (main を別スレッドで走らせ、fuse_loop に入ったら戻る)
  ntfs3g_session.{h,cpp} open_ntfs3g(BlockDevicePtr): ntfs-3g の main を ntfs3g_main として呼ぶ ("-o ro,no_detach,noatime /dev/null /mnt/...")
  rpc_volume.{h,cpp}     socketpair 越しの RPC (本番の主案の形)
  fuseshim_fuse3.c, wim_session.{h,cpp}   wimlib 用 (6 章)
  fsdump.cpp             測定・比較用の実行体 (自前の NTFS / shim 越しの ntfs-3g / wimlib を同じ walker で)
core/fuseshim.cmake      オプション USBMEM_BUILD_FUSESHIM (既定 OFF)。ntfs-3g のソースは外から渡す (USBMEM_NTFS3G_SRC / _BUILD)
tools/fuseshim/          fetch_ntfs3g.sh / fetch_wimlib.sh (固定バージョン + sha256 検証)、gen_wim_fixture.py
tools/gen_fuseshim_perf_fixture.sh、tools/run_fuseshim_tests.sh、.github/workflows/fuseshim.yml
```

- ntfs-3g のビルド: `configure` (`--with-fuse=internal --disable-plugins --disable-mtab ...`) は **config.h を作るためだけ**に CI で実行し、ビルドは `core/fuseshim.cmake` が行う (libntfs-3g の 26 ファイル (`unix_io.c` を除く) + `ntfs-3g.c` + `ntfs-3g_common.c` + shim)。
  コンパイルフラグだけの差し替え: `ntfs-3g.c` を `-Dmain=ntfs3g_main -Ddaemon=fuseshim_daemon_stub`、全体を `-DDISABLE_PLUGINS=1 -DFUSE_USE_VERSION=26`。**ソースは無改変**。
- ntfs-3g 2022.10.3 の `src/ntfs-3g.c` が呼ぶ fuse_operations (getattr / readdir / open / read / release / readlink / opendir / releasedir / init / destroy) の読み取り経路を `FsVolume` の lookup / stat / readdir / read / readlink に変換する。**statfs / getxattr は shim と FuseShimVolume に実装したが、試験では呼んでいない (未検証)**。
- 同一プロセスでの 2 つの NTFS 実装 (自前 = fs/ntfs + LinuxCompat と libntfs-3g) は **`ntfs_*` のシンボルが大量に衝突する** (`ntfs_attr_lookup`、`ntfs_inode_open` など関数定義の名前の grep で 179 個が両方にある (概算))。
  → 1 つの実行体に入らないので、比較は **別プロセスの出力を diff する**形にした (`usbmem_fsdump_core` / `usbmem_fsdump_ntfs3g`)。**これは同一プロセス方式の欠点の 1 つ** (名前空間の衝突。本番で同じアプリに自前の NTFS と ntfs-3g を入れるなら、別プロセスか、`objcopy --redefine-syms` でのシンボル名の付け替えが要る)。

### 5.2 検証 (自前の NTFS と shim 越しの ntfs-3g の突き合わせ)

同じ NTFS イメージを (a) 自前の NTFS (`NtfsVolume`、LinuxCompat 上のカーネル由来の fs/ntfs) と (b) shim 越しの無改変の ntfs-3g で読み、全ツリーの 行 = 種別・サイズ・mtime (秒)・sha256・パス を `diff` で比較。
データは両方とも **同じ `BlockDevice` (RawImageDevice) から**読む (b は `ntfs3g_io.c` 経由で、ファイルを open(2) していない: 渡したデバイス名は `/dev/null`)。

| イメージ | 内容 | 結果 (run 44189230731) |
|---|---|---|
| 基本フィクスチャ (`tools/gen_ntfs_fixture.sh`、クラスタ 4096) | LZNT1 圧縮ディレクトリ・20 MiB のスパース・3 MiB のファイル (クラスタをまたぐ)・800 ファイルのディレクトリ (INDEX_ALLOCATION)・日本語名・空ファイル。809 ファイル + 4 ディレクトリ | **813 行すべて一致** (名前・種別・サイズ・mtime・sha256)。生成時のマニフェストの 813 エントリとも一致 |
| 同 (クラスタ 2048) | 同上 | 同じく一致 |
| 性能用 (`gen_fuseshim_perf_fixture.sh`、クラスタ 4096) | 256 MiB のファイル・断片化した 2 ファイル (1 MiB ずつ交互に追記)・20000 ファイルのディレクトリ | **20004 行すべて一致** (369 MB を両方で sha256) |
| 別プロセス (socketpair RPC) 越し | 基本フィクスチャ (4096) | 同一プロセスの出力と一致 |

- 一致の意味: 2 つは独立した実装 (カーネルの fs/ntfs の流れと、ntfs-3g の libntfs-3g)。読み取りの意味 (圧縮・スパース・索引・日本語名・断片化) が同じと確認できた。**自前実装の読み取りの独立した確認**にもなる (従来は ntfs-3g (FUSE) が書いた内容をマニフェストの sha256 と照合していた)。
- 比較していないもの: ino の一致 (ダンプに含めていない)、パーミッション・所有者 (ntfs-3g は固定の擬似値)、リパースポイント / シンボリックリンク (フィクスチャに無い。ダンプは `l` 行に対応しているが未検証)、代替データストリーム、xattr、statfs。
- 失敗が見つかったか: 今回の試験で不一致はなかった (NTFS の自前実装の不具合は見つかっていない)。

### 5.3 大きさ (行数)

コメントと空行を除く論理行 (`gcc -fpreprocessed -dD -E -P` で数えた):

| 部品 | 行 (コメント込み / 論理) |
|---|---|
| libfuse 2.x 系 shim (`fuseshim_fuse2.c`) | 292 / 227 |
| libntfs-3g の I/O 差し替え (`ntfs3g_io.c`) | 201 / 150 |
| C API ヘッダ (`fuseshim.h`) | 65 / 32 |
| **ntfs-3g を載せるのに必要な C の合計** | **558 / 約 410** |
| libfuse 3.x 系 shim (`fuseshim_fuse3.c`。wimlib 用。fuse2 と同じ公開 API を共有) | 215 / 168 |
| FsVolume アダプター + セッション (`fuseshim_volume.{h,cpp}`、`ntfs3g_session.cpp`、`wim_session.cpp`) | 348 / 約 270 |
| RPC (`rpc_volume.{h,cpp}`) | 201 / 159 |

比較: `tools/fuse/usbmem_fuse.cpp` (逆向き: FsVolume → FUSE の低水準アダプター) は 978 行。shim は高水準 API の読み取り専用で約 4 割の大きさ。**書き込み**を足すと shim の `fuseshim_*` に create / write / unlink / rename / truncate / mkdir / rmdir / symlink / link / chmod / utimens などの呼び出し口が増え、アダプターの ino → パスの表の更新が要る (+300 行程度の見積もり)。低水準 (lowntfs-3g) は 1.2 章のとおり 3〜4 倍の見積もり。
実行体のサイズ (Release): `usbmem_fsdump_ntfs3g` 約 0.5 MB (自前の NTFS 側の 0.55 MB と同程度)。

### 5.4 性能 (CI の ubuntu-latest の値。**参考程度**: 共有マシンで揺れる。同じイメージを同じ実行体で 2 回ずつ)

イメージは tmpfs (ストレージの遅さは含まない = 層のオーバーヘッドだけを見る)。下位デバイス = `RawImageDevice` の `pread`。

| 項目 | 自前の NTFS | shim + ntfs-3g (同一プロセス) | shim + ntfs-3g + RPC (別プロセス) |
|---|---|---|---|
| 連続読み 256 MiB (1 MiB ごとの read)、3 回の最良 | 2286 / 2262 MB/s | **8008 / 8404 MB/s** | 3569 / 3282 MB/s |
| 下位デバイスへの read 回数 (256 MiB を読む間) | 65536 回 (4 KiB ずつ) | **257 回 (1 MiB ずつ)** | 257 回 |
| 20000 ファイルのディレクトリの readdir | 3.4〜3.5 ms | 4.2 ms | 6.1 ms |
| 20000 個を stat | 35〜37 ms | 49 ms | 204〜234 ms (往復 1 回約 9 µs) |
| 20000 個を lookup (名前引き) | 32 ms | 50 ms | 186〜250 ms |
| 20000 個の小さなファイルを 1 回ずつ読む | 50 ms | 55 ms | 238〜242 ms |
| 3 フェーズで下位デバイスから読んだバイト (stat / lookup / 小ファイルの各 20000 回) | 20 MB + 0 + 0 | **262 MB + 262 MB + 262 MB** | 同左 |
| ピークのメモリ (maxrss) | 約 225 MB (256 MiB のファイルをキャッシュ) | **約 8.5 MB** | 約 7 MB (子プロセス側。親は別) |
| マウント | 0.2〜0.4 ms | 0.5〜0.6 ms | 0.7 ms (fork + マウント) |

読み取りキャッシュ (64 MiB、16 KiB 行 LRU) を下位デバイスの前に置いた実験: ntfs-3g の 3 フェーズの下位読みは 262 MB × 3 → **20 MB + 0 + 0** (自前の NTFS と同じ) になり、時間も stat 36 ms / lookup 28 ms / 小ファイル 34 ms (自前より速い)。自前の NTFS は 4 KiB ごとの読みが 16 KiB 行に切り上げられて read 回数は 1/4 になる。

読み取れること (解釈。**CI の値なので傾向だけ**):

1. **ntfs-3g は大きな連続読みが速い** (1 MiB 単位で下位に直接読み、バッファのコピーが 0 回)。自前の NTFS は LinuxCompat の bh / ページ単位 (4 KiB) で読むので、同じ条件で約 3.5 倍遅い。**これは NTFS の自前実装の性能の発見**: USB の実機では `ReadAheadDevice` (core/include/usbmem/devices.h) のようなまとめ読みの層を挟む前提だが (NtfsVolume の下にそれが常に入るかは未確認)、NtfsVolume の下位への読み方が 4 KiB 単位になっている点は、実機での連続読みの性能を見る価値がある (別件。TODO に追記した)。
2. **ntfs-3g はパスを引くたびに索引ブロックを読み直す** (自前のキャッシュが小さく、OS のページキャッシュに頼る設計)。20000 回のパス解決で 262 MB を下位から読む。tmpfs では目立たないが、**実機の USB (ページキャッシュなし) では致命的**になりうる → 下位デバイスの前に**読み取りキャッシュ層が必須** (上の実験で解消。本番で実装する)。FsVolume 側の `lookup(dir, name)` の形ではなく、高水準 API の「パス全体」を毎回引く構造が原因。低水準 (lowntfs-3g、ino 単位) なら軽減される見込み (未測定)。
3. **層そのもののオーバーヘッドは小さい**: 同一プロセスの shim のオーバーヘッドは、ntfs-3g の処理に比べて無視できる (lookup / stat は mutex + 文字列の連結 + `struct stat` の詰め替え)。
4. **別プロセス (RPC) の増分**: 連続読みは約 8.2 → 3.3〜3.6 GB/s (2 回のコピーとコンテキストスイッチ)。USB ストレージの帯域 (数十〜数百 MB/s) より 1 桁以上大きいので、**帯域は問題にならない見込み**。メタデータは 1 往復約 9 µs の増分 (20000 回で +150〜190 ms)。ファイルマネージャのディレクトリ表示 (数百件) なら数 ms。
5. **余計なコピーの回数 (設計上)**: 読み (read): 同一プロセス = 0 回 (BlockDevice が ntfs-3g の呼び出し元のバッファへ直接読む。shim は `buf` をそのまま渡す)。別プロセス = 2 回 (子の送信 + 親の受信。親は呼び出し元のバッファに直接受ける)。
   列挙 (readdir): ntfs-3g の filler → `vector<FsDirEntry>` で名前 1 回。RPC では本体 + 1 回。実際の FUSE (/dev/fuse) 経由に比べると、カーネル往復 (コンテキストスイッチ 2 回 + ページ越しのコピー 2 回) が無い。

### 5.5 動かなかったもの / 未検証 (正直に)

- **2 回目のマウント (同一プロセス) は未検証**: ntfs-3g.c の `static` のグローバルは終了時に一部が解放され、再実行で壊れる可能性がある (3 章)。別プロセス方式なら問題にならない。
- 書き込み (rw マウント): shim と ntfs3g_io.c は write 経路を持つが、FuseShimVolume は読み取り専用。ntfs-3g の書き込みの突き合わせは 8 章。
- lowntfs-3g (低水準)、ntfs-3g のプラグイン (リパースポイントの dlopen。`DISABLE_PLUGINS` で無効にした)、`-o` の他のオプション (`streams_interface`、`show_sys_files`、ユーザーマッピング)、パーミッション (`POSIXACLS` なしのビルド)。
- Android (NDK / bionic) でのビルドと実行、AVD での試験、`:service` プロセス / exec の実測 (3.2 章は調査のみ)。ntfs-3g の config.h は Linux / glibc の configure の結果で、bionic では別途必要 (起こりうる差: `getopt_long` / `optind` の再初期化 (`optreset`)、`daemon()` の有無、`<sys/xattr.h>`)。
- **ntfs-3g の配布元 (tuxera.com) は GitHub の runner の既定の User-Agent に 403 を返した** (初回の run 44188036910)。User-Agent を付けると取得できた。配布元の都合で変わりうるので、取得先は 2 つ (配布元と Debian の orig tarball) を用意した (どちらも固定の sha256)。Debian の方は未試験 (configure が無く autoreconf が要る)。

## 6. wimlib の wimmount

**動いた (読み取り専用)**。無改変の wimlib 1.14.4 (`--without-ntfs-3g --with-fuse`、`libwim.a` だけをビルド) の `wimlib_mount_image()` を、libfuse **3.x 系**の shim (`fuseshim_fuse3.c`。`fuse_main_real_versioned` / `fuse_get_context` / `fuse_exit` の 3 関数) の上で動かし、同じ `FuseShimVolume` で読んだ。

- 取得とビルド: `tools/fuseshim/fetch_wimlib.sh` (sha256 検証。`./configure --without-ntfs-3g --with-fuse --disable-shared --enable-static` + `make libwim.la`)。config.h は `WITH_FUSE 1` / `WITH_NTFS_3G` 未定義。
  **`nm -u libwim.a` に `ntfs_` のシンボルが無い** (libntfs-3g に依存しない) ことと、libfuse の未解決シンボルが `fuse_exit` / `fuse_get_context` / `fuse_main_real_versioned` の 3 つだけであることを CI で確認した。libfuse3 のライブラリはリンクしていない (ヘッダだけ `libfuse3-dev`)。
- 試験: 同じ元ツリー (809 ファイル + 4 ディレクトリ。日本語名・空白入りの名前・空ファイル・3 MiB の圧縮できるファイル・30 万バイトの乱数・32 MiB のファイル・800 ファイルのディレクトリ) から `wimlib-imagex capture` で作った 3 種の WIM を読んで、元のツリーの (名前・種別・サイズ・sha256) と比較。
  **LZX 圧縮 / 無圧縮 / LZMS (solid) のすべてで 813 エントリ一致**。別プロセス (RPC) 越しの読みも一致。
- 性能 (CI): 32 MiB のファイルの連続読み: 無圧縮 6.6〜8.4 GB/s、LZX 約 1.27 GB/s (圧縮率の高い単語の繰り返しの文書なので展開が速い。実データでは遅くなる)。800 ファイルの stat / lookup は 0.1〜0.4 ms。**LZMS の solid WIM は全ツリーの読みに 46.8 秒** (wimlib が警告する「solid は遅い」: ファイル 1 つを読むたびに solid ブロックの先頭から展開しうる)。実データの solid WIM を開くと遅い、という実用上の制約。
- **読み取り専用では staging ディレクトリは作られない**: `make_staging_dir` は `WIMLIB_MOUNT_FLAG_READWRITE` のときだけ呼ばれる (mount_image.c)。書き込みマウントでは、変更したファイルの中身を staging ディレクトリ (既定は WIM と同じディレクトリの `.wimlib-stage-dir-*`) に**一時コピーとして書き出し**、アンマウント時に WIM へコミット (新しい WIM を書く) する。
  → **「一時コピーしない」方針とは両立しない** (書き込み = 一時ファイル + WIM の書き換えになる)。したがって wimmount の書き込みは載せない。**WIM は読み取り専用で中身を見る / 取り出す用途に限る**。
- 足りないもの (本番化): (1) WIM ファイルを実在のパスで渡す必要がある (2 章)。WIM が USB の FS の中にあるなら、open / pread の差し替え (または wimlib のファイル I/O 層の小さなパッチ) か一時コピーが要る。(2) 名前付きデータストリーム (`STREAM_INTERFACE_XATTR` / `WINDOWS`) は `STREAM_INTERFACE_NONE` で無効にした。(3) UNIX データ (`WIMLIB_MOUNT_FLAG_UNIX_DATA`)・シンボリックリンク (reparse)・ハードリンクの扱いは試験していない。(4) wimlib は POSIX メッセージキュー (`mq_open`。別プロセスからのアンマウント・進捗通知用。shim が `fuse_exit` を直接受けるので使わない) を `mount_image.c` で使う。Android の bionic には `mqueue.h` が無いと理解している (**未確認**) ので、Android 向けには mqueue を使う部分をビルドから外す必要がありうる。

## 7. 本番化の見積もりと推奨方針

**推奨: 別プロセス (RPC) を主案** (ユーザー決定。3 章)。最初の実装は Android の `:service` プロセスに shim + FUSE プログラムを載せる形 (3.2 章 a)。

見積もり (**大まかな目安。実機・AVD での検証を含まない**):

| 作業 | 目安 |
|---|---|
| shim の製品化 (自前の `fuse.h`、エラー処理、ログ、スレッド / 直列化、ホストアプリ側の API の整理) と RPC の拡張 (statfs / getxattr / 要求 ID / 巨大ディレクトリの分割 / プロセス死亡の扱い) | 1〜2 週間 |
| Android でのビルド (NDK で ntfs-3g / wimlib をクロスコンパイル。手書きの config.h、bionic の差分、mqueue の除外) と `:service` + ParcelFileDescriptor の結線、AVD の計装テスト | 1〜2 週間 |
| 読み取りキャッシュ層 (5.4 章の問題 2) と、本体の SAF / DocumentsProvider への FsVolume としての接続 (既存の `VolumeOps` 相当の経路に載せる) | 約 1 週間 |
| ライセンス表示 (アプリ内画面・ソース入手先)、docs/licenses.md の更新、法的確認 | 作業 +確認 (外部の判断が要る) |
| 書き込み対応 (ntfs-3g の rw) | **推奨しない** (下記) |

載せる価値のある FS:

- **wimlib の WIM (読み取り専用)**: 自前実装が無く、Windows のインストールメディア (install.wim / boot.wim) を見る用途がある。**最有力**。LGPL-3.0+ の選択のため `--without-ntfs-3g` でビルドできる (4.3 章)。solid の遅さと、WIM ファイルの渡し方 (6 章) が課題。
- **ntfs-3g**: 製品の読み書き経路には**載せる価値が低い**。自前実装が既にあり、読み取りは一致が確認でき、書き込みの安全方針 (未再生ログ / 休止状態は書かない。docs/dirty-volumes.md) が ntfs-3g の既定と一致しない (ntfs-3g の既定は `recover` = `$LogFile` を消して書き始める。man ntfs-3g の記述。休止状態は rw を拒否して読み取り専用に落とす)。GPL-2.0+ をアプリに追加する負担もある。
  ただし**テスト・診断の道具**としては価値が大きい (8 章)。ホストの CI は既に FUSE 版の ntfs-3g (apt) を参照実装に使っているので、shim は「FUSE の無い環境 (Android の AVD の計装テストなど) で参照実装を動かす」必要が出たときの手段。
- その他 (squashfuse / fuse-ext2 / dislocker): squashfs / ext2-4 / BitLocker は**すでにカーネル由来または自前の実装がある**ので不要。新しい FS (例: ZFS の一部、HFS+ / APFS の読み取り (apfs-fuse / hfsfuse)、Bcachefs など) で FUSE の実装だけがあるものが候補。**shim を製品に入れるかどうかは、そういう FS の需要次第**。
- 将来の CIFS / SMB (TODO の別項目): smbnetfs 系の FUSE プログラムを載せるより、libsmb2 を使った `VolumeOps` 実装を直接書くほうが小さい (shim の層が要らない)。

## 8. ntfs-3g は NTFS の書き込みの突き合わせの参照実装に使えるか

使える。**すでに使っている** (host-tests / Android CI は FUSE 版の ntfs-3g (apt の ntfs-3g) で、自前の書き込み結果を読み直して全ツリーを比較し、`ntfsfix -n` で整合を見る: docs/ci.md の「ntfs-3g の参照との比較」)。shim が足すのは、**FUSE の無い環境で同じプロセス内 / 同じ BlockDevice の上で**参照実装を動かせる点: (1) 自前の実装が書いたイメージを、BlockDevice 越しに ntfs-3g で読み直して突き合わせる (今回の試作の逆の向き = 今回の試験で既に双方向に読み合える状態になった)、
(2) 書き込み経路 (`write_at` を結んである) を足せば、同じ操作列を自前の実装と ntfs-3g に流して、結果のイメージのツリー・`$MFT` の状態を比較する「差分テスト」ができる (モデル検査より安い独立検証)。
注意点: (a) ntfs-3g の方針は自前と違う (未再生ログ → 既定は `recover` で `$LogFile` を消して書き始める (自前は書かない)。ダーティフラグを扱う。タイムスタンプの更新 (atime の相対更新)・Windows 互換の名前の扱いなど) ので、同じ操作の結果が**バイト一致する**とは限らず、「ツリーと内容の一致」「fsck (ntfsfix -n / chkdsk の代わり) の合格」までを比較の基準にする。
(b) 同一プロセスは 1 回しかマウントできない (3 章) ので、テストでは操作ごとに別プロセス (別プロセス方式) か、`fork` で回す。(c) GPL なので、テスト用の実行体として配布物に含めない限り問題はない (CI の専用ジョブ)。

## 9. 結果 (CI)

- ブランチ `usbmem-fuseshim-ntfs3g` (`usbmem-fuseshim*` への push で `.github/workflows/fuseshim.yml` が動く)。専用ジョブ (約 10 分: 取得・ビルド・フィクスチャ生成・比較・測定)。
- 初回 run 44188036910: 失敗 (tuxera.com が 403)。User-Agent を付けて取得元を 2 つに (run 44188363565 で ntfs-3g の取得・ビルド・比較が通った)。
- **run 44189230731 (最終構成: ntfs-3g + 別プロセス RPC + wimlib + キャッシュ実験): 成功**。上記 5・6 章の数字はこの run のもの (1 つ前の run 44188363565 の自前の NTFS の連続読みは 1891 / 1935 MB/s、ntfs-3g は 9156 / 9133 MB/s で、run 間で約 20% 揺れる)。
- 再現: `git push <リポジトリ> HEAD:refs/heads/usbmem-fuseshim-<名前>`。ローカルでは動かさない (大きなビルド・FUSE のマウントが要る)。

## 10. 製品化 (個人版のみ): wimlib の WIM を別プロセスで読む (Android の実測)

ユーザー決定: FUSE 層 (wimlib → 後で ntfs-3g) は**別プロセス**で動かし、本体とは RPC (列挙 / getattr / read / readlink / statfs) だけで話す。GPL / LGPL のコードを含むので**個人版のみ**の機能 (公開版には `core/fuseshim/` ごと入れない)。
コードは全部 `core/fuseshim/wimfuse/` (C/C++) と `android/app/.../wimfuse/` (Kotlin) と `tools/wimfuse/` にあり、既定のビルドと `USBMEM_BUILD_FUSESHIM` (既定 OFF) には影響しない。アプリ側は別のスイッチ (Gradle の `-PusbmemWimFuse=<ビルド出力>`、CMake の `USBMEM_WIMFUSE`) が入ったときだけ有効で、無ければ WIM の項目は出ない (`WimFuse.available()` が false)。

### 10.1 構成

```
アプリ本体 (org.usbmem.app)                 :wimfuse プロセス (Service)             子プロセス (libwimfuse.so。exec)
 VolumeRegistry.mountWim ─ JNI ─ RpcVolume ──(abstract UNIX ソケットで直結)─────────  wimlib (wimmount) ─ fuseshim (libfuse3 互換)
   ↑ SAF / UI は既存の経路                  ProcessBuilder で exec (監督だけ)            ↑ open/pread を RPC の callback に差し替え
 WIM ファイルの読み = 本体の BlockDevice  ←── kind=1 の callback フレーム (SrcSize / SrcRead) ←── WIM の読み
```

- **子は `libwimfuse.so` という名前の PIE の実行ファイル** (`jniLibs/<abi>/`、`packaging.jniLibs.useLegacyPackaging = true` = `extractNativeLibs=true`)。`nativeLibraryDir` から `ProcessBuilder` で exec する (W^X を避ける定番の形)。`tools/wimfuse/build_android.sh` が NDK (r30、API 26、arm64-v8a / x86_64、16 KiB ページ整列) でビルドする。wimlib 1.14.4 は `tools/fuseshim/wimlib_pin.sh` の URL・sha256 で取得し、**ソースは無改変** (bionic の足りない物 = `mqueue.h` / `glob` (API 28 以降) は代用ヘッダ + スタブで補う。mq は unmount / commit 用で読み取りには使わない)。
- **`:wimfuse` プロセスの Service は子の起動と監督 (alive / exit / kill / 出力の末尾) だけ**。Binder の `onTransact` で受ける。子へのデータは通らない: 本体が abstract 名前空間の UNIX ソケットを待ち受け、子がそこへつなぎに来る (`SO_PEERCRED` の uid を確認)。ソケットは `ParcelFileDescriptor.dup` + `detachFd` でネイティブへ渡し、既存の socketpair RPC (`rpc_volume.*`) をそのまま使う。
- **RPC の拡張** (`rpc_volume.*` は共有物なので最小限): 応答に `kind` (0 = 応答、1 = 子 → 本体の callback) を足した。WIM の読みは子 → 本体の callback (`SrcSize` / `SrcRead`) で、本体の `BlockDevice` (USB の fd / SAF の fd / ネストした親) から読む。子の `open` / `close` / `pread` / `read` / `lseek` / `fstat` / `realpath` はリンク時の `--wrap` で差し替え、特別なパス `usbmem-rpc://wim` を仮想の fd (`/dev/null` を開いて得た番号) に結ぶ。
- **子の終了**: (1) RPC の EOF (本体がソケットを閉じた = ボリュームを閉じた、本体が死んだ) で `rpc_serve` が戻り、`alarm(10)` の保険つきで exit する、(2) `PR_SET_PDEATHSIG(SIGKILL)` (`:wimfuse` プロセスが死んだら子も死ぬ。PDEATHSIG は**起こしたスレッド**に結びつくので、Service は専用の `HandlerThread` から起こす)。
- **イメージごとに 1 プロセス**: wimlib は 1 プロセスで 1 イメージしかマウントしない (グローバル状態・`exit()`、3 章)。`WimVolume` (`core/fuseshim/wimfuse/wim_volume.*`) が、ルート直下に `1 - 名前` / `2 - 名前` のフォルダを並べ、**フォルダを最初に開いたときにそのイメージの子を起こす** (一覧を見るだけでは起こさない)。ino は `(イメージ番号 << 40) | 子の ino` で名前空間を分ける。
- **キャッシュ**: 子の中の WIM の読み (callback) に、1 MiB ブロックの LRU (既定 32 MiB。`--cache-mb`) を置いた。wimlib は 32 KiB のチャンクを小さく何度も読むので、本体への往復を減らす (10.4)。
- **ネスト経路への接続** (段階 B): `VolumeRegistry.inspectWim` (先頭 8 バイトが `MSWIM\0\0\0` なら WIM。子を 1 つ起こして一覧を読み、すぐ終わらせる) と `mountWim` (イメージ番号を指定 = そのイメージがルート、`null` = イメージごとのフォルダ)。閉じる順は 内 → 外 (子プロセス → デバイス → 親のピン解除)。UI は、ネストのマウントの画面でファイルが WIM だと、パーティション表の代わりにイメージの一覧と「読み取り専用で公開」ボタンを出す。**読み取り専用で、一時コピーは作らない** (CI で確認)。

### 10.2 段階 A の結果 (AVD API 34 x86_64、CI の `wim-fuse` グループ、`usbmem-ci-only-wim-fuse`)

| 項目 | 結果 |
|---|---|
| ビルド | arm64-v8a / x86_64 とも NDK で無改変の wimlib 1.14.4 + 自前コードがビルドでき、`libwimfuse.so` は PIE (`/system/bin/linker64`)、NEEDED は libc / libm / libdl のみ。ビルド約 3 分 (run 44904471250) |
| exec (W^X) | **成立**: `nativeLibraryDir` (`/data/app/~~.../lib/x86_64`) の `libwimfuse.so` を `ProcessBuilder` で exec して `--version` が出た (`wimfuse wimlib 1.14.4`)。`useLegacyPackaging = true` (= `extractNativeLibs=true`) が必要 (APK 内のままでは exec できない)。実機では未確認 (AVD のみ) |
| `:service` | **成立**: アプリ pid と `:wimfuse` pid が別 (`app=4238 service=4423`)、子の PPid は `:wimfuse` (`child_ppid=4423`)。DIRECT (本体プロセスから直接 exec) でも同じ結果 |
| 一致 | 小さな WIM の LZX / 無圧縮 (813 エントリ、37 MB) の全ツリー (名前・種別・サイズ・sha256) が元のツリーと一致。LZMS (solid) の一部の読みも sha256 一致 |
| 起動時間 | spawn 2〜22 ms + 接続 13〜26 ms + マウント 19〜63 ms = 開くまで 約 25〜115 ms (`open_*_ms`。run によって揺れる)。イメージ一覧だけ (`info`) は 19〜390 ms |
| 子のメモリ | VmHWM: LZX マウント直後 6.3 MB、全ツリー読み後 10.5 MB、無圧縮 39 MB (チャンクの読みの大きさ) |
| スループット | 32 MiB の連続読み (1 ファイル): LZX 180〜800 MiB/s、無圧縮 314〜604 MiB/s (run 間で大きく揺れる)。全ツリー (813 ファイル・37 MB) の読み: 1.8〜2.3 s (LZX) / 1.7〜1.8 s (無圧縮) = 15〜21 MiB/s (ファイルごとの往復が支配) |
| 後始末 | ボリュームを閉じる / 本体側の RPC が EOF / `:wimfuse` が死ぬ / 閉じずにアプリが終わる、のどれでも子は残らない (`ps` で 0。CI の判定)。子の fd は pipe 2 + ソケット + `/dev/null` だけ (ソケットの fd 以外の USB の fd は渡さない) |
| 子が死んだあと | 本体の読みは `EPIPE` (32) で失敗し、本体は落ちない |
| 存在しない画像番号 | 子が終了し、本体は errno 5 と子のエラー文で拒否 (何も登録されない) |

### 10.3 段階 B の結果 (ボリューム API への接続・読み取り専用)

`WimNestedTest` (4 件) と `WimFuseTest` の `archive_*` が成功 (run 44906971508、52 件全部成功):

- `inspectWim`: 2 イメージの WIM は一覧 (`first` / `second image`) が返り、WIM でないファイル (`plain.txt`) は null (パーティション表の経路へ)。一覧用の子は読んだらすぐ終わる (`ps` で 0)。
- `mountWim` (イメージごとのフォルダ): ルートの一覧 (`1 - first` / `2 - second image`) だけでは子が 0、フォルダを開く / ファイルを読むと**そのイメージの子だけ**が 1 つずつ増える (遅延起動)。同名のファイル (`same.txt`) がイメージごとに別の中身で返る (ino の取り違えなし)。書き込み (`create`) は拒否。**SAF のプロバイダ経由** (`DocumentsContract` の document URI → `openInputStream`) でも読める。
- 一時コピーなし: 公開の前後で、アプリが書ける場所 (`cacheDir` / `filesDir` / `codeCacheDir` / `externalCacheDir` / `java.io.tmpdir`) のファイルの一覧とサイズが**変わらない**。
- 閉じる順: 親を閉じると子の公開、そのイメージの子プロセスが全部終わる (0)。1 イメージだけの公開 (`image = 2`) は、そのイメージの中身がルート。存在しない画像番号は拒否され、何も登録されない。
- 2 イメージの WIM の全ツリー (12 エントリ、フォルダ込み) が期待と一致。

### 10.4 段階 C の結果 (性能・堅牢性)

- **読みキャッシュ**: 1 MiB × 32 の LRU。3 MiB のファイル (64 KiB ずつ) + 200 個の小さいファイルの読みで、本体への callback は **355 回 → 2 回** (cache_hit 353、miss 2)。ただし時間は 0.05〜0.36 s → 0.06〜0.43 s で、AVD では差が出ない (揺れのほうが大きい) (callback が同一マシン内で安いため)。**実機 USB (1 往復が 100 µs〜数 ms) では効く見込みだが未測定** (実機 USB は使っていない)。
- **LZMS solid** (小さい 4 MB の WIM から 44 ファイルを読む): キャッシュなし 6.5〜10.0 s / あり 6.7〜8.4 s で**キャッシュは効かない**。LZMS の遅さは本体への読みではなく **wimlib の再展開** (同じブロックを読むたびに展開する) が支配しており、読みキャッシュでは減らない。展開済みデータのキャッシュ (wimlib の外) か、solid ブロックを一度ファイルへ展開する (一時コピーになる) しか手が無い。→ 10.7 の未了。
- **堅牢性**: 子が死んだあとの読みは EPIPE、プロセス全体の後始末は 10.2 の表の通り。

### 10.5 段階 D: ntfs-3g (結論: 製品の経路には載せない)

製品の経路に載せるのは**やめる** (5 章・7 章の結論のまま): 自前の NTFS (読み書き) が既にあること、書き込みの安全方針が自前と違うこと (ntfs-3g の `recover` は `$LogFile` を消す。docs/dirty-volumes.md)、GPL-2.0+ の負担が増えること、ntfs-3g は索引ブロックの読み直しが多く USB では下位キャッシュが必須なこと。**テスト・診断の参照実装としてだけ**残す (8 章。FUSE の無い AVD で参照実装を動かしたいときに、この別プロセスの仕組み (`libwimfuse.so` の ntfs-3g 版) を流用できる。今回は作っていない)。

### 10.6 CI の run

| run | ブランチ | 内容 | 結果 |
|---|---|---|---|
| 44904303267 | `usbmem-wimfuse*` | `libwimfuse.so` のビルド (1 回目) | 失敗: bionic の `glob` は API 28 以降 (wimlib の `reference.c` が使う) → 代用ヘッダ + スタブで解消 |
| 44904471250 | 同上 | ビルド (arm64-v8a / x86_64) | 成功 (約 3 分) |
| 44904811545 | `usbmem-ci-only-wim-fuse` | 1 回目の AVD | 失敗: Kotlin のコンパイル (`Process.pid()` が解決できない) → `toString()` の `pid=` を読む形に |
| 44905263884 | 同上 | 2 回目 | 計装テストまで行き、約 46 件中 2 件が `DeadObjectException`: サービスを強制終了する試験のあと、死んだ Binder を再利用していた → `isBinderAlive` で接続を張り直す |
| 44906174728 | 同上 | 3 回目 | 47 件成功、skip 1 (アプリ終了の試験が通常の実行で skip) → 通常の実行では何もせず戻る形に |
| **44906971508** | 同上 | 4 回目 (段階 A / B / C のテスト全部) | **成功 52 件** (約 7 分。`WimFuseTest` + `WimNestedTest`、子の残りは 2 回の確認とも 0) |

### 10.7 未了・注意

- 実機 (実 USB・実機の SELinux・arm64 の実行) では確かめていない。AVD (x86_64、API 34) のみ。arm64-v8a は**ビルドだけ**確認 (実行していない)。
- ライセンス: プロセスを分けても、**ソースの提供・ライセンス文の表示の義務は消えない** (4 章)。`docs/licenses.md` は編集版の担当が持つので、ここでは触れていない。個人版のビルドにだけ `libwimfuse.so` が入る。
- 書き込み (WIM の編集) は対象外 (読み取り専用)。
- LZMS solid の再展開 (10.4)。巨大な WIM (数 GB) の実測はしていない。
- `wim-fuse` グループは `android-emulator.yml` の既定のグループには入れていない (個人版の統合時に決める)。
