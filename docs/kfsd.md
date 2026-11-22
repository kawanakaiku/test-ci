# kfsd: カーネル由来 (GPL) のファイルシステム・ドライバを別プロセスに分ける (2026-11-22〜)

**この文書は法的な助言ではない。** ユーザーの決定: カーネル由来 (GPL) の FS ドライバ (NTFS / exFAT / FAT / ext2-4 / ISO 9660 / UDF / squashfs / erofs、必要なら Btrfs / XFS) を**別プロセス (別の実行ファイル `kfsd`)** に分け、
公開版 (Apache-2.0) の本体とは **RPC だけで通信**させる。目的は、公開版の本体に GPL のコードを一切含めず、GPL の部分 (kfsd の実行ファイルとそのソース) を**別に公開**できるようにすること。
別プロセスにしても、kfsd 自身のソース提供・ライセンス表示の義務は残る (承知済み。8 章)。**この作業で公開 (push・可視性の変更) はしていない。**

- 関連: docs/editions.md (公開版 / 個人版)、docs/licenses.md、docs/fuse-layer.md (別プロセス + RPC の試作。kfsd の RPC はこの試作を一般化したもの)、docs/nested-mount.md
- コード: `core/kfsd/` (プロトコル・クライアント・サーバ共通部 = Apache-2.0、kfsd の実行ファイル = GPL-2.0-or-later)、`core/CMakeLists.txt` の `USBMEM_BUILD_KFSD`
- 既存の `core/fuseshim/rpc_volume.*` (ntfs-3g / wimlib の試作用の読み取り専用 RPC) は**そのまま残す** (名前空間が違うので衝突しない)。kfsd は書き込み・デバイス callback・ページングを持つ別物。

## 1. 構成

```
[アプリ本体のプロセス (Apache-2.0)]                                        [kfsd (使い捨て、1 ボリューム 1 プロセス。GPL-2.0-or-later)]
 VolumeRegistry / SAF / JNI                                                 kfsd_main → serve() → FsVolume (NtfsVolume / Ext4Volume / FatVolume ...)
   └ FsVolume = RpcVolume ──── socketpair (SOCK_STREAM, fd 3) ────────→        └ LinuxCompat + カーネル由来のドライバ
        ↑ BlockDevice (USB・暗号・パーティション・ネスト) ←── Dev* callback ──      └ RemoteBlockDevice (読み書きを本体へ頼む)
```

- **ブロックデバイスは本体が持つ** (USB の fd・BitLocker / LUKS の復号・パーティションの切り出し・ネストの親ボリュームは本体の `BlockDevice` のまま)。kfsd は USB に一切触れず、`kDevRead / kDevWrite / kDevFlush` で本体へ頼む。
  → 暗号ボリュームやネストマウント (親が別の kfsd の RpcVolume でもよい) の上の FS も、そのまま動く。kfsd にネットワーク・ファイル・USB の権限は要らない。
- **FS の種類の判別 (`mount_auto` 相当) も kfsd 側** (`kfsd_mount.cpp`)。本体は「このデバイスをマウントして」と頼むだけ。本体側が持つ `probe_filesystem` (Apache-2.0) は、UI の一覧用に残る。
- **1 ボリュームにつき 1 プロセス**: ボリュームを閉じたらプロセスごと捨てる。ntfs-3g のような「1 回限りの main」「グローバル状態」「exit()」の問題 (docs/fuse-layer.md 3 章) も、ドライバの OOM・クラッシュも、本体へ波及しない。
- 同じ `kfsd` のバイナリを、(a) 個人版: アプリの APK 内 (`libkfsd.so`、nativeLibraryDir から exec) と、(b) 公開版: 別の「プラグイン APK」(GPL-2.0。ソースは別に公開) の両方で使う (6 章)。

## 2. プロセス間のプロトコル (`core/kfsd/include/usbmem/kfsd_proto.h`。Apache-2.0)

### 2.1 メッセージの形

1 本の `SOCK_STREAM` (socketpair) に、`Hdr` (40 バイト、リトルエンディアン) + payload (`Hdr.len` バイト) を並べる。

| フィールド | 意味 |
|---|---|
| `op` (u32) | 操作 (2.3 章) |
| `len` (u32) | payload のバイト数。`kMaxPayload` = 8 MiB を超えたら受信側は接続を切る |
| `seq` (u64) | 要求の通し番号 (本体が付ける)。Reply は要求の seq を返す (ずれたらプロトコル違反) |
| `a` `b` `c` (u64) | 引数 (op ごと)。Reply では `a` = rc (0 以上 = 成功 / 負 = `-errno`)、`b` `c` = 追加の戻り値 |

