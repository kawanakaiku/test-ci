# SMB (ネットワーク共有) の対応 — 設計・依存ライブラリ・検証・制限

状態 (2026-11-24): 実装済み。**検証は GitHub Actions の `smb` グループだけ** (手元ではビルドも実行もしていない。結果は 11 章の記録欄)。公開版 (Apache-2.0) に含める。仕上げ (APK サイズの測定の仕組み・プライバシーの文書・転送速度の切り分けと調整・接続フォームの検証とエラーの文言) は 11・13・14 章と `docs/privacy-and-permissions.md`。

USB ストレージとは別軸の機能で、**共有を 1 つ「マウント」すると、SAF のルートに「<接続名> (SMB)」が USB のボリュームと並んで出る**。ファイルアプリや他のアプリから、一覧・読み書き・作成・削除・名前変更・移動ができる。

## 1. 方式と、ライブラリの選択

| 候補 | ライセンス | 判断 |
|---|---|---|
| **SMBJ 0.15.0** (JVM) | Apache-2.0 | **採用**。SMB 2.0.2 〜 3.1.1、署名、SMB 3 の暗号化 (AES-128/256 GCM・CCM)、NTLMv2、DFS。純 JVM なので NDK のビルドが要らない |
| libsmb2 | LGPL-2.1 | 不可 (公開版に LGPL を入れない) |
| jcifs-ng | LGPL-2.1 | 不可 (同上) |
| カーネルの cifs.ko / smbnetfs (FUSE 層) | GPL | 不可 (LinuxCompat に載せるのは非現実的。docs/fuse-layer.md) |

SMBJ は **SMB1 を実装していない**ので、SMB1 は「既定で無効」ではなく**そもそも話せない** (画面の最小バージョンの選択肢は 2.0.2 / 2.1 / 3.0 / 3.1.1 のみ。上限は常に 3.1.1)。古い NAS の SMB1 専用機には接続できない (意図した制限)。

### 認証は NTLM だけ

SMBJ の SPNEGO / Kerberos は JGSS (`org.ietf.jgss`) を使うが、**Android にはその実装が無い**。そこで認証器を `NtlmAuthenticator` だけにしてある (`SmbRemoteFs.connect`)。Kerberos / ドメインの SSO は使えない。ユーザー名 + パスワード (+ 任意のドメイン / ワークグループ) の NTLMv2 と、空のユーザー名のゲスト接続のみ。

## 2. 構成

| ファイル (`android/app/src/main/java/org/usbmem/app/`) | 役割 |
|---|---|
| `smb/SmbConnection.kt` | 保存する接続の設定 (名前・ホスト・ポート・共有・ドメイン・ユーザー・最小バージョン・暗号化必須・署名必須)。**パスワードを持たない**。入力の検査・JSON |
| `smb/SmbErrors.kt` | NT ステータス / 例外 → `SmbException` (種類つき)。**メッセージは固定の英文 + 種類 + ステータスだけ** (パスワード・ユーザー名・パス・サーバーの文字列を入れない) |
| `smb/RemoteFs.kt` | 共有 1 つ分の操作の IF (`list` / `stat` / `openRead` / `openWrite` / `mkdir` / `delete` / `rename` / `copyTo` / `copyFrom`)、取り消し (`SmbCancel`)、パスの検査 (`SmbPaths`) |
| `smb/SmbRemoteFs.kt` | SMBJ での実装。接続 (タイムアウト・キープアライブ・取り消し)、操作の例外の写像、切断の検出 |
| `smb/SmbMounts.kt` | マウント中の共有の表。切れた共有を外す。SAF のルートの変更通知 |
| `smb/SmbController.kt` | 接続の保存 (`SmbStore`)、テスト・マウント・解除、パスワードの記憶 (`SecretVault`)、秘密の配列の消去 (`SmbSecrets`) |
| `smb/SmbDocuments.kt` | SAF の実装 (ID `smb-<接続 id>:/パス`)。`ProxyFileDescriptorCallback` で読み書き (専用スレッド `smb-io`) |
| `NtfsDocumentsProvider.kt` | 既存の USB のプロバイダ。`SmbDocuments.owns(id)` のドキュメントだけを委譲する (**USB の経路は変えない**) |
| `ui/SmbScreen.kt`、`res/values/strings_smb.xml`、`res/drawable/ic_network.xml` | 「ネットワーク」タブ: 接続の一覧・追加 / 編集・テスト・マウント / 解除 |

