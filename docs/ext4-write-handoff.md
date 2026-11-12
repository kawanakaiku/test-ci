# ext4 書き込み (jbd2) — 引き継ぎメモ (2026-11-13, Opus → Sonnet)

ブランチ: `worktree-agent-af62199935587fa17` (worktree: `.claude/worktrees/agent-af62199935587fa17`)。main へはマージしない (コーディネータがマージする)。
設計の詳細・テスト一覧は `docs/ext4-write.md` (結果の数値はまだ仮。下の「次の手順」で埋める)。

## 1. 設計と決めたこと・理由
- **fs/jbd2 を無改変で載せた** (`third_party/ext4/fs-jbd2/*.c`、`include/linux/{jbd2.h,journal-head.h}`、`include/trace/events/jbd2.h` は上流 067c5c24 のまま。
  取り込み元は scratchpad の linux チェックアウト = 同じコミット)。手書きの `core/compat/include/linux/jbd2.h` は削除。`mballoc.c` と `orphan.c` もビルドに追加。
- **buffer_head を ext4/jbd2 専用にカーネル互換で実装** (`core/compat/src/rt_kbh.c`、`-DUSBMEM_KBH`): jbd2 は bh の同一性・`b_count`・`b_private`(journal_head)・状態ビットに依存し、
  既定の `rt_bh.c` (呼び出しごとに別の bh) では動かない。他 FS への影響を避けるため関数名をマクロで `usbmem_kbh_*` に付け替え (`include/linux/buffer_head.h`)。
  folio に bh の輪 (`FOLIO_KBH_RING`)。bd_mapping の輪は全 bh が遊んだら外す。I/O は上流の新 API `bh_submit(bh, opf, end_io)` + `bio_endio_bh` を同期実装、REQ_PREFLUSH/FUA を flush に。
  `struct buffer_head` の `b_count` は `atomic_t` に変更 (旧実装側は `.counter` を使うよう rt_bh.c / optical.h / fat_shim.h を修正)。
- **kjournald2 を作らない**: `kthread_run("jbd2/...")` は TLS に「このスレッドのジャーナル」を登録するだけ。`wait_event` / `schedule` (more_jbd2.h で置換) →
  `usbmem_compat_run_deferred_work` → `usbmem_jbd2_poll_current` (rt_jbd2.c) が kjournald2 の 1 周 (コミット要求の処理 / UNMOUNT で j_task=NULL) を呼び出し元で実行。
  条件が成り立たない待ちは `usbmem_jbd2_stuck` で abort (デッドロック検出)。生きているジャーナルの登録表 (jreg) で解放済みを触らない。
- **データ (ordered)**: `nodelalloc` + `dioread_lock` で書き込み時に確保。ホスト API の write/truncate の直後に `usbmem_kbh_write_mapping` で dirty な bh を b_blocknr へ書く。
  jbd2 の `j_submit_inode_data_buffers` も同じ関数に差し替え (ext4 の page-io/writepages は使わない)。
- **マウント**: ro は従来どおり noload + デバイスに `usbmem_ro_` 印 (ext4 のエラー時の sb 書き込みも拒否)。rw は `writable_precheck` (ホワイトリスト、rt_vfs_ext4.c) を満たせば
  ジャーナルを読んで **ro でマウント** (needs_recovery ならここで再生)、最初の書き込みで remount rw → ext4 が needs_recovery を FUA で書く (ダーティ印の先行)。
  オプション: nodelalloc / dioread_lock / noinit_itable / no_prefetch_block_bitmaps。errors=remount-ro を後から設定。
- 互換層の共有部の変更 (他 FS に影響しうる): `kfree/kvfree/vfree` を関数化 (sync.h)、`__mark_inode_dirty` が `->dirty_inode` を呼ぶ (ext4 だけが持つ)、
  `FGP_ACCESSED` を 0x1→0x100 (FGP_LOCK と衝突していた)、`inode_init_always` で `i_io_list` 初期化、`truncate_inode_pages` が KBH の輪を外す (weak)、
  `usbmem_mapping_snapshot` / `usbmem_snapshot_inodes_` / `usbmem_bdev_is_ro` 追加、`struct block_device.usbmem_ro_`、run_deferred_work の末尾で weak な jbd2 ポーラ、
  `struct seq_file` に `file`、more_ext4.h の書き込み系スタブ約 40 個を `#ifndef USBMEM_KBH` で囲った。

