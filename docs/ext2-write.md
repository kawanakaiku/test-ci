# ext2 (Linux fs/ext2) の書き込み — 実験的

2026-11-12。読み取り専用で動いていた `fs/ext2` (無改変) を、LinuxCompat の**書き込み基盤**の上で書き込み可能にした。
`USBMEM_NTFS_WRITE` を付けたビルドだけで有効 (CMake `-DUSBMEM_NTFS_WRITE=ON` / Gradle `-PusbmemNtfsWrite=true`)。無効なビルドで `read_only = 0` を要求すると `-EROFS`。
**既定のビルドは今までどおり読み取り専用**。実機 (USB ディスク) での確認はまだ (AVD とホストのみ)。

## 先に結論
- 作成・書き込み (拡張・上書き・疎・三重間接まで)・truncate・削除・rmdir・rename (同一ディレクトリ・跨ぎ・上書き・ディレクトリ)・シンボリックリンク (高速/低速)・ハードリンクが動く。
- 検証はすべて**独立実装** (e2fsprogs の `e2fsck` と、libext2fs ベースの `fuse2fs`) で行った。自前ドライバで書いたイメージが `e2fsck -fn` で clean、かつ同じ操作を fuse2fs でやった結果とツリー全体 (種別・サイズ・sha256・リンク先) が一致。
- 電源断の模擬 (320 試行/構成) で、元ファイルの破損 0、ダーティ印なしで変更 0、`e2fsck -fy` 後に clean にならない 0。
- ジャーナルは無い (ext2 なので)。電源断・抜去のあとは**ボリュームが「汚れた」状態で残り、`e2fsck` が必要**になる。アプリは汚れたボリュームを書き込み可でマウントしない。これは仕様。

## 構成 (どこに何があるか)
書き込み基盤 (exFAT / ext4 でも使い回す部分) は `usbmem_compat_rt` にある。ext2 固有なのは `rt_vfs_ext2.c` と `ext2_volume.cpp`。

| 場所 | 内容 |
|---|---|
| `rt_pagecache.c` | `usbmem_bdev_writeback(bdev)`: デバイスの page cache (bd_mapping) の**ダーティな 512B 範囲だけ**を書く (隣のブロックを巻き込まない)。`usbmem_folio_clean_range`。`truncate_pagecache` が folio の n 以降をゼロ埋め (ダーティにはしない) |
| `rt_bh.c` | `mark_buffer_dirty` / `mark_buffer_dirty_inode` / `sync_dirty_buffer` (`__sync_dirty_buffer`: 書いて必ず flush) / `bforget` (ダーティを捨てる) |
| `rt_buffer.c` | `__block_write_begin` / `block_write_begin` / `block_write_end` / `generic_write_end` / `generic_perform_write` / `generic_file_write_iter` / `block_truncate_page` / `mpage_writepages` / `block_dirty_folio` / `generic_block_bmap` / `page_symlink`。`mpage_read_folio` は `fill_folio_from_disk` に分けて write_begin から使う |
| `rt_write.c` | `generic_write_checks` が `s_maxbytes` 以降を `-EFBIG`、手前までは件数を切り詰める |
| `rt_core.c` | `insert_inode_locked`、`sync_inode_metadata`、`sync_bh_fs` (bh 系 FS の同期)、`usbmem_compat_sync` (bh 系なら `sync_bh_fs`、NTFS は従来経路)、`usbmem_compat_remount_rw` (読み取り専用でマウント → 最初の書き込みの直前に rw へ) |
| `include/linux/quotaops.h` | quota は無いが `dquot_alloc_block*` / `dquot_free_block*` が `inode_add_bytes` / `inode_sub_bytes` を呼ぶ。**ext2 は i_blocks の更新をここに頼っている**ので必須 |
| `rt_vfs_ext2.c` | ホスト入口 (`ext2_c.h`)。書き込み操作は `#ifdef USBMEM_NTFS_WRITE` の中。無効ビルドは `-EROFS` のスタブ |
| `ext2_volume.cpp` | `FsVolume` の実装。`mount_rw`、write 系 virtual、`readlink` / `symlink` / `link` |

