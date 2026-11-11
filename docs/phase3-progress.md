# Phase 3 進捗: USB Host / Mass Storage (読み取り専用まで)

日付: 2026-11-11。**状態: USB 読み取り経路は実装済みだが、実機の USB メモリでは未検証。NTFS 書き込みは未着手。**

## できたこと
| 項目 | 内容 | 検証 |
|---|---|---|
| `BulkTransport` 抽象 | bulk IN/OUT・CLEAR_HALT・Mass Storage Reset のみを OS 依存として分離 (`core/include/usbmem/usb_msc.h`) | — |
| `MscDevice` (BOT + SCSI) | INQUIRY / TEST UNIT READY(待機・UNIT ATTENTION 再試行) / READ CAPACITY(10)→(16) / MODE SENSE(6) の WP / READ・WRITE(10,16) / SYNCHRONIZE CACHE。`BlockDevice` として振る舞う。端数は RMW、チャンク ≤64KiB、2TiB 超は 16 バイト CDB | PC: 模擬 BOT デバイス 19 件 |
| BOT エラー処理 | データ段/CSW の STALL → CLEAR_HALT (CSW は 1 回再試行)、CSW の署名・タグ・residue 検証、Phase Error / 不正 CSW → Reset Recovery、CHECK CONDITION → REQUEST SENSE、抜去は sticky な Disconnected | PC: 故障注入 |
| 模擬デバイス | `core/tests/fake_bot.h`: 疎ブロックストア、STALL/不正 CSW/UA/NOT READY/媒体エラー/WP/抜去/短 CBW/residue 偽装 | — |
| 統合 | 実 NTFS イメージ (圧縮ファイル含む) を模擬 USB デバイス経由で `fs/ntfs` が読み、sha256 一致 | PC (ASan/UBSan、リーク検出あり) |
| usbdevfs トランスポート | `USBDEVFS_BULK/CLEAR_HALT/CONTROL` で `BulkTransport` を実装 (`UsbDeviceConnection` の fd をそのまま使用)。引数不正・USB でない fd はエラーで安全に失敗 | PC: 失敗系のみ。**実機動作は未検証** |
| Android 側 | `UsbMscSelector` (class 8 / subclass 6 / protocol 0x50 の bulk IN/OUT)、`UsbStorageSession` (権限要求 → claimInterface → 読み取り専用で open)、取り外しで公開中ボリュームと接続を即クローズ、UI (一覧・パーティション・NTFS 公開) | JVM 単体テスト (選択ロジック)、`assembleDebug` (arm64-v8a, x86_64)。実機未検証 |

| Windows 製イメージ × 模擬 USB | Windows が作った MBR/512B と **USB 4Kn (4096B ブロック)** のイメージを模擬 BOT デバイス越しに読み、1020 ファイルの sha256 が一致 | PC (ASan/UBSan) |

## 設計上の判断 (理由つき)
- **常に読み取り専用で開く**: `MscOptions.read_only` の既定は true、Kotlin も `readOnly=true` 固定。書き込みは NTFS 書き込みが検証されるまで有効化しない (要件 16-8)。
- **UAS 非対応**: protocol 0x62 は選択しない。BOT のみ。UAS 専用デバイスは「見つからない」扱い。
- **fd の所有権は Kotlin**: `usbmem_open_usb_msc` は fd を閉じない。close 順序は NTFS → ネイティブデバイス → releaseInterface → connection。
- **エミュレータでは USB を検証できない**: 実機の USB メモリ (OTG) が必要。この環境に接続されている実機の端末には触れていない。

## 未検証・未実装 (残る制約)
1. **実機の USB メモリでの動作** (列挙・権限ダイアログ・転送・抜去・再接続)。usbdevfs の 1 回あたり長さ上限 (16KiB チャンクで回避しているが、機種差は未確認)。
2. UAS、複数 LUN (LUN は引数で指定可能だが UI は LUN 0 固定)、メディア交換検知、スループット計測。
3. **NTFS 書き込み** (iomap 書き込み・writeback・フォルダ作成/削除/名前変更・排他制御・クラッシュ時整合性) — Phase 3 の中心だが未着手。
4. 再接続後のデータ整合性の検証 (Phase 3 完了条件)。

