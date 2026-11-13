# XFS の書き込み — 実験的 (CreateOnly の範囲)

`USBMEM_NTFS_WRITE` (Gradle: `-PusbmemNtfsWrite=true`) のビルドだけで有効。無効なビルドでは書き込み入口はすべて `EROFS` を返し、読み取りの挙動は変わらない。
読み取り側の独立実装は `docs/xfs-port.md`。ジャーナル方式の参考は `docs/ext4-write.md`、構成・テスト体系は `docs/btrfs-write.md`。

## 先に結論
- **実装した範囲 (ステージ)**: V5 (CRC) ボリュームで **新規ファイル・ディレクトリ・シンボリックリンクの作成、ファイルへの追記・既存ブロックの上書き・穴あきファイル**。アプリ (`WriteMode.CreateOnly`) からは XFS は常に CreateOnly で開く (`NativeBridge.xfsCreateOnlyOnly`)。つまり既存ファイルの上書きもアプリからは拒否される。
- **未実装 (既定で無効、呼ぶと `EOPNOTSUPP`)**: `unlink` / `rmdir` / `rename` / `truncate` / `link` (Full の範囲)。ノード形式のディレクトリ (1 つのディレクトリが約 500 エントリ (4 KiB ブロック) を超えるもの) への追加は `ENOSPC`。
- **方式**: ログ (ジャーナル) を正しく書く方式 (a)。コミットごとに変更したメタデータを XFS のログに書き、そのあと本来の場所に書く。電源断で途中までのとき、Linux カーネルが同じログを再生して整合する。ログを避ける方式 (b) は採らなかった (下記)。
- **書き込みを許す条件はホワイトリスト**: 満たさなければ読み取り専用で開く (イメージは 1 バイトも変わらない)。ログが dirty のボリュームは読み取り専用 (自前では再生しない。読み取りでは従来どおり dirty log を拒否)。
- 検証は独立実装 (`xfs_repair -n` / `xfs_logprint` / **実カーネルでのログ再生とマウント**) と、カーネルが書いたイメージを自前エンジンが書き換える逆向きの確認。数値は末尾の「実測結果」。

## 構成
| ファイル | 内容 |
|---|---|
| `core/compat/src/xfs_write.inc` | 基盤: バッファキャッシュ (`WBuf`)、ログへの書き出し (`log_put`)、コミット (`commit`)、ホワイトリスト (`prepare_rw`)、`do_sync` |
| `core/compat/src/xfs_write_alloc.inc` | 空き領域 (bno / cnt B+ツリー)・AGFL・inode チャンク (ino / fino B+ツリー) の確保と解放 |
| `core/compat/src/xfs_write_ops.inc` | inode・フォーク・ディレクトリ (sf / block / leaf1)・`create` / `mkdir` / `symlink` / `write`、`run_op` (忙しい extent の再試行) |
| `core/compat/src/xfs_volume.cpp` | 読み取りのフック (`read_meta` がバッファを重ねる)、公開 API (`mount_rw` ほか) |
| `core/compat/tests/test_xfs_write.cpp` | テスト本体 (`--ops` / `--model` / `--noop` / `--refuse` / `--enospc` / `--norw` / `--tree` / `--post` / `--fuzzw`)、`crash_device.h` を使う |
| `tools/run_xfs_write_tests.sh` / `run_xfs_crash_tests.sh` / `run_xfs_mutation_tests.sh` | 独立実装との突き合わせ・電源断の模擬・突然変異 |
| `tools/xfs_vm/` (`kreplay.py`, `init.c`) | 実カーネルでログを再生させる (loop マウント または QEMU の VM) |
| `tools/xfs_patch_sb.py` | スーパーブロックを書き換えて CRC を付け直す (拒否・マウント失敗のテスト用イメージ) |
| `tools/gen_xfs_ops.py`, `tools/xfs_common.sh`, `tools/run_android_xfs_write_test.sh`, `android/.../XfsWriteTest.kt` | 操作列・共通関数・AVD 試験 |
| `.github/workflows/xfs-write.yml` | 実カーネル相互確認・電源断のフル版・突然変異 (CI) |

