# F2FS の書き込み (自前実装・チェックポイント方式) — 実験的

2026-11-13。読み取り専用だった自前実装リーダー `core/compat/src/f2fs_volume.cpp` (docs/f2fs-port.md) に、**書き込み**を足した。
`USBMEM_NTFS_WRITE` を付けたビルドだけで有効 (CMake `-DUSBMEM_NTFS_WRITE=ON` / Gradle `-PusbmemNtfsWrite=true`)。無効なビルドの `F2fsVolume::mount_rw` は読み取り専用のボリューム (書き込みは `EROFS`) を返す。
**既定のビルドは今までどおり読み取り専用**。実機 (USB ディスク) での確認はまだ (ホストと CI と AVD のみ)。

## 先に結論
- 段階: ① CreateOnly (新規作成・追記のみ。アプリの「新規作成のみ」モード) ② Full (上書き・truncate・削除・rename・symlink・hardlink)。②も有効にした。
- Linux の `fs/f2fs` は取り込んでいない (読み取りと同じ判断)。**ログ構造の更新を自前で書いた**: 変更はすべてメモリ上に溜め、`sync()` ごとに**フルチェックポイント 1 回**で確定する。
  roll-forward 用のデータ・orphan inode・fsync ログは**書かない** (書き込み可で開くときは、回復が必要なボリュームは拒否)。GC と discard も無い (空きが尽きたら `ENOSPC`)。
- 安全の芯 (これを壊すテストを置いて、壊れることを確認している):
  1. 新しいデータ・ノード・NAT・SIT・SSA はすべて**新しいブロック**へ。古いブロックは**次のチェックポイントが確定するまで解放も再利用もしない**。
  2. チェックポイント パックは**常に今使っていない側のパック**へ、ヘッダ → 本体 (ペイロード) → フラッシュ → フッタ (→ フラッシュ) の順に書く。フッタが書けるまで、マウント側は古いパックを使う。
  3. パックのバージョン (`checkpoint_ver`) は必ず増やす。
- 検証はすべて**独立実装**: `fsck.f2fs --dry-run -f` (f2fs-tools、電源断の全カットポイントでも) と、**本物の Linux カーネルの f2fs** (QEMU の VM、CI では sudo のループマウント) で双方向
  (自前が書いた → カーネルが読む / カーネルが書いた → 自前が読む、同じ操作列をカーネルで実行した結果とツリー全体一致)。
- 書き込み可にするのは**明示のホワイトリスト**の機能を持つクリーンなボリュームだけ。それ以外は黙って読み取り専用 (イメージ不変)。

## 構成
| 場所 | 内容 |
|---|---|
| `core/compat/src/f2fs_volume.cpp` | リーダー本体 + `mount_rw` / `writable` / `write_refusal` / 公開の書き込み操作 (ミューテックスで直列化)。`#ifdef USBMEM_NTFS_WRITE` で下の 3 ファイルを取り込む |
| `core/compat/src/f2fs_write.inc` | ノード (NAT・ノード木)・ブロック確保 (カレントセグメント)・ディレクトリ操作 (TEA ハッシュ・バケット・インライン ディレクトリ)・データページの書き戻し・truncate・inode の解放 |
| `core/compat/src/f2fs_write_ops.inc` | 公開操作の実体: create / mkdir / symlink / unlink / rmdir / link / rename / write / truncate (インライン データ ⇄ 通常、疎、上書き) |
| `core/compat/src/f2fs_write_cp.inc` | チェックポイント (`flush_nodes` → `roll_full_cursegs` → `write_ssa_blocks` → `flush_nat` → `flush_sit` → `write_cp_pack`)、書き込み状態の読み込み (`load_write_state`: ホワイトリスト) |
| `core/compat/public/usbmem/f2fs_volume.h` | `FsVolume` の書き込み系 virtual と `mount_rw` / `writable` / `write_refusal` |
| `core/compat/f2fs.cmake` | 書き込みテスト (`f2fs_write`、`f2fs_crash`) |
| `android/.../jni_ntfs.cpp` | `mount_auto` の F2FS 分岐が、書き込みを求められたら `mount_rw` (`allow_noload` のときは従来どおり読み取り専用) |
| `UsbScreen.kt` / `MainActivity.kt` / `NativeBridge.kt` | 書き込み可の FS 一覧に f2fs を追加、hardlink/symlink の対象に f2fs |

