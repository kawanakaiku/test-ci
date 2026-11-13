# 媒体の種別判別 (回転ディスク / フラッシュ・SSD)

USB 大容量記憶装置が**回転ディスク (HDD)** か**フラッシュ / SSD** かを、USB を開くときに 1 度だけ判別する。スピンダウン (SCSI START STOP UNIT) の UI を、この結果で出し分ける。

実装は `core/src/usb_msc.cpp` の `MscDevice::probe_media()`、結果は `MscDevice::media_info()` (`MediaInfo`)。C API は `usbmem_usb_media_info`、Kotlin は `UsbSession.mediaKind` (`MediaKindInfo`)。単体テストは `core/tests/test_usb_msc.cpp` の `media_*` と `UsbSessionManagerTest`。

## 1. なぜ判別するか

- フラッシュ / SSD に START STOP UNIT を送っても意味が無い。多くは無視されるか ILLEGAL REQUEST になる。
- 一部の安価な USB メモリは、これを「取り出し (LOEJ 相当)」と解釈して固まったり、再列挙して切断されたりする。
- これまでは「ユーザーが選ぶ」方式で、回転ディスクかどうかを見ていなかった。

## 2. 結果の型

| 項目 | 意味 |
|---|---|
| `kind` | `Rotational` (回転数が分かれば `rpm`) / `NonRotational` / `Unknown` |
| `basis` (根拠) | `VpdBlockDeviceChars` (VPD 0xB1) / `AtaInfoVpd` (VPD 0x89 内の IDENTIFY) / `AtaPassThrough12` / `AtaPassThrough16` / `None` |
| `hint` | `kind` が Unknown のときだけの手掛かり。`RemovableNoVpd` (RMB=1 で回転数の情報が無い = USB メモリの可能性が高い) / `UnmapSupported` (UNMAP/TRIM 対応 = SSD・フラッシュのヒント) / `None`。**確定ではない** |
| 生の値 | `removable` (RMB)、`inquiry_version`、`vpd_pages`、`rotation_rate_raw` (0xB1)、`unmap_raw` (0xB2 の LBPU)、`ata_word0/169/217`、`sat_tried`、`sat_skipped`、`stop_reason` |
| `describe()` | 上を ASCII 1 行にした診断文字列。診断画面とログ (`logcat -s usbmem`) に出る |

`probe_media = false` (`MscOptions`) で判別を止められる (その場合 `probed = false`、Unknown のまま)。

## 3. 判別の手順

すべて**データ読み取り方向 (IN) で、媒体の内容を変えないコマンド**だけ。open() の最後 (TEST UNIT READY・READ CAPACITY・MODE SENSE が済んでから) に 1 度だけ実行し、結果を保持する。以後このためのコマンドは送らない。

```
(a) 標準 INQUIRY (open が既に実行)         RMB (byte1 bit7)、VERSION (byte2)、ペリフェラルタイプ
(b) VERSION が SPC-2 (4) 以上なら
      INQUIRY EVPD=1 page 0x00              対応ページ一覧
        ILLEGAL REQUEST                     -> VPD 非対応
        応答が不正                          -> 打ち切り (Unknown)
      0xB1 が対応なら INQUIRY EVPD page 0xB1
        MEDIUM ROTATION RATE (byte4-5, BE)
          0x0001            -> NonRotational  [根拠: VPD 0xB1]  終了
          0x0401..0xFFFE   -> Rotational (rpm = 値)  [根拠: VPD 0xB1]  終了
          0x0000 (未報告) / その他 (予約) -> 決まらない。(c) へ
(c) ATA の IDENTIFY DEVICE の語 217 (NOMINAL MEDIA ROTATION RATE)。同じ解釈 (0001h = 非回転、0401h-FFFEh = rpm、0000h = 未報告)
      (c1) VPD 0x89 (ATA Information) が対応なら INQUIRY EVPD page 0x89
             byte56 = コマンドコード (0xEC = ATA ディスク、0xA1 = ATAPI -> 打ち切り)、byte60-571 = IDENTIFY の 512 バイト
             [根拠: VPD 0x89]。INQUIRY だけなので ATA コマンドは送らない
      (c2) 0x89 が無く、かつ「RMB=0」(または 0x89 を名乗る) なら SAT の ATA PASS-THROUGH
             PASS-THROUGH(12) (0xA1) で IDENTIFY DEVICE (0xEC)。ILLEGAL REQUEST のときだけ (16) (0x85) を 1 回
             [根拠: SAT(12) / SAT(16)]
             RMB=1 で 0x89 も無い機器には送らない (sat=skip(rmb))
(d) 決め手が無ければ Unknown。
      B2 (Logical Block Provisioning) が対応なら 1 回読み、LBPU (UNMAP 対応) を記録する
      ヒント: RMB=1 -> RemovableNoVpd、そうでなく UNMAP (0xB2 の LBPU または IDENTIFY 語 169 bit0 = TRIM) -> UnmapSupported
```