- **同期・1 要求ずつ**: 本体は要求を送り、Reply が来るまで待つ。kfsd は 1 つの要求の処理の間に、本体へ **Dev* の callback** (ブロックデバイスの読み書き) を何度でも送れる。本体は callback に Reply で答え、本来の Reply を待ち続ける。
  → ソケット上で同時に進む要求は常に 1 つ (入れ子になるだけで並行しない)。要求 ID によるパイプライン化はしない (FS ドライバは単一スレッド前提で、LinuxCompat は kthread を持たないため。ボリューム単位の mutex は本体の `LockedFsVolume` が持つ)。
- **ハンドルを持たない (ステートレス)**: `FsVolume` の `ino` (u64) をそのまま使う。ino は kfsd のプロセスの寿命の間だけ有効 (exFAT のように実行時採番のものも同じ)。kfsd が死んだら全 ino が無効になるので、本体は開き直す (=再マウント)。
  open / close / ファイルハンドルは無い (`FsVolume` に無い)。
- **エラーは負の errno**。通信の失敗は本体が errno に変える: kfsd が死んだ / 閉じた = `-EPIPE`、無応答 = `-ETIMEDOUT`、不正な応答 = `-EPROTO` (どれも、以後その RpcVolume は `-EPIPE`)。**本体は落ちない** (SIGPIPE は `MSG_NOSIGNAL` で避ける)。
- **名前**: 空・`/`・NUL を含む・4096 バイト超は受け付けない (本体は送る前に `-EINVAL`、kfsd から来た名前もこれで検査)。パスは本体が要素ごとに `lookup` で辿る (`FsVolume::resolve`)。
- **バージョン**: `kHello` で magic (`KFS1`) と version (1) を交換する。合わなければ接続を切る。構造体を拡張するときは version を上げる (互換のための余白 `reserved` を各所に置いた)。

### 2.2 大きな read / write、readdir

- **read / write / Dev\* は 1 メッセージ最大 `kMaxIo` = 4 MiB**。それより大きな `read(ino, off, buf, len)` は本体の `RpcVolume` が 4 MiB ずつに分けて送り、短い読み (EOF) で止める。メッセージの中身は分割も圧縮もせず 1 回の `sendmsg` (Hdr + payload の 2 つの iovec) で送る (大きな転送は socket の往復を減らすのが効く: 4 章)。
- **readdir はページング**: `kReaddir(dir, cookie, flags)`。最初 (cookie 0) に kfsd が `FsVolume::readdir` で全部取り、約 256 KiB ずつ返す (`b` = 次の cookie、`c` = 1 なら続きあり)。続きは kfsd が持つ一覧から返す (一覧が失われたら取り直す)。
  cookie は必ず増える (増えない応答は不正 = `-EPROTO`)。変更系の操作で kfsd は途中の一覧を捨てる。
- `flags` に `kReaddirWithStat` を立てると、各エントリに `WireStat` を付けて返す (lookup + stat の往復を省く)。本体の `MountOptions::prefetch_stat` で有効にし、続く `stat()` をキャッシュで答える (変更系の呼び出しで捨てる。kfsd は 1 ボリュームを占有するので、他の書き手が無く、一貫性は保てる)。
  → 効果は 4 章で測る。

### 2.3 操作 (`Op`)

| op | 引数 (a / b / c / payload) | Reply |
|---|---|---|
| `kHello` | payload = `HelloMsg` (magic / version / max_payload) | `HelloMsg` (kfsd 側) |
| `kMount` | payload = `MountReq` (flags / block_size / dev_size / tz / dev_read_only) | `MountRep` (`InfoRep` + 判別した FS 種別)。失敗は rc = -errno で、kfsd は終了コード 3 で終わる |
| `kLookup` | a = dir / 名前 | b = ino |
| `kStat` | a = ino | `WireStat` |
| `kReaddir` | a = dir / b = cookie / c = flags | `WireDirentHead + 名前 (+ WireStat)` の列、b = 次の cookie、c = 続きあり |
| `kRead` | a = ino / b = offset / c = len (<= 4 MiB) | a = 読めたバイト数 (0 = EOF)、payload = データ |
| `kReadlink` | a = ino | payload = リンク先 |
| `kSync` | — | rc |
| `kCreate` `kMkdir` | a = dir / b = mode / 名前 | b = ino |
| `kUnlink` `kRmdir` | a = dir / 名前 | rc |
| `kRename` | a = old_dir / b = new_dir / payload = u32 len ×2 + 名前 ×2 | rc |
| `kWrite` `kWriteInplace` | a = ino / b = offset / payload = データ | a = 書けたバイト数 |
| `kTruncate` | a = ino / b = size | rc |
| `kSymlink` | a = dir / payload = u32 name_len + 名前 + リンク先 | b = ino |
| `kLink` | a = dir / b = 対象 ino / 名前 | rc |
| `kInfo` | — | `InfoRep` (writable / state / root / fs_name)。`writable()` と `state()` は変わりうる (エラーで読み取り専用に落ちるなど) ので毎回問い合わせる |
| `kStats` | — | `StatsRep` (kfsd が本体へ頼んだデバイスの読み書きの回数とバイト数、要求数) |
| `kClose` | — | 正常なアンマウント (`FsVolume` の破棄 = NTFS はダーティフラグを下ろす、ext4 はジャーナルを閉じる、FAT / exFAT は FAT を書き戻す) をして rc を返し、kfsd は終了コード 0 で終わる |
| `kDevRead` `kDevWrite` `kDevFlush` (kfsd → 本体) | a = offset / b = len (<= 4 MiB) / payload | a = 0 / -errno、読みは payload = データ (短い読みは無い) |