### 書き込みの流れ (データ)
ファイルデータ・ディレクトリの内容は inode の page cache に載る。`get_block(create=0/1)` がディスク上の位置を教える。
1. `generic_file_write_iter` → `ext2_write_begin` → `__block_write_begin`: 部分書き込みで未読なら古い内容を読む。新しいブロックは確保してゼロ埋めし**全体をダーティ**にする。
2. コピー → `block_write_end` (範囲をダーティ) → `generic_write_end` (i_size)。
3. 書き出し (`write_common`) は `mpage_writepages` が `get_block(create=0)` で位置を引いて書く。

### 書き込みの流れ (メタデータ)
inode テーブル・ビットマップ・間接ブロック・グループ記述子は buffer_head (bd_mapping の folio への参照) 経由。`mark_buffer_dirty` で 512B 単位のダーティビットを立て、同期時に `usbmem_bdev_writeback` が**ダーティな範囲だけ**を書いて flush する。

### 同期の順序と安全の規則
ジャーナルが無いので、順序で安全を作る。
1. **ダーティ印を最初の変更より前に確定する**。最初の書き込みの直前に `usbmem_compat_remount_rw` が `ext2_reconfigure` を呼び、`s_state` から `VALID_FS` を落としたスーパーブロックを `sync_dirty_buffer` で書いて flush する。
   クリーンなアンマウントで `VALID_FS` を戻す (`ext2_put_super`)。
2. **解放したブロックを同期の前に再利用しない** (`free_pending`)。削除・縮小・rename での上書きでブロックを解放した操作のあと、次に**確保が起きる前**に同期する (`begin_alloc`)。これを入れないと、解放したブロックに新しいデータが先に書かれ、電源断のあと「まだ生きている古いファイル」の内容が新しいデータに入れ替わる。
   ネガティブコントロール: この同期を無効化すると電源断テストの (A) が失敗する (後述)。再利用が起きるようにルート直下にファイルを書く操作列にしてある (別グループに確保されると再利用が起きず、テストが何も検出できなかったため)。
3. 作成・書き込み・rename などは操作ごとに `after_op` で書き出す。呼び出し側 (SAF) は 1 操作ごとに `sync` する。
4. アンマウント: 全 inode を追い出す → bh 系 FS かつ rw のときもう一度 `usbmem_compat_sync` → `put_super` で `VALID_FS` を戻して flush。
5. 新規 inode の uid/gid は親ディレクトリから引き継ぐ (Android にはユーザーの概念がないため)。

### 書き込み可でマウントする条件 (`writable_precheck`)
次のどれかなら**読み取り専用に落ちる** (`writable() == false`、書き込みは `-EROFS`/30)。イメージは 1 バイトも変えない。
- `s_state` が `VALID_FS` のみ (他のビット = `ERROR_FS` やクリーンでない) でない。
- 未知の `ro_compat` 機能がある (サポートは `EXT2_FEATURE_RO_COMPAT_SUPP` = sparse_super / large_file / btree_dir)。
- マジックが ext2 でない (そもそも別の FS)。
rw でマウントされても、読み取りだけなら何も書かない (`--noop` テストで、rw を要求して書かずに閉じたイメージが**バイト一致**)。

## リンク API (UI なし)
`FsVolume` に `readlink(ino, &target)` / `symlink(dir, name, target, &ino)` / `link(dir, name, ino)` を追加 (既定は `-EOPNOTSUPP`=95。NTFS / exFAT / ext4 は未対応のまま)。
JNI: `nativeFsReadlink` / `nativeFsSymlink` / `nativeFsLink`。Kotlin: `NtfsVolume.readlink/symlink/link`。`createOnly` モードでは symlink (新規作成) は許可、link (既存への別名) は拒否。
**UI とプロバイダ (SAF) には出していない**。SAF にはシンボリックリンクの表現がなく、`readlink` で追いかけるべきか、リンクとして見せるべきかは別途決める必要がある。
(読み取りは `ext2` のシンボリックリンクを `readlink` で返す。)

## Android
- `jni_ntfs.cpp` の `mount_auto`: ext2 は rw 要求なら `Ext2Volume::mount_rw`。ext4 / exFAT は rw を要求されても今までどおり読み取り専用。
- `UsbScreen.kt`: 書き込み許可スイッチに従うのは `ntfs` と `ext2`。exFAT / ext3 / ext4 は読み取り専用のボタンのまま。
- 書き込みは `-PusbmemNtfsWrite=true` のビルドだけ。それ以外のビルドで書き込みを要求すると errno 30 (読み取り専用) になり、UI は従来どおり。
- 3 段階の書き込みモード (読み取り専用 / 新規作成のみ / すべて) は NTFS と共通の `NtfsVolume` クラスで強制される (ext2 でも同じ)。

