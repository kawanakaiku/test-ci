# CI (GitHub Actions): Android 計装テスト

`.github/workflows/android-emulator.yml` が、`reactivecircus/android-emulator-runner` で x86_64 のエミュレータ (AVD) を起動し、
ローカルで使っている `tools/run_android_<fs>_test.sh` をそのまま呼んで FS ごとの計装テストを実行する。まとめ役は `tools/ci_android_tests.sh`。

## 使い方

- トリガは `workflow_dispatch` (手動) と、`usbmem-ci` ブランチへの push だけ。他のブランチでは動かない。
  `workflow_dispatch` はワークフローが既定ブランチにあるリポジトリでしか手動実行できないので、既定ブランチに置かない運用 (テスト用の別リポジトリに `usbmem-ci` ブランチを push する) では push で起動する。
- 1 グループだけ回したいとき (修正の繰り返し): `crypt-ui` と `ro-misc` は、ブランチ名を `usbmem-ci-only-crypt-ui` / `usbmem-ci-only-ro-misc` (`-only-<グループ>` で終わる名前) にして push すると、そのグループだけが起動する (どちらも Host tests は「未知の構成」で即失敗するが無視してよい) (式に関数 `replace` が無いので汎用にはしていない)。`workflow_dispatch` の `groups` 入力なら任意のグループを 1 つ指定できる (ワークフローが既定ブランチにあるときだけ)。
- 実行: `git push <リポジトリ> HEAD:refs/heads/usbmem-ci` → `gh run watch` / `gh run view --log-failed`。
- ジョブは matrix で 6 グループに分かれ、`max-parallel: 4`・`fail-fast: false` (1 グループが落ちても他は最後まで走る)。
  各ジョブが独立に「SDK/NDK 導入 → APK ビルド → フィクスチャ生成 → エミュレータ起動 → テスト」を行う。結果の表は各ジョブの Summary にも出る。
- 失敗時のログ: artifact `logs-<グループ>` (`ci-logs/`: FS ごとのテスト出力 `run_<fs>.log`、フィクスチャ生成の出力 `gen_<fs>.log`、`logcat.txt` (adb logcat 全体)、`summary.md`)。
  テスト結果は `am instrument` の出力 (JUnit の XML ではない。Gradle の `connectedCheck` は使わない: テスト後にアンインストールされ、書き込みテストの結果イメージが消えるため)。

## グループ

`tools/ci_android_tests.sh` の `group_fs()` が定義する。最大 4 並列なので 5 つ目は 4 つのどれかが終わってから始まる。

| グループ | 中身 (`tools/run_android_<fs>_test.sh` を呼ぶ) | ジョブ時間の目安 |
|---|---|---|
| `ext-fat` | `fat`、`fat_write`、`ext2_write`、`exfat_write` | 約 7 分 |
| `btrfs-xfs` | `btrfs` (16 イメージ)、`btrfs_write` (書き込み: `run_android_btrfs_write_test.sh`)、`xfs`、`xfs_write` (書き込み: `run_android_xfs_write_test.sh`。CreateOnly の範囲 + Full の操作) | 約 8 分 (btrfs が約 80-100 秒) |
| `squash-erofs` | `squashfs`、`erofs`、`f2fs` (`ext` イメージのみ) | 約 6 分 |
| `nested-ntfs` | `nested`、`write` (NTFS 書き込み + SAF) | 約 5 分 |
| `crypt-ui` | `crypt_ui` (`CryptoUiTest`: 暗号ボリュームの UI。実画面を操作して LUKS2 / BitLocker を解錠・書き込み・ロック、中の ext4 のジャーナル未再生の「読み取り専用で開く」確認、パスワードの記憶 (`vault_*` 4 件。本物の AndroidKeyStore の鍵 + 偽の認証)。このグループのジョブは `VaultTest` (JVM 単体試験) も Gradle で実行する。フィクスチャは `tools/gen_crypt_ui_fixture.py`。docs/crypto-volumes.md 10 章) | 10 件成功 (run 43935001413、コミット 5204d16、約 9 分。パスワードの記憶 `vault_*` 4 件 + `VaultTest` の JVM 単体試験も成功)。以前: 6 件 (run 43917601086。単独実行 15 分。3 件は ジャーナル未再生の確認フロー)。以前: 3 件 (run 43660009210。全 6 グループ成功、18 分) |
| `ro-misc` | `ext2_read`、`ext4_read`、`exfat_read`、`iso_read`、`ntfs_read`、`app_misc` | 約 6 分 |

