# SMB (ネットワーク共有) の対応 — 設計・依存ライブラリ・検証・制限

状態 (2026-11-23): 実装済み。**検証は GitHub Actions の `smb` グループだけ** (手元ではビルドも実行もしていない。結果は 11 章の記録欄)。公開版 (Apache-2.0) に含める。

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
- `AndroidManifest.xml` に `INTERNET` 権限を足した (SMB の通信に必要。`tools/check_manifest_permissions.py` は、ソースの `java.net.Socket` / SMBJ の使用を根拠として見る)。**この権限は USB のアプリにとって初めてのネットワーク権限**なので、プライバシーポリシー・データセーフティ (TODO の公開準備) に「SMB のためにだけ使う、利用者が登録した共有にだけ接続する」と書く必要がある。

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

## 10. CI (`smb` グループ)

- ブランチ `usbmem-ci-only-smb` の push、または `workflow_dispatch` の `groups=smb`。既定の全グループにも入れた。
- runner に apt で `samba` と `smbclient` を入れ、system の smbd / nmbd は止める。`tools/ci_android_tests.sh` の `run_smb` が、**使い捨ての設定** (`/dev/shm/smbci` の tmpfs・ポート 4445・`lo` だけ・署名必須・SMB 2.02 〜 3.11) の smbd を立てる。共有 `test` と、`smb encrypt = required` の共有 `enc`。テストユーザー `smbtest` とパスワード (使い捨ての文字列) は、スクリプトと `SmbTest.kt` の両方に書いてある (CI の runner だけで使い、runner ごと消える)。`run_smb` は `GITHUB_ACTIONS` が無いと実行しない (手元の PC に Samba を立てない)。
- エミュレータからは `10.0.2.2:4445` (runner のループバック) へ接続する。
- 試験 (`SmbTest`、計装): 接続・一覧 / 認証失敗 (パスワード違い・ユーザー無し・共有無し・ポート閉・名前解決失敗) と、**失敗後に配列がゼロになる**こと / 読み取りの SHA-256 照合 / 書き込み (作成・上書き・位置指定・伸長・日本語名) / 名前変更・移動・mkdir・削除 (再帰) / **大きなファイル (320 MiB、ストリーム、Java ヒープのピークが 200 MiB 未満) の書き込みと読み戻しの SHA-256** / SMB 3 の暗号化 (`requireEncryption` + `enc` 共有) と署名 / **切断** (試験内の TCP 中継で通信を切る・止める。転送が固まらず `SmbException` で終わる・要求が時間切れになりマウントが外れる・取り消しできる) / SAF の `ContentResolver` 経路 (ルート・一覧・読み取り・作成・書き込み・名前変更・移動・削除・読み取り専用のマウントの拒否) / パスワードの保存 (11 章の 4)。
- **サーバー側の検証**: 試験が書いた内容を `SMB_EXPECT <共有>|<パス>|<sha256>|<サイズ>`、消したものを `SMB_ABSENT <共有>|<パス>` として logcat に出し、`run_smb` が runner の実ファイル (`sha256sum`・`stat`) と `smbclient ls` で独立に照合する (アプリ側の読み戻しが一致しても、サーバーに本当に書かれたかは別に確かめる)。
- JVM 単体試験 `org.usbmem.app.smb.*` (設定・パス・例外の分類・コピーの取り消し・秘密の配列。サーバー不要) を同じジョブで実行する。

## 11. 結果の記録

### APK サイズの増加

(CI の `Release APK size (smb only)` ステップ = `tools/ci/smb_apk_size.sh` の結果を、実行後にここへ記入する。release / R8 / 署名なしで、arm64-v8a と x86_64 の 2 ABI を含む APK の、origin/main との差と、SMB 関係のパッケージが dex に占める大きさ。)

| 項目 | サイズ |
|---|---|
| (記入待ち) | |

事前の見積もり: 依存の jar は合計約 11 MB (bcprov が 10 MB)。R8 で、SMBJ が使う BC の軽量 API (MD4・HMAC・SHA・AES-GCM / CCM / CMAC など) とその推移的な参照だけが残るので、dex は 1〜2 MB 台の見込み。

### 試験の結果

(記入待ち。CI の実行 ID と通過件数を書く。)

## 12. 制限・未解決

- **実機・実サーバーでの確認は未** (Windows の共有・各社の NAS・Samba の古い版。CI の Samba 1 種類 + 試験内の TCP 中継だけ)。
- 認証は NTLM のみ (Kerberos・SPNEGO のドメイン SSO は不可)。SMB1 は不可。DFS は有効にしてあるが、DFS のリンクを持つ共有は未確認。
- **自動の再接続はしない**。切れたら外れるので、再度マウントして入力が要る (パスワードを保存していれば、生体認証 / 画面ロックだけで入れる)。
- 1 つの共有 = 1 接続 (SMBJ の `Connection` は 1 本)。同じ共有を複数のアプリが同時に読むと、要求は順番に処理される。
- ファイルの更新時刻・属性は読み取りのみ (書き込み時刻・属性は設定しない)。シンボリックリンク・ACL・拡張属性・ロック (opportunistic lock・排他) は扱わない。
- 「共有の一覧」(サーバーが公開する共有名の列挙) は未実装。共有名は利用者が入力する。
- 検出・探索 (mDNS / NetBIOS) は無い。ホスト名か IP を入力する。
- ネットワークの権限 (`INTERNET`) を新しく使う。プライバシーポリシー・データセーフティ・Play の申告に反映が要る (6 章)。
- USB の暗号ボリューム・ネスト・ディスクツール (イメージ・パーティション・フォーマット) は SMB の共有には使えない (対象外)。
