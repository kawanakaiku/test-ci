# 公開前の監査 (2026-11-19)

方針: 「公開リポジトリは後で可。ただし先に監査する。キー・認証情報・メールアドレス・絶対パスをリポジトリに入れない」。
対象は作業ツリーと `git log --all` の全履歴 (329 コミット、全ブランチ)。**この監査では公開 (push・可視性の変更) をしていない。履歴の書き換えもしていない。**
本書には秘密・メールアドレス・固有のユーザー名・ホスト名の値を書かない (場所と種類だけ)。

## 結論

- **秘密情報の混入は無い。** 公開を妨げる技術的な問題は見つからなかった。
- **公開前に必須**: (1) アプリ全体のライセンスの決定と、ルートの `LICENSE` の追加 (3 章)、(2) `third_party/*/COPYING` に GPL-2.0 の全文が無い問題 (3 章)、(3) `docs/licenses.md` を実態に合わせる (3 章)。
- **公開前に判断**: 履歴にある `.pyc` が開発機の絶対パスを含む (2 章)、`<アカウント>/test-ci` への参照 (5 章)、README の整備 (6 章)、公開するブランチを `main` だけにする。
- 監査で直したもの (4 つのコミット。「直したもの」の節): スクリプト・docs・テストの絶対パスとホスト名、空の誤コミットファイル、`.gitignore`。

| 重大度 | 件数 | 内容 |
|---|---|---|
| 重大 (秘密の漏えい) | 0 | — |
| 要対応 (公開前に必須) | 3 | ライセンス未決定 / COPYING に GPL 全文なし / licenses.md が実態とずれ |
| 要確認 (ユーザーの判断) | 8 | 履歴の `.pyc`、test-ci 参照、機種名、ボリューム ID、`/tmp` の固定名、カーネルパッケージの既定パス 2 件、ブランチ名 |
| 直した | 6 | 絶対パス (スクリプト 8 ファイル)、ホスト名 (docs 5 箇所)、docs の絶対パス、テストの模擬値、`android/nice`、`.gitignore` |

## 1. 秘密情報 — OK

| 検査 | 結果 |
|---|---|
| gitleaks (全履歴 `--log-opts=--all` と作業ツリー `--no-git`、約 14 MB) | 検出 0 件 |
| 秘密鍵 (`BEGIN ... PRIVATE KEY`)、GitHub / AWS / Google / Slack / OpenAI 形式のトークンの `git grep` | 0 件 |
| 追跡ファイル・履歴に追加されたファイルの拡張子 / 名前 (`*.jks` `*.keystore` `*.pem` `*.key` `*.p12` `.env` `id_rsa` `adbkey` `local.properties` `keystore.properties` `*.apk` `*.img` `secret` `credential`) | 該当は `KeystoreVault.kt` (Android Keystore API のコード。鍵ではない) のみ |
| 署名 | `android/app/build.gradle.kts` は debug 用に標準の公開値 (`~/.android/debug.keystore`、パスワード `android`。Android SDK 既定で秘密ではない)。リリース署名は `~/.gradle/gradle.properties` の `usbmemKeystore*` で外部指定し、リポジトリに無い (`.gitignore` に `*.jks` `*.keystore`)。docs/TODO.md が書くリリース鍵のファイル名は場所の記述だけで、鍵そのものではない |
| GitHub Actions の `secrets.*` / `GITHUB_TOKEN` の埋め込み | 無し |
| テスト用パスワード | `usbmem-test-pass` (LUKS2 の合成フィクスチャ)、`synth-Password-1` (BitLocker 風の合成フィクスチャ)。**テスト専用で、公開してよい** (`tools/make_real_usb_testset.py`、`tools/bitlocker_synth.py`、`tools/run_*_tests.sh`、`docs/real-usb-testset.md`、`android/app/src/androidTest/.../RealUsb*Test.kt` に出る)。BitLocker の回復パスワード・BEK を書き出す `tools/winvhd/make_*` も使い捨て値を出力するだけで、値はコミットされていない |
| 高エントロピー文字列 | gitleaks に加え、`core/tests/crypto_vectors.inc` (209 KB) は既知値テストの暗号ベクタ (HMAC / XTS の公開テストベクタ。鍵ではない) |

## 2. 個人情報

### 2.1 コミットの作者情報 — OK
`git log --format='%an <%ae> | %cn <%ce>' --all` は `Claude Code <noreply@...>` の 1 種類 (329 件) だけ。個人のメールアドレス・氏名なし。コミットメッセージにも個人のメールアドレス・パス・ホスト名は無い (`Co-Authored-By` のみ)。`git config user.*` も同じ。