### 実行モデル
- 変更はメモリ上: ノード (inode・間接ノード・ディレクトリ)、NAT / SIT のエントリ、ダーティなデータページ。読み取りは未書き込みの内容を見る。
- ダーティなデータページが 1024 を超えると `writeback_pages()` で**新しいブロックへ**先に書く (まだ NAT に載せないので電源が切れても古い状態が見える)。
- `sync()` (アプリは操作ごとに呼ぶ。デストラクタも書き込み済みの変更があれば呼ぶ) が上のチェックポイントを行う。書き込みの途中で失敗したら `broken` にして以後の変更を拒否 (チェックポイントは古いまま)。
- 操作は「前提の検査 (副作用なし) → 変更」の順。検査で落ちるなら `errno` だけ返してメモリも変えない。変更の途中で失敗したら `broken`。
- 空き容量: 各操作の前に必要量 + 予約 8 セグメントを確認し、足りなければ `ENOSPC` (GC がないので、予約がないと解放と確保のためのチェックポイント自身が書けなくなる)。

### チェックポイントの書き方
1. `writeback_pages`: ダーティなデータを新しいブロックへ。
2. `flush_nodes`: ダーティなノードを新しいブロックへ (ノードのフッタの `cp_ver` は**古い**チェックポイントのバージョン、fsync ビット 0)。NAT の更新を記録。
3. `roll_full_cursegs` / `write_ssa_blocks`: 満杯のカレントセグメントを閉じて次を選ぶ。SSA (サマリ) を書く。
4. `flush_nat`: NAT は 2 世代 (インターリーブ) あり、CP の NAT ビットマップが指す側の**反対側**へ書いてビットマップを反転する。
5. `flush_sit`: SIT も同様に反対側へ書く (ビットマップ反転)。
6. `write_cp_pack`: 今使っていない側のパックに、ヘッダ → カレントセグメントのサマリ + NAT/SIT ジャーナル → フラッシュ → フッタ → フラッシュ。CRC とバージョンを付ける。UMOUNT フラグ付き・非コンパクト。
7. 成功したらメモリ上の「前のチェックポイントで使っていたブロック」を空きに戻す。

`alloc_type` が SSR (`sload.f2fs` が作るイメージ) のカレントセグメントは、末尾 (blkoff..511) が SIT 上で空いているときだけ受け入れ、チェックポイントでは LFS として書き直す。

### 書き込み可にする条件 (ホワイトリスト。満たさなければ読み取り専用でマウント成功・イメージ不変)
- 機能ビット: `extra_attr` / `inode_checksum` / `flexible_inline_xattr` / `inode_crtime` / `lost_found` / `sb_checksum` だけ。ブロック 4096・セグメント 512 ブロック・1 セクション 1 セグメント・1 デバイス。
- チェックポイント フラグ: UMOUNT (+ 互換) だけ。orphan / error / fsck 要求 / large nat bitmap / checkpoint 停止 (`disabled`) / resize が立っていれば不可。
- SIT の有効ブロック数の合計 = チェックポイントの `valid_block_count` (NEW_ADDR など SIT に載らないブロックがあれば `EUCLEAN`)。
- 拒否 (読み取りは可): 暗号化・圧縮 (compression)・fs-verity・casefold・zoned・multi-device・quota / project quota・未知の機能。
- roll-forward が必要 (書いていない暖かいノードがある) なら**マウント自体を `EUCLEAN` で失敗**させる (回復を適用しないので、書き込み可では開かない)。
- inode 単位: 圧縮・暗号化・verity のファイルへの書き込みは不可。

## テスト
| テスト | 内容 |
|---|---|
| `tools/run_f2fs_write_tests.sh` (ctest `f2fs_write`) | 1) 決まった操作列 (`gen_ext2_ops.py` 4096) を `mkfs.f2fs` + `sload.f2fs` のフィクスチャと、カーネルが作った `k_ext` / `k_nofeat` に実行 → `fsck.f2fs --dry-run -f` clean → カーネルが読み取り専用で読んだツリー = 自前のダンプ。`ext` はカーネルに同じ操作列を実行させた結果ともツリー一致 2) 乱数操作 + モデル (6 構成: feature 有無・CreateOnly・サイズ違い) → fsck clean、カーネルのツリー = モデル 3) 3000 ファイル 4) 往復: 自前 → カーネルが書く (続きの操作列) → 自前が読んで書く → カーネル 5) noop (マウントしただけで 1 バイトも変えない)・拒否 (encrypt / verity / quota / project_quota / compression / casefold / ro / roll-forward 要 / 圧縮のカーネル製) ・満杯 (`ENOSPC`)・書き込みファジング |
| `tools/run_f2fs_crash_tests.sh` (ctest `f2fs_crash`) | 電源断の模擬 (`crash_device.h`)。{最後に 1 回 sync, 操作ごとに sync} × {prefix, cache} × torn {0,1} × カットポイント。カットポイントは先頭 6 回 + 全体に均等 + **CP パックへの書き込みの前後** (`tools/f2fs_cutpoints.py`)。検査: (F) fsck.f2fs clean (N) 回復して開いても 1 バイトも変わらない (S) 原子性 (どれかの操作境界の状態と完全一致) (A) 元からあったファイルが壊れない・消した元ファイルが別内容で残らない (K) 一部をカーネルにも読ませる |
| `tools/run_f2fs_mutation_tests.sh` | 突然変異 (下) |
| `test_f2fs_write` | `--ops / --model / --noop / --replay / --fuzzw / --dump / --refuse / --enospc`。環境変数 `USBMEM_F2_CREATEONLY`、`USBMEM_CRASH_AT/MODE/SEED/TORN`、`USBMEM_SYNC_EVERY`、`USBMEM_DUMP_DIR` |
| `tools/f2fs_vm/kcheck.py` + `init.c` | 本物のカーネルによる検査 (QEMU/TCG、cachyos カーネルの f2fs.ko。1 回約 10 秒)。CI は sudo のループマウント (`KMODE=auto/loop/vm/off`、環境変数 `USBMEM_F2FS_KERNEL`) |
| `androidTest/F2fsWriteTest.kt` + `tools/run_android_f2fs_write_test.sh` | AVD で操作列・CreateOnly の拒否・圧縮ボリュームの読み取り専用フォールバック → pull してホストで fsck.f2fs / カーネルのツリー比較 (CI の `squash-erofs` グループの `f2fs_write`) |