## 設計
### ログ方式 (a) と (b) の比較
(b) ログを使わず、書く順序だけで安全にする方式は、XFS では成り立たない: XFS のメタデータ更新 (AGF / AGI / 空き領域 B+ツリー / inode B+ツリー / inode / ディレクトリ) は複数ブロックにまたがり、カーネルは「ログが CLEAN ならその順序での整合」を前提にし、`xfs_repair` は途中の状態を壊れたものとして扱う。
一方、(a) はコミット単位でカーネルが再生できる形でログを書けば、電源断のどの点でも「トランザクション全体が反映」か「無かったこと」のどちらかになる。ext4 (jbd2) と同じ考え方で、こちらを採った。

### コミットの順序 (電源断に対する安全の核心)
メタデータの変更は操作 (1 回の `create` / `write` など) の間はメモリ上のバッファだけを書き換える。コミットは次の順:
1. **データブロック**は新しく確保した領域へコミットより前に書く (どのメタデータからも指されていないので、切れても無害)。
2. 変更のあった 128 バイト単位の塊 (`XFS_BLF_CHUNK`) だけを `XFS_LI_BUF` (0x123c) のログ項目としてログレコードに書く (inode は DINO バッファ)。レコードは 32 KiB 以下で項目は分割しない。4 レコードごとと最後に **flush (F1)**。
3. **別のレコード**でコミット操作 (op フラグ 0x02) を書く。**flush (F2)**。ここから先はカーネルが再生すれば同じ結果になる。
4. 変更したバッファを本来の場所に、セクタ単位で差分のある所だけ書く。**flush (F3)**。
5. アンマウントレコード (`clientid 0xaa`、フラグ 0x20) を書いてログをクリーンに戻す。この flush は次のコミットの最初の flush、`sync()`、`close` のどれかに寄せる。

- 2 のあと 3 の前で切れる → 未完了のトランザクション: カーネルの再生は最後の完全なコミットまでで止まる (捨てられる)。
- 3 のあと 5 の前で切れる → ログが dirty で、カーネルが再生して 4 と同じ結果になる (再生は「バッファの上書き」で冪等。バッファの LSN は付けない)。
- 全レコードの `tail_lsn` は**そのトランザクションの先頭レコードの LSN** にする (最初に作った実装は各レコードが自分の LSN を使っていて、カーネルがトランザクションの途中から再生してしまう不整合を起こした。電源断の模擬 (K) で見つけて直した。`USBMEM_MUTATE_TAIL_OWN` で再現できる)。アンマウントレコードは自分の LSN。
- ログのレコードは周回を考慮して書く。ヘッダは magic 0xFEEDbabe、version 2、CRC (crc32c) はヘッダ 328 バイトとデータ。LSN は (サイクル << 32) | ブロック。
- 自前のリーダーは**ログが dirty のボリュームを再生せずに拒否する** (`EUCLEAN`)。書き込みマウントも同じ。dirty log をそのままにするのは安全側 (カーネルで再生してから使う)。

### 空き領域の確保と busy extent
- `fs_alloc` / `fs_free` と `fix_freelist` (AGFL を補充・縮小、予約あり)。bno / cnt の 2 本の B+ツリーを同時に更新する。短い B+ツリーのヘルパは bno / cnt / ino / fino で共通。
- 同じコミットの窓の中で解放したブロックに、直ちにデータを書いてはいけない (カーネルが再生で古い内容に戻しうる): 解放したブロックは busy にして、必要なら `-EDEADLK` (`kRetryBusy`) で `run_op` がいったんコミットしてから再試行する。
- inode チャンクの確保 (64 個)、finobt (あれば) / sparse inode / inobtcount / bigtime / nrext64 を扱う。reflink のあるボリュームは**新規ファイルと追記**は可、**参照の共有があり得る既存ブロックの上書き**は `EOPNOTSUPP` (reflink フラグ・未書き込み extent・リアルタイム inode のファイルは in-place 上書きしない)。
- 既存ブロックへの上書きはその場書き込みで、メタデータのコミットとは原子的ではない (ext4 の ordered と同じ)。電源断の検査 (S) は上書きするファイルの sha256 を比較から外す。

### ディレクトリ
形式は sf (インライン) → block (1 ブロック) → leaf1 (データブロック + 末尾に leaf と bests) まで。それ以上 (leafn / da ノード / free 索引のノード形式) は未実装で、`ENOSPC` を返す (約 505 エントリ (4 KiB)/ディレクトリ)。ノード形式のディレクトリが既にあるボリュームの**読み取り**は従来どおり。そのディレクトリへの追加は `EOPNOTSUPP`。