`ro-misc` は専用の `run_android_*_test.sh` が無いクラスを `ci_android_tests.sh` が直接 push して実行する (`am instrument -r` の結果を数え、失敗・スキップ (assumption 失敗 = フィクスチャ未送信) が 1 つでもあれば失敗にする)。
`app_misc` は DocumentsProviderTest / LinkProviderTest / WriteSelfTestInstrumented / RawImageInstrumentedTest / TransferTuningTest / VaultRealPathTest (本物の BiometricManager 経路。下の「AndroidManifest の権限…」節) をまとめて実行する。`ntfs_read` の `DirtyConfirmUiTest` はジャーナル未再生の ext4 (`ext4_e4_needs_recovery.img`) も使うので、`ext4` のフィクスチャも端末へ送る。

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
| ntfs_state | 1 | ダーティ / 未再生ログ / 休止状態の NTFS 5 種 (`gen_ntfs_state_fixture.py`) の `state()`・書き込みの拒否・イメージ不変 (docs/dirty-volumes.md)。Android 側は `ro-misc` の `ntfs_read` が同じ 5 種を使う `VolumeStateInstrumentedTest` と、ダーティな NTFS の確認ダイアログを実画面で通す `DirtyConfirmUiTest` (イメージ画面・偽の USB セッション。回転も) |
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

## Gradle の一時失敗の再試行 (`tools/ci/retry_gradle.sh`、2026-11-20)

依存解決の一時失敗 (Google / Maven Central への HTTP 429、`Could not find org.jetbrains.kotlin:kotlin-stdlib:2.2.0` がどのリポジトリからも取れない、接続リセット、タイムアウト) で「Build APKs」が落ちることが直近の run で複数回あった (ext-fat など。`gh run rerun --failed` で成功する)。ワークフロー側で自動的に吸収する。

- 呼び出し: `tools/ci/retry_gradle.sh ./gradlew --no-daemon ...` の形で Gradle を包む。対象は `android-emulator.yml` の「Build APKs」と「JVM unit tests (VaultTest)」、`build-apk.yml` の「Build APKs (write-enabled)」。`tools/ci_android_tests.sh` と `tools/run_android_*_test.sh` は CI では `USBMEM_SKIP_BUILD=1` でビルドしないので、Gradle を呼ぶのはこの 3 ステップだけ。
- 再試行する条件: コマンドが失敗し、ログに一時失敗のしるし (`Could not resolve `、`Could not find group:artifact:version`、`Could not GET/HEAD/download`、`status code 429/5xx`、`Too Many Requests`、`Connection reset`、`Connect/Read timed out`、`UnknownHostException`、`SSLHandshakeException` など) があり、かつ本当の失敗のしるし (Kotlin の `e: `、`: error:` (C++ / Java)、`Compilation error`、`There were failing tests`、`Unresolved reference`) が無いとき。
  本当の失敗のしるしが同居していれば再試行しない (同じ失敗が繰り返されるだけ)。`Execution failed for task ':app:compileDebugKotlin'` は依存解決の失敗でも出るので、判定には使わない。`Could not find method ...` のようなビルドスクリプトの誤りは再試行しない。