突然変異テスト (`-DUSBMEM_MUTATE_...` で `f2fs_volume.cpp` だけ別コンパイル。通常ビルドには入らない):
| 突然変異 | 内容 | 結果 |
|---|---|---|
| `USBMEM_MUTATE_F2FS_CP_SAME_PACK` | チェックポイントを使用中のパックへ上書き | MUTATION_RESULTS |
| `USBMEM_MUTATE_F2FS_NO_FLUSH` | フラッシュを全部外す | |
| `USBMEM_MUTATE_F2FS_FOOTER_FIRST` | フッタを本体より先に書く | |
| `USBMEM_MUTATE_F2FS_REUSE_FREED` | 解放したブロックを次のチェックポイントの前に再利用 | |
| `USBMEM_MUTATE_F2FS_NAT_INPLACE` | NAT を古い側へ上書き | |
| `USBMEM_MUTATE_F2FS_SIT_INPLACE` | SIT を古い側へ上書き | |
| `USBMEM_MUTATE_F2FS_CP_VER_SAME` | パックのバージョンを増やさない | |

## 実測結果 (2026-11-13)
RESULTS

この段階で見つけて直したもの (どれもテストが捕まえた):
- **ディレクトリ エントリのハッシュ**: TEA ハッシュの最上位ビットをマスクしていた (`fsck.f2fs` の "Mismatch hash_code")。カーネルの `F2FS_HASH_COL_BIT` は 64 ビット側の印で 32 ビットのハッシュには掛けない。
- **`i_ext` の古さ**: ブロック配置を変えたときに inode 内の extent キャッシュ (offset 348) を 0 にしないと、fsck が "wrong ext" とし inode チェックサムも合わない。`sload.f2fs` 作成のファイルへの書き込みで表面化。
- **SSR のカレントセグメント**: `sload.f2fs` のイメージは CP の `alloc_type` が SSR。拒否していたのを、末尾が空きなら受け入れるようにした。
- **読み取り専用 (拒否) ボリュームの `sync()`**: `-EROFS` を返していたが ext4 の契約 (0) に合わせた。
- 突然変異テストの初版は `CP_SAME_PACK` を検出できなかった: カットポイントが CP パック書き込みの途中に当たらなかった。CP 書き込みの前後を必ず含めるようにして検出された。

## 既知の限界・未実装
- **実機 (USB ディスク) では未確認**。flush を無視する USB ブリッジでは順序の保証が効かない。
- GC・discard・roll-forward・orphan inode・fsync ログ・quota の更新は無い。空きが尽きたら `ENOSPC` (予約 8 セグメントを残す)。**断片化したボリュームでは容量が早めに尽きる**。
- ホワイトリスト外 (圧縮・暗号化・verity・casefold・zoned・multi-device・quota・project quota・ブロック 4 KiB 以外・セグメント/セクション構成違い) は書き込み不可。
- xattr / ACL / プロジェクト ID の書き込みは公開していない (inode の既存 xattr は保つ)。
- 回復が必要な (roll-forward が要る) ボリュームは書き込み可で開かない。読み取りの `allow_noload` (確認のうえ回復を無視) のときは読み取り専用。
- チェックポイントはタイマーで起きない: アプリは操作ごとに `sync` する。`sync` しないまま電源が切れると、最後の同期以降の操作は失われる (整合性は保たれる)。
- フルチェックポイントは操作ごとに NAT/SIT/SSA とパック全体を書くので、小さい操作を大量に行うと遅い。
- 性能は未最適化 (ページごとの確保、ノードの全走査はしないが NAT/SIT の全読み込みを持つ)。