`FsVolume` の約 25 の仮想関数のうち、`fs_name` と `root` はマウント時に受け取って本体が持つ。それ以外はすべて上の op に 1 対 1 で対応する (`resolve` は本体側の lookup の繰り返し)。

### 2.4 マウントのオプション

本体の `MountOptions`: `rw` (書き込み可を要求。ドライバが書けないと判断したら読み取り専用に落ち、`writable()` で分かる)、`allow_noload` (ext4 / Btrfs / F2FS / XFS のジャーナル未再生を、利用者が確認したうえで無視して読み取り専用で開く)、
`allow_dirty` (NTFS のダーティフラグだけのボリュームを書き込み可で開く。`false` なら確認待ちの読み取り専用で返し、`state()` に `USBMEM_VOL_CONFIRM_WRITE`)、`tz_minutes_west` (FAT の時刻)。これらは `MountReq.flags` で渡り、kfsd が既存の `mount_auto` と同じ規則で振り分ける。
下のデバイスが読み取り専用 (ネストの子など) なら、書き込み可を要求しても読み取り専用でマウントする (`dev_read_only`)。

## 3. ライセンス境界

| ファイル | ライセンス (SPDX) | 公開版 | kfsd |
|---|---|---|---|
| `core/kfsd/include/usbmem/kfsd_proto.h` | Apache-2.0 | 入る (プロトコルの仕様) | 使う |
| `core/kfsd/include/usbmem/kfsd_client.h`, `src/kfsd_client.cpp` (`RpcVolume`) | Apache-2.0 | 入る | — |
| `core/kfsd/include/usbmem/kfsd_server.h`, `src/kfsd_server.cpp` (`serve`・`RemoteBlockDevice`。ドライバを持たない) | Apache-2.0 | 入る (試験用の偽のサーバにも使う) | 使う |
| `core/kfsd/src/kfsd_io.h` | Apache-2.0 | 入る | 使う |
| `core/kfsd/tests/test_kfsd_proto.cpp` (メモリ上の FsVolume でプロトコルの両端を試す) | Apache-2.0 | 入る (公開版の CI で動く) | — |
| `core/kfsd/src/kfsd_mount.{h,cpp}` (FS の判別 + マウント。カーネル由来のドライバを呼ぶ) | GPL-2.0-or-later | **入らない** | 入る |
| `core/kfsd/src/kfsd_main.cpp` (kfsd の `main`) | GPL-2.0-or-later | **入らない** | 入る |
| `core/kfsd/tests/test_kfsd_diff.cpp` (プロセス内のドライバと比べる) | GPL-2.0-or-later | **入らない** | — |
| `core/compat/**` の LinuxCompat・各 FS のボリュームラッパ、`third_party/**` (カーネル由来) | GPL-2.0 系 (docs/licenses.md) | **入らない** | 入る |
| `core/include/**` / `core/src/**` (`usbmem_core`: BlockDevice・fs_probe・sha256 など) | Apache-2.0 | 入る | 静的にリンクされる |
| `core/compat/public/usbmem/fs_volume.h`, `vol_state.h` (`FsVolume` 共通 IF) | 個人版では GPL-2.0-or-later 表記 → 公開版では Apache-2.0 へ書き換え (editions.md の `relicense`) | 入る | 使う |

- **公開版の本体には、`RpcVolume` とプロトコルのヘッダだけが入り、kfsd は入らない**。kfsd が無い (見つからない / 起こせない) ときの `RpcVolume::mount` は `err = ENOPKG` (65、`kErrNoKfsd`) を返す。UI は「この FS には kfsd (プラグイン) が必要です」と案内する (段階 4)。
- **kfsd の実行ファイルの配布**: GPL-2.0 の義務 (ソース提供・ライセンス全文・著作権表示) を kfsd 自身が負う。カーネル由来のソースは `third_party/` に無改変で置いてあり、LinuxCompat 層・ラッパ・`kfsd_main` を含めて、kfsd のソース一式 (別リポジトリ / 別 APK のソース) として公開する。
  プラグイン APK の説明文に、ソースの入手先とライセンスを表示する (6 章)。