### 3.1 VPD 0xB1 (Block Device Characteristics)

SBC-3 / SPC-4。ページ長 64 バイト (ヘッダ 4 + 60)。`byte 4-5` が MEDIUM ROTATION RATE。`0x0000` = 未報告、`0x0001` = 非回転 (フラッシュ / SSD)、`0x0401..0xFFFE` = 公称回転数 (rpm)、他は予約。予約値は**判定に使わない** (Unknown のまま次へ)。

### 3.2 SAT の IDENTIFY DEVICE

CDB は正規形 1 通りだけ (`scsi_cdb_ata_passthrough_ok`)。

| | PASS-THROUGH(12) | PASS-THROUGH(16) |
|---|---|---|
| オペコード | `A1` | `85` |
| byte1 | `08` (PROTOCOL=4 PIO データイン、EXTEND=0) | `08` |
| byte2 | `0E` (T_DIR=1 デバイス→ホスト、BYTE_BLOCK=1、T_LENGTH=2 セクタ数) | `0E` |
| セクタ数 | byte4 = 1 | byte6 = 1 |
| ATA コマンド | byte9 = `EC` | byte14 = `EC` |
| その他 | 0 | 0 |

応答は**ちょうど 512 バイト**であること。`parse_ata_identify` が次を検査する。全部 0 なら不正。語 0 bit15 = 1 (ATAPI) なら不正。語 255 の下位バイトが `A5` のときだけチェックサム (512 バイトの和 mod 256 = 0) を要求する。語 217 は 8 ビット単位のリトルエンディアン。

### 3.3 バージョンの足切り

Linux (`scsi_device_supports_vpd`) と同じく、INQUIRY の VERSION が SPC-2 (4) 未満の機器には EVPD を送らない (古い USB メモリは EVPD で固まるものがある)。この場合 VPD は未実行で、(c2) の条件 (RMB) だけで SAT の可否が決まる。

## 4. 安全策

判別は「読み取り専用のコマンドだけ」でも、未知のコマンドを USB メモリに送れば固まる恐れがある。次の対策を入れた。