## 2. 実装済み・確認済み / 未実装・未検証
確認済み (wsan = ASan+UBSan 書き込み有効、wrel):
- ① readlink (コミット済み 2f31b7d)。ext4_read / corrupt / ioerr / fuzz(600) の読み取りテストが新 bh 層でも通過 (wsan)。
- ② 再生: debugfs (`jo -c; jw ...`) で書いたジャーナルを `--replay` で再生 → e2fsck の再生結果とブロック一致・e2fsck -fn clean (4k, 手動確認)。
- ③④ `--model` 600 ops × {4k ext4, 1k ext4, ext3 4k, 2k ^metadata_csum} 全て mismatch 0 + e2fsck clean。CreateOnly モデルも OK。
  `tools/run_ext4_write_tests.sh` の 1〜3 (e4_def4k / e4_def1k / e3_4k の操作列が fuse2fs 参照とツリー一致、モデル 6 種、3000 ファイル) 通過 (wsan)。
- 電源断 `tools/run_ext4_crash_tests.sh` (wrel): 旧シナリオで 160 試行 (20 点) 全検査 0 失敗。突然変異 4 種 (NO_FLUSH / NO_SB_WRITE / NO_ORDERED / WRITEBACK) すべて検出。
- `--fuzzw` 300 反復 (wsan, 1k) クラッシュ・ハングなし。
未検証・未完:
- **4 構成のホストテスト全体 (plain/san/wsan/wrel) は未実行**。他 FS への回帰は未確認。
- run_ext4_crash_tests.sh の「ほぼ満杯 + 解放ブロック再利用」版を正しいビルドで回す途中で中断 (WRITEBACK 突然変異はこの版で A=42 検出済み)。
- run_ext4_write_tests.sh の 4 (再生のカットポイント収束) / 4b (fuzz) / 5 (noop・拒否・enospc) は通しで未実行 (noop だけ単独で OK)。
- Android: JNI (`mount_auto` で ext3/ext4 rw → `Ext4Volume::mount_rw`)、UsbScreen (ext3/ext4 を書き込みスイッチに従わせる)、MainActivity canLink、
  `Ext4WriteTest.kt` 新規、`Ext2WriteTest.kt` の ext4 テストを「書かずに閉じればイメージ不変」に変更、`tools/run_android_ext4_write_test.sh` 新規 — **すべて未ビルド・未実行**。
- docs/ext4-write.md の結果欄 (件数) が未記入。

## 3. 既知の失敗と見立て
- plain / san / wsan の**全体ビルドが失敗していた**: `core/compat/include_fat/compat/fat_shim.h:91` の `(bh)->b_count++` (b_count を atomic_t にしたため)。
  **このコミットで `.counter++` に修正済みだが、再ビルドは未確認**。他にも `b_count` を int として使う箇所が無いか、全体ビルドで確認すること (grep では見当たらない)。
- `usbmem-compat: unimplemented super_set_uuid called` が出る (無害)。`WARN_ON inode.c:396` は i_io_list 初期化で解消したはず。

