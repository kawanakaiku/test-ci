# CI (GitHub Actions): Android 計装テスト

`.github/workflows/android-emulator.yml` が、`reactivecircus/android-emulator-runner` で x86_64 のエミュレータ (AVD) を起動し、
ローカルで使っている `tools/run_android_<fs>_test.sh` をそのまま呼んで FS ごとの計装テストを実行する。まとめ役は `tools/ci_android_tests.sh`。

## 使い方

- トリガは `workflow_dispatch` (手動) と、`usbmem-ci` ブランチへの push だけ。他のブランチでは動かない。
  `workflow_dispatch` はワークフローが既定ブランチにあるリポジトリでしか手動実行できないので、既定ブランチに置かない運用 (テスト用の別リポジトリに `usbmem-ci` ブランチを push する) では push で起動する。
- 実行: `git push <リポジトリ> HEAD:refs/heads/usbmem-ci` → `gh run watch` / `gh run view --log-failed`。
- ジョブは matrix で 5 グループに分かれ、`max-parallel: 4`・`fail-fast: false` (1 グループが落ちても他は最後まで走る)。
  各ジョブが独立に「SDK/NDK 導入 → APK ビルド → フィクスチャ生成 → エミュレータ起動 → テスト」を行う。結果の表は各ジョブの Summary にも出る。
- 失敗時のログ: artifact `logs-<グループ>` (`ci-logs/`: FS ごとのテスト出力 `run_<fs>.log`、フィクスチャ生成の出力 `gen_<fs>.log`、`logcat.txt` (adb logcat 全体)、`summary.md`)。
  テスト結果は `am instrument` の出力 (JUnit の XML ではない。Gradle の `connectedCheck` は使わない: テスト後にアンインストールされ、書き込みテストの結果イメージが消えるため)。

## グループ

`tools/ci_android_tests.sh` の `group_fs()` が定義する。最大 4 並列なので 5 つ目は 4 つのどれかが終わってから始まる。

| グループ | 中身 (`tools/run_android_<fs>_test.sh` を呼ぶ) | ジョブ時間の目安 |
|---|---|---|
| `ext-fat` | `fat`、`fat_write`、`ext2_write`、`exfat_write` | 約 7 分 |
| `btrfs-xfs` | `btrfs` (16 イメージ)、`btrfs_write` (書き込み: `run_android_btrfs_write_test.sh`)、`xfs`、`xfs_write` (書き込み: `run_android_xfs_write_test.sh`。CreateOnly の範囲) | 約 8 分 (btrfs が約 80-100 秒) |
| `squash-erofs` | `squashfs`、`erofs`、`f2fs` (`ext` イメージのみ) | 約 6 分 |
| `nested-ntfs` | `nested`、`write` (NTFS 書き込み + SAF) | 約 5 分 |
| `ro-misc` | `ext2_read`、`ext4_read`、`exfat_read`、`iso_read`、`ntfs_read`、`app_misc` | 約 6 分 |

`ro-misc` は専用の `run_android_*_test.sh` が無いクラスを `ci_android_tests.sh` が直接 push して実行する (`am instrument -r` の結果を数え、失敗・スキップ (assumption 失敗 = フィクスチャ未送信) が 1 つでもあれば失敗にする)。
`app_misc` は DocumentsProviderTest / LinkProviderTest / WriteSelfTestInstrumented / RawImageInstrumentedTest / TransferTuningTest をまとめて実行する。

## ローカルとの違い

- ビルドはジョブごとに 1 回だけ (`-PusbmemNtfsWrite=true`、arm64-v8a と x86_64) 行い、各スクリプトには `USBMEM_SKIP_BUILD=1` を渡してビルドを省く。
  FAT / ネストなど、ローカルでは読み取り専用ビルドを使うスクリプトも、CI では書き込み有効ビルドの APK で動く (読み取りテストの結果は同じ)。