1. **1 コマンド 1 回、再試行なし**。タイムアウトは `probe_timeout_ms` (既定 5 秒) の上限。`exec_checked` の UNIT ATTENTION 自動再試行は使わない。
2. **想定外は 1 つでも打ち切る**。転送エラー・タイムアウト・CSW の署名/タグ不一致・Phase Error・ILLEGAL REQUEST 以外の CHECK CONDITION (UNIT ATTENTION を含む)・不正な長さ/ページ番号/ペリフェラルタイプ・IDENTIFY の不正は、その時点で `stop_reason` を記録して判別を終える (Unknown)。BOT のリセット回復 (`reset_recovery`) は既存の `exec` が行う。**open は失敗させない**。ただし Disconnected (抜去) だけは open の失敗として伝える。
3. **RMB=1 の機器には、0x89 を名乗らない限り ATA PASS-THROUGH を送らない**。USB メモリは RMB=1 が普通で、未知のオペコード (0xA1 は MMC では別のコマンド) で固まる恐れが最も高いのがここ。代わりに `RemovableNoVpd` のヒントにする。この結果、RMB=1 の古い USB-IDE ブリッジなどは判別できず Unknown になる (安全側)。
4. **許可リスト** (`scsi_cdb_allowed_read_only`): 読み取り専用の open では、INQUIRY は EVPD ビットのみ許可 (CMDDT は拒否)、ATA PASS-THROUGH は `probe_media()` が `allow_ata_identify_` を立てている間だけ、正規形の IDENTIFY DEVICE (0xEC) のみ。0xEC 以外の ATA コマンド (WRITE DMA、SECURITY ERASE、SET FEATURES、SMART、TRIM など 255 通りすべて)、EXTEND 付き、別プロトコル、レジスタに値のあるもの、CDB 長違いは拒否する。
5. **書き込み可のモードでも ATA PASS-THROUGH は同じ検査**を通す (`scsi_cdb_ata_passthrough_ok` を `exec_impl` が常に呼ぶ)。アプリはこれ以外の用途で ATA PASS-THROUGH を使わない。
6. **スピンダウン側の守り**: 種別が Rotational と分からない限り、UI は既定で折りたたみ、自動停止は働かない (§6)。

## 5. 限界

- **RMB=1 の機器は判別できないことが多い** (USB メモリの大半は VPD 0xB1 も SAT も持たない)。Unknown + ヒントになる。
- **USB-SATA / USB-NVMe ブリッジが回転数を伝えない**ことがある。ブリッジが IDENTIFY を握りつぶす / 偽の値を返すと、誤判定または Unknown になる。語 217 が 0 (未報告) の古い HDD もある。
- **RAID ケース / 複数ディスク**: 論理ユニットとして見えるので、先頭ディスクではなくコントローラの応答になる (実機の Ratoc RAID の応答は未確認。§7 で確認する)。
- **UNMAP 対応は SSD の証拠にならない**。シン・プロビジョニングの RAID も UNMAP を報告するため、ヒント止まりにしている。
- ブリッジは起動時に 1 度 IDENTIFY を取ってキャッシュすることが多く、**ディスクを入れ替えても (ホットスワップ) 結果が変わらない**場合がある。判別は open のたびに行う。
- BOT のみ対応 (UAS は対象外。アプリは Bulk-Only のインタフェースだけを開く)。

## 6. UI と自動停止

| 種別 | ディスクの電源セクション | アイドル自動停止 |
|---|---|---|
| Rotational | 従来どおり (停止 / 再開、アイドル停止) | 保存された設定を使う (既定は「しない」) |
| NonRotational | 既定で折りたたみ。「フラッシュ/SSD では停止しても効果がほとんどありません。一部の機器は切断されることがあります」の警告。「それでもスピンダウンを使う…」→ 確認ダイアログ → 展開 | 確認するまで働かない。確認しても「しない」から始める |
| Unknown | 同じく折りたたみ + 警告 (種別が確認できない)。ヒントがあれば「USB メモリの可能性が高い」等を併記 | 同上 |

- 確認は機種 (VID:PID) ごと・**確認した種別ごと**に保存する (`spindown_ack:<vid:pid>` = 種別名)。同じ VID:PID でも、中身が HDD ⇔ SSD に変わる (ケースのディスク換装) ことがあるので、開いたセッションの種別が確認した種別と**一致するときだけ**有効にする。不一致・初回は無効に倒す。
- 確認の直後、アイドル停止の分数は 0 (しない) に戻す。確認だけで自動停止が有効になることはない。
- 強制は `UsbSessionManager` が行う (`setSpinDownConfirmed` / `spinDownAllowed` / `effectiveIdleSpinDownMinutes`)。JVM 単体テストで、回転ディスクは従来どおり、フラッシュ/不明は確認なしで `setIdleSpinDown` が呼ばれないこと、確認した種別と実際の種別が違うと無効なこと、抜去で確認を忘れることを検査している。
- 「スピンダウンの設定を閉じる」で確認を取り消せる (停止中は閉じられない: 再開の操作を残すため)。

