# Btrfs の書き込み — 実験的

2026-11-13。読み取り専用だった自前の Btrfs リーダー (`core/compat/src/btrfs_volume.cpp`、docs/btrfs-port.md) の上に、**COW (コピーオンライト) の書き込みエンジン**を足した。
カーネルの `fs/btrfs` は載せていない (読み取りと同じ理由)。`USBMEM_NTFS_WRITE` を付けたビルドだけで有効 (CMake `-DUSBMEM_NTFS_WRITE=ON` / Gradle `-PusbmemNtfsWrite=true`)。
無効なビルドでは `.inc` ファイルを取り込まないので、書き込みの経路は**コードとして存在しない** (`mount_rw` は `-EROFS`)。
**既定のビルドは今までどおり読み取り専用。実機 (USB ディスク) での確認はまだ (ホストのイメージと AVD のみ)。**

## 先に結論
- 段階: ① CreateOnly (新規作成・mkdir・追記・サブディレクトリ・symlink。アプリの「新規作成のみ」モード) ② Full (削除・rmdir・上書き・truncate・rename・hardlink)。①②とも実装し、ホストで検証した。
- 書き込み可にするのは**明示のホワイトリスト**に合うボリュームだけ。合わなければ**黙って読み取り専用**でマウントに成功し、イメージは 1 バイトも変わらない (`read_only_reason()` に理由)。
- 検証はすべて**独立実装**: btrfs-progs の `btrfs check --readonly --check-data-csum`、`btrfs restore` で取り出したツリー全体と、同じ操作列を POSIX 操作として実行した参照 (`tools/ops_ref.py`) との一致。
  実カーネル (sudo + loop マウント、scrub) との相互確認は CI だけ (手元に sudo / FUSE が無い)。
- 電源断の模擬 (書き込みごとのカットポイント × {接頭辞のみ・キャッシュ並べ替え} × 破れ書き) と、安全機構を 1 つずつ外した「突然変異」ビルドが検出されること。

## 構成
| 場所 | 内容 |
|---|---|
| `core/compat/src/btrfs_volume.cpp` | リーダー本体。`USBMEM_NTFS_WRITE` のとき `btrfs_write.inc` と `btrfs_write_ops.inc` を `Impl` の中に取り込む。`FsVolume` の書き込み virtual |
| `core/compat/src/btrfs_write.inc` | **トランザクションエンジン**: ダーティノード表、COW、B 木の挿入・削除・分割・統合、遅延参照 (extent ツリー)、ブロックグループ、空き領域ツリー、chunk 確保、コミット |
| `core/compat/src/btrfs_write_ops.inc` | **ホワイトリスト検査 (`prepare_rw`)** とファイル操作 (create / mkdir / write / truncate / unlink / rmdir / rename / link / symlink) |
| `core/compat/public/usbmem/btrfs_volume.h` | `mount_rw`、`writable()`、`read_only_reason()`、`sync()` |
| `core/compat/tests/test_btrfs_write.cpp` | テスト本体 (`--ops` `--model` `--noop` `--refuse` `--enospc` `--norw` `--tree` `--post` `--fuzzw`) |
| `tools/run_btrfs_write_tests.sh` | 決まった操作列 / モデル / 拒否 / 満杯 / ファジング / 実カーネル相互確認の統括 |
| `tools/run_btrfs_crash_tests.sh` | 電源断の模擬 |
| `tools/run_btrfs_mutation_tests.sh` | 突然変異テスト |
| `tools/gen_btrfs_ops.py`、`tools/btrfs_patch_sb.py` | 操作列・元ファイル群の生成 / スーパーブロックの意図的な書き換え (拒否テスト用) |
| `tools/run_android_btrfs_write_test.sh`、`android/.../BtrfsWriteTest.kt` | AVD (Android エミュレータ) での実行 |
| `.github/workflows/btrfs-write.yml` | 実カーネル相互確認・電源断のフル版・突然変異 (CI) |