## 4. コマンド
自分のビルド: `/tmp/claude-1000/-run-media-u-Data-claude-code-android-usbmem/aa992d81-592f-492d-9efb-ac5703a990af/scratchpad/e4w/` (以下 E)。
- `E/build.sh <name> [USBMEM_MUTATE_X]` : `E/<name>` に usbmem_ext4_write_tests を -j4 ビルド (name=wsan/wrel/mut_*)。
- `E/host4build.sh` : 4 構成を `E/h4/<mode>` に -j4 ビルド (ログ `E/h4_<mode>.build.log`)。ctest は `flock -w 7200 /tmp/usbmem_host.lock nice -n 10 ctest --output-on-failure` を各 `E/h4/<mode>` で。
- `BUILD=wrel sh E/run.sh crash [N] [bs] [mke2fs -O]` → `E/crash.log`、`sh E/run.sh write [モデル試行数]` → `E/write.log` (フィクスチャ `/dev/shm/e4w_fix`)。
- フィクスチャ: `cd <worktree> && PATH=$PATH:/sbin:/usr/sbin python3 -I tools/gen_ext4_fixture.py /dev/shm/e4w_fix` (約 36 秒・500MB。**終わったら消す**。現在は削除済み)。
- 単体: `ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=1 E/wsan/compat/usbmem_ext4_write_tests --model img seed nops` (`USBMEM_E4_CREATEONLY=1` で新規作成のみ)。
- Android: `cd android && JAVA_HOME=/run/media/u/disk9/usr ANDROID_HOME=/run/media/u/disk31/android_home ANDROID_SDK_ROOT=/run/media/u/disk31/android_home ./gradlew --no-daemon --offline :app:assembleDebug :app:assembleDebugAndroidTest -PusbmemNtfsWrite=true -q` → `./gradlew --stop`。
  AVD は `ANDROID_SERIAL=emulator-5556 ADB=/run/media/u/disk31/android_home/platform-tools/adb SKIP_BUILD=1 flock -w 7200 /tmp/usbmem_avd.lock sh tools/run_android_ext4_write_test.sh /dev/shm/e4w_fix`。
  emulator-5554 / localhost:5555,5560,5565,5570 には触らない。

## 5. 落とし穴と次の手順
落とし穴:
- サンドボックスが `$VAR` を含む複合コマンドや `cd /dev/shm` を拒否する。スクリプトをファイルにして `sh /full/path/script.sh` で実行するとよい。
- 実行中の sh スクリプトを書き換えない (sh は逐次読みなので壊れる)。
- dash の `echo "$out"` はバックスラッシュを解釈する → `printf '%s\n'` を使う (スクリプトは修正済み)。
- `e2fsck -fn` は未再生のジャーナルがあっても 0 を返しうる → needs_recovery が消えたことも見る (スクリプトに追加済み)。
- debugfs の `jw` は 2 つ目以降のトランザクションにコミットを書かないことがある (自前と e2fsck の一致だけを見ている)。
- e4_nocsum2k / e4_gdtcsum4k / e2_4k フィクスチャはジャーナルなし → 書き込み不可 (拒否テストに回した)。
- `/dev/shm` とメモリが逼迫している。ASan ビルドの並列実行は避ける。

次の手順:
1. 4 構成ビルド (`E/host4build.sh`) を通す。失敗したら b_count / more_ext4.h のガード / kfree 関数化 / FGP_ACCESSED 変更の影響を疑う。
2. 4 構成で ctest 全体 (flock 付き)。既存テスト (NTFS/exFAT/ext2/FAT/UDF/isofs/erofs/squashfs/…) の回帰が無いこと、wsan/wrel に ext4_write と ext4_crash が加わること。
3. `BUILD=wrel sh E/run.sh crash 40` (満杯 + 再利用シナリオ)、`... crash 20 4096`、`... crash 20 1024 ^metadata_csum` を回して全検査 0 を確認。件数を記録。
4. 突然変異 4 種を新シナリオで再確認 (`E/build.sh mut_x USBMEM_MUTATE_...` → `BUILD=mut_x sh E/run.sh crash 12`)。
5. `sh E/run.sh write 2` を wsan で通す (再生カットポイント収束・fuzz・拒否・enospc を含む)。
6. Android ビルド → AVD で Ext4WriteTest と Ext2WriteTest (`tools/run_android_ext2_write_test.sh` も ext4_empty.img の期待が変わった)。
7. docs/ext4-write.md の結果欄・docs/TODO.md の数値を埋め、段階ごとにコミット (日本語、Co-Authored-By 行)。/dev/shm の自分のフィクスチャを消す。
