# ext4 (Linux fs/ext4 + fs/jbd2) の書き込み — 実験的

2026-11-13。読み取り専用で動いていた `fs/ext4` (無改変) に、カーネルのジャーナル層 `fs/jbd2` (**無改変**、`third_party/ext4/fs-jbd2/`) を加え、
LinuxCompat の上で**ジャーナル付きの書き込み**を動かした。ext3 (ジャーナル付き・ブロックマップ) も同じドライバで書ける。
`USBMEM_NTFS_WRITE` を付けたビルドだけで有効 (CMake `-DUSBMEM_NTFS_WRITE=ON` / Gradle `-PusbmemNtfsWrite=true`)。無効なビルドで `read_only = 0` を要求すると `-EROFS`。
**既定のビルドは今までどおり読み取り専用**。実機 (USB ディスク) での確認はまだ (ホストと AVD のみ)。

## 先に結論
- 段階: ① `Ext4Volume::readlink` (読み取り専用でも可。LinkResolver が ext4 のリンクを辿れる) ② jbd2 を互換層に載せ、**書き込み可のマウントのときだけ**未再生のジャーナルを再生
  ③ CreateOnly (新規作成・追記のみ。アプリの「新規作成のみ」モード) ④ Full (上書き・truncate・削除・rename・symlink・hardlink)。①〜④ ともホストで検証済み。
- 作成・書き込み (拡張・上書き・疎・未初期化エクステントへの書き込み・深い extent 木)・truncate・削除・rmdir・rename (同一/跨ぎ/上書き/ディレクトリ)・
  シンボリックリンク (高速/低速)・ハードリンクが動く。ブロックサイズ 1k/2k/4k、metadata_csum あり/なし、ext3。
- 検証はすべて**独立実装**: e2fsprogs の `e2fsck -fn` (+ `needs_recovery` が消えていること) と、libext2fs ベースの `fuse2fs` で同じ操作列を実行した結果とのツリー全体比較。
  ジャーナルの再生は、e2fsprogs の `debugfs` が書いたトランザクションを自前で再生した結果と `e2fsck` が再生した結果のブロック一致。
- 電源断の模擬: 全カットポイントで、自前の再生後に `e2fsck -fn` が clean、e2fsck の再生結果と一致、操作ごとに同期する場合は「同期済みの操作まで」か「その次まで」の状態と完全一致 (原子性)。
  安全機構を壊す突然変異 4 種がすべて検出される (後述)。
- 書き込み可にするのは**明示のホワイトリスト**の機能を持つ、クリーンな、内部ジャーナル付きのボリュームだけ。それ以外は黙って読み取り専用 (イメージ不変)。

## 構成 (どこに何があるか)
| 場所 | 内容 |
|---|---|
| `third_party/ext4/fs-jbd2/` | `fs/jbd2/*.c` (6 ファイル、無改変)。`include/linux/{jbd2.h,journal-head.h}`、`include/trace/events/jbd2.h` も上流のまま (手書きの jbd2.h は削除) |
| `third_party/ext4/fs-ext4/` | 追加でビルドするもの: `mballoc.c` (ブロック確保)、`orphan.c` (孤立 inode・orphan_file)。他 (page-io・resize・ioctl・fast_commit・mmp …) は従来どおりスタブ |
| `core/compat/src/rt_kbh.c` | **カーネルと同じ意味の buffer_head** (`USBMEM_KBH`)。folio に bh の輪を付け、同じブロックには同じ bh を返す。`b_count` / `b_private` (journal_head) / 状態ビット、`bh_submit` (同期 I/O、REQ_PREFLUSH/REQ_FUA)、`create_empty_buffers` / `folio_buffers`、`block_write_end` / `block_read_full_folio`、`sync_blockdev`、ext4 のデータ書き出し `usbmem_kbh_write_mapping` |
| `core/compat/src/rt_jbd2.c` | jbd2 の実行時: kjournald2 の代わり (下記)、`wait_event` / `schedule`、xarray (mballoc 用)、crc32_be、zeroout |
| `core/compat/include/compat/more_jbd2.h` | ext4/jbd2 の翻訳単位だけに入る定義 (タイマ・per-CPU・lockdep・list・bit spinlock など)。`more_ext4.h` の「読み取り専用なので何もしない」スタブのうち、書き込みで動く必要があるものはここで置き換える |
| `core/compat/include_ext4/` | jbd2 用の小さなシム (`linux/crc32.h` `hash.h` `poison.h` `hrtimer.h` `bug.h`、`asm/page.h`) |
| `core/compat/src/rt_vfs_ext4.c` | ホスト入口。書き込み操作は `#ifdef USBMEM_NTFS_WRITE` の中 |
| `core/compat/src/ext4_volume.cpp` | `FsVolume`: `mount_rw`、write 系 virtual、`readlink` / `symlink` / `link` |