## 書き込み検証に使えるもの
`tools/winpe/` の WinPE 環境 (QEMU/KVM + `autounattend.xml`) は、**書き込み実装の独立検証**にも使える: 私たちのドライバが書いたイメージを WinPE で起動し、
`chkdsk W:` (読み取り専用チェック) と `dir`/`fc` で Windows 自身に整合性を判定させる。`ntfs-3g`/`ntfsfix -n` に加えて第 2 の独立した検証者になる。

## 次の作業 (提案)
1. 互換層に書き込み経路 (dirty folio / writeback / bio write / iomap write) を追加し、**イメージファイル上で** `ntfs-3g` / `ntfsfix -n` / `ntfsinfo` による独立検証と fuzz を通す。
2. 通るまで書き込みはビルドオプション + 実行時の明示許可の二重ガードで無効のままにする。
3. 実機検証は USB メモリを用意してもらったうえで、読み取り → 書き込みの順に進める。

## NTFS 書き込み経路の実装計画 (調査結果, 2026-11-11)
上流 `fs/ntfs` の書き込みが依存する、互換層でまだスタブ (`tools/compat_stub_names.txt`) の機能:
`iomap_file_buffered_write` / `iomap_dirty_folio` / `iomap_writepages` + `iomap_add_to_ioend` + `iomap_ioend_writeback_submit` / `iomap_zero_range` /
`iomap_invalidate_folio` / `iomap_release_folio` / `iomap_is_partially_uptodate` / `copy_folio_from_iter_atomic` / `generic_write_checks` / `file_modified` / `setattr_*` / `notify_change` ほか。
MFT は `ntfs_mft_writepages` (別系統) で書く。

段階 (各段で ntfs-3g / ntfsfix -n / WinPE chkdsk と fuzz で検証。書き込みは build オプション + 実行時 opt-in の二重ガードのまま):
1. folio の dirty 管理と `iomap_dirty_folio`、ホスト API の `sync` (全 dirty folio を `writeback_range` 経由で BlockDevice へ bio 書き込み、flush)。
2. `iomap_file_buffered_write` (既存ファイル内の上書きのみ。拡張なし) → `usbmem_ntfs_write` API。
3. 拡張・新規作成・削除・rename・mkdir (上流の `ntfs_create`/`unlink` 等と `$LogFile`/`$MFTMirr`/ビットマップ更新の確認)。
4. 電源断模擬 (書き込み途中の任意位置で打ち切り) で `ntfsfix -n`/chkdsk が許容できる状態か確認。
注意: 64KiB クラスタで `$MFTMirr` 警告 (上流の偽陽性) が書き込み有効化時に読み取り専用化を引き起こさないか段 1 で確認する。

### 段階 1・2(足場) 実装結果 (2026-11-11)
- 実装: `iomap_dirty_folio` / `writeback_iter` (dirty を index 昇順、lock・dirty クリア済みで返す) / `iomap_writepages` + `iomap_add_to_ioend` (同期 1 範囲 1 bio) /
  `mark_inode_dirty*` / `write_inode_now` / `usbmem_compat_sync` (全 inode の write_inode → ページ書き出し → 不動点まで反復 → `sync_fs` → デバイス flush)。アンマウント時にも sync。
- ガード: CMake `USBMEM_NTFS_WRITE` (既定 OFF)。OFF では `read_only=0` のマウントは `-EROFS`。ON でも `NtfsVolume::mount_rw` で明示したときだけ書き込み可。
- 足場 API: `write_inplace` (非常駐・非圧縮・非暗号・非スパースの既存範囲の上書きのみ。サイズ拡張なし、mtime 等は更新しない)。
- 検証 (`tools/run_write_tests.sh`, ctest `ntfs_write`): 自前ドライバで書いたイメージを独立実装の **`ntfsfix -n` と `ntfs-3g` で検証**。1 バイト、ページ境界、70000B、EOF 直前、1MiB 境界またぎ。
  書いた範囲が期待パターン、`big.bin` 以外の 807 ファイルの sha256 が不変、`ntfsfix -n` OK。変更バイトは書いた 70000B の窓のみ (メタデータ不変)。ASan/UBSan・リークなし。