### ホワイトリスト (`prepare_rw`。満たさなければ読み取り専用 = `ro_reason`)
次のどれかに当てはまると読み取り専用 (書き込み系は `EROFS`)。スーパーブロックのビットが読み取りのリーダーも拒否するもの (未知の incompat / realtime / 外部ログ / dirty log) は、マウントそのものが失敗する (`mount_rw` もエラー)。
| 条件 | 備考 |
|---|---|
| デバイスが読み取り専用 / 論理ブロックが 512 バイトより大きい (4Kn) | |
| V4 (CRC なし) | 書き込みは V5 だけ |
| ログが dirty / 未再生 / `noload` で開いた / ログ位置を確定できない | 再生はカーネルに任せる |
| ログに未知の機能 (`features_log_incompat`)、未知の compat | |
| 未対応の incompat (meta_uuid / parent pointer / metadir など)、ftype 無し | 既知の incompat (ftype / sparse inode / bigtime / nrext64 / inobtcount など) は可 |
| ro_compat に rmapbt / その他の未対応ビット | finobt / reflink フラグ / inobtcount は可 (reflink は上記の制限) |
| ascii-ci (大文字小文字を区別しない) | |
| クォータが有効 (`qflags`、プロジェクトクォータ含む) | |
| ストライプ (`sunit` / `swidth`)、ログのストライプ/セクタ > 512 バイト | |
| ディレクトリブロックがファイルシステムブロックより大きい (`dirblklog > 0`) | V5 のブロックサイズ 1 / 2 KiB のボリュームは mkfs が必ずこうする |
| ブロックサイズが 1〜16 KiB の範囲外、セクタサイズ > 4 KiB、ログ < 512 KiB / 大きすぎる | |
| AGF / AGI / AGFL の magic・CRC・UUID・btree の高さ・AGFL 位置が不正 (全 AG を検査)、AGF に rmapbt、finobt があるはずが AGI に無い | |
| 外部ログ・リアルタイム (`rextents` / `rblocks` ≠ 0) | `mount` 自体が拒否 |
| `imax_pct` > 100 | |

mkfs.xfs の既定構成 (xfsprogs 6.x、V5・finobt・sparse inode・reflink・bigtime・inobtcount・ftype) は書き込み可。**新しい mkfs.xfs (手元の 7.2 で確認) の既定の `rmapbt=1` は拒否**する (読み取りは従来どおり。`parent=1` も未知の incompat として拒否)。そのため CI のテストイメージは `-m rmapbt=0 -n parent=0` で作る (古い mkfs.xfs は該当オプションを外す)。

### 書き込みの境界
- `fs_write` / `write_inplace`: ファイルの末尾までの追記・穴あきの拡張・既存ブロックの上書き。extent は inode 内 (extents 形式) と B+ツリー形式 (bmbt) の両方を扱う。シンボリックリンクはインライン (local) と extent 形式。
- 1 つの操作が `ENOSPC` などで失敗したら、その操作の開始時点に戻す (バッファの undo と、カウンタの巻き戻し)。ログには書かない。
- 例外 (`bad_alloc` / 想定外) は `ENOMEM` / `EIO` にし、以降の書き込みを止める (`failed`)。

## テスト
| テスト | 内容 |
|---|---|
| `tools/run_xfs_write_tests.sh` | 決まった操作列を複数の mkfs 構成 (既定 / inode 1024 / agcount=2, 8 / finobt=0 / sparse=0 / bigtime=0 inobtcount=0 reflink=0 / nrext64 / 2 GiB / imaxpct=90) で実行し、**自前のリーダーのツリーと POSIX 操作の参照の結果が一致**・`xfs_repair -n` clean・ログ CLEAN。乱数操作 + モデル (構成ごとにシード)。満杯 (`ENOSPC`)。多数ファイル (5010 操作)。noop (書き込みマウントのみで不変)。拒否 (V4 / dirblklog / rmapbt / ストライプ / ログのストライプ / ascii-ci / クォータ / 未知の log_incompat / ブロックサイズ 1, 2 KiB) と非マウント (未知の incompat / realtime / dirty log)。書き込みマウントのファジング。実カーネル相互確認 |
| `tools/run_xfs_crash_tests.sh` | 電源断の模擬 (後述)。実カーネルで再生 |
| `tools/run_xfs_mutation_tests.sh` | 安全機構を 1 つずつ外した 7 種のビルドを電源断の模擬で検出 |
| ctest `xfs_write` / `xfs_crash` | 上の 1・2 (軽量版)。実カーネルのバックエンドが無ければスキップ (終了コード 77) |
| `tools/run_android_xfs_write_test.sh` + `XfsWriteTest` | AVD: CreateOnly の操作列 + 拒否 + noop。結果イメージを pull してホストで `xfs_repair -n` と実カーネル |