既定の bh 実装 (`rt_bh.c`: 呼び出しごとに別の bh) は他の FS (exFAT・FAT・UDF・ext2 …) のために残し、`USBMEM_KBH` の翻訳単位だけ関数名をマクロで `usbmem_kbh_*` に付け替える
(同じプロセスに両方が入る)。jbd2 は bh の同一性・参照数・状態ビット (`BH_JBD` など) に依存するため、既定の実装では動かない。

### 実行モデル (kjournald2 を作らない)
互換層は単一スレッドで動く。jbd2 のコミットスレッドは作らず:
- `kthread_run("jbd2/…")` は「このスレッドのジャーナル」を記録するだけ。
- 待ち (`wait_event` / `schedule`) に入ったとき、kjournald2 の 1 周分 (コミット要求があれば `jbd2_journal_commit_transaction`、アンマウント要求なら `j_task` を消す) を呼び出し元で行う。
- 待っても条件が成り立たない = 他に条件を変えられる主体がいない (デッドロック) ので、止まらずに中断 (`usbmem_jbd2_stuck`)。テストで一度も起きていない。
- タイマーは発火しない。コミットは: 明示的な同期 (`sync` → ext4 の `sync_fs` → `jbd2_log_wait_commit`)・トランザクションの大きさ / ログの残量・アンマウント。

### マウント
- 読み取り専用 (`read_only=1`): 従来どおり `noload` (ジャーナルを読まない)。**デバイスにも読み取り専用の印を付ける** (`bdev_read_only`): ext4 がエラー時にスーパーブロックを書こうとしても書かない (二重の守り。`bh_submit` も拒否)。
- 書き込み可 (`read_only=0`) で `writable_precheck` を満たすとき: ジャーナルを読み込んで**読み取り専用で**マウントする (needs_recovery ならここで jbd2 が再生)。
  マウントオプション: `nodelalloc` (書き込み時に確保・ordered データ)、`dioread_lock` (未初期化エクステントを使わない)、`noinit_itable` / `no_prefetch_block_bitmaps` (バックグラウンドのスレッドを作らない)。
  `errors=remount-ro` 相当 (既定の continue だとエラー後も書き続ける)。
- 最初の書き込みの直前に remount rw。ext4 自身 (`ext4_setup_super`) がスーパーブロックに `needs_recovery` を立てて **FUA で**書く = **ダーティ印が最初の変更より先にメディアに確定**する。
  クリーンなボリュームは、マウントしただけ・読んだだけでは 1 バイトも書かない (`--noop` テストでイメージがバイト一致)。

### 書き込み可にする条件 (ホワイトリスト。満たさなければ読み取り専用でマウント成功・イメージ不変)
- 下のデバイスが書き込み可、リビジョン 1 以上、ブロックサイズ ≤ 4096、`s_state` が VALID_FS でエラーなし、**内部ジャーナルあり** (外部ジャーナル・ジャーナルなしは不可)、既定のデータモードが journal / writeback でない。
- compat: dir_prealloc / imagic_inodes / has_journal / ext_attr / resize_inode / dir_index / orphan_file
- incompat: filetype / recover (再生する) / extent / 64bit / flex_bg / csum_seed
- ro_compat: sparse_super / large_file / huge_file / gdt_csum / dir_nlink / extra_isize / metadata_csum / orphan_present
- それ以外 (meta_bg・inline_data・bigalloc・quota・project・largedir・ea_inode・mmp・encrypt・casefold・verity・fast_commit …) は**テストしていないので書き込み不可**。
- inode 単位: data=journal 指定 (chattr +j)・inline data・暗号化・verity・EA inode のファイルへのデータ書き込み / truncate は `-EOPNOTSUPP`。

