# 複数 USB メモリの同時利用

複数の USB メモリを同時に開き、1 本ずつ独立に管理する。開く・閉じる・抜去・書き込みモード・アイドル停止・公開ボリュームのどれも、他のデバイスに影響しない。

## 1. 構成

```
UsbApp (Application)  ---- 抜去ブロードキャストの受信 (Activity が無くても動く)
   |
UsbSessions.manager()  -- プロセスに 1 つの UsbSessionManager
   |                         deviceName ("/dev/bus/usb/001/005") -> Slot
   |   UsbSessionFactory --- UsbStorageSessionFactory (本番) / FakeUsbSessionFactory (JVM テスト)
   |   onDeviceClosing ----- VolumeRegistry.removeByDevice(name) + SAF ルートの notifyChange
   v
UsbSession (= UsbStorageSession)  デバイスごとに 1 つ。ネイティブハンドル、USB 接続、アイドル監視スレッドを持つ
   |
VolumeRegistry  公開中ボリューム。Entry.deviceName で持ち主を持ち、キーは usb:<deviceName>@<パーティション開始バイト>
   |
NtfsDocumentsProvider  ルートはボリューム (= パーティション、ネストした子も) ごと
```

UI 側は、プロセスで 1 つの `AppModel` が `UsbDeviceState` をデバイスごとに持つ。`UsbSessionListener` がマネージャのイベントをメインスレッドへ渡し、`refreshUsb()` が `UsbManager.deviceList` とマネージャの状態を突き合わせる (開始時・再開時・抜き差し時)。

### 主な型 (`UsbSessionManager.kt`)

| 型 | 役割 |
|---|---|
| `UsbSessionManager` | `deviceName` をキーにセッションを管理する。`open / close / onDetached / closeAll`、書き込みモード・アイドル分数の設定 |
| `UsbSession` | 1 本のデバイスの操作面 (パーティション表、`mountNtfs`、`startStop`、`withHandle` など)。`UsbStorageSession` が実装し、テストは偽物を差す |
| `UsbSessionFactory` | セッションを作る。本番は `UsbStorageSessionFactory` |
| `WriteMode` | `ReadOnly` (既定) / `CreateOnly` / `Full` |
| `UsbSessionException.Kind` | `AlreadyOpen` / `Closing` / `LimitReached` / `WriteNotAllowed` / `ClosedWhileOpening` |
| `UsbDeviceNaming` | 画面・公開名に付ける識別子 (後述) |

## 2. 状態遷移

デバイスごとの状態 (スロットの `Phase`)。スロットが無い = 未オープン。

```
            open()                          open 完了
 (なし) ------------> Opening ----------------------------> Open
   ^                    |  \                                  |
   |   open 失敗         |   \ open 中に close/抜去            | close() / onDetached()
   +--------------------+    \ (pendingClose を記録)           v
   |                          +--> 開き終えた直後に ----> Closing
   |                               ClosedWhileOpening を投げる    |
   +--------------------------------------------------------------+
        後始末 (ボリューム → session.close) が終わるとスロット削除、listener.onClosed
```

- `Opening` / `Open` のデバイスへの `open()` は `AlreadyOpen` で拒否する (二重オープン防止)。`Closing` の間は `Closing` で拒否し、閉じ終わってから開き直す。
- 開く最中に抜去・閉じる要求が来たら `pendingClose` に理由を残す。開き終えた時点で同じロックの中で `Closing` にして、他のスレッドが二重に閉じないようにする。開いたセッションは呼び出し元に返さず `ClosedWhileOpening`。
- 抜去 (`CloseReason.Detached`) で閉じたデバイスは、書き込みモードとアイドル分数の記憶も捨てる (次に挿したときは必ず読み取り専用から)。ユーザーが閉じた (`User`) 場合は覚えている。
- 書き込みモードは、スロットが無いときだけ変えられる (`setWriteMode` は開いている間は false)。変えられるのは開く前だけ。

## 3. スレッドとロック