### 実カーネルの相互確認
- **自前 → カーネル**: 自前エンジンが書いたイメージを実カーネルで rw マウント (ログがあれば再生) し、ツリーが一致することと、そのあとカーネル自身に追加の書き込み (mkdir / ファイル / symlink / fsync) をさせても `xfs_repair -n` が clean なこと (自前が書いた空き領域・inode の木をカーネルが信用して使えること)。
- **カーネル → 自前**: カーネルが書いた (mkfs 後にカーネルでファイルを作った) イメージを自前エンジンが書き換えマウントして操作列を実行 → カーネルで再マウントして読めること。
- バックエンドは 2 つ: `loop` (`sudo -n mount -o loop`。CI) と `vm` (QEMU + `cachyos` のカーネルパッケージの `vmlinuz` と `xfs.ko`。ローカル。KVM があれば使う。`tools/f2fs_vm` と同じ方針)。どちらも無ければスキップ。

### 電源断の模擬
`crash_device.h` でデバイスを包み、書き込み操作列の N 回目の書き込みで「電源を落とし」残ったイメージを検査する。方式は `prefix` (発行順に反映) と `cache` (flush までは未確定: 電源断の瞬間に、未 flush の書き込みのランダムな部分集合だけが反映)。`torn` は電源断の瞬間の 1 回の書き込みをセクタ途中で切る。同期は `batch` (最後に 1 回) と `each` (操作ごと)。
`cache` 方式では **flush の呼び出しもカットポイントに数える** (`USBMEM_CRASH_FLUSH_CUTS=1`。flush の直前、未 flush の書き込みがまだ全部バッファにある瞬間に電源が落ちる)。これがないと「最後の書き込みと flush の間」で切れず、flush を省いたビルド (後述の `NO_FLUSH_LOG` / `NO_FLUSH_INPLACE`) の壊れ方を観測できなかった (書き込みの前でしか切れないため、省いた flush と同じ窓がカットポイントに現れない)。カットポイント数は `batch` 543 / `each` 1030 (書き込みのみ)、`cache` + `each` は 1230 (書き込み + flush)。
各カットポイントで:
- (K) 実カーネルで rw マウント (再生) できて、そのあと `xfs_repair -n` が完全に clean。
- (S) カーネルが見るツリーが「いずれかの操作の境界の状態」(電源断なしの実行が各操作のあとに書いたダンプ) と完全に一致 (原子性)。
- (A) 元からあるファイル (操作列が触らないもの) が元のまま。
- (P) 続き: 再生後のイメージを自前で書き込みマウントして追加の操作 → clean。標本の一部は実カーネルにも追加の書き込み (KP)。
- 電源断後に dirty log が残っていれば、自前のリーダーは再生せずに書き込みマウントを拒否する (想定どおり)。

### 突然変異
各変異は通常ビルドには入らない (`-DUSBMEM_MUTATE_<名>`)。
| 変異 | 内容 |
|---|---|
| `NO_FLUSH_LOG` | 項目レコードのあと、コミットレコードの前の flush を省く (cache 方式 + flush カットで S が検出。局所実測 S=6) |
| `NO_FLUSH_COMMIT` | コミットレコードのあと、本来の場所への書き込みの前の flush を省く |
| `NO_FLUSH_INPLACE` | 本来の場所への書き込みのあと、アンマウントレコードの前の flush を省く (cache 方式 + flush カットで K が検出。局所実測 K=48) |
| `INPLACE_FIRST` | 本来の場所への書き込みをログより先にする (WAL 違反) |
| `TAIL_OWN` | 各レコードの `tail_lsn` を自分の LSN にする |
| `DATA_AFTER` | データをコミットのあとに書く |
| `NO_CRC` | メタデータの CRC を付けない |