### 共通の IF にしなかった判断 (並行した経路)

TODO の当初案は「ボリューム API (`VolumeOps`) の実装を 1 つ足して SAF と UI を USB と共通にする」だった。**共通にせず、並行した小さな経路にした**:

- USB の `NtfsVolume` / `VolumeRegistry` は、ネイティブの `FsVolume` (JNI)・マウントの記録・暗号ボリューム・ネスト・「ダーティ確認」・ディスクツールと深く結びついていて、ネットワーク共有には意味が無い部分 (パーティション、イメージ、`fsync` の意味、取り外し、ブロックデバイス) が多い。
- ネットワーク側は、**失敗の種類 (切断・タイムアウト・認証)・取り消し・パスワードの寿命**が主題で、USB には無い。
- そこで SAF のプロバイダの先頭で `owns(documentId)` を見て、SMB のドキュメントだけを `SmbDocuments` に渡す。**USB の既存の分岐は 1 行も変えていない** (`NtfsDocumentsProvider.kt` の差分は委譲の分岐だけ)。ID の形式 (`smb-` 始まり) が USB の ID (16 進数) と混ざらない。
- 将来、共通にしたくなったときは、`RemoteFs` と USB 側の `VolumeOps` を共通の IF に引き上げられる (どちらも「パス → 読む / 書く / 一覧」の形)。判断が要る点として最終報告に載せた。

## 3. 接続フォーム

項目: 接続名・ホスト (名前または IP)・共有名・ドメイン (任意)・ユーザー名・ポート (既定 445)・最小バージョン (2.0.2 / 2.1 (既定) / 3.0 / 3.1.1)・暗号化を必須にする・署名を必須にする・パスワードを保存する (既定オフ)。

- 「接続テスト」(取り消せる): 接続・認証・共有のルートの一覧まで試し、方言・暗号化・署名・書き込みの可否を出す。何もマウントしない。
- 「保存」で接続の設定だけを `SharedPreferences("usbmem_smb")` に JSON で置く (**パスワードは入らない**)。ダイアログを取り消したら何も変えない。
- 「マウント」: パスワードの入力 (または保存したパスワードの取り出し) → 接続。**書き込みを許すスイッチは既定でオフ** (読み取り専用でマウントし、必要なときだけ許す。USB の「作成のみ」の思想に合わせた)。サーバーが書き込みを許さなければ、スイッチがオンでも読み取り専用になる。
- **入力の検証** (14 章): ホスト名・IP の形式 (IPv4 は 4 つの 0〜255、IPv6、ホスト名のラベル)・ポートの範囲・共有名が空でないことを、保存・接続テストを一度押したあとは入力のたびに、その欄の下へ出す。
- 表記: 画面は「マウント」、ネットワークの説明には「接続」を使う。各ボタンに `contentDescription`・テストタグを付け (TalkBack)、横向きでも使えるよう縦にスクロールする。

## 4. パスワードの扱い

規則は暗号ボリュームのパスワード (docs/crypto-volumes.md) と同じ。

1. **既定では保存しない**。マウントのたびに入力する。
2. 保存を選んだときだけ、`SecretVault` (= Android Keystore の AES-256-GCM、使うたびに生体認証 / 画面ロック。LUKS / BitLocker の「記憶」と同じ部品) で暗号化して別の場所 (`usbmem_vault`) に置く。鍵の別名と保存の ID は `smb:<接続 id>`。接続を消すと暗号文と鍵も消す。端末に画面ロックが無い等で記憶できないときは、既存の `unlock_remember_unavailable_*` の文言で理由を出し、保存を選べない。
3. **メモリでの寿命を最短にする**: 入力欄の `String` から `CharArray` に移したあと、欄を空にする。接続 (`SmbRemoteFs.connect`) が認証の直後と `finally` で配列と SMBJ の `AuthenticationContext` の配列を 0 で埋める。UTF-8 への変換の一時配列も 0 で埋める (`SmbSecrets`)。**自動の再接続はしない** (再接続にはパスワードの保持が要るため。切れたら外して、もう一度マウントしてもらう)。
4. **ログ・例外に出さない**: `SmbException` のメッセージは固定。原因の例外 (cause) をつながない (SMBJ の例外はユーザー名などを含むことがある)。コード中に `Log` の呼び出しを置かない。
5. 画面が見えなくなったら (`ON_STOP`) 入力途中のパスワードを消す。ダイアログが閉じたときも消す。
6. 試験 (`SmbTest.passwordIsNotStoredByDefaultAndSavedOnlyEncrypted`) で、既定では保存先が空であること、保存後も SharedPreferences のどのファイルにも平文 (UTF-8 / Base64 展開後) が無いこと、取り消し・認証失敗で取り出せないこと、接続の削除で全部消えることを確かめる。