- 未対応: サイズ拡張、新規作成/削除/rename/mkdir、`$LogFile`・ボリュームダーティビットの扱い (rw マウントで上流が何を書くかは未精査 — 今回の差分は書いた窓のみ)、
  Windows 製イメージ (64KiB クラスタの `$MFTMirr` 警告含む)・圧縮/疎への書き込み、電源断模擬。

### 書き込み検証の追加結果 (2026-11-11, 続き)
- **不具合を修正**: Windows 製 (ダーティ) イメージの sync/アンマウントで `$MFT` 書き込み時に ASan が heap-use-after-free。原因は上流が `bioset` の `front_pad` で bio を自前 ctx に埋め込む (container_of) のに、互換層が pad を無視していたこと。
  `bioset_init`/`bio_alloc_bioset` の front_pad、`bio_chain` (同期実行での残数管理) を実装して解消。これで `$MFT`/`$MFTMirr` へのレコード書き込みが動く。
- **Windows 自身による検証**: `tools/winpe/verify_with_windows.sh <Win11.iso> <image> [期待manifest]`。自前ドライバが書いたイメージを WinPE で起動し、
  `chkdsk` (読み取り専用) を実行、さらにボリューム全体を Windows にログ用ディスクへコピーさせてホストで sha256 照合する (WinPE には fc/comp/certutil/where が無いため)。
  結果: MBR/4KiB クラスタの Windows 製イメージに `big.bin` の 70000B 上書き → **chkdsk「問題は見つかりませんでした」、Windows が読んだ 1019 ファイル全て期待どおり**。
  `ntfsfix -n` / `ntfs-3g` と合わせ、独立した 3 つの検証者が一致。
- 観測:
  - `win_gpt64k`: 上流が `$MFTMirr` 不一致 (64KiB クラスタで比較範囲が広すぎる偽陽性) を理由に**自動で読み取り専用マウントに落ちる** (`write_inplace -> -EROFS`)。データは壊さず安全側だが、64KiB クラスタの Windows ボリュームには書けない。対処 (上流の比較を先頭 4 レコードに限る等) は別途判断。
  - `win_dirty` (ダーティフラグ付き): rw マウントし、クリーンなアンマウントでダーティフラグを下ろす (`$Volume` flags 0x0181→0x0180)。これは上流の方針。`$LogFile` が静止状態のため許容されたが、未再生ログのあるボリュームでの挙動は別途確認が必要。
  - 4Kn (4096B ブロック) の書き込みも成功。

### 段階 3 (名前空間の変更) 実装結果 (2026-11-11)
- 互換層: `iomap_file_buffered_write` / `iomap_zero_range` (rt_write.c)、単一バッファの `iov_iter`、`generic_write_checks`、`kiocb_modified` 等 (更新時刻)。
  ホスト API: `usbmem_ntfs_write` (上流 `ntfs_file_write_iter` 経由 = 拡張・常駐→非常駐の昇格・隙間のゼロ埋めは上流)、`create` / `mkdir` / `unlink` / `rmdir` / `rename` (上流の inode_operations を VFS 同様に呼ぶ)。
  クラスタサイズ: 512B / 2048B / 4096B で検証済み (小クラスタは 1 folio に複数クラスタ。folio ごとに 512B 単位の dirty ビットを持ち、書いた範囲だけを書き戻す。穴/未初期化範囲でゼロしか載っていない書き戻しは I/O を省く)。
- 検証 (`tools/run_write_tests.sh`): 同じ操作列を (a) 自前ドライバ、(b) ntfs-3g の POSIX で実行し、**ツリー全体 (名前・サイズ・sha256) を比較** + `ntfsfix -n`。
  全て一致: 作成 / mkdir (入れ子) / 削除と再作成 / rename (ファイル・ディレクトリ・別ディレクトリへ移動) / **1200 ファイルの一括作成と間引き削除 (索引の拡張・縮小)** / 日本語名・254 文字名・空白括弧名 /
  EOF 超えの拡張・隙間・数 MiB 拡張・常駐ファイルの昇格。
  圧縮ファイル (LZNT1) の削除は上流が `-EOPNOTSUPP` で拒否するが、**失敗してもツリーは変わらず fs も整合** (負のコントロールをテストに含む)。