## 同期の順序と安全の規則
ジャーナルが順序を作る (カーネルの ext4 data=ordered と同じ)。互換層が守るのは、jbd2 が要求する I/O の意味を正しく実装すること:
1. **ダーティ印の先行**: 上記 (remount rw で needs_recovery を FUA で書く)。突然変異 `NO_SB_WRITE` で検出 (B)。
2. **コミットの順序**: jbd2 はジャーナルのブロック (記述子・メタデータの写し・revoke) を書き、コミットブロックを `REQ_PREFLUSH | REQ_FUA` で書く。
   `bh_submit` は PREFLUSH = 書く前に flush、FUA = 書いた後に flush。つまり「ジャーナル書き込み → flush → コミットブロック → flush」。
   チェックポイント (ホーム位置への書き戻し) の後、ログの末尾を進める前にも jbd2 が flush してからジャーナルのスーパーブロックを FUA で書く。突然変異 `NO_FLUSH` で検出 (S・B)。
3. **ordered データ**: ファイルデータは確保したブロックへ、**それを参照するメタデータのコミットより前に**書く。ホスト API の write / truncate は操作の中でデータを書き
   (`usbmem_kbh_write_mapping`)、jbd2 のコミット時のデータ書き出し (`j_submit_inode_data_buffers`) も同じ関数で行う (コミットが操作の途中で起きる場合)。突然変異 `NO_ORDERED` で検出 (S)。
4. **解放したブロックをコミット前に再利用しない**: ext4 の mballoc が、解放したブロックをトランザクションのコミットまで確保対象から外す (`ext4_mb_free_metadata`)。
   突然変異 `WRITEBACK` (data=writeback にして即時再利用させる) で検出 (A)。テストはボリュームをほぼ満杯にして、消したファイルのブロックを使わないと書けない状況を作る。
5. **再生のクラッシュ安全**: jbd2 の再生は「ホーム位置へ書く → `sync_blockdev` (書いて flush) → ジャーナルのスーパーブロックで空にする」。途中で切れてもジャーナルが残るので、
   もう一度再生すれば同じ結果に収束する (テスト 4)。
6. アンマウント: データの残りを書く → `ext4_put_super` → jbd2 が最後のコミットとチェックポイント → needs_recovery を消して書く → flush。

## テスト
| テスト | 内容 |
|---|---|
| `tools/run_ext4_write_tests.sh` (ctest `ext4_write`) | 1) 決まった操作列 (`gen_ext2_ops.py` + 未初期化エクステント・深い extent 木への書き込み) を `mke2fs -d` で作った ext4 1k/4k・ext3 4k のフィクスチャに実行 → `e2fsck -fn` clean + needs_recovery なし → fuse2fs で同じ操作列を実行した結果とツリー一致 2) 乱数操作 + モデル (4k / 1k / 2k ^metadata_csum / ext3 / ^orphan_file / CreateOnly) → e2fsck clean、モデルのダンプが fuse2fs のツリーと一致 3) 3000 ファイルの作成・削除・rename・再作成 (1k/4k) 4) debugfs が書いたジャーナルの再生が e2fsck の再生とブロック一致、再生の全カットポイント (prefix/cache × torn) で再実行すれば収束 4b) 壊したイメージの書き込み可マウント + 操作のファジング 5) noop / 拒否 (汚れ・エラー・quota・ジャーナルなし・meta_bg・inline_data・bigalloc・ext2) / 満杯・inode 枯渇 |
| `tools/run_ext4_crash_tests.sh` (ctest `ext4_crash`, 軽量版) | 電源断の模擬 (`crash_device.h`)。{最後に 1 回 sync, 操作ごとに sync} × {prefix, cache} × torn {0,1} × N 点。検査 R / E / S / A / B (スクリプト冒頭) |
| `test_ext4_write` | `--ops / --model / --noop / --refuse / --enospc / --replay / --dump / --fuzzw`。環境変数 `USBMEM_E4_CREATEONLY` (モデルを新規作成系だけに)、`USBMEM_DUMP_DIR` (各操作後のツリー)、`USBMEM_CRASH_*` |
| `androidTest/Ext4WriteTest.kt` + `tools/run_android_ext4_write_test.sh` | AVD (emulator-5556) で操作列・CreateOnly モードの拒否・汚れたボリュームの読み取り専用・ジャーナル再生 → pull してホストで e2fsck / fuse2fs / e2fsck の再生と比較 |