## 5. ファイル操作・ストリーミング・取り消し・切断

- 読み取り: `RemoteReader.read(offset, ...)` の位置指定 (SAF の `ProxyFileDescriptorCallback.onRead` に合う)。書き込みも位置指定 (`onWrite`、`onFsync` は `flush`)。SMBJ の `File.read/write` は 1 リクエストの最大サイズ (サーバーが許す MaxRead / MaxWrite、`withBufferSize` で上限) ごとに分けられ、**全体をメモリに載せない**。`copyTo` / `copyFrom` は 1 MiB のバッファで流す。
- **読み取りの先読み** (13 章): 位置が続けて (2 回連続で) 順番に読まれたとき、SMBJ の入力ストリームを使って、いまの 1 MiB の応答を使っている間に次の 1 要求を先に出す。位置が飛んだらストリームを捨てて直接読む (ランダムなアクセスでは先読みしない)。**書き込みもするハンドル (`rw`) は先読みしない** (先読みした古い内容を、書いた後の読み取りに返さないため)。取り消し・切断は、ソケットを閉じて待ち手に伝わる既存の仕組みのまま (ストリームの待ちも `withTimeout` の時間切れが効く)。
- 作成 / 削除 / 名前変更 / 移動 / mkdir: 名前変更は上書きしない (同名があれば `Exists`)。フォルダの削除は中身ごと (再帰)。`..` / `.`・Windows で使えない文字・長すぎる名前は拒否 (パスの混入を防ぐ)。
- **取り消し**: `SmbCancel`。転送は 1 チャンクごとに確認し、接続の途中は取り消しの旗に「接続を閉じる」処理を登録して固まった接続を中から壊す。
- **タイムアウト**: 接続は 10 秒 (`SocketFactory`)、要求ごとの応答待ちは 30 秒 (`withTimeout`。画面からの接続は 30 秒、試験では 3 秒に縮める)。**ソケットの SO_TIMEOUT は付けない** (SMBJ は読み取りの時間切れを「切断」と見なすため、何もしない時間が長いだけで接続が死ぬ。キープアライブは TCP のものを使う)。
- **切断の扱い**: 通信が壊れた種類 (切断・時間切れ・ネットワーク) の失敗が出たら、その共有の接続を「死んだ」と記録し、以後の操作はすぐ `Disconnected` で失敗する (待たされない)。死んだマウントは、SAF のルート一覧を作るとき・画面の更新 (4 秒ごと) のときに外れ、ルートが消える。SAF の読み書きは errno (`ENODEV` / `ETIMEDOUT` / `EIO` ほか) で失敗する。
- **ディレクトリのリースは使わない** (`withDirectoryLeasingEnabled(false)`): SMBJ 0.15 は SMB 3 でディレクトリのリースを取り、2 回目以降の一覧をキャッシュから返す (サーバーに問い合わせない)。CI の無応答の試験で、止めたサーバーに対して `list` が成功してしまうことで判明した (2026-11-24)。無応答・切断を一覧で検出でき、他のクライアントの変更も遅れずに見える。
- **取り消し・強制終了はソケットを閉じる**: SMBJ はネゴシエーション中の接続を `SMBClient.close()` では閉じられず、取り消しても時間切れ (30 秒) まで戻らなかった (CI で判明)。そこで接続のソケットを `SmbSocketFactory` で全部持ち、取り消し・失敗・切断後の `close()` で閉じる (SMBJ の読み取りスレッドが失敗を待ち手に伝える)。接続中のスレッドへの割り込みも併用する。
- ANR を避ける: ネットワークの呼び出しは、画面のスレッドでは行わない (画面は IO ディスパッチャ、SAF は専用の `smb-io` スレッド)。