## 設計
### COW とトランザクション
- ツリーブロックは**必ず新しい場所**に書く。現世代 (`sb.generation + 1`) で作ったブロックだけをその場で更新できる (ダーティ表)。確定済みのブロックは決して上書きしない。
- データの extent は新しい領域に確保して書く。上書きは新 extent + 旧 extent の参照を外す (部分上書きは旧 extent を分割して参照する)。
- extent ツリーの更新は**遅延参照**に積み、コミットで「ブロックグループ項目・空き領域ツリー・ルート項目が安定するまで」繰り返して反映する (これらの更新自体が新しいブロックを使い参照を増やすため)。
- トランザクション中に解放したブロックは、コミットが終わるまで**再利用しない** (`pending_free`: 空きには戻すが確保可能には戻さない)。
- 共有された (参照数 > 1 / 共有バックリファレンス) ツリーブロックは COW せず `EOPNOTSUPP` で中止する。

### コミットの順序 (電源断に対する安全の核心)
1. 遅延参照・ブロックグループ・空き領域ツリー・ルート項目を安定するまで更新
2. 変更したツリーブロックとデータをすべて書く
3. **flush**
4. スーパーブロック (primary → 64 MiB と 256 GiB のミラー) を書く。バックアップルートは世代 % 4 のスロットに入れる
5. **flush**

クラッシュしても、旧スーパーブロックが指す旧世代のツリーは一切上書きされていないので整合している。新スーパーブロックが書かれていれば新世代が全部ディスクにある。
スーパーブロックのチェックサムが合わない (破れ書き) コピーは、カーネルと同じく無視される。

### ホワイトリスト (`prepare_rw`。満たさなければ読み取り専用)
- 単一デバイス、プロファイルは single / DUP だけ。RAID 系・複数デバイス・シード・zoned・extent_tree_v2・raid_stripe_tree は拒否。
- incompat / compat_ro に**未知のフラグが無い**。必須: 空き領域ツリー (v2)、skinny metadata、no_holes。mixed block groups は拒否。
- スーパーブロックのミラーが有効で、世代が一致していること。
- **ログツリーが未再生でない** (`log_root == 0`)。未再生なら書き込みマウントは拒否 (読み取りは確認のうえ可)。ログツリーへの書き込みも再生もしない。
- サブボリューム / スナップショット / quota (ルートツリーの未知の項目) が無い。既定サブボリュームがトップレベル。
- デバイスの大きさが `total_bytes` 以上。ルートサブボリュームが読み取り専用でない。
- ブロックグループ・chunk・空き領域ツリー・デバイス割り当てが互いに矛盾していない (すべて検査してから初めて書く)。
- 条件を満たしても、**操作の途中で想定外の失敗**が起きたらその場でトランザクションを破棄して読み取り専用に落とす (最後のコミット以降の操作は失われるがディスクは整合)。

### 空き容量の予約
- 通常の操作は空きメタデータノードが 96 個必要。**解放する操作** (unlink / rmdir / truncate(0)) は 32 個で足りる。満杯のボリュームでも削除で回復できる。
- 足りなければ chunk を新しく確保する (メタデータ / データ)。システム chunk の確保は未対応。
- ENOSPC は安全に失敗し (`-ENOSPC`)、削除のあと書き込みが再び成功する (`--enospc` で確認)。

### チェックサム・データ
- チェックサム種別 (crc32c / xxhash64 / sha256 / blake2b-256) は**変えない**。メタデータの全ノードと新しいデータに csum を付ける (NODATASUM のファイルは付けない)。
- インライン extent と通常 extent の相互変換 (inline → regular) を行う。圧縮されたインライン extent への書き込みは展開して書き直す。圧縮された通常 extent は**分割するだけ**で再圧縮しない。暗号化など他のエンコードの extent への書き込みは `EOPNOTSUPP`。
- 新しい chunk は 64 MiB のスーパーブロックミラーの範囲を避ける。

## テスト
すべてホスト。実機 USB は使っていない。