- **Apache-2.0 の自前コードが kfsd のバイナリにも入る問題**: kfsd は `usbmem_core` (Apache-2.0) と `core/kfsd` の共通部 (Apache-2.0) を静的にリンクする。GPL-2.0-only のコード (ext4・FAT・squashfs など) と Apache-2.0 のコードを 1 つの実行ファイルにすることは、
  FSF の見解では (Apache-2.0 の特許条項が GPL-2.0 と両立しないため) GPL-2.0-only とは組み合わせられない。**著作権者 (このプロジェクト) は自前のコードに別の条件を足して配れる**ので、kfsd 向けのソースには「この kfsd のバイナリに入る自前のファイルは GPL-2.0-or-later としても提供する」旨を明記する、
  というのが現実的な対処 (SPDX は公開版では `Apache-2.0` のまま。公開版の検査は `A OR B` の片方が GPL 系のものを弾くため、kfsd 向けの配布物にだけ別の表記を付ける)。**配布前に決める項目** (docs/TODO.md)。
- **「別プロセスなら GPL が波及しない」という判断はしていない**。ユーザーが承知のとおり、(1) kfsd と本体が RPC (ソケット) だけで通信し、共有メモリ・同一アドレス空間・内部のデータ構造の共有が無いこと、(2) プロトコルが汎用の `FsVolume` 操作で、kfsd 固有の内部 API を露出していないこと、を設計で守る。
  (1) と (2) は、本体が kfsd の内部をリンクしない・kfsd が本体の内部をリンクしない (kfsd が本体の Apache のコードをリンクするのは、上の注記のとおり共通部と `usbmem_core` の一方向) という形になっている。

## 4. 性能

測定は `kfsd_bench` (`core/kfsd/tests/bench_kfsd.cpp`、`tools/run_kfsd_bench.sh`)。GitHub Actions の Linux ホスト (wrel 構成、2 コア級) で、128 MiB のファイル 1 本 + 空ファイル 3000 個を、`/dev/shm` の画像に作って測る。
本体側のデバイスは**メモリ上の画像** (USB より桁違いに速い) なので、RPC の往復 (約 14 us) が最も目立つ「最悪側」の数字。実機の USB は 1 回の転送が数百 us 以上かかるため、デバイス呼び出しの回数さえ減らせば、差はずっと小さくなる。
値は CI run 44925892263 (usbmem-ci-kfsd-wrel) の 1 回分で、ばらつきは ±10% 程度。

変種:
`inproc` = 従来どおりプロセス内 / `rpc` = kfsd 経由 (素の実装) / `rpc+prefetch` = readdir に stat を同梱して本体側にキャッシュ / `rpc+ra1M` = kfsd 側のデバイス先読み (最大 1 MiB) /
`rpc+wc1M` = kfsd 側で連続する小さなデバイス書き込みをまとめる (最大 1 MiB) / `rpc+all` = prefetch + ra1M + wc1M。

| 項目 (ext4) | 単位 | inproc | rpc | rpc+all |
|---|---|---|---|---|
| 連続書き込み 1 MiB 単位 | MB/s | 216 | 85 | 151 |
| 連続読み 64 KiB 単位 (cold) | MB/s | 1849 | 129 | 692 |
| 連続読み 1 MiB 単位 (cold) | MB/s | 1860 | 142 | 1012 |
| 連続読み 4 MiB 単位 (cold) | MB/s | 1807 | 141 | 965 |
| 空ファイル作成 | us/op | 22 | 64 | 55 |
| readdir + 全件 stat (3000 件) | ms | 4.2 | 81.5 | 16.2 |
| lookup (名前、hot) | us/op | 11 | 37 | 38 |
| stat (同じ ino、hot) | us/op | 0.0 | 20 | 0.0 (キャッシュ) |
| デバイス読み 平均サイズ | KiB | - | 4.0 | 912 |
| デバイス書き 平均サイズ | KiB | - | 4.0 | 989 |

他の FS (同じ run、MB/s):

| FS | 連続書き 1 MiB (inproc / rpc / rpc+all) | 連続読み 1 MiB cold (inproc / rpc / rpc+all) |
|---|---|---|
| ext4 | 216 / 85 / 151 | 1860 / 142 / 1012 |
| exFAT | 719 / 116 / 782 | 1289 / 116 / 1294 |
| FAT32 | 791 / 109 / 699 | 1802 / 124 / 1159 |
| NTFS | 9.8 / 9.7 / 10.3 (FS 側の書き込みが遅いのでどれも同じ) | 1993 / 126 / 1324 |

読み取り:

- **素の RPC の律速は、デバイスへのコールバックの回数**。FS ドライバは 4 KiB ずつ読み書きするので、1 MiB の読みで約 256 回、本体へ往復する (`device reads` 65585 回 / 4 KiB)。往復 1 回が約 14 us (socketpair の send + recv + スレッドの起床) なので、約 140 MB/s で頭打ちになる。
  ファイル I/O 自体の分割 (4 MiB 上限) や kfsd⇔本体のデータのコピーは、この規模では効いていない (64 KiB 〜 4 MiB で同じ約 130 MB/s)。