## 6. 公開版 (Apache-2.0) に含める手続き

- 新しいソースは `// SPDX-License-Identifier: Apache-2.0`。`tools/public_manifest.txt` の `android/app/src/main/java/**` に含まれる (新しい行は `docs/smb.md` の分だけ)。
- 依存は Maven Central から解決し、リポジトリにバイナリを置かない (`implementation("com.hierynomus:smbj:0.15.0")`)。
- `tools/public_edition/NOTICE` の「Third-party components」に SMBJ と依存ライブラリを載せた。`docs/licenses.md` にも行を足した。
- `AndroidManifest.xml` に `INTERNET` 権限を足した (SMB の通信に必要。`tools/check_manifest_permissions.py` は、ソースの `java.net.Socket` / SMBJ の使用を根拠として見る)。**この権限は USB のアプリにとって初めてのネットワーク権限**なので、プライバシーポリシー・データセーフティ (TODO の公開準備) に「SMB のためにだけ使う、利用者が登録した共有にだけ接続する」と書く必要がある → **`docs/privacy-and-permissions.md`** (通信の範囲・保存するもの・Play のデータセーフティの下書き・プライバシーポリシーの雛形・権限の一覧) に書いた。保存した接続とパスワードが Google ドライブのバックアップに入らないこと (`allowBackup="false"` に加えて `dataExtractionRules` / `fullBackupContent` で全ドメインを除外) も、そこで確認している。

## 7. 依存ライブラリとライセンス (Maven Central の POM と jar の LICENSE で確認)

SMBJ 0.15.0 の `runtimeElements` (Gradle module metadata) の依存は次の 4 つ。**GPL / LGPL は無い**。

| ライブラリ | 版 | ライセンス (POM) | jar の LICENSE | 用途 |
|---|---|---|---|---|
| com.hierynomus:smbj | 0.15.0 | Apache-2.0 | (jar に LICENSE なし。POM と GitHub のリポジトリが Apache-2.0) | SMB クライアント |
| org.bouncycastle:bcprov-jdk18on | 1.85.2 | "Bouncy Castle Licence" | `META-INF/LICENSE.md` = **MIT License** (Legion of the Bouncy Castle Inc.) | 暗号 (MD4 / HMAC-MD5 / SHA-512 / AES-GCM・CCM / CMAC など) |
| org.slf4j:slf4j-api | 2.0.18 | (親 POM = MIT) | `META-INF/LICENSE.txt` = **MIT** (QOS.ch) | ログの IF。**バインディングは同梱しない** (何も出力しない) |
| net.engio:mbassador | 1.3.2 | MIT | (jar に LICENSE なし。POM が MIT) | SMBJ のイベントバス |
| com.hierynomus:asn-one | 0.6.0 | Apache-2.0 | (jar に LICENSE なし。POM が Apache-2.0) | ASN.1 (SPNEGO / NTLM のトークン) |

注意点:

- BouncyCastle の POM の名前は独自 ("Bouncy Castle Licence") だが、jar の `META-INF/LICENSE.md` の本文は MIT そのもの。MIT は寛容で、著作権表示と許諾表示を配布物に添える義務がある → NOTICE に載せた。
- 上の表の版は、SMBJ の module metadata が `requires` で指定するもの。Gradle は同じ版に解決する (他の依存と衝突して上がった場合は、再確認する)。
- bcprov は jar が約 10 MB あるが、release は R8 で使う分だけに縮む (11 章)。
- SMBJ の Kerberos / SPNEGO 用に `org.ietf.jgss` などを参照するクラスは、使わない (認証は NTLM のみ)ので、読み込まれない。

## 8. minSdk 26 との互換性

- 全 jar が Java 8 のバイトコード (クラスファイル 51 / 52)。ラムダ・`invokedynamic` は AGP の desugar が処理する (`coreLibraryDesugaring` は不要: SMBJ が使う `java.nio.file` / `java.time` は API 26 にある。使っていない `SmbFileSystem` 系は読み込まれない)。
- bcprov・slf4j-api はマルチリリース jar (`META-INF/versions/**`)。Android では不要なので `packaging.resources.excludes` で外した。同名の `META-INF/LICENSE.md` / `LICENSE.txt` / `NOTICE.md` は `pickFirsts` にした (APK のマージの衝突を避ける。ライセンスの文面は NOTICE に載せてある)。
- BouncyCastle は**軽量 API** (`org.bouncycastle.crypto.*`) を SMBJ が直接参照するので、Android 組み込みの (縮められた) BC プロバイダとは衝突しない。Provider としては登録しない。
- slf4j-api 2.x は、バインディングが無いと起動時に NOP の警告を stderr に 1 回出す。ログには何も出ない。