| 層 | 内容 |
|---|---|
| 決まった操作列 | 8 構成 (crc32c 新規 / 元ファイル入り、xxhash n4k、sha256 n32k、blake2 n64k dup、zstd 圧縮入り、zlib bgt なし、single/single) で同じ操作列を実行。`btrfs check --check-data-csum` clean、`btrfs restore` のツリーが POSIX 参照と一致、自前リーダーのツリーとも一致 |
| モデル / 乱数 | メモリ上のモデルと突き合わせ (6 構成 × `NM` シード × 1000 操作)。毎回 `btrfs check` と `btrfs restore` の全ツリー比較 |
| 大量ファイル | 1626 エントリのツリーが参照と一致 |
| 不変 | 書き込み可でマウントして何も書かず閉じたイメージが 1 バイトも変わらない (`--noop`) |
| 拒否 | 11 ケースは読み取り専用に落ち、イメージ不変 (no-holes なし・空き領域ツリー v1・skinny なし・mixed・サブボリューム・非トップレベル既定・未知の compat_ro・未知の sb フラグ・ミラー世代不一致・ミラー破損・デバイスが小さい)。3 ケースはそもそもマウントしない (simple quota・未知の incompat ビット・log_root≠0)。いずれもイメージ不変 |
| 満杯 | `--enospc`: 4 フェーズ (データ満杯 / メタデータ満杯 / 削除で回復 / 再書き込み) |
| ファジング | メタデータを壊し (チェックサムを直して検査の中身に届かせる) 書き込みマウントして操作: 2 構成 × 150 イメージ、クラッシュ・ハング・メモリエラー無し (ASan / UBSan の構成でも実行) |
| 電源断 | `tools/run_btrfs_crash_tests.sh`: 書き込み 1 回ごとにカット × {prefix / cache} × 破れ書き。検査 K (clean) / S (操作境界の状態か) / A (元ファイルが壊れていない) / P (続きの書き込みが通る) |
| 突然変異 | 安全機構を外したビルド 4 種 (`USBMEM_MUTATE_{NO_FLUSH,NO_SB_WRITE,DATA_AFTER,REUSE_FREED}`、通常ビルドには入らない) がすべて検出される |
| CI (実カーネル) | `btrfs-write.yml`: loop マウントして kernel が読めること・scrub・カーネルが書いたイメージを自前エンジンが読み書きできること |
| AVD | `BtrfsWriteTest` (操作列 + 読み戻し、サブボリューム拒否、書かずに閉じて不変) → 結果イメージを pull して `btrfs check` と `btrfs restore` 比較 |

実測値は末尾の「実測結果」を参照。

## 既知の限界・未実装
- システム chunk の確保は未対応 (chunk ツリーが極端に大きくなるボリュームは `ENOSPC`)。
- 操作の途中で失敗するとその時点で読み取り専用に落ち、最後のコミット以降の操作が失われる (ディスクは整合)。
- スーパーブロックのミラー世代の不一致は書き込みを拒否する (読み取りは可)。カーネルは自動で直すが、ここでは直さない。
- 大量の削除 (数十万ファイル) は 1 つのトランザクションで行う (メモリ使用量が増える)。
- コミットの前にクラッシュすると、そのトランザクションで書いたデータ領域は参照されない (空き扱いのまま)。ファイルは旧状態で見える。
- 圧縮された通常 extent は分割だけで再圧縮しない。暗号化 / 他エンコードの extent への書き込みは `EOPNOTSUPP`。
- サブボリューム / スナップショット / quota / reflink 共有 extent / ログツリー / RAID / 複数デバイスの書き込みは未対応 (読み取り専用で開く)。
- xattr・ACL の書き込みなし。
- 実機 USB、4Kn、ARM の速度は未確認。
- 古い btrfs-progs (Ubuntu の 6.6 など) には `mkfs.btrfs --compress` / `--subvol` が無く、その構成のテストは飛ばす (出力に skip と数が出る)。

## 実測結果 (2026-11-13)
TBD