`tools/run_xfs_mutation_tests.sh` は 7 種すべてを検出できなければ失敗する。flush を省く 2 種 (`NO_FLUSH_LOG` / `NO_FLUSH_INPLACE`) は cache 方式の全カットポイント (stride 1、乱数の種を変えて最大 3 回) で試す。flush カットを入れる前は、この 2 種は検出できなかった (3150 試行で 0 件。順序の入れ替えが起きる窓がカットポイントに存在しなかったため。省いた flush は同値変異ではなく、テスト側の観測の穴だった)。

## 既知の限界・未実装
- **Full の操作 (`unlink` / `rmdir` / `rename` / `truncate` / `link`) は未実装** (呼ぶと `EOPNOTSUPP`)。アプリは CreateOnly に固定。ブロックの解放自体 (`fs_free`) は失敗時の巻き戻しとバッファの再利用のために実装済みだが、削除系の操作には接続していない。
- **ノード形式のディレクトリ** (leafn / da ノード / free 索引): 1 ディレクトリの上限は約 500 エントリ (4 KiB ブロックの leaf1)。それ以上は `ENOSPC`。
- ログが dirty のボリュームの再生は未実装 (カーネルで再生してから使う)。
- 4Kn デバイス、dirblklog > 0 (V5 でブロックサイズ < 4 KiB)、ストライプ、クォータ、rmapbt、外部ログ、リアルタイム、V4 は書き込み拒否。
- 実機の USB での書き込みは未確認 (ホスト・CI・AVD のみ)。
- 既存ブロックの上書きはメタデータのコミットと原子的ではない。
- reflink で共有されたブロックの上書きは行わない (`EOPNOTSUPP`。コピー・オン・ライトは未実装)。
- xattr / ACL / プロジェクト ID は書かない (新規 inode には付けない)。

## 実測結果
(2026-11-13 / 14。CI は kawanakaiku/test-ci の `usbmem-ci-xfs` ブランチ。mkfs.xfs / xfs_repair 6.18.0、カーネル再生は loop マウント)

- **書き込みテスト** (`tools/run_xfs_write_tests.sh`、CI run 43482508837): all passed。構成 (fresh / seeded / isize=1024 / agcount=2 / 2 GiB agcount=16 / finobt=0 / sparse=0 / bigtime=0 inobtcount=0 reflink=0 / imaxpct=90 …) ごとに 1045 操作が失敗 0 で、自前リーダーのツリーが POSIX 参照と一致 (1073 エントリ)、`xfs_repair -n` clean、ログ CLEAN。ファジング (書き込みマウント) 400 イメージ × 2 回 (339 / 334 がマウント、250 / 252 が書き込み可、61 / 66 が拒否) でクラッシュなし。
- **電源断の模擬のフル版** (run 43482508837): 6708 試行 (cache 方式は flush もカットに数える)、(K) / (S) / (A) / (P) / (KP) すべて 0。うち未再生のログが残っていたのが 3242、カーネルが再生して mount できたのが 6708、`xfs_repair` 計 13840 回、カーネルの続きの書き込み 422 回。
- **突然変異** (同 run の別ジョブ): 7 種すべて検出 (7/7)。NO_FLUSH_LOG は S=6、NO_FLUSH_COMMIT は K=278、NO_FLUSH_INPLACE は K=48、INPLACE_FIRST は K=1182、TAIL_OWN は K=1032、DATA_AFTER は S=1716、NO_CRC は自己検査 (CRC なしの通常実行をカーネルが拒否)。flush カットを入れる前は NO_FLUSH_LOG / NO_FLUSH_INPLACE は検出できていなかった (5/7)。
- **Android エミュレータ** (CI run 43482508697、`btrfs-xfs` グループ): `XfsWriteTest` 3 テスト OK。書き込んだイメージをホストに取り出して、1073 エントリが実カーネルの参照と同一、`xfs_repair -n` clean、ログ CLEAN。
- **ホストテスト** (ctest、CI run 43482508701): `xfs_write` / `xfs_crash` は plain / san / wrel / wsan で通る。失敗は既存の 13 (plain / san) と 20 (wrel / wsan) の別テスト (`xfs_read` など。本変更と無関係)。
- 手元: `run_xfs_write_tests.sh` で `xfs_repair` 47 回・カーネル相互確認 17 回。plain 7/7・wrel 9/9 (`xfs` ctest)。
- 未確認: 実機 USB、V4 / rmapbt / realtime / 外部ログ / クォータ / ストライプ (すべて書き込み拒否)、Full の操作 (未実装)。