## 9. R8 / ProGuard (`android/app/proguard-rules.pro`)

- mbassador は `@Handler` の付いたメソッドをリフレクションで呼ぶ → `net.engio.mbassador.**` と `@Handler` メソッドを残す。注釈の属性 (`RuntimeVisibleAnnotations`) も残す。
- 参照だけがあって実行時には読み込まれないクラス (`org.ietf.jgss`、`javax.security.auth.kerberos`、`javax.el`、slf4j の `impl` など) は `-dontwarn`。
- **R8 の警告・欠けたクラスは、CI の `Release APK size` ステップが release ビルドで確かめる** (失敗しても計装試験は続ける)。
- **これ以上の絞り込みを入れなかった理由**: SMBJ が名前で参照する BouncyCastle の部品 (AES / GCM / CCM / CMAC / HMAC / SHA / MD4 / MD5 / DES / RC4 / KDF) は、`BCSecurityProvider` のファクトリが直接 `new` するので、R8 が既に到達できるものだけに縮めている。`-keep` で足しているのは mbassador (リフレクション) だけ。mbassador を絞る案は、`@Handler` の呼び出しが `ReflectiveHandlerInvocation` などをリフレクションで作るため、絞ると動かなくなりうる (実行時にしか分からない) ので採らない。bcprov の不要部分を `-assumenosideeffects` などで落とす案も、削れる量が小さい (dex は 1〜2 MB 台の見込み) のに対して、署名・暗号化が壊れたときの被害が大きいので、**CI の dex パッケージ別の数値 (11 章) を見てから**、大きい部品があれば個別に検討する。暗号の実行を JCE に移した (13 章) ので、BC の純 Java 実装は実行時にはほぼ使われなくなるが、フォールバック・MD4・DES・RC4・AES-CCM のために残す。

## 10. CI (`smb` グループ)

- ブランチ `usbmem-ci-only-smb` の push、または `workflow_dispatch` の `groups=smb`。既定の全グループにも入れた。
- runner に apt で `samba` と `smbclient` を入れ、system の smbd / nmbd は止める。`tools/ci_android_tests.sh` の `run_smb` が、**使い捨ての設定** (`/dev/shm/smbci` の tmpfs・ポート 4445・`lo` だけ・署名必須・SMB 2.02 〜 3.11) の smbd を立てる。共有 `test` と、`smb encrypt = required` の共有 `enc`。テストユーザー `smbtest` とパスワード (使い捨ての文字列) は、スクリプトと `SmbTest.kt` の両方に書いてある (CI の runner だけで使い、runner ごと消える)。`run_smb` は `GITHUB_ACTIONS` が無いと実行しない (手元の PC に Samba を立てない)。
- エミュレータからは `10.0.2.2:4445` (runner のループバック) へ接続する。
- 試験 (`SmbTest`、計装): 接続・一覧 / 認証失敗 (パスワード違い・ユーザー無し・共有無し・ポート閉・名前解決失敗) と、**失敗後に配列がゼロになる**こと / 読み取りの SHA-256 照合 / 書き込み (作成・上書き・位置指定・伸長・日本語名) / 名前変更・移動・mkdir・削除 (再帰) / **大きなファイル (320 MiB、ストリーム、Java ヒープのピークが 200 MiB 未満) の書き込みと読み戻しの SHA-256** / SMB 3 の暗号化 (`requireEncryption` + `enc` 共有) と署名 / **切断** (試験内の TCP 中継で通信を切る・止める。転送が固まらず `SmbException` で終わる・要求が時間切れになりマウントが外れる・取り消しできる) / SAF の `ContentResolver` 経路 (ルート・一覧・読み取り・作成・書き込み・名前変更・移動・削除・読み取り専用のマウントの拒否) / パスワードの保存 (11 章の 4)。
- **サーバー側の検証**: 試験が書いた内容を `SMB_EXPECT <共有>|<パス>|<sha256>|<サイズ>`、消したものを `SMB_ABSENT <共有>|<パス>` として logcat に出し、`run_smb` が runner の実ファイル (`sha256sum`・`stat`) と `smbclient ls` で独立に照合する (アプリ側の読み戻しが一致しても、サーバーに本当に書かれたかは別に確かめる)。
- **2 つめの smbd (署名なし)**: `run_smb` が、同じ設定の `server signing = auto` (署名を求めない)・ポート 4446 の smbd を、別の状態の置き場 (`run2` / `lock2` / `cache2` / `state2`・`ncalrpc dir`) で立てる (ユーザー・共有は同じ)。署名の有無の転送速度の比較にだけ使う (13 章)。立たなければ、その比較だけ省いて続ける。
- JVM 単体試験 `org.usbmem.app.smb.*` (設定・パス・例外の分類・コピーの取り消し・秘密の配列。サーバー不要) を同じジョブで実行する。