## テスト
すべて独立実装で検証する。`USBMEM_NTFS_WRITE=ON` の構成 (wsan / wrel) の ctest に `ext2_write` と `ext2_crash` がある。

| テスト | 内容 |
|---|---|
| `tools/run_ext2_write_tests.sh` (`ext2_write`) | 1) 決まった操作列 (`tools/gen_ext2_ops.py`、1k/2k/4k 各 763 操作、1252 エントリ) → `e2fsck -fn` clean → 同じ操作列を fuse2fs rw で実行した結果とツリーが一致 2) 乱数操作 + メモリ上のモデル (ブロックサイズ 1k/2k/4k × 複数 seed、1500 操作、チェックポイントと再マウント後に比較) → e2fsck clean → モデルのダンプが fuse2fs の ro ツリーと一致 2b) 3000 ファイルの作成・半分削除・rename・再作成・削除 (1k/4k) 3) rw を要求して何も書かない / 汚れ・エラー状態・未知の ro_compat は読み取り専用でイメージ不変 / 満杯と inode 枯渇で安全に失敗して回復 |
| `tools/run_ext2_crash_tests.sh` (`ext2_crash`) | 電源断の模擬。{最後に 1 回 sync, 操作ごとに sync} × {prefix, cache} × torn {0,1} × N 点。(A) `e2fsck -fy` 後に元ファイルが同一 (消した元ファイルは「消えた」か「元のまま」)、(B) スーパーブロック以外が変わっていればダーティ印がある、(C) `e2fsck -fy` の後 `-fn` が clean |
| `androidTest/Ext2WriteTest.kt` + `tools/run_android_ext2_write_test.sh` | AVD (emulator-5556) で 203 操作を実行 → 結果イメージを pull → ホストで e2fsck と fuse2fs 参照で検証。汚れたボリュームが読み取り専用に落ちること、ext4 が rw 要求でも読み取り専用でイメージ不変なこと |
| `test_ext2_write` (`--ops/--model/--noop/--refuse/--enospc`) | 上のスクリプトから呼ばれるテストバイナリ。ASan/UBSan (wsan) で実行 |

電源断の模擬は CrashDevice (`tests/crash_device.h`): prefix = 書き込みを発行順に反映して N 回目で切る / cache = flush までバッファして切る瞬間に未 flush の書き込みの一部だけ反映 (順序保証のない USB キャッシュ) / torn = 切る瞬間の 1 回の書き込みをセクタ途中で切る。


## 既知の限界・未実装
- **ジャーナルなし**: 電源断・抜去のあとは `e2fsck` が必要。アプリは直さない (読み取り専用に落とすだけ)。
- 予約ブロック (root 予約、既定で全体の 5%): このランタイムの `current_fsuid()` は uid 0 で `capable()` は常に偽。`s_resuid` が既定の 0 のボリュームでは**予約ブロックも使える** (root と同じ扱いでディスクを 100% まで使う)。`s_resuid` / `s_resgid` を別の値にしたボリュームでは予約領域は使えない。USB ディスクの用途では実害は小さいが、`df` の空きが 0 になるまで書ける点は Linux の一般ユーザーと違う。
- アンマウント時の 2 回目の同期は bh 系 FS (rw) だけ。
- ディレクトリを再帰削除すると、解放のたびに `free_pending` の同期が走る (遅い)。大量削除の性能は未最適化。
- 件数ゼロになった inode のキャッシュが残る (長時間の巨大な作成・削除でメモリが増える)。
- xattr / ACL は未ビルド (読み取りと同じ)。
- ホスト入口の操作コードは `rt_vfs.c` (NTFS) と重複している部分がある (lookup/readdir 等)。
- ブロックサイズは 4096 以下 (folio 固定の制約は読み取りと同じ)。
- **実機 USB ディスクでの確認は未実施**。USB デバイス側の書き込みキャッシュ・flush の挙動 (SYNCHRONIZE CACHE が効くか) は AVD では確かめられない。