- マネージャは 1 つの `lock` で `slots / modes / idleMinutes` の読み書きだけを守る。**遅い処理 (USB を開く、ボリュームの後始末、書き込みの確定、`session.close`) は全部ロックの外**で行う。1 本の遅い処理が他のデバイスの open/close/状態参照を止めない。
- listener はロックの外から呼ぶ。UI は受け取ったらメインスレッドへ post する。
- `open()` / `close()` はブロックする (USB の初期化は数秒)。UI スレッドからは呼ばない (`MainActivity` はバックグラウンドで実行する)。
- 抜去は `UsbApp` のレシーバが `goAsync()` + 専用スレッドで `onDetached` する。Activity が止まっている間でも後始末する。
- ネイティブ側の状態:
  - ネイティブハンドル (`usbmem_device*`) は**デバイスごと**に独立。
  - 診断用の最後の open トレース (`MscDevice::last_open_trace`) だけがプロセス共通だった。2 本を同時に開くと混ざるので `thread_local` にした (open と取得は同じスレッドで行うので結果は変わらない)。
  - LinuxCompat ランタイム (fs の互換層) はプロセス共通の状態を持つが、内部はミューテックスで保護されている。マウント処理 (`g_mount_mu`) はプロセス全体で直列化される。2 本のマウントを同時に始めても壊れないが、順に処理される (§7 制約)。ボリューム単位の `LockedFsVolume` のミューテックスは別々なので、マウント済みのボリューム同士の読み書きは並行する。

## 4. 書き込みモード

デバイスごとに保持する (`UsbSessionManager.writeMode / setWriteMode`)。

- 既定は `ReadOnly`。`CreateOnly` / `Full` は従来どおりの確認ダイアログ (書き込み可で開くときの警告) を通る。安全規則 (汚れた FS・未知機能は読み取り専用、OS マウント中は開かない、など) は従来と同じ。
- `BuildConfig.WRITE_SUPPORTED` (`-PusbmemNtfsWrite=true` のときだけ true) が false のビルドでは、書き込みモードを UI で選べず、マネージャも `ReadOnly` 以外の `open` を `WriteNotAllowed` で拒否する。
- 変更できるのは開く前だけ。開いている間はカードの選択が無効になる。

## 5. 命名

同じ機種を何本挿しても、画面と SAF のルートで区別できるようにする (`UsbDeviceNaming`)。

- 識別子: シリアル番号の下 4 桁 (英数字、4 桁以上、全部 0 でない)。シリアルが無い・他のデバイスと重なる (安価なメモリは同じ値を返す) ときは、バス番号-デバイス番号 (`/dev/bus/usb/001/005` -> `1-5`)。同時に接続している間は必ず一意。
- カードの題: `機種名 [識別子]`。ボリューム名: `機種名 [識別子] #パーティション番号`。
- SAF ルートの説明 (summary) に FS 名・書き込みモード・`USB 1-5` (バス番号-デバイス番号) を出す。

## 6. 抜去とクローズの流れ

1. `ACTION_USB_DEVICE_DETACHED` を `UsbApp` が受け、`deviceName` を取り出す。
2. `UsbSessionManager.onDetached(name)`: そのデバイスのスロットだけ `Closing` にする。
3. `onDeviceClosing(name)` -> `VolumeRegistry.removeByDevice(name)`: そのデバイスのエントリを取り出し、**ネストした子 -> 親** の順に閉じる (閉じる処理は VolumeRegistry のロックの外。他のデバイスの SAF アクセスを止めない)。その後 `notifyRoots` で `content://…/roots` を `notifyChange`。
4. `session.close()` (USB の解放)。失敗しても他のデバイスの処理は続く。
5. スロット削除 -> `listener.onClosed(name, Detached)` -> UI が「<機器名> が抜かれました」の通知を出す。通知はデバイスを特定する。

ユーザーが「安全に閉じる」を押した場合も 3-5 は同じ (書き込み可なら先に確定)。

## 7. セッションの生存 (回転・Activity 再生成・バックグラウンド)

セッションと `AppModel` は Activity ではなくプロセスに属する。回転やシステムによる Activity の破棄・再生成では閉じない。`MainActivity.onDestroy` で閉じるのは `isFinishing` のときだけ (ユーザーが戻るボタンなどで終了したとき)。プロセスが死んだ場合はセッションも消え、**自動で再オープンはしない** (権限ダイアログ込みで、ユーザーが開き直す)。

## 8. 制約