## 7. 実機で確認する手順 (ユーザーの許可後に実施)

**判別の測定は実機の USB に触る**。事前にユーザーの許可を得てから行うこと。コマンドはすべて読み取り方向の INQUIRY / IDENTIFY のみ。

1. `ANDROID_SERIAL=<対象> adb logcat -c` をしてから、アプリで対象の USB を「開く」(読み取り専用)。
2. アプリの「診断」タブ → 「ディスクの種別」のカードを見る。ログでも確認できる: `adb logcat -s usbmem | grep "media kind"`。
3. 表示の見方:
   - 1 行目 (ラベル): 「HDD 7200rpm」「SSD/フラッシュ」「種別不明 (USB メモリの可能性)」など。
   - 2 行目: 根拠 (VPD 0xB1 / VPD 0x89 / SAT)。
   - 等幅の行 (`describe()`): `rmb=… ver=… vpd=… b1=0x… b2.lbpu=… ata.w0=… w169=… w217=… sat=… -> … [根拠] hint=… stop=…`
4. 期待値:

   | 機器 | 期待 |
   |---|---|
   | F-51A + Ratoc RAID (回転ディスク) | `Rotational`。ブリッジが 0xB1 / 0x89 / SAT のどれかで回転数を伝えれば rpm 付き。伝えなければ `Unknown` (その場合は RMB=0 のはずで、ヒント無し)。**RAID が先頭ディスクを見せているかは要確認** |
   | A80 Pro の USB メモリ 3 本 | 多くは `Unknown` + ヒント「USB メモリの可能性」(`rmb=1 vpd=none sat=skip(rmb) hint=removable`)。まれに SSD 型の USB メモリは `NonRotational` |
   | USB-SATA 変換の SSD | `NonRotational` (SAT か 0x89 経由)。伝えないブリッジは `Unknown` |

5. 見るべき異常: `stop=` が空でない (途中で諦めた理由)、`Rotational` なのに rpm が 0 / `NonRotational` の HDD、同じ機器で open のたびに結果が変わる。診断の「送信した SCSI 命令」に `a1` / `85` が出るのは SAT を試したときだけで、WRITE 系 (`2a` `8a` `35`) が出ないこと。
6. 確認したら TODO.md の項目に結果 (機器・describe() の行) を書き残す。
7. UI の出し分けの確認: Rotational でスピンダウンが従来どおり出る。NonRotational / Unknown で折りたたまれ、警告が出て、「それでも…」→ ダイアログ → 展開、「しない」が選ばれていること。アプリを再起動して開き直すと、同じ機器・同じ種別では展開済み (確認の保存)、別の種別になれば折りたたみに戻ること。

## 8. 検証 (ホスト、偽 USB 転送)

`core/tests/fake_bot.h` の `BotDevice` に `MediaCfg` (INQUIRY の VERSION/RMB、VPD ページ一覧、0xB1/0xB2、SAT の (12)/(16)、0x89、IDENTIFY の語 217/169・チェックサム・ATAPI・全 0) と、故障注入 (cdb を見て Timeout / Phase Error / CSW タグ違い / データ短縮 / CHECK CONDITION) を足した。`media_*` 19 件で次を網羅: B1 = 1 / 7200 / 0 / 予約値、VPD 非対応、SAT 成功 (語 217 = 1 / 7200 / 0)、SAT(16) のみ、0x89、SAT 非対応、タイムアウト (VPD / SAT)、Phase Error、タグ違い、UNIT ATTENTION、不正な長さ、ATAPI、チェックサム不正、RMB のヒント、UNMAP / TRIM ヒント、抜去、判別が 1 度だけであること、判別の無効化、書き込み可モード、ATA PASS-THROUGH の 0xEC 以外 255 通りと正規形でないものの拒否。