## 11. 結果の記録

### APK サイズの増加

**測り方** (`tools/ci/smb_apk_size.sh`。CI の smb グループの `Release APK size` ステップ。ローカルでは動かさない):

1. この木の release APK (R8 あり・署名なし、arm64-v8a と x86_64 の 2 ABI を含む) を作る。
2. 比較元を環境変数で指定する: `SMB_SIZE_BASE_REF` = **SMB を入れる直前のコミット**、`SMB_SIZE_SMB_REF` = **SMB だけを入れた時点のコミット** (どちらもコミット SHA またはブランチ名。`git fetch --depth=1 origin <ref>` で取るので、チェックアウトの深さは増やさない。ローカルに既にあればそれを使う)。ワークフローは、push では下の既定値、`workflow_dispatch` では入力 `smb_size_base` (空なら既定値) で `SMB_SIZE_BASE_REF` を渡す。
   - `SMB_SIZE_BASE_REF` = `4930829380b51f862cd322ba854dd34c0b1be43d` (SMB の統合 `26f4729` の親。これが「SMB なし」の基準)
   - `SMB_SIZE_SMB_REF` = `cbf9b73aec83df414d191f720573ea28c6d9c685` (SMB の枝の先端 = 基準 + SMB だけ。R8 の追加の絞り込み・他の機能の追加を含まない)
   - 理由: 基準だけと比べると、この木には SMB 以外の機能 (ディスクツールの画面・削除ファイルの救出など) の増加も混ざる。**SMB 単独 − 基準 = SMB の素の増加**、**この木 − SMB 単独 = その後の変更 (暗号の JCE 化など)**、パッケージ別の dex (smbj / bouncycastle / mbassador / slf4j / asn-one) は SMB 単独とこの木で並べて比べられる。
   - 比較元を取れない (CI のリポジトリに履歴が無い) ときは、その比較だけ省き、理由を出す。
3. 出すもの: APK 全体 (MB・%)、dex (圧縮前 / APK 内)、ネイティブライブラリ (ABI ごと、圧縮前 / APK 内)、リソース・その他、**パッケージ別の dex の大きさ** (`apkanalyzer dex packages` の C 行をパッケージ接頭辞ごとに合計: asn-one / smbj / bouncycastle / mbassador / slf4j / アプリ側の SMB コード)、`apkanalyzer apk download-size`。結果は `$GITHUB_STEP_SUMMARY` と、ログの成果物 (`ci-logs/smb_apk_size.md`・生のダンプ `size_*_dexpkgs.txt`・`size_*.json`) に残る。

結果 (CI の実行後にここへ記入する):

| 項目 | この木 | SMB 単独 | SMB なし (基準) |
|---|---|---|---|
| APK 全体 | (記入待ち) | (記入待ち) | (記入待ち) |
| dex | | | |
| ネイティブライブラリ (APK 内) | | | |
| smbj / bouncycastle / mbassador / slf4j / asn-one の dex | | | |

事前の見積もり: 依存の jar は合計約 11 MB (bcprov が 10 MB)。R8 で、SMBJ が使う BC の軽量 API (MD4・HMAC・SHA・AES-GCM / CCM / CMAC など) とその推移的な参照だけが残るので、dex は 1〜2 MB 台の見込み。**ネイティブライブラリは SMB で増えない** (SMBJ は純 Java。NDK のビルドが要らない) はずで、増加は dex だけ。