- 回数と待ち: 最大 3 回実行 (失敗 → 30 秒待ち → 失敗 → 90 秒待ち → 最後の 1 回)。再試行のたびに `::warning::` を出す (どのしるしで再試行したかが run の注釈に残る)。3 回続けて失敗したら `::error::` で諦め、終了コードは Gradle のものをそのまま返す。環境変数 `RETRY_GRADLE_DELAYS` (既定 `30 90`) で待ちを変えられる (要素数 = やり直しの回数)。
- `--refresh-dependencies` は使わない: キャッシュ済みの依存を全部取り直すので、リモートへの要求が増えて 429 を悪化させる。再試行は同じ `~/.gradle/caches` のまま、取れなかった分だけ取り直す。
- 単体試験: `bash tools/test_retry_gradle.sh` (fake のコマンドで 14 ケース。429 で再試行して成功、ずっと 429 なら 3 回でやめる、Kotlin / C++ のコンパイルエラーとテスト失敗では再試行しない、など。Gradle もネットワークも不要で 1 秒未満)。

### 効いているキャッシュと `--offline` を使えない理由

- Gradle のキャッシュ (`actions/cache`、キーは `gradle-<OS>-<*.gradle.kts と wrapper の hash>`、`restore-keys` で前のキーに fallback) は効いている。ただしキーが変わる (依存の更新や `*.gradle.kts` の編集) 最初の run は、fallback のキャッシュに無い分だけ取りに行く。また `actions/cache` はジョブが成功したときだけ保存するので、依存取得で落ちた run のあとも同じキーは未保存のまま (再実行で成功すれば保存される)。
- `--offline` はキャッシュ未命中・部分命中のとき確実に落ちるので使わない。

### 提案 (入れていない)

- リポジトリの優先順位は `android/settings.gradle.kts` の `google()` → `mavenCentral()` (plugin は後ろに `gradlePluginPortal()`)。入れるなら次の案:
  - `mavenCentral()` のミラー (例: Google の Maven Central ミラー `https://maven-central.storage-download.googleapis.com/maven2/`) を `mavenCentral()` の後ろに足す。Maven Central が 429 のとき、同じ成果物をミラーから取れる。`FAIL_ON_PROJECT_REPOS` なので `settings.gradle.kts` の 2 か所 (`pluginManagement` と `dependencyResolutionManagement`) に書く。最後の候補にするので、ミラーが古くても通常のビルドは変わらない。
  - `exclusiveContent` / `content { includeGroupByRegex(...) }` で `google()` には `androidx.*` / `com.android.*` / `com.google.*` だけを聞くようにする。無駄な問い合わせ (と 429 の機会) が減る。
  - Gradle の組み込みの HTTP 再試行を伸ばす `-Dorg.gradle.internal.repository.max.retries=<回数>` / `-Dorg.gradle.internal.repository.initial.backoff=<ミリ秒>` (内部プロパティ。Gradle のバージョンが上がると効かなくなる可能性があるので、入れるなら `gradle.properties` ではなく CI の `GRADLE_OPTS` で)。
  再試行スクリプトで足りなければ、この順に検討する。

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

確認 (2026-11-14): 修正後の `usbmem-ci-hosttests` ブランチで Host tests (run 43572195880) は plain / san / wsan / wrel の 4 構成すべて成功 (plain 130 件、wrel 148 件)。同じブランチの Android emulator tests (run 43572195885) も 5 ジョブすべて成功。変更したのは host-tests.yml だけなので、btrfs-write・xfs-write は影響を受けない (どちらも自分のブランチでしか動かない)。

## 大きな疎イメージは nbdfuse で作る (`tools/ci/nbdfuse.sh`)

大きなテスト用イメージ (C 相当の 29 GiB、XFS の `large`、Btrfs の 4.5 GiB 入りなど) を runner のディスクや `/dev/shm` に疎ファイルとして置くと、ディスク不足か `/dev/shm` のユーザークォータ (EDQUOT) に当たる。代わりに `nbdfuse` + `nbdkit memory allocator=sparse` で作る。ファイルシステムには書かず、触れたページだけプロセスのメモリに持つ。手元でも同じ手順を使う (Data の NTFS に大きな書き込みをしない)。

```sh
. tools/ci/nbdfuse.sh
nbdfuse_install            # CI のみ: nbdkit / libnbd-bin / fuse3
nbdfuse_apparmor           # CI のみ: fusermount3 の FUSE マウントを許可 (host-tests.yml と同じ対処)
nbdfuse_start "$NBDFUSE_ROOT/c" 31G     # $NBDFUSE_ROOT/c/nbd が 31 GiB の疎ファイル
...                                      # mkfs / テスト
nbdfuse_stop "$NBDFUSE_ROOT/c"
```