- エミュレータは `emulator-5554` (runner の既定ポート)。スクリプトは `ANDROID_SERIAL` / `ADB_SERIAL` が `emulator-*` のときだけ動き、実機のシリアルは拒否する (ローカルと同じ判定)。
- フィクスチャは `/dev/shm` ではなく `$RUNNER_TEMP/usbmem_fix` に作る。書き込み系スクリプトの作業場所も `$FIX/tmp` (`USBMEM_TMP`)。
- `tools/gen_erofs_fixture.py` は `USBMEM_FIXTURE_LENIENT=1` (CI だけ) で、ツール側の不具合による一部の検証失敗を警告/省略にする。
- `run_android_btrfs_test.sh` は push 失敗でも FAIL を数える (CI で見逃さないため)。
- スクリプトが「フィクスチャ/ツールが無いので skip (終了コード 77)」で終わった場合や、テストが 0 件の場合は CI では失敗として数える。
- AVD は `google_apis` / x86_64 / API 34。android-emulator-runner の AVD スナップショットキャッシュ (`actions/cache`) を使うので、2 回目以降は起動が速い。
  プロファイルは pixel_6、コア 3・RAM 3 GB。エミュレータのオプションは `-no-window -no-audio -no-snapshot-save -gpu swiftshader_indirect -no-boot-anim -camera-back none` (キャッシュ作成のステップだけ snapshot を保存する)。
- `flock /tmp/usbmem_avd.lock` は nested のスクリプトが内部で取る。CI はジョブごとに別マシン・別エミュレータなので競合しない。

## 必要なホストツール (Ubuntu の apt パッケージ)

`e2fsprogs fuse2fs fuse3 mtools p7zip-full dosfstools exfatprogs exfat-fuse ntfs-3g btrfs-progs xfsprogs f2fs-tools erofs-utils squashfs-tools zstd lzop lz4 xz-utils gdisk fdisk gcc python3 bc genisoimage xorriso`
(フィクスチャ生成と、ext2 / exFAT / NTFS の書き込み結果の独立検証 (fsck、fuse2fs / exfat-fuse / ntfs-3g による参照実装との比較) に使う)。
Java は Temurin 17、Gradle は 9.0.0 (ラッパー)、NDK 30.0.16248370、CMake 4.1.2、platform android-36 (`android/app/build.gradle.kts` に合わせている)。

## キャッシュ

`actions/cache` で全ジョブ共通のキーを使う: Gradle (`~/.gradle/caches`, `wrapper`)、SDK の追加分 (NDK / CMake / platform / build-tools)、AVD (`~/.android/avd`)。

## 結果 (実測)

最終の成功 run: https://github.com/kawanakaiku/test-ci/actions/runs/43378532024 (コミット 6d5944c、ブランチ `usbmem-ci`)。
全体 (5 ジョブ、4 並列 + 1) で約 11.5 分。各ジョブは 5-8 分で、そのうちビルド (Gradle/SDK キャッシュ命中時) とエミュレータ起動が大半を占め、テスト本体は FS ごとに 2-80 秒。

テスト件数 (`am instrument` の `OK (N tests)` の合計。すべて PASS):

| FS | 件数 | 備考 |
|---|---|---|
| fat | 6 | |
| fat_write | 3 | FAT12 / FAT16 / FAT32 の操作列 (fsck.fat・7z・ホスト参照ツリーとの比較) + ダーティ 2 種が読み取り専用に落ちる |
| ext2_write | 3 | ホスト側の fsck / fuse2fs 参照実装との比較を含む |
| exfat_write | 2 | 同上 (exfat-fuse) |
| btrfs | 18 | 16 イメージ (チェックサム 4 種・圧縮 4 種・ノード/セクタサイズ・dup・subvol・ログ再生拒否など) |
| xfs | 7 | |
| squashfs | 8 | |
| erofs | 18 | |
| f2fs | 1 | `mountsAndVerifiesMkfsSload` のみ |
| nested | 13 | |
| write (NTFS) | 4 | ntfs-3g の参照との比較 (13 エントリ、不一致 0) を含む |
| ext2_read | 5 | |
| ext4_read | 9 | |
| exfat_read | 3 | |
| iso_read | 5 | |
| ntfs_read | 2 | |
| app_misc | 12 | DocumentsProvider / Link / WriteSelf / RawImage / TransferTuning |
| 合計 | 116 | |