- **デバイス先読み (`dev_readahead_kb`)** は、連続読みでコールバックを約 220 分の 1 (65585 → 290 回) にし、読みを 5〜9 倍に戻す (inproc の 50〜65%)。連続でない読み (ランダム読み) は先読みを始めない (連続を検出するまで 64 KiB から倍々で伸ばす) ので、余計な読みは出ない。
- **書き込みのまとめ (`dev_write_coalesce_kb`)** は、連続する小さな書き込みを 1 回にまとめる。FAT32 は 109 → 699 MB/s (inproc の 88%)、ext4 は 85 → 151 MB/s (70%)。ext4 はジャーナル (jbd2) の書き込みが飛び飛びなので FAT ほど伸びない。
  **要求の応答を返す前に、まとめた分を必ず本体へ書き切る** (`reply()` の前に drain)。FS の flush / 読みの前にも書き切るので、順序は変わらない。書き切れなかった場合は応答を失敗にする (kfsd_proto の `write_coalescing` で試験)。
  ただし USB 機器が電源断で書き込みを失う条件は、プロセス内の場合と同じ (FS の `sync` が応答を返した時点でデバイスに渡っている)。
- **メタデータ**: 1 回の往復 (lookup・stat) は 20〜37 us で、プロセス内の 10 倍前後だが絶対値は小さい。**readdir に stat を同梱 (`prefetch_stat`)** すると、3000 件の readdir + 全 stat が 81.5 → 16.2 ms (5 倍速い) になり、続く stat は本体のキャッシュで 0 us。
  (readdir 自体は同梱の分だけ少し遅くなる (2.6 → 16 ms) が、全体は速い。名前だけが欲しい呼び出しでも、同梱は 1 ページ約 256 KiB で収まる。) FAT32 の readdir は FS 側 (ディレクトリのエントリ解析) が 600 ms かかり、RPC の差は見えない。
- **共有メモリ (memfd / ashmem) は、今回は入れない**。理由: (1) 先読み + まとめ書きで inproc の 50〜90% に届いており、残りはほぼ FS ドライバの 4 KiB ずつのコールバックの往復とコピー (メモリ帯域より遅い) で、共有メモリで減るのはコピーだけ。
  (2) 本体と kfsd の間の共有メモリは、「RPC だけで通信する」というライセンス境界の説明 (3 章) を弱める。(3) Android の ashmem / memfd は Binder 越しの受け渡しが要る。実機の USB は 1 回数百 us 以上なので、このメモリ上の測定で見える差は実機ではさらに小さい。
- 既定値: `dev_readahead_kb` と `dev_write_coalesce_kb` と `prefetch_stat` は、`MountOptions` の既定は 0 / 0 / false (kfsd_diff の同値性試験は既定のままで通す)。Android の本体 (段階 3) で RpcVolume を使うときは、先読み 1024、まとめ書き 1024、prefetch 有りにする。

## 5. 堅牢性・セキュリティ

**kfsd は信頼しない側**: 入力は USB 上の信頼できないデータ (壊れた・悪意のある FS)。ドライバ (カーネルのコードの移植) の脆弱性で kfsd が乗っ取られうる前提で、被害を小さくする。

| 項目 | 設計 |
|---|---|
| kfsd が異常終了 | 本体は `-EPIPE` を返し、以降その `RpcVolume` は使えない (本体は落ちない。SIGPIPE を使わない)。UI は開き直しを案内する。`exit_status()` で終了の様子を取れる |
| 本体が死ぬ | ソケットの EOF で `serve` が抜けて kfsd は終わる (デバイスはもう無いので、アンマウントの書き込みは失敗する = 電源断と同じ)。保険として `PR_SET_PDEATHSIG(SIGKILL)` (kfsd の起動時。親を作ったスレッドが終わっても発火する性質があるので、起こす側は終わらないスレッドから起こす。`--no-pdeathsig` で切れる) |
| kfsd が応答しない | 本体は 1 回の待ち (`timeout_ms`、既定 60 秒) で見切り、kfsd を SIGKILL して `-ETIMEDOUT`。マウント (ジャーナルの再生を含む) は別の上限 (既定 120 秒)。callback の処理中は待ちの時計が止まる (本体が USB を読んでいるだけ) |
| 本体から止める | `RpcVolume::abort()` (別スレッドから可): ソケットを shutdown して kfsd を SIGKILL。デバイスの抜去・ユーザーの中止用 |
| デバイスの抜去 | 本体の `BlockDevice` が Disconnected / Io を返す → callback の rc = -errno → kfsd のドライバが `-EIO` などを返す。kfsd は死なない (ボリュームは壊れた状態のまま) |
| kfsd の不正な応答 | seq の不一致・長すぎる payload・短すぎる / 長すぎる構造体・不正な名前 (`/`・NUL・空)・増えない cookie・要求より多い read → `-EPROTO` で kfsd を殺す。受信側はすべて長さを検査してから確保する (`kMaxPayload` で上限) |
| 本体 → kfsd の不正な要求 | kfsd は同じ検査をする (本体は信頼する側だが、バグで壊れた要求を送っても kfsd が落ちない)。未知の op は `-ENOSYS`、手順違反 (Hello の前に Mount など) は終了コード 2 |
| 権限の絞り込み | kfsd は USB・ファイル・ネットワークに触れない。起動時に `PR_SET_NO_NEW_PRIVS`・`PR_SET_DUMPABLE=0`・コアダンプ無効・継承した fd の全クローズ (socket だけ残す)・`umask 077`・任意で `RLIMIT_AS` (`--mem-limit-mb`)。Android では、kfsd を exec する側 (アプリの `:kfsd` サービス / プラグイン APK) が別プロセス・別 UID (プラグインの場合) で、ファイルの権限が無い |
| 将来 | seccomp-bpf でシステムコールを絞る (read / write / sendmsg / recvmsg / poll / mmap / munmap / brk / futex / exit だけにできるはず。Android のアプリのプロセスが seccomp を自分でかけられるかは AVD で確認する)。`isolatedProcess` の Service (docs/fuse-layer.md 3.2 章 b) |