確認 (`tools/ci/nbdfuse_smoke.sh`、`.github/workflows/nbdfuse-smoke.yml`、run 43588211176、2026-11-14): ubuntu-latest で 40 GiB / 100 GiB のイメージ、39 GiB 付近の読み書き、`mkfs.xfs` (agcount 4) が通り、nbdkit の常駐メモリは 6 MiB。手元 (tmpfs) でも 40 GiB で 9 MiB。ルート権限は要らない。マウントポイントは `NBDFUSE_ROOT` (既定 `/dev/shm/nbdfuse`) の下に作る。

次の段階: 大きなイメージを使う既存の試験 (host-tests の XFS / Btrfs の大きなフィクスチャ、`xfs-write.yml` など) をこの関数に載せ替える、実際の XFS / Btrfs のイメージを nbdkit に載せてアプリの NBD クライアントで読み書きする通し試験を足す。

## AndroidManifest の権限・機能・コンポーネントの検査 (`manifest` ジョブ、2026-11-19)

背景: `AndroidManifest.xml` に `USE_BIOMETRIC` が無く、`BiometricManager.canAuthenticate` が SecurityException を投げて、パスワードの記憶が実機・AVD で一切出なかった。CI の計装テストは偽の vault を差す (`VaultProvider.override`) ので気づけなかった (docs/crypto-volumes.md 末尾の「修正 (2026-11-19)」)。同種の漏れを、ビルドもエミュレータも要らずに数秒で検出する。

- **`tools/check_manifest_permissions.py`** (標準ライブラリだけの Python): `android/app/src/main/java` の Kotlin / Java (コメントは除く) を、規則表 `RULES` (API の使用 → 要る権限 / uses-feature) と、マニフェストの宣言に突き合わせる。
  - 漏れ: 例 `BiometricManager` / `BiometricPrompt` → `USE_BIOMETRIC`、`UsbManager` / `UsbDevice` → `uses-feature android.hardware.usb.host` (`required="false"`)、`startForeground` → `FOREGROUND_SERVICE`、`NotificationManager` など → `POST_NOTIFICATIONS`、`java.net.Socket` / `URL` など → `INTERNET`、`Environment.getExternalStorageDirectory` など → `READ/WRITE/MANAGE_EXTERNAL_STORAGE`、そのほか WAKE_LOCK / VIBRATE / CAMERA / RECORD_AUDIO / 位置 / Bluetooth / 起動 / 正確なアラーム / インストール / WRITE_SETTINGS / オーバーレイ (規則は 21 件)。`Manifest.permission.X` や `"android.permission.X"` を名指しした使用も、その権限の宣言を要求する。
  - 余り: マニフェストの `uses-permission` が、どの規則・名指しの使用でも根拠づけられないと失敗 (使わない権限を残さない)。使う API を足したら、規則表にも足す。
  - コンポーネント: `Activity` / `Service` / `BroadcastReceiver` / `ContentProvider` / `DocumentsProvider` を継承する名前つきクラスが、マニフェストに `<activity>` などで宣言されていなければ失敗 (匿名の `object : BroadcastReceiver()` は対象外)。
  - 終了コード 0 = 問題なし / 1 = 問題あり (標準エラーに `NG [規則名] ...` と使用箇所) / 2 = 入力の誤り。`--manifest` / `--src` で対象を差し替えられる。
  - 限界: ソースの文字列照合なので、リフレクションや間接呼び出しは見ない。core の C++ (NBD のネットワーク接続など) も見ない (アプリは NBD に host を渡さないので `INTERNET` は不要)。権限の要否そのものは API ごとの知識 (規則表) に依存する。
