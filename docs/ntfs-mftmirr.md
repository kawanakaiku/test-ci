# NTFS の `$MFTMirr` の扱い (2026-11-19)

2 つの話: **(A)** 64 KiB クラスタの Windows 製ボリュームが `$MFTMirr` の偽陽性で書き込めない件の修正、**(B)** torn write で壊れた MFT システムレコード (0-3) を `$MFTMirr` のコピーで読み出す緩和 (読み取り専用のみ)。

## (A) 64 KiB クラスタの `$MFTMirr` 偽陽性

### 原因 (上流の比較の範囲)
上流 (`fs-ntfs/super.c`) の `parse_ntfs_boot_sector()` は、ミラーの大きさ `vol->mftmirr_size` を「クラスタが 4 レコード以下なら 4、それより大きければ **1 クラスタに入るだけのレコード数**」とする
(64 KiB クラスタ・1 KiB レコードなら **64 レコード**)。`check_mft_mirror()` はその全レコードを `$MFT` と `memcmp` し、1 つでも違えば
`"$MFT and $MFTMirr record N do not match.  Run chkdsk."` → 書き込みマウントなら `SB_RDONLY` に落とす (`load_system_files()`)。

mkntfs が作るミラーは 1 クラスタ分すべてが `$MFT` の先頭と同じなので通るが、**Windows は大きなクラスタのミラーを全部は同期しない**。
独立実装の記述 (tuxera/ntfs-3g の edge ブランチのソースを読んで確認):
- `ntfsprogs/ntfsfix.c` の `fix_mftmirr()`: 「Since 2017, Windows 10 does not mirror to full $MFTMirr when using big clusters, and some records may be found different. Nevertheless chkdsk.exe mirrors it fully, so we do similarly.」
- `libntfs-3g/volume.c` の `ntfs_volume_startup()` のマウント時比較: 「Windows 10 does not update the full $MFTMirr any more」とし、比較を `i < FILE_first_user` (= 先頭 **16** レコード) に限る。
- 実測 (docs/phase2-spike-result.md): Windows 製 `win_gpt64k` で最初の不一致はレコード 24 (0-23 は一致。24 以降は `$Extend` の子など)。

つまり上流の比較範囲 (1 クラスタ分) が広すぎる偽陽性。4 KiB クラスタ (ミラーも 4 レコード) では起きない。大きな USB ディスク (64 KiB クラスタが既定のことがある) の実害は「書き込みを要求しても自動で読み取り専用になる」。

### 修正 (最小の変更)
比較を **MFT レコード 0-3 (`$MFT` / `$MFTMirr` / `$LogFile` / `$Volume`) だけ**にする (`check_mft_mirror()` のループの終端を 4 に)。
- 4 にした理由: 4 レコードは Microsoft が `$MFTMirr` の中身と説明する範囲 (MFT 自身の復旧に要る) で、ミラーを使うのはこの範囲だけ。ntfs-3g のマウント時比較は 16 だが、4 はその部分集合なので「ntfs-3g が許す状態」をすべて許す
  (4-15 の不一致を見逃すが、読み書きの安全には関係しない。ミラーを読む実装はこの層の (B) だけで、それも 0-3 だけ)。
- ミラーの実体 (`mftmirr_size`、書き込み時に先頭 `mftmirr_size` レコードを同期する範囲、ミラーのランリスト検査) は**上流のまま**。書き込みで MFT レコードを更新したとき、64 KiB クラスタでもミラーの先頭 64 レコードを同期する
  (Windows より多く同期するだけで害はなく、ntfs-3g が見る先頭 16 レコードのミラーが古くならない)。
- 適用方法: `third_party/` は無改変 (UPSTREAM.md の方針) のまま、`core/compat/ntfs_patch.cmake` が configure 時に `super.c` をビルドディレクトリへ複製して該当の 1 行を置換する。
  `patch` コマンドに依存せず、置換元が無い / 一意でない (上流の更新で前提が変わった) と configure が失敗する。`core/compat/CMakeLists.txt` が `super.c` をこの複製に差し替える。一覧は `third_party/ntfs-resurrection/patches/README.md`。
- 対象外: ダーティ / 未再生ログの扱い (`mount_rw` が先に読み取り専用で調べて止める構造、docs/dirty-volumes.md) には触れていない。ミラーの本物の不一致 (先頭 4 レコード) は従来どおり読み取り専用に落ちる。