## 6. 配布

どちらの配り方も**同じ kfsd の実行ファイル**で、起こし方も同じ: 起こす側のサービス (`KfsdServiceBase`) が、本体から Binder で `ParcelFileDescriptor.createSocketPair()` の片端を受け取り、
JNI の `fork + exec` (`kfsd_spawn.cpp`。posix_spawn は API 28 以降なので使わない) でそれを子の **fd 3** にして `libkfsd.so --fd=3` を起こす。本体は残りの片端で `RpcVolume::mount_fd()` を呼ぶ。
Java の `ProcessBuilder` は fd 3 を渡せない (0・1・2 以外を閉じる) ので JNI が要る。子の標準出力・標準エラーはパイプで受けて logcat (タグ `kfsd-child`) へ流す。

実装 (段階 3): 共通部は `android/kfsd-common/` (`KfsdSpawner` = 起動・見張り・終了の待ち・子の出力、`KfsdProtocol.kt` = Binder の呼び出し (SPAWN / ALIVE / EXIT / KILL / TAIL / SELF / VERSION) と `KfsdServiceBase`、`kfsd_spawn.cpp`)。
本体側の入口は `Kfsd.kt` (設定・入手先の選択・サービスへの接続・ネイティブ層への fd の受け渡し)、JNI は `jni_kfsd.cpp` (方式の切替 `nativeSetMode`、`usbmem_kfsd_mount`)。

### (a) 個人版: アプリの APK 内の `libkfsd.so`

Gradle の `-PusbmemKfsd=true` のとき (CMake の `USBMEM_KFSD_EXE=ON`)、kfsd の実行ファイル (`usbmem_kfsd_bin`) を `libkfsd.so` の名前の PIE として作る。CMake の実行ファイルでも、名前が `lib*.so` なら AGP が `lib/<abi>/` へ梱包する
(CI の `Check kfsd in APKs` で確認済み)。`extractNativeLibs=true` (`useLegacyPackaging`) で nativeLibraryDir に展開され、`:kfsd` プロセスの `KfsdService` がそこから exec する (`:wimfuse` と同じ方式)。
**指定が無いビルドは、従来どおり (libkfsd.so なし・パッケージの方式も同じ)**。

### (b) 公開版: プラグイン APK (GPL-2.0。ソースは別に公開)

別の APK (`org.usbmem.kfsd`。`android/kfsd-plugin/`、公開版のツリーには入れない) に `libkfsd.so` と `PluginService` (`KfsdServiceBase` の具象) を入れる。権限なし。本体とは Binder (起動の依頼) と socketpair (RPC) だけでつながる。

- **発見**: 本体のマニフェストに `<queries><package android:name="org.usbmem.kfsd"/></queries>`、`bindService` は明示コンポーネント (`org.usbmem.kfsd/.PluginService`)。bound service なので、プラグインの寿命は OS が管理する。
- **信頼 (試作)**: 本体はバインド前に `PackageManager.checkSignatures(本体, プラグイン) == SIGNATURE_MATCH` を確かめ、`PluginService` は呼び出し元の署名が自分と同じかを確かめる (Binder の `onTransact` ごと)。**配布では、署名が同じ (= 同じ開発者) という条件を、本体に埋めたプラグインの証明書の SHA-256 の一覧との照合に置き換える**
  (本体と別の開発者・別の鍵でプラグインを配るため)。偽の kfsd は USB の内容を読めてしまうので、ここは配布前に必ず確定する。