- **`tools/test_check_manifest_permissions.py`** (unittest、13 件): 実マニフェストが通る / **`USE_BIOMETRIC` の行を消した実マニフェストが実ソースに対して失敗する (過去の漏れの再現)** / 各規則の漏れ・コメントは無視・匿名 receiver は対象外・余りの検出・コンポーネントの宣言漏れ。
- 実行場所: `android-emulator.yml` の `manifest` ジョブ (ubuntu-latest、約 10 秒。matrix の `android` ジョブとは独立なので、落ちても他は走る。`usbmem-ci*` の push のたびに動く) と、`build-apk.yml` の最初のステップ。手元は `python3 -I tools/test_check_manifest_permissions.py && python3 -I tools/check_manifest_permissions.py` (Gradle 不要)。
- 過去の漏れの検出の確認: `6ffa362` の 1 つ前のマニフェスト (`USE_BIOMETRIC` なし) を `--manifest` で渡すと `NG [biometric] 権限の宣言漏れ: android.permission.USE_BIOMETRIC が AndroidManifest に無い ... KeystoreVault.kt:5 ...` で終了コード 1。現在のマニフェストは 0。

**本物の (偽でない) Keystore / BiometricManager 経路の計装テスト** (`VaultRealPathTest`、`ro-misc` の `app_misc`): `VaultProvider.override` を使わず、アプリの権限で本物を呼ぶ。(1) `PackageManager.checkPermission(USE_BIOMETRIC)` が GRANTED、(2) `BiometricManager.canAuthenticate(BIOMETRIC_STRONG or DEVICE_CREDENTIAL)` が SecurityException なしで既知の値を返す、(3) `VaultProvider.supportState(ctx)` (`SecretVault` を通さない。通すと例外が `Unavailable` に丸められる) と `VaultProvider.get(ctx).supportState()` が一致し、`KeyguardManager.isDeviceSecure` が偽 (画面ロック未設定の CI のエミュレータ) なら `NoCredential`、真 (AVD で PIN 設定済み) なら `Supported`。`USE_BIOMETRIC` を外したビルドで失敗することは、変異ブランチ `usbmem-ci-mut-only-ro-misc` の CI で確認した。

結果 (2026-11-19、コミット dfaa738):
- 通常: `usbmem-ci-only-ro-misc` run 44442949470 成功 (`manifest` ジョブ成功、`ntfs_read` 22 件 (`DirtyConfirmUiTest` の新しい横並び・ext4 の文言の試験を含む)、`app_misc` 15 件 (`VaultRealPathTest` 3 件を含む))、`usbmem-ci-only-crypt-ui` run 44442953102 は 12 件成功 (最初の実行は `journalNeedsRecovery_confirmOpensReadOnly` / `_mixedInnerPartitions` が失敗: compose-test の再コンポーズが IO のワーカースレッドで走って `CalledFromWrongThreadException` になる競合。同じコミットの失敗ジョブの再実行 (`gh run rerun --failed`) で 12 件成功。原因と修正は下の「crypt-ui の CalledFromWrongThreadException」)。
- 変異 (`USE_BIOMETRIC` の行を消したコミット 698867c、ブランチ `usbmem-ci-mut-only-ro-misc`、run 44441291200): `manifest` ジョブが失敗 (`NG [biometric] ...`)、計装テストは `VaultRealPathTest` の 3 件とも失敗 (`SecurityException: Must have USE_BIOMETRIC permission` ×2、`manifestDeclaresUseBiometric` の `expected:<0> but was:<-1>`)。= この仕組みは、過去の漏れを静的にも実機相当の経路でも検出する。
- ブランチ名の注意: `usbmem-ci-mut-only-ro-misc` のように `-only-ro-misc` で終わる任意の名前は ro-misc だけ回るが、`usbmem-ci-*` なので Host tests も (通常の 2 構成で) 起動する。

## 暗号ボリュームの書き込み (`crypt-write.yml`、`usbmem-crypt-write` ブランチ)

暗号層の書き込み (docs/crypto-volumes.md 9 章) の独立検証。`host-tests.yml` (`usbmem-ci*`) の単体テストに加えて、sudo + loop + `cryptsetup open` が要るものをここで行う。トリガは workflow_dispatch と `usbmem-crypt-write` への push。