### 試験の結果

(記入待ち。CI の実行 ID と通過件数を書く。)

## 12. 制限・未解決

- **実機・実サーバーでの確認は未** (Windows の共有・各社の NAS・Samba の古い版。CI の Samba 1 種類 + 試験内の TCP 中継だけ)。
- 認証は NTLM のみ (Kerberos・SPNEGO のドメイン SSO は不可)。SMB1 は不可。DFS は有効にしてあるが、DFS のリンクを持つ共有は未確認。
- **自動の再接続はしない**。切れたら外れるので、再度マウントして入力が要る (パスワードを保存していれば、生体認証 / 画面ロックだけで入れる)。切れたときの案内は、通知と各エラーの文言に入れた (14 章)。
- 1 つの共有 = 1 接続 (SMBJ の `Connection` は 1 本)。同じ共有を複数のアプリが同時に読むと、要求は順番に処理される。
- ファイルの更新時刻・属性は読み取りのみ (書き込み時刻・属性は設定しない)。シンボリックリンク・ACL・拡張属性・ロック (opportunistic lock・排他) は扱わない。
- 「共有の一覧」(サーバーが公開する共有名の列挙) は未実装。共有名は利用者が入力する。
- 検出・探索 (mDNS / NetBIOS) は無い。ホスト名か IP を入力する。
- ネットワークの権限 (`INTERNET`) を新しく使う。文書は `docs/privacy-and-permissions.md` に書いた (草稿)。**提出前に、Play の最新の定義と照らす・実機でバックアップの除外を確認する**必要がある。
- 書き込みの先行送信 (write-behind: 応答を待たずに次の書き込みを出す) は**入れていない**: SAF の `onWrite` は 1 回 128〜256 KiB なので、先行送信がなければ 1 要求ずつ応答を待つ。入れると、失敗 (空き容量不足・切断) が後の書き込みや `close` で初めて分かり、`close` は SAF からエラーを返せない (`ProxyFileDescriptorCallback.onRelease`) ので、**書き込みの失敗に気づかないままファイルが欠ける**。データを失う失敗の方が、速度より重い。
- USB の暗号ボリューム・ネスト・ディスクツール (イメージ・パーティション・フォーマット) は SMB の共有には使えない (対象外)。

## 13. 転送速度の切り分けと調整

CI のエミュレータで、署名必須の SMB 3.1.1 のとき write 5 MiB/s・read 12 MiB/s (320 MiB。通信なしでデータの生成 + SHA-256 だけの基準は 73 MiB/s)。遅さの原因の仮説と、確かめ方・調整:

| 仮説 | 確かめ方 | 対応 |
|---|---|---|
| (a) 署名 (AES-CMAC) の CPU 時間。SMBJ の既定は BouncyCastle の軽量 API (純 Java) で、エミュレータでは APK の Java コードが JIT 前 / 未最適化のことがある | 署名なしの smbd (ポート 4446) と署名必須 (4445) で、同じ条件の速さを比べる (`SmbTest.transferSpeedMatrix`) | 署名・暗号化・ハッシュを端末の JCE (Conscrypt など。ネイティブ) で行う `HybridSecurityProvider` (`SmbCrypto.kt`)。**BC と結果が一致した部品だけ使う** (固定のデータで両方を動かして比べる。AES-GCM・HMAC-SHA256・SHA-512・MD5・端末にあれば AES-CMAC)。一致しない・端末に無いものは BC のまま (MD4 / DES / RC4 / AES-CCM は JCE に無い) |
| (b) 1 要求ずつ応答を待つ往復 (エミュレータでは小さいが、実機の Wi-Fi・WAN では支配的) | 先読みの有無・バッファの大きさで比べる | 読み取りの先読み (5 章)。バッファは 1 MiB (サーバーの MaxRead / MaxWrite までに丸める) |
| (c) 生成・SHA-256 の側 (テストの基準) | `speed baseline` の行 | (測るだけ) |
| (d) TCP の送受信バッファ | `SmbTuning.socketBuffer` を 1 MiB にして比べる | 効かなければ既定 (0) のまま |