### 検証 (ctest `ntfs_mirror`、`tools/run_ntfs_mirror_tests.sh`)
`tools/gen_ntfs_mirror_fixture.py` が mkntfs (`-c 65536`、64 MiB) + ntfs-3g で `a.txt` と `d/f00..f29.txt` を作り、その写しの `$MFT` / `$MFTMirr` を直接編集する。
mkntfs のミラーは 1 クラスタ分すべてが `$MFT` と同じなので、Windows が書いた後の状態を「**使用中の MFT レコード 16-63 (ntfs-3g は利用者のファイルを 64 より後ろに置くので、実際には `$Extend` の子の 24-26) の LSN を `$MFT` 側だけ進め、使っていないスロットにはミラーだけ古い写しを置く**」ことで再現する。

| イメージ | 状態 | 期待 |
|---|---|---|
| `plain` / `plain4k` | ミラーは `$MFT` と一致 (対照。64 KiB / 4 KiB) | 書き込み可 |
| `win` | 64 KiB。レコード 16 以降のミラーが古い (Windows 製の状態) | **書き込み可** (修正前は読み取り専用に落ちた) |
| `win_mid` | `win` に加え、レコード 5・10・11 も `$MFT` 側だけ更新 (ntfs-3g のマウントは拒否する範囲) | 書き込み可 |
| `bad0` / `bad3` | 先頭 4 レコード内 (`$MFT` / `$Volume`) の本物の不一致 (負の対照) | 読み取り専用のまま (**書かれず、イメージ不変**) |

書き込み可の 4 つには `new.txt` (200000 バイト)・`newdir/inner.txt` を作って `sync` → アンマウントし、読み戻して内容を照合 (`state() == 0` = ダーティフラグが下りている)。
そのイメージを独立実装で検証する: `ntfs-3g.probe --readwrite` (`win_mid` を除く)、`ntfsfix -n` (対照 `plain` / `plain4k` は成功を要求。`win*` は ntfsfix 自身がミラーの古いレコードを「直す」と言って `-n` では失敗扱いにするので、報告だけを記録)、自前の fsck (`usbmem_disktools_cli check --fs ntfs`。ミラーは先頭 4 レコードだけを見る。エラーが無いこと)。
書き込みビルド (`USBMEM_NTFS_WRITE=ON`) でなければ読み取り専用の部分だけ走る。

## (B) torn で壊れた MFT システムレコード (0-3) を `$MFTMirr` で読む (読み取り専用)

### 問題
ジャーナル無しの書き込み (電源断の模擬、docs/phase3-progress.md「電源断の模擬」) で、MFT レコード (1 KiB = 2 セクタ) がセクタの途中までしか書かれない (`torn=1`) と、レコード内のマルチセクタ保護 (fixup の USN) が食い違う。
上流はそのレコード (特に `$MFT` 自身・`$MFTMirr`・`$LogFile`・`$Volume`) を読めずマウントに失敗する (80 試行中 2 件)。`ntfsfix` も同じ理由で拒否する。上流の `ntfs_read_inode_mount()` には「FIXME: Try to use the $MFTMirr now.」とだけある。

### 緩和 (`core/compat/src/ntfs_volume.cpp`、上流は無改変)
1. **通常のマウントが失敗したときだけ**試す (健全なボリュームは従来どおり、余計な読み取りもしない)。
2. ブートセクタから `$MFT` / `$MFTMirr` の位置とレコード長を読み、先頭 4 レコードを両方読む。レコードが使えるかは、`FILE` の印・fixup 配列の位置と個数・**全セクタ末尾の USN の一致**・`bytes_allocated`・`bytes_in_use`・(NTFS 3.1 なら) レコード番号欄。
3. `$MFT` 側が使えず `$MFTMirr` 側が使えるレコードがあれば、そのレコードの位置の**読み取りだけ**を `$MFTMirr` のコピーに差し替える読み取り専用のデバイス (`MirrorOverlayDevice`。書き込みは `ReadOnly` で拒否) を作り、それ越しに**読み取り専用で**マウントする。書き込み可を要求されても読み取り専用 (`writable() == false`)。
   ログ・ダーティフラグ・`$MFT` にも触れず、イメージは 1 バイトも変わらない。
4. `state()` に `USBMEM_VOL_MFT_FROM_MIRROR` (32) を立てる (UI は `VolumeStateNotice` が「MFT の一部が壊れている。読み取り専用。chkdsk /f を」と案内する)。
5. `$MFTMirr` 側も壊れている (差し替えるものが無い) 場合は救わない (従来どおりマウント失敗)。