## 既知の制約

- erofs: Ubuntu 26.04 の `mkfs.erofs` 1.9.1 が作る `lz4_dedupe` イメージは `fsck.erofs` にも本アプリのドライバにも拒否される (作り手側の不具合)。CI では `USBMEM_FIXTURE_LENIENT=1` でその変種を省く。
  `lz4_fragments` の `fsck.erofs` による独立した展開確認も、展開できないので警告にして続ける (ローカルでは従来どおり失敗扱い)。host-tests.yml の ctest も同じ環境変数を渡す。
- f2fs: `ext` イメージ (mkfs + sload.f2fs) のみ。QEMU で作る `k_*` のカーネル製イメージは CI に無い。
- 実行スクリプトが無く CI に入っていないテストクラス: `UdfInstrumentedTest`、`DocumentsWriteTest`、`NestedMountTest` の一部 (nested スクリプトが扱う分は実行される) など。ローカルの `tools/run_android_*` に無いものは未対応。
- テストは `assumeTrue` でフィクスチャが無いと黙って skip になる。`ci_android_tests.sh` は 0 件・rc 77・skip を失敗にして見逃しを防ぐが、新しいテストを足したら group_fs と生成/実行の分岐を足すこと。
- Btrfs: 既定の AVD のヒープ上限は 192 MB で、旧テストは 206 MB のイメージを `readBytes()` で全部読んで OOM になった。テストをストリームの SHA-256 にして解決した (AVD 側の `vm.heapSize` や `adb root` での書き換えは効かなかった)。
- 書き込み系スクリプト (`ext2_write` / `exfat_write` / `write`) の作業場所は、ローカルでは `/dev/shm`、CI では `$FIX/tmp` (`USBMEM_TMP`)。runner の `/dev/shm` では FUSE の後始末 (`rm -rf` の失敗が EXIT trap の終了コードを変える) で失敗した。
- runner の `/dev/shm` で FUSE が使えない原因 (Android CI の「FUSE の後始末で失敗」と同じ): ubuntu 26.04 の AppArmor の `fusermount3` プロファイルが、FUSE のマウント / アンマウントを `/home`・`/mnt`・`/media`・`/tmp`・`/run/user/<uid>` の下に限る
  (`/dev/shm`・`/var/tmp` は不可)。`fuse2fs` / `exfat-fuse` の mount は `fusermount3: mount failed: Permission denied`、root 権限の helper で mount された `ntfs-3g` の `fusermount3 -u` は `Operation not permitted` で落ちる。
  host-tests.yml は `/etc/apparmor.d/local/fusermount3` に `/dev/shm` の mount / umount を足して `apparmor_parser -r` で読み直す (プロファイルを無効にはしない)。ホストのテストはフィクスチャも作業場所も `/dev/shm` 固定なので、スクリプト側を直すより環境を整える方が影響が小さい。
- xfsprogs 6.18 (ubuntu 26.04) の `mkfs.xfs -p <dir>` は、ブロックサイズが 4 KiB より大きい (16 KiB / 64 KiB) と、1 ブロックの中にデータと穴 (ページ単位) が混ざる疎ファイルの内容を取り違えて書く。
  `xfs_repair -n` は clean と言うが、カーネルで loop マウントして元のファイルと比べると違う (実カーネルの読みと本ドライバの読みは一致するので、ドライバの不具合ではない)。xfsprogs 7.2 では起きない。
  `tools/gen_xfs_fixture.py` は 4 KiB より大きいブロックの変種で、データを含むブロックの中の穴をゼロで埋めてから (内容と mtime は変わらない。ブロック全体の穴は残す) `mkfs.xfs -p` に渡す。