**試験が出す値** (`SMB_METRIC`、アサートなし。内容は毎回 SHA-256 で確かめる): `speed baseline` / `speed native crypto parts` / `speed [signing=required|none] <設定>: write X MiB/s, read(1 MiB calls) Y MiB/s, read(128 KiB calls) Z MiB/s`。設定は、BC の暗号・先読みなし (これまでの既定) / JCE の暗号・先読みなし / JCE + 先読み (新しい既定) / バッファ 4 MiB / 256 KiB / ソケットバッファ 1 MiB。128 KiB の読みは SAF の `onRead` に近い使い方。

検討して**採らなかった/実装しなかったもの**:

- **`withMultiProtocolNegotiate`**: SMBJ は SMB1 を話せないので、SMB1 → SMB2 のマルチプロトコルのネゴシエートは意味がない (既定の false のまま)。
- **最大トランザクションサイズ (`withTransactBufferSize`)**: ディレクトリの一覧と IOCTL の大きさ。`withBufferSize` が 3 つとも同じ値にする (1 MiB)。大きなディレクトリの一覧の速さは、リースを使わない (5 章) ため毎回サーバーへ問い合わせる分だけで、要求の大きさは律速でない。
- **書き込みのパイプライン**: `File.writeAsync` は、1 回の呼び出しの中の複数の要求を同時に出せるが、(1) 応答の NT ステータス (空き容量不足・アクセス拒否) が `Share.write` のようには検査されず、失敗が「サーバーが少ししか書かなかった」という汎用の例外になって、エラーの分類 (`SmbErrors`) が崩れる、(2) SAF の `onWrite` は 1 回 128〜256 KiB で、1 回の呼び出しの中に複数の要求を作れない。先行送信 (12 章) は失敗に気づかなくなるので入れない。
- **読み取りの 2 本以上の先読み**: SMBJ の入力ストリームは 1 本先読みだけ。自前で `readAsync` を複数出すには、SMBJ の内部 (パッケージ非公開) の API が要る。まず 1 本の先読みの効果を CI で測り、足りなければ検討する。

結果 (CI の実行後に記入): (記入待ち)

## 14. 接続フォームの検証・エラーの文言・切断時の案内

- **入力の検証** (`SmbConnection.validateFields` / `isValidHost`、画面は `SmbScreen.kt`): ホスト名 (「.」で区切った 1〜63 文字のラベル。英数字・「-」・「_」・日本語などの文字。先頭と末尾が「-」は不可) / IPv4 (4 つの 0〜255。`999.1.1.1` や `192.168.1` は誤り) / IPv6 (16 進と「:」、`%` のあとはインターフェース名。画面では `[fe80::1]` の角かっこを取る)、ポートは 1〜65535 の数字、共有名は空でなく `/ \ : * ? " < > |` を含まない、名前・ドメイン・ユーザー名の文字。誤りは、保存・接続テストを一度押したあとは入力のたびに、その欄の赤い表示と下の説明で出す (先頭のバナーにも出す)。単体試験 `hostFormatIsChecked`。
- **接続の失敗の文言** (`SmbUiText.error`): 認証失敗 (ユーザー名・パスワード・ドメインの確認) / ホスト不明 (`UnknownHostException`) / 接続拒否 (`ConnectException` で `ECONNREFUSED`) / 経路なし (`NoRouteToHostException`・`ENETUNREACH`) / 接続の時間切れ (`SocketTimeoutException`) / 署名・暗号化・バージョンの不一致 (`Protocol`: 接続の設定が「暗号化を必須」「署名を必須」「3.0 以上」のどれかによって、案内を変える) / 共有名なし / 切断・応答なし。`SmbException.detail` に例外のクラス名と固定の接尾辞 (`/refused` `/unreachable`) だけを入れ、メッセージ本体 (ホスト名・ポートを含む) は保存しない。単体試験 `connectFailuresAreToldApart`。
- **切断時の案内**: 通信が切れたときの通知 (`smb_notice_lost` と `_detail`) と、切断・応答なしのエラーの文言に、「Wi-Fi・VPN がつながっているか確かめて、もう一度『マウント』する。自動では再接続しない (パスワードを保存してあれば、生体認証 / 画面ロックだけでマウントできる)」と書いた。
- **文言の使い分け**: 共有に入ることは「マウント」(ボタン・状態「未マウント」・「マウントを取り消す」)。サーバーとの通信そのもの (接続テスト・「接続が切れました」・「接続を取り消す」= 接続テストの取り消し) は「接続」。