## 他の FS への再利用
- **exFAT**: bh の dirty / writeback (`usbmem_bdev_writeback`) とデータの write_begin 経路はそのまま使える。足りないのは exFAT 固有のもの — `VolumeFlags` のダーティ印 (`ext2` の `s_state` に相当) を最初の変更の前に確定する処理、`exfat_get_block(create=1)`、FAT チェーン更新の順序規則、クラッシュテスト。
- **ext4**: 同じ基盤に加えて jbd2 (ジャーナル) の移植が必要。`insert_inode_locked` / `sync_inode_metadata` などは入っている。ジャーナルなしの ext4 (`^has_journal`) だけなら ext2 に近い順序規則で可能かもしれないが、extents / bitmap checksum / flex_bg の整合性検査が増える。
- 共通で守る規則: ①ダーティ印を最初の変更の前に確定 ②解放したブロック/クラスタを同期の前に再利用しない ③書き込みごとの同期の粒度は呼び出し側 (SAF) の 1 操作 ④書き込み可の条件を満たさないボリュームは黙って読み取り専用。

## 結果 (2026-11-12)
- ホスト `tools/run_host_tests.sh` 4 構成: plain 25/25、san 25/25、wsan 31/31、wrel 31/31 (書き込み有効の構成は `ext2_write` と `ext2_crash` が加わる。既存の NTFS 14 / exFAT 4 / ext2 読み取り 5 / ext4 6 のテストは変わらず通過)。
  - 回帰を 1 件直した: `ext2_read` が「`read_only=0` なら必ず `-EROFS`」を前提にしていたため、書き込み有効ビルドでは成功を許す形に変更 (書き込むまでメディアに何も書かない検査は残した)。
- `ext2_write` (wsan = ASan/UBSan): 決まった操作列 (1k/2k/4k とも 763 操作、1252 エントリが fuse2fs 参照と一致、e2fsck clean)、モデル (1k/2k/4k × seed、1500 操作)、3000 ファイルの作成・削除 (1k/4k、2252 エントリが参照と一致)、noop / refuse 3 種 / enospc。
  別途、大きめの手動実行: ブロックサイズ 1k/4k × seed 11〜18 × 2500 操作 = 16 回、mismatch 0、予期しない結果 0、すべて `e2fsck -fn` clean。
- 電源断 (`tools/run_ext2_crash_tests.sh <bin> <dir> 40`、b1k): {最後に 1 回 sync (総書き込み 299), 操作ごとに sync (同 813)} × {prefix, cache} × torn {0,1} × 40 点 = **320 試行**。
  (A) 元ファイル破損 **0** / (B) ダーティ印なしで変更 **0** / (C) `e2fsck -fy` 後に clean にならない **0** (e2fsck 異常終了 0)。
  電源断後の状態は、汚れている (要 fsck) 312 / clean のまま 8 (変更がメディアに出る前に切れた試行)。`e2fsck -fn` が実際の不整合 (終了コード 4 以上) を報告した試行は 93 — 電源断で本当に壊れかけた状態が作れており、それを `e2fsck -fy` で直せることを確認している。
  - **ネガティブコントロール**: `free_pending` の同期 (解放したブロックを同期前に再利用しない規則) を無効化して同じテストを回すと (A) が多数失敗する。最初の版の操作列は新しいファイルが別グループに確保されて再利用が起きず、規則を外しても失敗しなかった (テストが感度を持っていなかった)。ルート直下に書く操作を足して、規則の有無を検出できることを確認してから、規則を戻して 0 件を確認した。
- AVD (emulator-5556、x86_64、`-PusbmemNtfsWrite=true`): `Ext2WriteTest` 3 件通過 (203 操作 / 汚れたボリュームは読み取り専用でイメージ不変 / ext4 は rw 要求でも読み取り専用でイメージ不変)。結果イメージは `e2fsck -fn` clean、`s_state` = VALID_FS、1092 エントリが fuse2fs 参照と一致。
- ファイル作成のモードは JNI が 0644 (ファイル) / 0755 (ディレクトリ) を渡す (NTFS と共通のコード経路)。

## 未確認 (正直に)
- 実機 USB ディスクでの書き込み (F-51A 等)。arm64-v8a の実行。
- 電源断の模擬は「書き込みの途中で切れる」ことしか表せない。USB ブリッジやディスクが flush を無視する場合や、セクタの一部だけが壊れる故障は表せない。
- ブロックサイズ 2k / 4k での電源断 (b1k のみ実施)。