突然変異テスト (`-DCMAKE_C_FLAGS=-DUSBMEM_MUTATE_...` で別ビルド。通常ビルドには入らない):
| 突然変異 | 内容 | 結果 |
|---|---|---|
| `USBMEM_MUTATE_NO_FLUSH` | PREFLUSH/FUA と flush を全部外す | S (同期済みの操作が消える) と B が検出 |
| `USBMEM_MUTATE_NO_SB_WRITE` | スーパーブロック (needs_recovery の印) を書かない | B・E・S が検出 |
| `USBMEM_MUTATE_NO_ORDERED` | データをコミットの後に書く | S (サイズはあるのに中身が違う) が検出 |
| `USBMEM_MUTATE_WRITEBACK` | data=writeback: 解放したデータブロックをコミット前に再利用 | A (消したはずの元ファイルの中身が新しいデータに入れ替わる) が検出 |

## 実測結果 (2026-11-13、ホストと AVD。実機は未確認)
| 項目 | 結果 |
|---|---|
| `run_ext4_write_tests.sh` (wsan = ASan+UBSan 書き込み有効) | 全通過 (`e2fsck -fn` 61 回): 決まった操作列 3 フィクスチャ (各 767 操作・6269 エントリ) が fuse2fs とツリー一致、モデル 14 構成 (4k/1k/2k ^metadata_csum/ext3/^orphan_file/CreateOnly) 一致、3000 ファイル (1k/4k) 一致、ジャーナル再生 3 構成が e2fsck の再生とブロック一致、書き込みファジング 2 × 200 反復 (クラッシュ・ハング・リークなし)、noop・拒否 10 種・満杯/inode 枯渇 |
| 再生中の電源断 (カットポイント 6 書き込み × prefix/cache × torn) | 4k: 12/12 収束 (うち 2 点は超ブロックがセクタ途中で切れて拒否、バックアップ無し)、1k: 12/12 (うち 2 点は拒否 + e2fsck が直して clean)、4k ^metadata_csum: 14/14 |
| `run_ext4_crash_tests.sh` (wrel、満杯 + 解放ブロック再利用シナリオ) | 1k 40 点 = 320 試行、4k 20 点 = 160 試行、1k ^metadata_csum 20 点 = 160 試行: 合計 640 試行・カットポイント 80 点で R/E/S/A/B すべて 0 失敗 (e2fsck -fn 検査 643 回) |
| 突然変異 4 種 (各 12 点 = 96 試行) | NO_FLUSH: S 12・A 18・B 16 / NO_SB_WRITE: E 36・S 24・A 36・B 88 / NO_ORDERED: S 20 / WRITEBACK: A 42 — すべて検出 |
| AVD (emulator-5556) `Ext4WriteTest` 4 件 | OK。操作列の結果は 6109 エントリが fuse2fs の参照と一致、e2fsck clean、needs_recovery 消去。Android 上のジャーナル再生は e2fsck の再生とブロック一致。CreateOnly の拒否・汚れたボリュームの読み取り専用も通過 |
| AVD `Ext2WriteTest` 3 件 (ext4 を書かずに閉じればイメージ不変を含む) | OK (1092 エントリが fuse2fs の参照と一致) |
| ホスト全 ctest (4 構成) | 他ファイルシステムの回帰確認 (共有の互換層を触ったため)。san (読み取りのみ・ASan+UBSan): 137 件、wsan (書き込み有効・ASan+UBSan)・wrel (書き込み有効・Release): 146 件 (うち `ext4_write` と `ext4_crash` を含む)、plain: 137 件。`ntfs_windows_images` は Windows 製イメージが無いためスキップ。結果の詳細は最終報告を参照 |