- `crypt write`: `USBMEM_NTFS=ON USBMEM_NTFS_WRITE=ON` のビルド → 単体テスト → カーネル dm-crypt の事前確認 → `tools/run_crypt_write_tests.sh` (`USBMEM_KERNEL_DMCRYPT=1`: LUKS の書き込み結果を実カーネルの dm-crypt で復号して cmp。BitLocker は libbde と bitlocker_synth.py) → `tools/run_crypt_fs_tests.sh` (暗号層の上の ext4 / ext2 / NTFS / FAT。実カーネルの ext4 / vfat でも ro マウントして照合)。
- `crypt mutation`: `tools/run_crypt_mutation_tests.sh` (安全機構を外した 10 種のビルドがすべて検出される)。
- `crypt san`: ASan + UBSan のビルドで単体テストと `run_crypt_write_tests.sh` (ファジング・失敗注入)。
- 作業ディレクトリは `/tmp` (`USBMEM_WORK_DIR`)。`/dev/shm` はユーザークォータがあるので使わない。FUSE は AppArmor の都合で `/tmp` の下にだけマウントする。
- ローカルで `sync` が止まる PC 用の `LD_PRELOAD=/tmp/nosync.so` は、ここにも各スクリプトにも入れない (CI には不要)。
- 結果 (最終ツリー): crypt-write.yml run 43647958462 (3 ジョブ成功)、host-tests run 43647956713 (usbmem-ci-full-cryptwrite、plain / san / wsan / wrel 成功)。libbde 比較は、試験の平文の先頭 16 セクタが本物の NTFS (mkntfs) でないと、実行ごとに違うオフセットで EOF になる (docs/crypto-volumes.md 9.8)。host-tests には libbde が無いので、その比較は crypt-write.yml だけで動く。

## 暗号ボリュームの「ディスク全体」の試験 (cryptodisk_*、2026-11-14)

host-tests.yml の ctest に `cryptodisk_gen_fixture` / `cryptodisk_part` / `cryptodisk_whole` / `cryptodisk_nested` / `cryptodisk_errors` / `cryptodisk_high_offset` が入った (4 構成すべてで走る)。内容と結果は docs/crypto-volumes.md 6.5。CI 側の要点:
- 道具: `cryptsetup-bin gdisk fdisk e2fsprogs ntfs-3g (FUSE) nbdkit libnbd-bin` は既にあり、`python3-cryptography python3-numpy` を apt に足した (LUKS のペイロード暗号化と `bitlocker_synth.py` が使う。無いと既存の `crypto_volume_fixtures` (LUKS 35 ケース) も黙って SKIP になっていたはずなので、そちらも効くようになる)。
- `USBMEM_CRYPTODISK_REQUIRE=1` を ctest の step の env に渡し、道具が無いときは SKIP ではなく失敗にする。
- フィクスチャ (約 430 MB) は `/dev/shm/usbmem_cryptodisk_$USER` (10 GB の tmpfs の step の後)。NTFS の中身を入れるための `ntfs-3g` の FUSE マウントは `_work` の下 (= `/dev/shm/**`) で、AppArmor の許可の対象。
- 8 GiB の疎ディスクは `tools/ci/nbdfuse.sh` (nbdfuse_start/stop) で作る (`cryptodisk_high_offset`)。ファイルシステムには何も書かない。
- 実 Windows 製の BitLocker (`USBMEM_WINBL_DIR`) は CI に無いので、その構成は走らない。
- **結果**: run 43590409180 (最初の push): plain / san / wrel は成功 (cryptodisk 6 件とも Passed。plain で合計約 1.5 分、san で約 15 分: 異常系 39 件が 677 秒)。**wsan は 180 分の `timeout-minutes` で打ち切り** (cryptodisk 6 件は全て Passed。打ち切られたのは別の試験 `xfs_write` の途中で、wsan は元々 2.5 時間超かかる)。そこで `USBMEM_CRYPTODISK_JOBS=3` (ケースを 3 並列) にして異常系を短縮した。run 43605628654 (並列化後): **plain / san / wsan / wrel の 4 ジョブすべて成功** (所要 144 分、wsan が最長)。


## FUSE プログラムを載せる層の試作 (`fuseshim.yml`、`usbmem-fuseshim*` ブランチ、2026-11-18)