- **利点**: kfsd がプラグインの別 UID・別プロセスで動くので、本体のデータ・権限 (USB・ストレージ) から分離される。GPL のコード・ライセンス全文・ソースの入手先は、プラグイン APK に閉じる。
- **AVD での確認結果 (CI run は 9 章)**: 別 APK の nativeLibraryDir の実行、Binder で渡した socketpair の fd を子の fd 3 にすること、本体 (uid 10xxx) とプラグイン (別 uid、別プロセス) の間の RPC で、プロセス内のマウントと同じツリー・内容が読めること、kfsd を kill しても本体が落ちないこと、
  プラグインが無いとき ENOPKG (65) になること、すべて確認した。SELinux の拒否は出なかった (socketpair は Binder で渡した fd なので、abstract socket の connect は使っていない)。
- **未確認 / 残り**: 配布の導線 (Play ストア / F-Droid / 手動インストール)、配布用の署名と証明書の固定、プラグインの説明文 (ソースの入手先・ライセンス)、プラグインのバージョンとプロトコルのバージョンの不一致の扱い (Hello で検出するが、UI の案内は未実装)。

### 切り替え (設定)

`Kfsd.configure` (`UsbApp.onCreate`。メインプロセスだけ) が、`SharedPreferences` (`usbmem_kfsd`: `enabled` / `source`) とデバッグビルドだけの上書きファイル (`/data/local/tmp/usbmem_kfsd` の内容 `off` / `local` / `plugin` / `auto`。CI が使う) を読んでネイティブ層に反映する。
**個人版の既定はプロセス内 (従来どおり)、公開版の既定は kfsd 経由** (公開版は GPL 系のドライバを持たないので、F2FS 以外は kfsd 経由が唯一の道。kfsd もプラグインも無ければ ENOPKG)。設定を変える画面 (UI) は未実装 (`Kfsd.save` はある)。

## 7. テスト

| テスト | 内容 | 公開版 |
|---|---|---|
| `kfsd_proto` (`test_kfsd_proto.cpp`) | メモリ上の FsVolume を載せたサーバ (`serve`) と `RpcVolume` を 1 つのプロセスのスレッドで繋ぎ、全操作が直接呼びと一致すること、大きな read / write (4 MiB 超)、readdir のページング (3 万エントリ)・stat キャッシュ、デバイス callback (読み・書き・flush・エラー)、EOF・タイムアウト・abort、不正な応答 (seq 違い・長すぎる payload・不正な名前・要求より多い read・進まない cookie・知らない op・構造体の大きさ違い) で `-EPROTO` / `-EPIPE`、kfsd 不在の `ENOPKG` | ○ (公開版の CI でも動く) |
| `kfsd_read_<fs>` (`test_kfsd_diff.cpp read`) | 既存のフィクスチャ (ntfs / exfat / ext2 / ext4 / fat / iso / udf / squashfs / erofs の `*.img` 全部) を、プロセス内と RPC (実際の kfsd) で別々にマウントし、マウントの成否と errno・`fs_name` / `root` / `writable` / `state` / FS 種別・ツリー全体 (名前・dtype・stat・mtime・内容の sha256・ランダムな範囲の読み・readlink) が一致すること。未再生のジャーナルなどで拒否されたものは `allow_noload` で開き直しても同じ。`prefetch_stat` も同じ | — |
| `kfsd_write_<fs>` (`write`、`USBMEM_NTFS_WRITE=ON` のビルドだけ) | イメージを 2 つにコピーし、同じ操作列 (CreateOnly の範囲: mkdir / create / write (3 MiB の細切れ・5 MiB の 1 回) / 日本語名・200 文字の名前 / sync、Full の範囲: truncate / write_inplace / 伸ばす write / rename / symlink / link / unlink / rmdir、エラーの操作) をプロセス内 / RPC で流し、各操作の戻り値と、書いた結果のツリー (mtime を除く) が一致すること。ntfs / exfat / ext2 / ext4 / fat | — |
| `kfsd_abnormal` | 実際の kfsd で: SIGKILL → `-EPIPE`・本体は落ちない、本体がソケットを閉じる → kfsd は終了コード 1、本体のプロセスが (デストラクタなしで) 死ぬ → kfsd も消える、SIGSTOP → `-ETIMEDOUT` で kfsd が殺される、デバイスの抜去 → 読みが負の errno・kfsd は生きている、ゼロのデバイスのマウント失敗がプロセス内と同じ errno、kfsd が無いときの `ENOPKG` | — |

既存のテスト (`tools/run_*_tests.sh`、各 FS の `test_*.cpp`) は**変えていない** (ctest の一覧は、kfsd の追加分が増えるだけ)。これらは各ドライバの C API を直接叩く形 (`usbmem_ext4_*` など) で、ボリュームを開く経路を環境変数で切り替えられる構造ではない。
代わりに `kfsd_read_*` / `kfsd_write_*` が、同じフィクスチャ・同じ `mount_auto` の規則で「プロセス内と RPC が同じ結果」を直接比べる。書き込みの正しさそのもの (fsck・参照実装との突き合わせ・電源断) は既存のテストが引き続き担う。

## 8. 段階と進捗