- **同時に開ける数**: 実装に上限はない (`maxOpen` を渡せば制限できるが、本番は無制限)。物理的な制約は USB の帯域と給電。画面にも 1 行出す。ハブ経由なら**給電付きハブ**を使うこと。同時に動かす転送が増えれば 1 本あたりの速度は下がる。
- **マウントのプロセス全体の直列化**: LinuxCompat の `g_mount_mu`。2 本を同時に「マウント」しても順に処理される。大きなボリュームのマウント (exFAT のビットマップ読み込み、ext4 のジャーナル再生など) の間は、別のデバイスのマウントが待つ。読み書き中のボリューム同士は並行する。
- **ネストのマウント / inspect**: VolumeRegistry のロックを持ったまま行うので、その間は別デバイスの登録・解除が待つ。
- **OS マウントの検出**: 開く前に `/proc/mounts` の vold のボリュームを見て、そのデバイスのものがあれば開かない。vold のノード (`public:8,1`) から sysfs (`/sys/dev/block/<maj:min>`) をたどって親の USB デバイス (busnum/devnum -> `/dev/bus/usb/BBB/DDD`) を求め、**別のデバイスの OS マウントは理由にしない**。持ち主を求められなかったマウントは、従来どおり安全側に倒して妨げる。sysfs の構成は機種で違い、実機では未確認 (§10)。
- **DiskTools / PartitionEditor / Recovery**: ライブラリ層は `UsbSession.withHandle` でデバイスごとのハンドルを使える。UI の入口は現状ない (従来から。`docs/disk-tools.md` `docs/partition-edit.md`)。
- アイドル停止は、セッションごとの監視スレッド。分数は (VID:PID) ごとに保存し、デバイスごとに設定する。同じ機種を 2 本挿すと設定は共有される (別々にしたければ別の機種にする必要がある。シリアル単位の保存は未実装)。回転ディスクと判別できないデバイス (フラッシュ/SSD・種別不明) は、ユーザーが明示的に確認した種別と一致しない限り自動停止しない (docs/media-kind.md の 6 章)。

## 9. 検証

JVM 単体テスト (偽のセッション):

| クラス | 件数 | 内容 |
|---|---|---|
| `UsbSessionManagerTest` | 29 | 3 本同時、片方の close/抜去が他に影響しない、二重 open 拒否、open 中の抜去、閉じている最中の open 拒否、デバイスごとの書き込みモード (書き込み無効ビルドでの拒否含む)、抜去で設定を捨てる、アイドル分数、回転ディスク以外では確認なしに自動停止しない (docs/media-kind.md)、命名 (同機種 3 本、シリアル重複、シリアル無し)、2 本以上の同時 I/O + open/close/抜去の混在 (ストレス) |
| `OsMountScopeTest` | 3 | OS マウントの持ち主判定。別デバイスのマウントで拒否しない、持ち主不明は妨げる、sysfs パスから USB デバイスを求める |
| 既存 | 9 | `OsMountGuardTest` / `UsbMscSelectorTest` / `HandleTest` / `PartitionModelTest` |

AVD (emulator-5556) の計装テスト `MultiUsbTest` (4 件、`tools/run_android_multi_usb_test.sh`): 本物の `UsbSessionManager` + 偽セッション + 実イメージ (ext4) のマウントで、ルートがデバイス・ボリュームごとに分かれること、1 本を閉じるとそのデバイスのルート (ネストした子を含む) だけが消え `notifyChange` が届くこと、1 本を抜去している間も別のデバイスを読み続けられること、3 本分のマウントを別スレッドで同時に行って壊れないこと。

既存の計装テスト (fat 6 件、ext2 書き込み 3 件、nested 13 件、btrfs 全構成) もそのまま通る。

## 10. 実機での確認手順 (未実施)

USB メモリ 3 本 + 給電付きハブ。**実機に触れるときは、OS が先に自動マウントするボリュームがあれば取り出しておく**。

1. ハブに 3 本を挿し、アプリを開く。3 枚のカードが出る (機種名 + 識別子、VID:PID、未接続/接続中)。同じ機種なら識別子で区別できる。
2. 1 本目のカードで書き込みモード (まず読み取り専用) を選び「開く」。許可ダイアログ -> パーティションが出る -> マウント -> ファイルアプリで読む。
3. 2 本目を開く。1 本目のファイルアプリでの読み取りが途切れないこと。2 本目の公開名が 1 本目と違うこと。
4. 3 本目も開く。3 本を並行して読む (大きなファイルのコピーなど)。速度が下がるのは帯域のため。
5. 1 本を物理的に抜く。通知がそのデバイスを指していること、そのデバイスのルートがファイルアプリから消え、他の 2 本は読み続けられること。
6. 抜いたものを挿し直し、書き込みモードが読み取り専用に戻っていること。
7. 回転・ホームボタンで離れて戻る・最近使ったアプリから戻る、でセッションが生きていること。
8. 1 本ずつ「安全に閉じる」。他の本がそのままであること。
9. (確認したい点) OS が一方の USB の FAT 領域を自動マウントしている状態で、もう一方のメモリが開けること (sysfs による持ち主判定が機種で効くか)。効かなければ従来どおり「OS マウント中」で拒否される (安全側)。

## 11. 未了

- 実機での上記の確認、同時転送の性能・給電の挙動。
- マウントのプロセス全体の直列化の緩和 (LinuxCompat のマウント状態の分割)。
- シリアル単位のアイドル・チューニング設定の保存。
- DiskTools / PartitionEditor / Recovery の UI の入口。