docs/fuse-layer.md の試作 (ntfs-3g / wimlib を libfuse 互換の shim の上で動かし、自前の NTFS と比較・測定する) の専用ジョブ。`usbmem-fuseshim*` ブランチへの push か workflow_dispatch で動く (約 10 分)。**製品のビルドには入らない** (CMake の `USBMEM_BUILD_FUSESHIM`、既定 OFF)。

- サードパーティのソースはリポジトリに置かず、`tools/fuseshim/fetch_ntfs3g.sh` / `fetch_wimlib.sh` が固定バージョンを **sha256 検証つき**で取得する。検証に失敗したらジョブを失敗にする (skip しない)。ntfs-3g の配布元は GitHub runner の既定 User-Agent に 403 を返すので、ブラウザ風の User-Agent を付け、失敗時は Debian の orig tarball (autoreconf が要る) に切り替える。
- 試験は `tools/run_fuseshim_tests.sh`: 自前の NTFS と shim 越しの ntfs-3g の全ツリー比較 (基本フィクスチャ 2 種 + 20000 ファイルの性能用イメージ)、別プロセス (socketpair RPC) 越し、wimlib の WIM 3 種 (LZX / 無圧縮 / LZMS)、性能測定。数字は共有 runner の参考値。
- 実測 (run 44189230731 成功): 比較はすべて一致。詳細は docs/fuse-layer.md の 5・6・9 章。
- 注意: `usbmem_fsdump_core` と `usbmem_fsdump_ntfs3g` は別の実行体 (自前の NTFS と libntfs-3g は `ntfs_*` のシンボルが衝突するため 1 つにできない)。

## CI の完了待ちは `tools/ci/gh_wait.sh` で 1 本だけ (CPU 暴走の再発防止)

`until [ "$(gh run view ID ...)" = completed ]; do sleep 20; done` のような待機ループを前景の Bash で何度も起動すると、ツールのタイムアウトで背景に回された古いループが終了せずに溜まる (2026-11-14 に 30 本超、同じ run を 4 本で待つ例も)。固まった `gh` が 1 個あたり CPU 約 30% を 10 分以上使い続け、PC がほぼフリーズした。

- 待つときは `tools/ci/gh_wait.sh RUN_ID [REPO] [INTERVAL秒] [MAX秒]` を **run_in_background で 1 本だけ**起動する。同じ run の二重起動は、ロックで即終了 (終了コード 3)。間隔は 60 秒以上 (既定 120 秒)、上限は 4 時間 (終了コード 4)。`gh` は 1 回ごとに `timeout 30` と `nice -n 19` で包む。
- 前景の Bash に、gh を呼ぶ `until` / `while` を書かない。状況確認は `gh run view ID --json status,conclusion` の単発 (連打しない)。
- `gh run view --log` で巨大なログを何度も取らない (1 回だけ、ファイルに落として grep)。
- 用が済んだ待機は止める。新しい run を push したら、古い run の待機は不要。

## Host tests の構成選びと並列実行 (待ち時間の短縮、2026-11-14)

Host tests (`host-tests.yml`) は、ctest が 1 本ずつ直列で、wsan が 139 分、san 66 分、wrel 34 分、plain 23 分かかっていた (4 構成を同時に走らせても、最長の wsan で全体が決まる)。次の 2 つで短くした。

1. **push で動く構成は、ブランチ名で決まる** (`plan` ジョブ)。

   | ブランチ名 | 動く構成 |
   |---|---|
   | `usbmem-ci-<名前>` (通常) | plain / wrel (軽い 2 構成) |
   | `usbmem-ci-full-<名前>` | plain / san / wsan / wrel (全部) |
   | `usbmem-ci-only-<構成>` (例 `usbmem-ci-only-wsan`) | その 1 構成だけ |
   | `workflow_dispatch` | 入力 `modes` (空白区切り、既定は全部) |

   公開版 (`.github/workflows/public-edition.yml`、docs/editions.md) は `usbmem-public` / `usbmem-public-<名前>` / `usbmem-ci-full-<名前>` への push と `workflow_dispatch` で動く (通常の `usbmem-ci-<名前>` では動かさない)。公開版ツリーを書き出して単独でビルド・テストする。公開版だけ確認したいときは `usbmem-public-<名前>` に push する。

   **統合前の最終確認は、`usbmem-ci-full-*` で 4 構成すべてを通すこと** (通常の push では san / wsan が走らない)。