限界: コピーは**最後に同期された時点**のもの。壊れたレコードがその後に更新されていた (最後の書き込みが途中だった) なら、読み出す内容は更新前になる。`$MFT` (レコード 0) の古いコピーは、
最新のランリストを欠くことがある (MFT が拡張された直後など)。内容が最新でない可能性は UI の文言に書いた。先頭 4 レコードが連続していること (`$MFT` の最初のエクステントの先頭) を前提にする (Windows / mkntfs の既定。違っても、壊れた位置にある別の壊れた領域を差し替えるだけ)。
修復 (書き戻し) はしない: それは Windows の `chkdsk` または `ntfsfix` の仕事。

### 検証
`tools/gen_ntfs_mirror_fixture.py` が 4 KiB クラスタのボリュームで、レコード n の先頭 1 セクタだけ新しい内容 (USN も新しい) で残りが古い状態を作る (`torn0`..`torn3`、`torn03`、ミラーも壊した `torn3m`)。
修正前の CI (run 44505055906、wrel) の実測: `torn0` / `torn3` / `torn03` は `fill_super failed: -22` でマウントできない。一方 `torn1` (`$MFTMirr`) と `torn2` (`$LogFile`) は上流が「Errors were recorded during mount.  Mounting read-only」で読み取り専用のままマウントでき (壊れたシステムファイルだけ読めない)、書き込み可は要求しても落ちる。このため緩和は**通常のマウントが失敗したときだけ**使い (0 / 3 が対象)、1 / 2 は従来の挙動 (読み取り専用・ミラー不使用) を確認するだけにしている。
`usbmem_ntfs_tests --mirror` が、`torn0` / `torn3` / `torn03` は読み取り専用でマウントできる・`state()` に `MFT_FROM_MIRROR` が立つ・`mount_rw` でも `writable() == false` で `create` が `-EROFS`・`a.txt` と `d/` の内容が読める・イメージ不変を、`torn3m` はマウントできない (読み取り専用・書き込み可とも失敗) ことを確かめる。

### Windows の chkdsk での回復可否 (今回は対象外 — 見積もり)
Windows イメージが要る。手順の見積もり: (1) `torn0`..`torn3` と同じ破壊を Windows 製ボリュームに行う (VHDX を `tools/winvhd/` の VM に渡す。`$MFT` / `$Volume` のレコードを直接書き換える)、
(2) WinPE (`tools/winpe/verify_with_windows.sh` の仕組み) で `chkdsk /f` を実行して回復後の `chkdsk` が「問題は見つかりませんでした」になるか、(3) 回復後のイメージを自前ドライバで読んで manifest と照合。
`tools/winpe/make_windows_fixtures.sh` で作った既存のイメージを流用すれば 1 イメージあたり数分、WinPE の起動を含めて 1 回 15 分程度の見込み (ただし Windows イメージ (ISO) の用意が前提)。**未実施**。

## CI の結果
- 修正前 (テストだけのコミット、wrel): run 44505055906。`ntfs_mirror` が失敗 — `win` / `win_mid` は `writable=0 want=1`、`torn0` / `torn3` / `torn03` は `fill_super failed: -22`。対照 (`plain` / `plain4k` / `bad0` / `bad3` / `torn3m`) は期待どおり。
- 修正後 (wrel): run 44509397523 成功 (`ntfs_mirror` 通過)。
- 最終確認 `usbmem-ci-full-mftmirr` (コミット 5d305a0): Host tests run 44512729677 は plain / san / wsan / wrel の 4 構成すべて成功 (146 分)。Android emulator tests run 44512729772 は、初回の ext-fat (Gradle の依存解決が HTTP 429) と crypt-ui (既知の `CalledFromWrongThreadException`、docs/ci.md) の失敗を `gh run rerun --failed` して全グループ成功。

## 未検証・残る課題
- **実 Windows 製の 64 KiB クラスタのイメージでの確認** (`win` は mkntfs 製のイメージに Windows の状態を模した編集をしたもの。ctest `ntfs_windows_images` は Windows 製イメージが無く skip のまま)。レコード 4-15 を Windows が実際にどう更新するかも未確認 (修正は 0-3 だけを見るので、どちらでも通る)。
- (B) の Windows の `chkdsk` での回復可否 (上記)、`$MFT` の古いコピーを使うときの内容の古さの実害。