- runner の FUSE: `fusermount3` に `user_allow_other` が要る (`/etc/fuse.conf` に追記)。ホストツールのバージョンは `ubuntu-latest` (今は 26.04) の追従で変わる。
- FAT の書き込みは host-tests.yml の wrel で `tools/run_fat_kernel_tests.sh` (sudo + loop の `mount -t vfat -o utf8=1`) を走らせ、カーネル vfat と双方向に突き合わせる。ctest が既存の環境要因で落ちても走る。`ctest_regex=fat_gen_fixture` の手動実行ならこのステップだけ走る (約 3 分)。
- ビルドは arm64-v8a と x86_64 の両方が作られる (ABI を絞ると APK のパスが変わる)。ビルド時間は Gradle/SDK キャッシュで短縮している。
- Actions のキャッシュ (Gradle / SDK / AVD) は同じリポジトリの他のワークフローのキャッシュを押し出す可能性がある。キー: AVD は `v5` を上げると作り直し。
- push で起動するのは `usbmem-ci` ブランチだけ。テスト専用リポジトリ (公開) に push する運用で、このリポジトリのフィクスチャや鍵は含まれない。

## Host tests (host-tests.yml) が赤かった原因と対処 (2026-11-14)

`.github/workflows/host-tests.yml` は ctest を 4 構成 (plain / san / wsan / wrel) で走らせる。ubuntu-latest が 26.04 になってから ctest 130 件中 13 件 (wrel は 148 件中 20 件) が落ちていた。直近の変更ではなく環境の変化が原因で、1 件だけ製品コードの不具合が隠れていた。

| 失敗 | 原因 | 対処 |
|---|---|---|
| `partedit_tools` | exfatprogs 1.3.2 の `mkfs.exfat -U` はボリューム GUID を RFC 4122 の並びで書く (1.4.3 は Microsoft 形式)。ドライバは仕様どおり Microsoft 形式で読むので、期待値と食い違った | テスト側: イメージに実際に書かれたバイト列を探し、それを Microsoft 形式で読んだ GUID を期待値にする (どちらの並びも見つからなければ失敗) |
| `ntfs_gen_fixture` ほか (`disktools_tools`、`ntfs_read`、`ntfs_fuzz`、`fat_probe`、`ntfs_gen_fixture_c2048`、`ntfs_write` が連鎖) / `ext2_write` / `ext4_write` / `ext4_crash` / `exfat_write` | AppArmor の `fusermount3` プロファイルが `/dev/shm` への FUSE マウントとアンマウントを許さない (下の既知の制約) | host-tests.yml に `/dev/shm` を許可するローカル上書きを足す |
| `fat_write`・`xfs_gen_fixture`・`xfs_write`・`xfs_crash` (環境を直した後に顕在化) | runner の `/dev/shm` はユーザーのクォータが約 6.3 GB で、全フィクスチャで超える ("Disk quota exceeded") | クォータなしの tmpfs (10 GB) を `/dev/shm` に重ねる |
| `erofs_gen_fixture` と依存する 5 件 | `fsck.erofs` が `lz4_fragments` を展開できない (`failed to verify packed file`)。やり直しが `File exists` で失敗して原因が隠れていた | `USBMEM_FIXTURE_LENIENT=1` を渡す (Android CI と同じ)。やり直しの前に出力先を消す |
| `xfs_read` (`v5_bs64k` のみ) | xfsprogs 6.18 の `mkfs.xfs -p` の不具合 (既知の制約の項) | `gen_xfs_fixture.py` で穴をゼロで埋めてから `mkfs.xfs -p` に渡す |
| `erofs_fuzz` (fixture が直ってから顕在化) | **製品コードの不具合**: マウント失敗時の後始末 (`get_tree_bdev` の失敗経路) が inode を先に解放してから `erofs_kill_sb` を呼ぶため、`erofs_drop_internal_inodes` が解放済みの packed inode を `iput` する (UAF・二重解放)。ルート inode の読み込みだけが失敗する壊れたイメージ (`bogus i_mode`) で起きる。fragments (packed inode) のイメージが対象 | erofs だけ `kill_sb` を先に呼ぶ (rt_core.c)。回帰テストを `erofs_corrupt` に追加 (ルート inode の i_mode を 0 にしてチェックサムを無効にした 5 イメージ。ASan で修正前の UAF を再現) |