### 2.2 メールアドレス — OK
`third_party` 以外の追跡ファイルにメールアドレスは無い。履歴の追加行にも無い。`third_party` のカーネル由来ファイルのメールアドレスは上流の著作権表示 (公開情報)。

### 2.3 絶対パス・ホスト名 — 直した (一部は要確認)
作業ツリーで見つけたもの (開発機の `/run/media/<ユーザー>/...`、`/home/<ユーザー>/...`):

| 場所 | 内容 | 状態 |
|---|---|---|
| `tools/run_android_{btrfs,erofs,f2fs,squashfs,xfs}_test.sh` | adb の既定パス | **直した**: `${ANDROID_HOME:+$ANDROID_HOME/platform-tools/}adb` (第 2 引数での指定は従来どおり) |
| `tools/run_android_fat_test.sh` | `android-sdk-exec` の既定パス | **直した**: PATH 上の `android-sdk-exec` (`ANDROID_SDK_EXEC` で指定も可) |
| `tools/winvhd/make_bitlocker_matrix.sh`、`make_encrypted_fixtures.sh`、`README.md` | w11 ツールの既定パス | **直した**: 引数 → 環境変数 `W11` → PATH 上の `w11` |
| `third_party/ntfs-resurrection/patches/host-kmod-build.sh.txt` | `~/Downloads` の絶対パス (参考用で実行不可の拡張子。上流のファイルではなく自前) | **直した**: `$HOME/Downloads` |
| `docs/real-usb-testset.md` | 検証用 PC への SSH 接続名 (ホスト名) 5 箇所 | **直した**: `ssh <pc>` |
| `docs/phase3-progress.md` | 開発機の JDK / SDK のパス | **直した**: プレースホルダ |
| `tools/test_make_real_usb_testset.py` | 模擬マウント点のユーザー名 3 箇所 | **直した**: `user` (入力と期待値を同時に変えた。テストは 110 件中、直す前から失敗している 1 件 `test_dry_run_default_runs_nothing` (xfs の mkfs 探査の呼び出しが入る) を除き通る。今回の変更とは無関係) |
| `tools/xfs_vm/kreplay.py:21-22`、`tools/f2fs_vm/gen_k_fixtures.py:20-21` | cachyos カーネルパッケージの既定の置き場所 (開発機の絶対パス 4 行) | **要確認 (直していない)**: `xfsfollow` の worktree と衝突しやすく、既定値を変えると開発機の挙動が変わるため。案: 環境変数 `USBMEM_KERNEL_PKG_DIR` から作る (環境変数 `USBMEM_XFS_KERNEL_PKG` / `USBMEM_F2FS_KERNEL_PKG` は既にある) |
| `tools/winvhd/*.sh`、`*.ps1` の `C:\Users\user\` | Windows ゲスト VM の既定アカウント名 `user` (汎用名) | OK |
| `tools/**` の `/tmp/ntfs_basic.img` など | 固定名の `/tmp` の作業ファイル (個人情報ではない) | 要確認 (低): 公開するなら `mktemp` に寄せるのが望ましい |
| `docs/real-usb-testset.md` の `/media/usb/ntfs`、`OsMountGuardTest.kt` の `/mnt/media_rw/...` | 例示 | OK |
| `~/.gradle` `~/.android` `~/.cache` | 一般的なホーム相対パス | OK |

**履歴には残る** (書き換えはしない): 上記の絶対パスは過去のコミットに残る。履歴に出た固有の絶対パスは、開発機のユーザー名を含む `/run/media/<ユーザー>/{Data,disk9,disk31}`、`/home/<ユーザー>`、SSH 接続名 (docs の追加行)。特に **`tools/__pycache__/*.pyc` と `tools/fuse/__pycache__/*.pyc` (20 個のブロブ、コミット 2c7f3b7 で追加、8a892c4 で削除。`.gitignore` 済み)** は、コンパイル時のソースの絶対パス (`/run/media/<ユーザー>/...`) を含む。個人のメールアドレス・秘密ではなく、ユーザー名と開発機のディレクトリ構成が分かる程度。
**提案**: 公開時に履歴を残すかを決める。(a) このまま公開 (影響: ユーザー名とディレクトリ構成が見える程度)、(b) 公開用に `git filter-repo --path-glob '*.pyc' --invert-paths` で `.pyc` だけ除き、必要ならパスの置換 (`--replace-text`) も行った**新しい履歴**を公開用リポジトリに作る (影響: コミットハッシュが変わり、docs 中のコミット参照 (例: `main 22695e3`、`6d5944c`) と CI の run との対応が崩れる)、(c) 履歴を捨てて現在の状態から 1 コミットで公開 (最も安全だが、設計判断の経緯が消える)。

### 2.4 IP アドレス・SSH 設定・MAC・IMEI・シリアル — OK
`third_party` 以外に、エミュレータ (`10.0.2.x`) と `127.0.0.1` 以外の IP は無い。`.ssh` や `authorized_keys`、MAC アドレス、IMEI は無い。「serial」は FS のボリュームシリアル (生成したフィクスチャの値) か `ANDROID_SERIAL=emulator-*` の例のみで、**実機の ADB シリアルは無い** (`ci_android_tests.sh` などは `emulator-*` 以外を拒否する)。USB のシリアル番号は `RealUsbWriteTest.kt` がコードで取得して表示するだけで、値の記述は無い。

### 2.5 実機の機種名・識別子 — 要確認
公開してよいか判断を要するもの (シリアル・IMEI・MAC は無いので、機種名だけの話):

| 種別 | 値 | 出る場所 | 所見 |
|---|---|---|---|
| スマートフォン | F-51A、J9260、Xperia XQ-FS44、A80 Pro | docs/TODO.md、docs/media-kind.md、docs/real-usb-testset.md、docs/*-port.md、`MainActivity.kt` `UsbStorage.kt` `TransferTuning.kt` のコメント | 機種名は多くの場合 OK。「どの端末で何が起きたか」は技術情報として有用。**OK と判断してよいが、個人の所持品が分かる点だけユーザーが了承すること** |
| USB RAID ブリッジ | Ratoc RS-EC32-U3R (4 TB / 7.3 TiB)、5400 rpm | docs/TODO.md、docs/phase3-progress.md、`test_usb_msc.cpp` | 機種名のみ。OK |
| USB の VID:PID | `0930:6544`、`0781:5581`、`05e3:0749`、`05e3:0751`、`090c:2000`、`1908:0226` | docs、`RealUsb*Test.kt`、`MainActivity.kt` | 製品型番の識別子で個体識別ではない。OK |
| ボリュームのシリアル | `OsMountGuardTest.kt` の `/mnt/media_rw/FFD9-7D43`、`0922-588F` | 単体テストの模擬 `/proc/mounts` | 実機のボリューム ID の可能性がある (個体は特定されにくい)。気になるなら `AAAA-BBBB` 等に置換 |
| 実機の内容 | `docs/real-usb-testset.md` は `tools/make_real_usb_testset.py` が作る**決定的な合成データ** (ファイル 3029・4.57 GiB のマニフェスト等)。実機の RAID に作ったのは `usbmem_write_test` というフォルダと 286 KB のファイルだけで、**実データの記述は無い** | | OK |

## 3. ライセンス

### 3.1 現状 (作業ツリー)

| 項目 | 状態 |
|---|---|
| ルートの `LICENSE` / `COPYING` / `NOTICE` | **無い (要対応。決定後に追加。決定前なので作っていない。空のプレースホルダも作っていない)** |
| `core/src` `core/include` `core/tests` | SPDX は全 112 ファイルが `Apache-2.0` (一部は「本ファイル Apache-2.0、取り込み物は GPL-2.0 系」の注記つき) |
| `core/compat` (263 ファイル) | GPL-2.0-or-later 160、Apache-2.0 17、**SPDX なし 86** (公開 API ヘッダ `core/compat/public/usbmem/*` の 23 ファイルは GPL-2.0-or-later) |
| `core/fuseshim` (13) | すべて GPL-2.0-or-later |
| `android/app` (83 の .kt / .java) | SPDX あり 9 (Apache-2.0 6 / GPL 3)、**なし 74** |
| `tools` (135 の .py / .sh 等) | SPDX あり 19、なし 116 (テスト用スクリプトなので影響は小さいが、揃えるのが望ましい) |
| `third_party` (10 ディレクトリ) | すべてに `COPYING` と `UPSTREAM.md` あり (取り込み元のコミット 067c5c24… を記録)。ソースの SPDX: GPL-2.0-or-later 104、GPL-2.0 86、GPL-2.0-only 49、GPL-2.0+ OR BSD-3-Clause 31 (zstd)、GPL-2.0 WITH Linux-syscall-note 16、0BSD 9 (xz)、GPL-2.0+ 9、MIT 3、LGPL-2.1 3、LGPL-2.0+ 1。**SPDX の無い 22 ファイル** は上流にも SPDX が無いもの (`ext4/orphan.c`、`fat/nls/nls_cp437.c` `nls_cp932.c`、`kernel-lib` の lz4 / zlib / xxhash、`udf/ecma_167.h` `osta_udf.h`)。冒頭コメントに BSD-2-Clause (lz4)、zlib ライセンス、GPL の記載がある |
| `third_party/*/COPYING` | **全 10 個が同一 (496 バイト) の、カーネルの `COPYING` の導入部のみ** (「`LICENSES/preferred/GPL-2.0` に従う」と書いてあるだけ)。**GPL-2.0 の全文は無い。`UPSTREAM.md` の「COPYING は GPL-2.0 全文」は誤り (要対応)**。BSD-3 (zstd)・BSD-2 (lz4)・0BSD (xz)・zlib の条文は各ファイルの冒頭コメントにあるが、`LICENSES/` の個別ファイルは無い |
| `third_party/ntfs-resurrection/patches` | 上流への自前パッチ 3 本 (適用せず参考保存)。ファイルは無改変方針で、`UPSTREAM.md` の記述と一致 |
| `docs/licenses.md` | **実態とずれている (要対応)**: 「Linux `fs/ntfs` は未取り込み」だが実際は `third_party/ntfs-resurrection` に取り込み済み。ext2 / ext4 / exFAT / FAT (+ NLS) / ISO 9660 / UDF の取り込みが表に無い。`kernel-lib` の「各ファイルの精査は未了」。「Apache-2.0 (暫定)」の自前コード行と、実際には `core/compat` の大半が GPL-2.0-or-later 表記である点が整合していない |
| 使い捨てのツール類 (libbde、cryptsetup、nbdkit、qemu-img、ntfs-3g、wimlib など) | docs/licenses.md は「配布物に含めない・テスト時のみ」と記載。リポジトリにソースは無い (`tools/fuseshim/fetch_*.sh` が固定版を取得するだけ) — OK |

### 3.2 アプリ全体のライセンスの選択肢 (ユーザーの決定が要る)

事実関係: ネイティブライブラリには、**GPL-2.0-only** の Linux カーネル由来のファイル (ext2 / ext4 / FAT / ISO 9660 / UDF / erofs / squashfs など、`GPL-2.0` と `GPL-2.0-only` が計 135 ファイル程度) が静的に入る。したがって**ネイティブ層を 1 つの配布物にする限り、全体は GPL-2.0 の条件になる**。GPL-2.0-only のコードを含む結合物は GPL-3.0 にできない (GPL-3.0-or-later を全体のライセンスにする案は、`GPL-2.0-only` の FS を外すか別プロセスにしない限り成立しない)。Apache-2.0 は GPL-2.0 と非互換とされる (特許条項) ので、自前コードを GPL に寄せる場合は著作権者 (ユーザー) による再許諾が要る。

| 案 | 内容 | 長所 | 必要な作業・注意 |
|---|---|---|---|
| A. アプリ全体を GPL-2.0-or-later (結合物としては GPL-2.0) | 自前コードの SPDX を `GPL-2.0-or-later` に統一。ルートに GPL-2.0 の全文を `LICENSE` として置く | 最も単純。`core/compat` の現状 (GPL-2.0-or-later 表記) と合う。ソース公開で GPL の義務を果たせる | 自前の Apache-2.0 表記ファイル (約 112 + android) の SPDX を書き換え。Kotlin 側が使う AndroidX / Compose (Apache-2.0) との組み合わせの扱いを確認 (GPL-2.0 との非互換の懸念。アプリ側に追加許可 (例外条項) を付けるか、法務確認)。GPL-2.0 全文を `third_party/*/COPYING` 側にも置く。Google Play 配布時のソース提供の方法 |
| B. 全体を GPL-3.0-or-later | — | Apache-2.0 とは互換 | **現状は不可** (GPL-2.0-only のカーネル由来ファイルを含むため)。成立させるには、GPL-2.0-only の FS (ext2/ext4/FAT/ISO/UDF/erofs/squashfs) を外すか下記 C の別プロセスにする |
| C. プロセス分離: GPL 部分を別プロセス (別実行ファイル / 別 APK) に隔離し、アプリ本体は緩いライセンス (Apache-2.0) を維持 | 既に docs/fuse-layer.md が ntfs-3g / wimlib で採る方針 (主案) と同じ考え方を、カーネル由来の FS ドライバ全体に広げる | アプリ本体と公開 API を Apache-2.0 のままにできる | **大きな設計変更**: 現在 `core/compat` のドライバは JNI で同一プロセスにリンクされ、Apache の `core/src` が直接呼ぶ。IPC の層 (AIDL / ソケット) と、子プロセスの配布 (別 .so を `exec` する実行ファイル化、または `:isolated` サービス) が要る。通信の境界を「mere aggregation」と言えるプロトコルにする法務確認。GPL 側のソース提供・ライセンス全文・通知 |
| D. デュアルライセンス (GPL-2.0-or-later と商用/Apache) | 自前コードのみ | 将来の選択肢を残せる | カーネル由来の部分は GPL のままなので、配布物全体は結局 GPL。実質 A と同じ。自前コードの著作権者が 1 人 (ユーザー) であることを確認 |

**いずれの案でも必要なこと**: ルートの `LICENSE` (決定後)、`third_party/*/COPYING` の実態に合わせた修正 (GPL-2.0 全文の追加、`UPSTREAM.md` の文言の修正、BSD / 0BSD / zlib の条文を `LICENSES/` に置くか冒頭コメントで足りるかの判断)、`docs/licenses.md` の更新 (取り込み済みの FS の行を足し、「未取り込み」を直す)、`core/compat` `android/app` `tools` の SPDX の揃え、アプリ内の「オープンソースライセンス」画面 (配布時の表示義務)。**exFAT の特許・Play ポリシー・targetSdk は docs/licenses.md の未解決事項のまま**。

## 4. 配布物・不要物の混入 — OK (直したものを含む)

| 項目 | 結果 |
|---|---|
| リポジトリの大きさ | 追跡 1055 ファイル、`.git` は圧縮後 約 8.5 MiB (loose 3.26 MiB + pack 5.25 MiB)。履歴の最大ブロブは `third_party/fat/nls/nls_cp932.c` (491 KB。上流の文字表) で、バイナリの巨大ファイルは無い |
| 追跡ファイルの拡張子 | `.apk` `.aab` `.img` `.iso` `.bin` `.so` `.a` `.o` `.zip` `.7z` `.db` `.log` は無し (ディスクイメージのフィクスチャは `tools/gen_*` で都度生成し、コミットしていない) |
| `.pyc` | 現在の追跡には無い (8a892c4 で除外、`.gitignore` 済み)。**履歴には 20 個のブロブが残る** (2.3 章) |
| `.gradle` / `build/` / `local.properties` / `.idea` / `captures` | 追跡に無い (`.gitignore` 済み) |
| `android/nice` (空ファイル、誤コミット) | **直した**: 削除 |
| 作業ディレクトリの未追跡ファイル `pw` (21 バイト)、`usbmem_testset.log` | 追跡されていない (中身は読んでいない)。`git add -A` で誤って入らないよう、**直した**: `.gitignore` に `/pw` と `usbmem_testset.log` を追加。`pw` はパスワードのメモの可能性があるので、公開前に消すか別の場所へ移すのが安全 |
| 実機の USB の中身 | OK (2.5 章)。`docs/real-usb-testset.md` は合成データの作り方と手順で、実データは無い |
| `.gitignore` の妥当性 | 概ね妥当。`*.img` `*.bin` `*.jks` `*.keystore` を含む。`.env` / `*.pem` / `*.key` / `keystore.properties` は未記載 (今は無いが、予防として追加を推奨 — 未実施: 影響範囲が小さく、決めるのはユーザー) |
| `git count-objects` の `garbage` 警告 | worktree のメタデータ (`.git/worktrees/*/refs`) によるもの。公開物とは無関係 |

## 5. 外部への参照 — 要確認

テスト用リポジトリ `<アカウント>/test-ci` (GitHub のアカウント名が出る) への参照:

| 場所 | 内容 |
|---|---|
| `.github/workflows/build-apk.yml:3` | コメントの使い方 (push 先の URL) |
| `tools/ci/gh_wait.sh` | 使い方のコメントと、リポジトリの既定値 (引数 `REPO`) |
| `docs/ci.md` (1)、`docs/btrfs-write.md` (3)、`docs/crypto-volumes.md` (1)、`docs/fat-write.md` (1)、`docs/xfs-write.md` (1) | CI の run の URL とブランチ名 (`usbmem-ci-*`) |

判断: アカウント名はもともと公開される情報 (リポジトリの公開先が同じアカウントなら自然) ので、**公開に支障は無い**。ただし (a) テスト用リポジトリが非公開だと、docs 中の run の URL は読者が開けず、(b) 公開先が別のアカウントになる場合は書き換えが要る。公開先を決めたあとに、`gh_wait.sh` の既定値は `${GH_REPO:?}` 等にし、docs は `<owner>/test-ci` に置換する (今回は直していない)。ブランチ名 `usbmem-wt-*` / `worktree-agent-*` は docs に出ていないが、**リポジトリ自体にはこれらのブランチが多数ある。公開するのは `main` だけにする** (`git push <公開先> main`。`--all` / `--mirror` は使わない)。

## 6. README — 要対応 (ユーザーが決めたあとに書く)

ルートに `README.md` は**ある**が 8 行 (Phase 1 の記述のまま: 「現在 Phase 1 (RAW イメージ + MBR/GPT 列挙、読み取り専用)」)。実態 (多数の FS の読み書き、暗号ボリューム、ディスク編集) と合っていない。作り直さず、構成案だけを置く:

1. **概要**: root 不要で USB ストレージ / ディスクイメージを扱う Android アプリ。何ができるか (読み取り、実験的な書き込み、パーティション編集、暗号ボリューム、バックアップ)
2. **免責**: **書き込みは実験的**。データを失う可能性があり、重要なメディアでは読み取り専用で使うこと。実機での検証範囲 (docs/real-usb-testset.md の「検証したこと・できなかったこと」) へのリンク
3. **対応しているもの**: ファイルシステムの表 (docs/fs-feature-matrix.md と `tools/fs_feature_matrix.json` から生成。各 FS の「読み / 作成のみ / 全書き込み / fsck / mkfs」)、暗号ボリューム (LUKS1/2、BitLocker)、ディスクイメージ (QCOW2 / VHD / VHDX / VMDK / NBD)
4. **インストール**: リリース APK の入手先、署名、必要な Android のバージョン・権限 (USB ホスト)
5. **ビルド**: 必要なもの (JDK、Android SDK / NDK、CMake)、`cd android && ./gradlew :app:assembleDebug`、書き込み有効ビルド (`-PusbmemNtfsWrite=true`) の意味
6. **テスト**: ホスト側 (`tools/run_host_tests.sh`、4 構成)、AVD の計装テスト、実機テストは**実行者のデバイスを消す**ので手順 (docs/real-usb-testset.md) を読むこと、CI (docs/ci.md)
7. **設計と docs の案内**: docs/architecture.md、各 FS の `*-port.md` / `*-write.md`
8. **ライセンス**: 決定したライセンス、`third_party` の由来 (`UPSTREAM.md`)、ソース提供の方法、GPL の通知
9. **貢献・報告**: 不具合の報告先、データ破損の報告に添える情報 (診断画面の出力)
10. **プライバシー**: 通信・計測をしないこと (確認のうえ記載)

## 直したもの (コミット)

| コミット | 内容 |
|---|---|
| `1fa94f2` | スクリプトの絶対パスを環境変数 / PATH 参照に (run_android_*_test.sh 6 本、winvhd 3 ファイル、host-kmod-build.sh.txt) |
| `344266d` | docs のホスト名 (`ssh <pc>`)・JDK/SDK のパス、テストの模擬値 |
| `d3d31aa` | 空の `android/nice` を削除 |
| (この文書のコミット) | `docs/release-audit.md`、`.gitignore` に `/pw` と `usbmem_testset.log` |

## 公開前のチェックリスト (ユーザー用)

1. [ ] ライセンスを決める (3.2 章) → ルートの `LICENSE` を追加、SPDX を揃える、`docs/licenses.md` と `third_party/*/COPYING` / `UPSTREAM.md` を直す
2. [ ] 履歴の扱いを決める (2.3 章: そのまま / `.pyc` を除いた新しい履歴 / 1 コミットで公開)
3. [ ] 公開先のアカウントを決め、`test-ci` への参照を置換する (5 章)
4. [ ] 機種名・ボリューム ID を載せてよいか判断する (2.5 章)
5. [ ] README を書く (6 章)
6. [ ] 作業ディレクトリの `pw` を処分する
7. [ ] 公開する前に、公開用のクローンで `gitleaks detect --log-opts=--all` と `git grep` をもう一度かける
8. [ ] `main` だけを push する (他のブランチは公開しない)