- **Windows による検証**: Windows 製 MBR イメージ (WinPE の `diskpart` で作成) に 1615 操作 (作成・削除・rename・書き込み・600+ ファイルの索引拡張) を実行 →
  WinPE の **`chkdsk` が「問題は見つかりませんでした」**、Windows がボリュームから読み戻した 1215 ファイルが ntfs-3g の見たツリーと sha256 まで一致。
- UBSan: 上流の `memcpy(NULL, ..., 0)` (nonnull-attribute) だけ上流 C で除外。
- 未対応/次: 64KiB クラスタの `$MFTMirr` 偽陽性 (自動で読み取り専用)、電源断模擬、圧縮・疎ファイルへの書き込み、fsync/flush の意味づけ、`usbmem_ntfs_write` の mtime 等の属性、
  書き込み中の USB 抜去時の整合性、実機 USB。

### 電源断の模擬 (2026-11-11)
- `CrashDevice` (test_ntfs.cpp, `USBMEM_CRASH_AT/MODE/TORN/SEED`): N 回目の write_at で電源が落ち、以降の書き込みは消える。
  `prefix` = 発行順に反映する素直なデバイス / `cache` = flush までバッファし、電源断時に未 flush 分のランダムな部分集合だけ反映 (順序保証のない USB キャッシュ) /
  `torn` = 落ちる瞬間の書き込みを 512B セクタ境界で途中まで反映。
- **最初の発見**: 上流の `ntfs_set_volume_flags` は非同期なので、`cache` モードでは「他の更新は届いたのにダーティ印だけ届かない」状態が起きた
  (Windows の `chkdsk` は修復前に「MFT ビットマップが正しくありません」「$I30 のインデックス エントリが正しくありません」を報告)。
  → **`begin_write()`**: 最初の変更系 API の前に `VOLUME_IS_DIRTY` をメディアへ書いて flush まで確定してから処理を始める (失敗なら書き込み拒否)。アンマウント時は最終状態 (ダーティ解除) の後にもデバイス flush。
  修正後、`run_crash_tests.sh` の不変条件「意味のある差分がメディアに届いているならダーティ印も届いている」が全試行で成立。
- 不変条件の検証 (`tools/run_crash_tests.sh`, ctest `ntfs_crash` は軽量版): 213 回の書き込みになる操作列 (mkdir/create/write/rename/unlink 混在) を、
  序盤を密に全域を粗くサンプリングした点で切断 → (A) 元ファイル全件が manifest どおり読める (B) ダーティ印がある。80 試行で違反 0。
- **Windows による検証** (旧実装でダーティ印なしのまま落とした 6 イメージ、`USBMEM_VERIFY_FIX=1 tools/winpe/verify_with_windows.sh`): `chkdsk` が指摘 (索引エントリ不正・MFT ビットマップ) を `chkdsk /f` で修正し、
  **修復後の 2 回目 chkdsk は 6 件すべて「問題は見つかりませんでした」、Windows が読み戻した元 1020 ファイルは全件 manifest と一致、修復後イメージを自前ドライバで読んでも全件一致**。
  (データを失うのは電源断で書きかけだった新規ファイルだけ。)
- 既知の限界: MFT レコード (1024B) がセクタ境界で途中までしか書かれると (`torn=1`) `$Volume` の fixup が食い違いマウントできないことがある (80 試行中 2)。
  `ntfsfix` も「Incomplete multi sector transfer detected in $Volume」で拒否 = ジャーナル無しの書き込みに共通の限界。
  緩和案 (TODO): MFT のシステムレコード 0-3 の fixup 失敗時は `$MFTMirr` のコピーで読む (読み取り専用)。Windows の `chkdsk` が直せるかは未確認。