| 段階 | 内容 | 状態 |
|---|---|---|
| 0 | この設計文書 | 済 |
| 1 | プロトコル・kfsd・RpcVolume・ホストのテスト | 済 (CI run 44924247214 / 44925892263、wrel。全構成は `usbmem-ci-full-kfsd` で確認) |
| 2 | 性能の測定と最適化 (CI のホストで) | 済 (4 章。先読みとまとめ書きを実装。共有メモリは見送り) |
| 3 | Android: `:kfsd` サービス + `libkfsd.so`・JNI の kfsd 経由マウント (切替)・emulator の `kfsd` グループ・(b) プラグイン APK の試作 | 済 (6 章。CI の結果は 9 章) |
| 4 | 公開版への反映 (manifest・UI の「プラグインが必要」・docs・公開版の CI) | 済 (manifest・公開版の JNI の ENOPKG・UI の文言・docs。公開版 CI 成功: run 44936730057)。Android の kfsd への接続は段階 3 |

## 9. Android の試験の結果 (段階 3、2026-11-22、kawanakaiku/test-ci)

`usbmem-ci-full-kfsd3` (db・全構成): Host tests run 44960382930 (plain / san / wsan / wrel すべて成功)、Public edition run 44960382936 (成功)、Android emulator tests run 44960383001 (ext-fat / btrfs-xfs / squash-erofs / nested-ntfs / ro-misc / crypt-ui / wim-fuse / **kfsd** すべて成功)。
`kfsd` グループ (API 34 の x86_64 エミュレータ):

| テスト | 結果 |
|---|---|
| `KfsdInstrumentedTest` (8 件): libkfsd.so の exec と `--version`・:kfsd は別プロセス・ext4 (6050 エントリ) のツリー + 先頭 8 KiB の sha256 がプロセス内と一致 (local / plugin)・閉じると kfsd が終了コード 0 で終わる・kfsd を SIGKILL しても本体は落ちず、以降の呼び出しは errno 32 (EPIPE) で失敗し、再マウントはできる (local / plugin)・プラグインが無いと ENOPKG (65)・子が残らない | 成功 |
| 既存の Ext2 / Ext4 / Exfat / Isofs / Ntfs / VolumeState / DirtyConfirmUi の計装テスト (44 件) を kfsd 経由 (libkfsd.so) で | 成功 (テストは変えていない) |
| 同じ 44 件を、別 APK のプラグイン経由で | 成功 |
| WriteSelfTestInstrumented (2 件。NTFS / ext2 の書き込み) を kfsd 経由で | 成功 |

プラグインは別 UID (u0_aNNN) の別プロセスで動き、ps で `org.usbmem.kfsd` として見える。socketpair は Binder で渡した fd で、SELinux の拒否は出なかった。
測定 (エミュレータ・ext4 の 6050 エントリのツリーの走査と先頭 8 KiB の読み。**参考値**: エミュレータは CPU の共有が不安定で、同じテストでも 2.4〜9 秒ばらつく): プロセス内 2.4 s (ウォームアップ後)、プラグイン経由 3.7 s、libkfsd.so 経由 9.5 s (同じ run でプロセス内の最初の測定が 9.0 s だったので、JIT / キャッシュの影響が大きい)。
ホストの測定 (4 章) のとおり、メタデータ中心の走査は RPC の往復で 1.3〜4 倍になる。

CI で見つかった問題と対処 (記録): (1) libkfsd.so のリンクに `log` が要る (LinuxCompat が `__android_log_print`) (2) AGP は `EXCLUDE_FROM_ALL` を見ずにすべてのターゲットをビルドするので、`-PusbmemKfsd` なしのビルドでも kfsd をリンクできるようにした (3) プラグイン APK は本体と同じ NDK / CMake のバージョンを指定する
(4) `posix_spawn` は API 28 以降なので、kfsd のクライアントも JNI の起動も fork + exec にした (minSdk 26) (5) 全構成の run で wsan だけ「ランナーが応答しなくなった」で 2 回落ちた (メモリ逼迫の疑い。kfsd の差分試験をサニタイザ構成では直列にし、ASan の隔離領域を小さくして解消。原因は確定していない)。

## 10. 残る課題 (現時点)

- (解決済み) `mount_auto` の重複: jni_ntfs.cpp のプロセス内マウントは `kfsd::mount_auto` (kfsd_mount.cpp) を呼ぶ。公開版 (kfsd_mount.cpp が無い) だけ、F2FS の分岐を jni_ntfs.cpp に持つ。
- kfsd の seccomp。共有メモリ (memfd / ashmem) による read のゼロコピー (4 章の数字を見て判断)。
- XFS を kfsd に載せるのは、`usbmem-wt-xfsrmap` の main への統合後 (現状は `kfsd_mount.cpp` が `usbmem_xfs` のターゲットがあれば載せる)。
- 上の GPL-2.0-only と Apache-2.0 の同居の扱い (3 章)。