この段階で見つけて直したもの (どれもテストが捕まえた):
- **ENOSPC 後の folio 漏れ**: `ext4_write_begin` は ENOSPC で `ext4_truncate_failed_write` が folio をキャッシュから捨てたあと、同じ folio を持ったまま再試行する。
  カーネルは切り離した folio の `mapping` を NULL にするので再試行側が `folio->mapping != mapping` で取り直せるが、互換層は NULL にしていなかった (KBH の folio だけ NULL にするよう修正、`rt_pagecache.c`)。
- **マウント失敗時の漏れ**: `fill_super` 失敗で根 dentry (と inode 参照) が漏れる (ext4 は `dput(s_root)` してから `s_root = NULL` にし、互換層の `dput` は何もしない)。`d_make_root` が作った dentry を `sb->usbmem_root_made_` に覚えて失敗経路で解放。
  失敗した inode の extent status 木も漏れていた (`drop_inodes_sb` から `usbmem_drop_inode_extra` で解放)。
- **ext2 マウント失敗時の誤適用**: 上の解放フックを ext2 の inode にも適用し ASan の heap-buffer-overflow (`nested_ioerr_*_ext2`)。ext4/ext3 の inode にだけ適用するよう修正。
- テスト `xfs_features` は uuid を固定値 0xAB に書き換えていて、乱数 uuid がたまたま 0xAB だと変化せず 1/256 で落ちる。元の値と必ず違う値にした。
- テストスクリプト: `run_ext4_write_tests.sh` の再生カットポイントの節に `set -e` で途中終了する行があった (削除)。作業ディレクトリは `USBMEM_WORK_DIR` で /dev/shm 以外にできる。

## 既知の限界・未実装
- **実機 (USB ディスク) では未確認**。ホスト (ASan/UBSan を含む 4 構成) と AVD のみ。flush を無視する USB ブリッジでは順序の保証が効かない (ジャーナルのチェックサムで壊れたトランザクションは捨てられるが、同期済みの変更が失われうる)。
- ホワイトリスト外の機能 (meta_bg・inline_data・bigalloc・quota・largedir・ea_inode・mmp・fast_commit・外部ジャーナル・ブロックサイズ 8K 以上) は書き込み不可 (読み取りはできる)。
- 遅延確保 (delalloc) と dioread_nolock は使わない (書き込み時に確保)。大きなファイルの書き込みの性能は未最適化 (ページごとにハンドル)。
- コミットはタイマーで起きない: アプリは操作ごとに `sync` する (SAF と同じ使い方)。`sync` しないまま電源が切れると、最後の同期以降の操作は失われる (ジャーナルなので整合性は保たれる)。
- ACL・xattr の書き込み (security.* など) は公開していない。所有者は親ディレクトリから引き継ぐ (ext2 と同じ)。
- 予約ブロック: ext2 と同じく uid 0 扱い (root 予約まで使える)。
- **超ブロックの途中で切れた書き込み (torn write)**: 1024 バイトの超ブロックがセクタ途中で切れる (前半だけ書けてチェックサムが合わない) と、カーネルの ext4 と同じくマウントは拒否される (1 バイトも書かない)。バックアップ超ブロックがあれば e2fsck で直る。ブロックグループが 1 つだけの小さなボリュームにはバックアップが無い。
- jbd2 の `/proc` 統計・lockdep・トレースは空実装。`kthread_run` は jbd2 以外を拒否する (lazyinit / kmmpd はマウントオプションで起きないようにしている)。