2. **ctest を並列 (`-j4`、`CTEST_JOBS` で変更可)**。FIXTURES / RESOURCE_LOCK が宣言済みなので依存は守られる。並列で落ちたテストは `ctest --rerun-failed` で直列にやり直し、直列で通れば警告 (`::warning::`) と step summary に出して通す (並列で干渉するテストの目印)。

実測 (run 43597804995、`usbmem-ci-only-wsan`): wsan の ctest は **139 分 → 72 分** (run 全体 74 分)、並列で落ちたテストは無し (警告なし)。

## crypt-ui の `CalledFromWrongThreadException` (散発。原因と修正、2026-11-20)

`crypt-ui` (`CryptoUiTest`) が、ときどき `journalNeedsRecovery_confirmOpensReadOnly` / `_mixedInnerPartitions` / `vault_*` などで `CalledFromWrongThreadException` になり、`gh run rerun --failed` で通っていた (run 44442953102、44512729772 の初回。どちらも artifact `logs-crypt-ui` の logcat に全スタックがある)。

- スタック (logcat の `E TestRunner:`。`am instrument` の出力は 10 行で切れる): `ViewRootImpl.checkThread` ← `View.requestLayout` ← `AndroidComposeView.scheduleMeasureAndLayout` ← `LayoutNode.requestRemeasure` ← `NodeChain.updateNode` ← `Recomposer.runRecomposeAndApplyChanges` ← `TestMonotonicFrameClock.performFrame` ← `Snapshot.sendApplyNotifications` ← `ApplyingContinuationInterceptor$SendApplyContinuation.resumeWith` ← `DispatchedCoroutine.afterResume` (= `withContext(Dispatchers.IO)` の終わり) ← `CoroutineScheduler$Worker`。呼び出しスレッドは `DefaultDispatcher-worker-N`。
- 原因: compose-test の中の `rememberCoroutineScope()` / `LaunchedEffect` のコンテキストは recomposer のもの (`ApplyingContinuationInterceptor` + `FrameDeferringContinuationInterceptor`) で、コルーチンを再開したスレッドでそのまま続きを実行する。`scope.launch { busy = true; withContext(Dispatchers.IO) { ... }; busy = false; result = ... }` の形では、IO から戻ったワーカースレッドで state の更新と `sendApplyNotifications` が走り、recomposer が待機中だとフレーム (再コンポーズ・モディファイア更新・`requestLayout`) までワーカーで走る。製品の scope は `AndroidUiDispatcher` (メイン) なので、実害はテスト環境で顕在化する。散発する理由 (recomposer が待機中かどうかの競合、レイアウトに影響する変更のときだけ例外になる) は推測で、未検証。
- 修正 (コミット 2da5194): `ui/Components.kt` の `rememberUiScope()` (= `rememberCoroutineScope { Dispatchers.Main }`) と `LaunchedEffectUi()` (本体を `withContext(Dispatchers.Main)` で包む) を、`ImageScreen` / `LinkDialog` / `NestedMountSection` の `rememberCoroutineScope` と、IO を含む `LaunchedEffect` の代わりに使う。新しい画面で IO のあとに state を書くときも、素の `rememberCoroutineScope` / `LaunchedEffect` ではなくこの 2 つを使うこと。
- 確認: `usbmem-ci-only-crypt-ui` run 44591300790 (コミット 2da5194) を attempt 1 から 4 まで (`gh run rerun` で全体を再実行) 連続 4 回成功 (`CalledFromWrongThreadException` なし)。修正前は直近 2 run のどちらも初回で失敗していたが、もとの失敗率が高くないので 4 回では確率的な裏付けにとどまる。再発したら、logcat のスタックと失敗したテストをここに足す。
