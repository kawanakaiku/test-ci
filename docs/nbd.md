# NBD クライアント

`core/src/blockdev_nbd.cpp` — Network Block Device (https://github.com/NetworkBlockDevice/nbd/blob/master/doc/proto.md) のクライアント。`BlockDevice` として振る舞うので、
パーティション解析・ファイルシステム・[ディスクイメージ形式](blockdev-formats.md)・COW オーバーレイをそのまま重ねられる (例: リモートの qcow2 を NBD 越しに開く → `open_disk_image` → パーティション → ext2)。

- C++: `NbdDevice::connect(NbdOptions, &dev, &detail)` / `NbdDevice::list_exports` (`core/include/usbmem/disk_image.h`)
- C: `usbmem_nbd_open` / `usbmem_nbd_list_exports_json` / `usbmem_nbd_info_json` (`c_api_blockdev.h`)
- JNI / Kotlin: `BlockDevNative.nativeNbdOpen` / `BlockDeviceHandle.nbd(...)`

## 必要な権限 (重要)

**ネットワーク越し (host 指定) の接続には `android.permission.INTERNET` が必要。AndroidManifest にはまだ追加していない** (この作業の方針: NBD は core とホストテストまで。UI 統合のときに追加を判断すること)。
`unix_socket` (localhost の UNIX ソケット。Linux では先頭 `@` で抽象名前空間) なら権限は不要。TLS は未対応なので、**平文**で流れる。信頼できないネットワークでは使わないこと (読み書きの内容がそのまま見える・改ざんできる)。
ローカルホスト以外への接続を UI から行う場合は、平文であることの警告を出す想定。

## プロトコル対応

- ハンドシェイク: **fixed newstyle のみ**。旧形式 (oldstyle) と、fixed newstyle を宣言しないサーバは `Unsupported`。`NBD_FLAG_C_NO_ZEROES` に対応。
- オプション: `NBD_OPT_STRUCTURED_REPLY` (サーバが断っても続行)、`NBD_OPT_GO` (EXPORT 情報 + BLOCK_SIZE 情報)、**`NBD_OPT_GO` 非対応のサーバは `NBD_OPT_EXPORT_NAME` にフォールバック**、`NBD_OPT_LIST` (`list_exports`)、`NBD_OPT_ABORT`。
  `NBD_REP_ERR_UNKNOWN` → `InvalidArgument` (理由に名前)、`ERR_TLS_REQD` → `Unsupported` ("TLS")、`ERR_POLICY` など → `Io`。
- 転送: READ / WRITE / FLUSH / TRIM / WRITE_ZEROES / DISC。**構造化応答** (OFFSET_DATA / OFFSET_HOLE / ERROR / ERROR_OFFSET / NONE、複数チャンク、DONE フラグ) と単純応答の両方。
  READ の構造化応答は、チャンクが要求範囲内に収まり、**重ならず、全体を過不足なく覆う**ことを検証する (違反は `Corrupt`)。ハンドル不一致・不明なチャンク種別・不正なマジックも `Corrupt`。
- **FUA**: サーバが `SEND_FUA` を持てば、既定で**全書き込みに FUA を付ける** (`NbdOptions::fua_writes`)。これにより、再接続をまたいでも「成功を返した書き込み」は失われない。
- **ブロックサイズ制約**: サーバが通知する最小/最大 (`NBD_INFO_BLOCK_SIZE`) を守る。最大を超える要求は分割し、最小に揃わない読み書きは**揃えた範囲を読んで書き戻す (RMW)**。揃えた範囲が最大を超えないようにチャンクを決める (nbdkit の blocksize-policy で EINVAL を出させて検証)。矛盾した通知 (最大 < 最小 など) は無視。
  TRIM は助言的なので揃わない端を捨て、WRITE_ZEROES の揃わない端は通常の書き込みで処理する。
- エラー番号の対応: EPERM→`ReadOnly`、EIO→`Io(5)`、ENOMEM→`NoMemory`、EINVAL→`InvalidArgument`、ENOSPC→`Io(28)`、EOVERFLOW→`OutOfRange`、ENOTSUP→`Unsupported`、ESHUTDOWN→`Disconnected`、その他→`Io(errno)`。サーバのエラー応答のあとも接続は生きている (再接続しない)。
- 読み取り専用: サーバの `READ_ONLY` フラグ、または `NbdOptions::read_only` なら、書き込み・TRIM・WRITE_ZEROES はネットワークに出さずに `ReadOnly`。

## タイムアウト・キャンセル・再接続

- すべての I/O は `poll` で待ち、`IoOptions::timeout_ms` (0 なら `NbdOptions::io_timeout_ms`、既定 30 s) と `CancelToken` を 100 ms 刻みで監視する。接続は `connect_timeout_ms` (既定 10 s、非ブロッキング connect)。
- **タイムアウト / キャンセルで応答の途中を捨てた接続は同期が壊れているので閉じる**。次の I/O で (`auto_reconnect` なら) 接続を張り直す。古い応答を次の要求の応答と取り違えることはない (テスト済み)。
- 切断 (`Disconnected` / `Io`: 相手が切った・リセット) は、`auto_reconnect` (既定 true) なら**再接続して同じ要求を 1 回だけ再送**する。READ・WRITE・FLUSH・TRIM・WRITE_ZEROES はどれも冪等。
  再接続後にエクスポートのサイズや読み取り専用フラグが変わっていたら別物とみなして `Disconnected`。`Timeout` / `Cancelled` は再送せず呼び出し側へ返す。
- `fua_writes=false` の場合の注意: FUA 無しの書き込みを flush 前に切断で失うかもしれないので、**再接続をまたいだ未 flush の書き込みがあれば、次の `flush()` が `Disconnected` を 1 回返して知らせる** (握りつぶさない)。
- 1 接続・同時 1 要求 (内部ミューテックスで直列化)。複数スレッドから呼んでも安全だが並列には流れない。

## 検証

`core/tests/test_nbd.cpp`:

1. **自前の疑似サーバ** (localhost TCP / UNIX / 抽象ソケット) — プロトコルの全経路と故障・不正応答の注入:
   OPT_GO と EXPORT_NAME フォールバック (NO_ZEROES 有無)、構造化/単純応答、複数チャンクと HOLE、最小/最大ブロックサイズ (違反を疑似サーバが数える: 0 件)、エラー応答 9 種、存在しないエクスポート、TLS 必須、読み取り専用、
   タイムアウトと復帰、キャンセル、応答前/応答途中の切断 → 再送、`auto_reconnect=false`、FUA 無しの flush 通知、プロトコル違反 6 種 (ハンドル違い・範囲外チャンク・カバー不足・重複・不正マジック・不明チャンク)、旧形式サーバ、ゴミの挨拶、即切断、無応答サーバ、接続拒否、TRIM/WRITE_ZEROES、`list_exports`、16 MiB の大きな転送と 4 スレッド並行、
   **ゴミ応答ファジング 150 ケース** (ハンドシェイク中 50 / 転送中 100。クラッシュ・ハングなし)。
2. **実サーバとの独立照合** (ツールが無ければ skip と表示して通す):
   - **nbdkit** `file` プラグイン (構造化応答 / `--no-sr` 単純応答のみ / TCP) の 40 MiB を全読みし RAW と一致。`nbdinfo --size` と一致。
   - **nbdkit** `memory` + **`nbdcopy`**: こちらが書いた内容を `nbdcopy` で取り出して一致、逆向き (`nbdcopy` で書いた内容をこちらが読む) も一致。WRITE_ZEROES / TRIM も確認。
   - **nbdkit** `pattern` (4 TiB): 4 GiB を超える 64 ビットオフセット (5 GiB+3、1 TiB+777、末尾など) の読み取りが、プラグインの定義どおり。
   - **nbdkit フィルタ**: `error` (トリガファイルのあいだ EIO → `Io(5)`、エラー後も接続は生きている/再接続 0 回)、`delay` (遅い読み取り → `Timeout`、復帰、十分待てば成功)、`blocksize-policy` (最小 4096 / 最大 128 KiB、違反は EINVAL にする設定) + `log` (150 回のランダム読み書きでエラー 0、ログ上の全要求が 4096 に揃い 128 KiB 以下、**全 Write に fua=1**)、旧形式 (`-o`)。
   - **qemu-nbd**: qcow2 を公開 → NBD で読むと RAW と一致 (エクスポート名つき、別名は拒否、`list_exports`)。qcow2 を RAW のまま公開 → **NBD を下位にして `open_disk_image`** が形式を判別して展開し、パーティション表と ext2 も一致。書き込み: こちらが書いて flush、サーバ停止後のファイルの中身が一致。

## 制限・未解決

- TLS (STARTTLS / PSK) 未対応 → 平文。`ERR_TLS_REQD` のサーバには接続できない。
- 旧形式 (oldstyle) 未対応。メタデータコンテキスト (BLOCK_STATUS) 未対応 (疎なイメージの高速コピーはできない)。`NBD_CMD_CACHE`・`NBD_OPT_INFO` 単独・マルチコネクション (`CAN_MULTI_CONN`) は使わない。
- 名前解決 (`getaddrinfo`) はブロッキングで、`connect_timeout_ms` では中断できない (IP アドレス直指定なら影響なし)。
- IPv6 のスコープ付きアドレスなどは未検討。Android でのネットワーク越しの実測 (INTERNET 権限追加後) は未実施。AVD では未検証 (ホストのみ)。