### Android (SAF) 経由の書き込み (2026-11-11, 実験的・既定は無効)
- **二重の安全弁**: (1) ビルド: `-PusbmemNtfsWrite=true` を付けたビルドだけ書き込み可能 (既定は無効 → `mount_rw` が -EROFS)。(2) 実行時: UI の「書き込みを許可」スイッチ (保存しない = 起動のたびに明示許可)。
  永続化された URI の復元は常に読み取り専用。上流が異常 (ダーティ等) で読み取り専用に落とした場合は `NtfsVolume.writable = false` となり SAF の書き込み系は拒否される。USB 経路は読み取り専用のまま (実機検証前)。
- 追加: `truncate` (SAF の "wt")、`writable()`、JNI `nativeNtfsMountRw/Write/Create/Remove/Rename/Truncate/Sync`、Kotlin `NtfsVolume.write/create/remove/rename/truncate/sync`。
  `NtfsDocumentsProvider`: `createDocument` (重複名は " (1)")、`deleteDocument`、`renameDocument`、`openDocument` の "w"/"wt"/"rw"/"rwt" (ProxyFileDescriptorCallback の `onWrite`/`onFsync`/`onRelease` で sync)。
  名前の検査 (`/` `\` 制御文字 `.` `..` を拒否)、ルートは削除・名前変更不可、空でないディレクトリの削除は上流が拒否。名前空間の変更は直後に sync。
  注意: Android 10 以降 "w" は切り詰めない ("wt" が切り詰め)。プロバイダはこの規約どおり。
- 検証 (x86_64 エミュレータ, ContentResolver = 他アプリと同じバインダ経路, `tools/run_android_write_test.sh`): mkdir / 日本語名の作成 / 300KB 書き込み・読み戻し / 重複名 / 切り詰め上書き /
  "rw" の途中書き込み / 既存ファイルの上書き / rename / 削除 / 不正名の拒否 / 読み取り専用ボリュームでの create・delete 拒否 → すべて成功。
  結果イメージを `adb exec-out run-as` で取り出し、ホストの `ntfsfix -n` と ntfs-3g (ro) で全 812 エントリの名前・サイズ・sha256 を期待値と照合 → **不一致 0**。
  既定 (書き込み無効) ビルドでは書き込みテストがスキップされ、既存の 8 件は成功。arm64-v8a は書き込み有効でもビルド成功 (実行は未検証)。
- 環境メモ: JDK は `/run/media/u/disk9/usr` (JAVA_HOME)、SDK は `/run/media/u/disk31/android_home`。エミュレータは `-port 5556` で自前起動し、`ANDROID_SERIAL=emulator-5556` を必ず指定 (他のデバイスには触れない)。


## USB 書き込み (作成のみ) と実機での発見 (2026-11-11)
- 実機 RAID (Ratoc RS-EC32-U3R, 7.3 TiB NTFS) の**マウントが 3 分以上かかる**不具合: 互換層の `queue_work` が同期実行だったため、マウント時の「空きクラスタ数の事前計算」(全 $Bitmap 読み取り, 約 12 万回の READ) を待っていた。`queue_work` を保留にし、`wait_event` で待つ必要が出たときだけ実行 → 読み取り専用マウントは 3 TiB で 5 ms。書き込み可のマウントは、最初の書き込みで止まらないようマウント内で実行する。
- 実機 RAID は**クラスタ 2048B**で、書き込みマウントが旧安全策 (クラスタ < 4096 を拒否) に当たった (ログの READ 回数 119344 ≒ 7.3TiB/2048/8/4096 から推定)。上記のとおり互換層を直した。
- USB 書き込みは「作成のみ」: `NtfsVolume.createOnly` が、削除・名前変更・切り詰め・既存ファイルへの書き込みを拒否し (EPERM)、書き込みはこのセッションで作ったファイルだけに許す。書き込み可は USB を開く前のスイッチで決める (開き直すと OS が EFI を再マウントするため)。
- 検証: 模擬 BOT (USB Mass Storage) 越しの NTFS 書き込み (SCSI opcode が既知のものだけ、WRITE/SYNC が出ていること + ntfsfix + ntfs-3g リファレンスとのツリー比較)、操作なしの rw マウントでメディア不変 (3 TiB 疎イメージ, ハッシュ)、Android の「作成のみ」計装テスト。
