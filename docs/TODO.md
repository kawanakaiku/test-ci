# TODO (未解決・保留事項)

## 書き込み / NTFS
- [x] **LFS (LogFile) 2.0 の件は訂正済み (ご指摘どおり書き込める)** — Windows 11 は使用中の `$LogFile` を v2.0 で持つが、**クリーンにアンマウントすると restart page を v1.1 に戻す**
      (実測: 正常ディスマウント後は v1.1、ダーティのままデタッチしたイメージは v2.0)。クリーンな Windows 11 製ボリュームには `ntfs-3g` も自前ドライバも書き込めた。
      v2.0 が問題になるのは **ダーティ (未再生ログあり) のボリューム**だけで、そこは元々書かない (ログ再生が要る) ので現方針で正しい: 読み取り専用で開き、書くなら Windows で安全に取り外してもらう。
- [ ] ダーティ/未再生ログのボリュームをどう案内するか (UI: 「Windows で安全に取り外してください」)。ログ再生の自前実装はしない。
- [ ] **64KiB クラスタの `$MFTMirr` 偽陽性** — 上流は 1 クラスタ分 (64 レコード) を比較するが Windows が同期するのは先頭 4 つ。書き込み時に自動で読み取り専用になる。
      対処案: 比較を先頭 4 レコードに限るパッチ (ntfs-3g と同じ挙動)。`third_party/ntfs-resurrection/patches/` に追加するか判断。
- [ ] 書き込み段階 3 以降: サイズ拡張・新規作成・削除・rename・mkdir (`iomap_file_buffered_write`、setattr、create/unlink)。
- [ ] 電源断模擬 (書き込み途中で打ち切り) → `ntfsfix -n` / WinPE chkdsk で確認。
- [ ] 圧縮 (LZNT1/WOF)・疎ファイルへの書き込み。
- [ ] ダーティフラグ付き・未再生ログのボリュームの扱い (現状: ログ静止ならクリーンアンマウントでフラグを下ろす = 上流方針)。
- [ ] 実機 USB での読み書き検証 (USB メモリが用意できたら)。

## 互換層
- [ ] `iomap_readahead` 未実装 (読み取りは read_folio で足りている)。
- [ ] マウント失敗経路の inode 内部確保物のリーク (既知・許容)。
- [ ] 同期 writeback の ioend 結合 (性能)。`writeback_iter` の O(N) スナップショット。
- [ ] 残りのスタブ 55 個 (`tools/compat_stub_names.txt`)。書き込み拡張で必要になったものから実装。

## 検証・ツール
- [ ] `tools/winvhd/make_encrypted_fixtures.sh` の起動待ちループが雑 (約 12 分かかる)。DHCP 取得後すぐ進むよう直す。
- [ ] EFS: 復号は未実装 (読み出しは -EOPNOTSUPP で拒否)。BitLocker は Phase 4。
- [ ] 巨大 (TB 級) ボリューム上の Windows 製データ、未再生 `$LogFile` ボリューム。
- [ ] システムの Documents UI (SAF) の実機確認、arm64 実機。

## Phase 4 以降
- [ ] BitLocker / LUKS 復号、他 FS (exFAT, ext4 等) の統合、UAS (USB Attached SCSI) 対応。

## 電源断・書き込みの残課題 (2026-11-11 追加)
- [ ] `torn` で壊れた MFT システムレコード (0-3) を `$MFTMirr` のコピーで読み出す緩和 (読み取り専用) と、Windows `chkdsk` による回復可否の確認。
- [ ] 電源断模擬を Windows 製イメージ (gpt64k/4Kn) と、より長い操作列・大きなファイル書き込み (ランダム/シーケンシャル混在) に拡大。フル版 (`run_crash_tests.sh <bin> <dir> 40`) を定期実行。
- [x] ~~クラスタサイズ < 4096 のボリュームは書き込みマウントを拒否~~ → **解消 (2026-11-11)**: 実機 RAID (7.3 TiB, クラスタ 2048B) で拒否に当たった。原因は互換層の folio 全体 dirty 管理 (1 folio にデータ・未初期化・穴が混在すると書き戻しが失敗)。512B 単位の dirty ビットと、穴/未初期化範囲のゼロ確認スキップで解消。クラスタ 512/2048 で書き込みテスト一式と電源断模擬を通す
- [ ] 書き込みの `fsync` / SAF 側の flush の意味づけ、書き込み途中の USB 抜去 (I/O エラー) 時の挙動テスト。

## Android 書き込みの残課題 (2026-11-11 追加)
- [ ] USB 経路の書き込み (SubRangeDevice を rw で開き `mount_rw`)。実機 USB メモリ + 抜去テストが先。
- [ ] SAF 書き込み中の抜去 / ボリューム close と ProxyFileDescriptor の競合 (onRelease が close 後に呼ばれる) の挙動確認。
- [ ] Documents UI (システムのファイルアプリ) での実操作確認、`FLAG_SUPPORTS_MOVE/COPY` の対応検討、`wa` (追記) モード。
- [ ] 大きなファイルの書き込み性能 (onWrite 128KiB ごとの JNI コピー、sync の頻度) の計測。
- [ ] 書き込み有効ビルドの配布方針 (既定ビルドでは無効のまま。警告 UI と同意の文言)。

- [x] (実装済み・実機で検証中) 端末ごとの読み取り転送サイズの保存: 「最適化」ボタンで小さい設定から順に読み取りを試し、最初の失敗の手前の最大設定を (USB 機器 + 端末) ごとに保存して次回から使う。書き込みは常に小さい既定のまま。J9260 は 64K/256K で EPROTO、Xperia は 128K/512K まで OK。
- [ ] 任意の「ディスクを止める」(START STOP UNIT) ボタン。閉じた後に OS が FAT を再マウントするので、確保を保持したまま使う運用が前提。
- [x] USB を安全に閉じるボタンの誤タップ対策 (書き込み可で開いているときは確認ダイアログ。実機で確認待ち)。

- [ ] UI の実機確認 (USB 接続中の表示: 公開中カード、OS マウント警告、確認ダイアログ、進行バー)。エミュレータでは USB 無しの状態だけ確認済み。
- [x] UI の文言を strings.xml へ移した (106 件)。画面文言は「公開」ではなく「マウント」(ネットに出るように読めるため)。UsbStorage.kt の NativeException の日本語メッセージ (OS マウント中・権限など) だけ、Context が無いので未移行。
- [x] 署名鍵: ~/.android/usbmem-release.jks + ~/.gradle/gradle.properties (リポジトリ外)。**バックアップ必須** (無くすとアプリを更新できない)。debug 鍵で入れた端末は、一度アンインストールが必要。
- [x] ディスクのスピンダウン: 手動 (「アンマウントして、ディスクを止める」) とアイドル自動 (しない/5/10/30 分、既定しない)。SCSI START STOP UNIT。止めた後の I/O は自動で START してから行う。F-51A + Ratoc RAID で実機確認済み (ユーザーが停止を確認、アイドル 5 分で自動停止、マウントで自動再開、復帰 3.3 秒)。未対応のブリッジは 1 度だけ試してあきらめる。
- [ ] スピンダウンの実機確認: 他の端末 (J9260, Xperia) と、停止中に USB を抜いたときの挙動、書き込み可で止めた場合。
- [x] **媒体の種別判別 (回転ディスク / フラッシュ・SSD)** (docs/media-kind.md): open() の最後に 1 度だけ、読み取り専用の INQUIRY (標準の RMB → EVPD 0x00 → 0xB1 の回転数 → 0x89 の IDENTIFY → SAT の ATA PASS-THROUGH(12)/(16) の IDENTIFY DEVICE 語 217) で判別。結果は {Rotational(rpm) / NonRotational / Unknown} + 根拠 + ヒント (RMB=1 で情報なし = USB メモリの可能性、UNMAP/TRIM) + 生の値。安全策: 1 コマンド 1 回・タイムアウト付き・想定外は打ち切り、RMB=1 で 0x89 が無ければ ATA PASS-THROUGH は送らない、許可リストは IDENTIFY DEVICE (0xEC) の正規形だけ (書き込み可でも同じ)。UI はスピンダウンを回転ディスクだけ従来どおりにし、フラッシュ/不明は既定で折りたたみ + 警告 + 確認ダイアログ。確認は機種 (VID:PID) と種別ごとに保存し、種別が変われば無効に戻る。アイドル自動停止は確認しない限り働かない (UsbSessionManager が強制)。ホストの偽 USB 転送の単体テスト 19 件 + JVM 単体テストで検証。
- [x] 種別判別の実機確認 (Ratoc RAID RS-EC32-U3R / Xperia XQ-FS44、2026-11-13): `Rotational rpm=5400 basis=AtaPassThrough12` (VPD は 00,83 のみで B1/89 なし → SAT(12) の IDENTIFY 語 217=0x1518=5400rpm)。open 後の SCSI 命令は 00 12 1a 25 88 9e a1 のみ (書き込み系なし、a1 は 1 回)。固まらず、パーティション表 (GPT 7.3TiB、#1 NTFS Ventoy / #2 FAT16 VTOYEFI) も通常どおり出た。
- [x] J9260 (Android 11) + Ratoc RAID (2026-11-13): 種別判別 = `Rotational 5400rpm [sat12]` (Xperia と同じ生の値)。手動の「アンマウントして、ディスクを止める」→「ディスクを停止中」表示 (SCSI 1b 送信) → 「ディスクを再開する」→「再開しました」(1b 2 回目)。エラーなし。アイドル自動停止 5 分 (設定 18:14 → 18:19:09 に `idle spin-down after 5 min`、ユーザーが実際に停止を確認) と、停止中のマウントによる自動再開 (START → 最初の READ まで約 3.1 秒、マウント全体 3.1 秒) も確認済み。停止中の抜去、書き込み可で止めた場合は未確認。注意: 画面が消えてロックされると操作できない (設定は usbmem_settings に保存される)。
- [ ] 種別判別の実機確認 (残り。**ユーザーの許可後**): F-51A + Ratoc RAID (回転ディスクが Rotational/rpm 付きで出るか、RAID が何を返すか)、A80 Pro の USB メモリ 3 本 (Unknown + 「USB メモリの可能性」か、SSD 型は NonRotational か)、USB-SATA の SSD。診断画面の「ディスクの種別」の `describe()` の行を控える。手順は docs/media-kind.md の 7 章。
- [ ] 種別判別: UAS 経路 (今は BOT のみ)、RAID ケースが先頭ディスクではなく論理ユニットとして答える場合の扱い、USB-NVMe ブリッジ (語 217 を返さないものが多い)、判別結果の再取得 (ホットスワップ) の要否。
- [x] **複数の USB メモリの同時利用** (docs/multi-usb.md): プロセス単位の `UsbSessionManager` が `deviceName` ごとにセッションを持ち、開く・閉じる・抜去・書き込みモード (読み取り専用 / 新規作成のみ / 読み書き)・アイドル停止・公開ボリューム (SAF ルート、ネストした子を含む) をデバイスごとに独立して扱う。二重オープン拒否、デバイスごとのカード UI、同機種でも区別できる公開名 (機種名 + シリアル下 4 桁、無ければバス番号)、抜去は該当デバイスだけ後始末 + ルートの通知、回転・Activity 再生成でセッションを維持 (プロセス再起動後の自動再オープンはしない)。JVM 単体テスト 26 件 (偽セッション。3 本同時・二重 open・個別 close/抜去・命名衝突・並行 I/O・OS マウントの持ち主判定) と AVD の計装テスト `MultiUsbTest` 4 件で検証。
- [ ] 複数 USB の実機確認: 給電付きハブに 3 本 -> 1 本ずつ開く -> 読む -> もう 1 本開く -> 1 本抜く (手順は docs/multi-usb.md の 10 章)。特に (1) 別の USB の OS 自動マウントがあるとき、sysfs による持ち主判定でこちらのメモリが開けるか (判定できなければ従来どおり拒否 = 安全側)、(2) 同時転送時の帯域・給電、(3) 同じシリアルを返す安価なメモリを複数挿したときの命名。
- [ ] 複数 USB: マウント処理のプロセス全体の直列化 (LinuxCompat の `g_mount_mu`) の緩和、アイドル停止・転送サイズ設定のシリアル単位の保存 (今は VID:PID 単位)、DiskTools / PartitionEditor / Recovery のデバイスごとの UI の入口 (ライブラリ層は `UsbSession.withHandle` で対応済み)。

## 他のファイルシステム (LinuxCompat に載せる)
- [x] exFAT (fs/exfat 無改変) 読み取り専用: ホスト 4 構成のテスト、AVD (x86_64) と F-51A (arm64) の計装テスト (3 イメージ・314 ファイルをバイト一致)。アプリは USB パーティションの FS を判別して NTFS / exFAT を自動で切り替える (exFAT は書き込み許可スイッチが ON でも読み取り専用でマウント)。docs/exfat-port.md 参照。
- [ ] exFAT の実機 USB ディスクでの確認 (数 TiB のボリュームの mount 時間: exfat_load_bitmap が全ビットマップを読む)。
- [x] exFAT の書き込み (fs/exfat 無改変、実験的・`USBMEM_NTFS_WRITE` ビルドのみ): VolumeDirty の先行確定、確保系/解放系の同期順序、電源断の模擬 (突然変異テストで検出力を確認)、fsck.exfat + exfat-fuse での独立検証、AVD。docs/exfat-write.md。
- [ ] exFAT の書き込み: 実機 USB での確認 (flush の効き方)、UI の書き込み許可スイッチ (親側)、ビットマップ走査の差分化、確保系/解放系が混在する rename の電源断検証。
- [ ] ピン表 (ino → inode) の上限 / LRU (巨大ディレクトリでメモリが増える)。
- [x] ext2 (読み取り専用、docs/ext2-port.md。ホスト4構成+AVD通過、実機未確認) / [x] ext3・ext4 (読み取り専用・ジャーナル非再生、needs_recovery は既定で拒否。docs/ext4-port.md。ホスト4構成+AVD通過、実機未確認)
- [ ] ext4: 実機 USB での確認、ACL/xattr の公開、ブロックサイズ 8K 以上。書き込みは実装済み (下の 5 番、docs/ext4-write.md)。
- [x] ext2 の書き込み (実験的、`USBMEM_NTFS_WRITE` ビルドのみ。docs/ext2-write.md): bh の dirty/writeback 基盤、作成・書き込み・truncate・削除・rename・symlink・hardlink。ホスト (e2fsck + fuse2fs 参照との全ツリー比較、モデル、3000 ファイル、電源断 320 試行/構成) と AVD で検証。ダーティ印を先に確定し、汚れた/エラー/未知機能のボリュームは読み取り専用。
- [ ] ext2 の書き込み: 実機 USB での確認 (flush の効き方)、ディレクトリ再帰削除の性能 (解放のたびに同期)、予約ブロックの扱い (uid 0 扱いで 100% まで使える)、inode キャッシュの上限。
- [x] リンク: DocumentsProvider はボリューム内でシンボリックリンクを辿って見せる (`LinkResolver.kt`: 絶対パスはボリュームのルート基準、".." はルートで止める、辿りは 40 回まで、祖先/自分を指すディレクトリリンク・壊れたリンク・循環はリンク先の文字列を中身とする小さなファイルとして見せる、削除/名前変更はリンク自身、再帰削除はリンクを辿らない)。アプリ内の「リンクを作る…」ダイアログ (ext2 のシンボリック/ハード。ハードは「すべて許可」のみ)。AVD の `LinkProviderTest` で検証。実機未確認。
- [x] 書き込み自己診断 (診断タブ。「すべて許可」でマウント中のみ。専用の一時フォルダだけを使い、全部消す)。AVD で検証。**実機の RAID ではまだ実行していない (ユーザーが任意のタイミングで実行する)**。
- [ ] NTFS のリパースポイント (シンボリックリンク/ジャンクション) の表示は未対応。
- [x] ext4 の書き込み: jbd2 (fs/jbd2 無改変) を互換層に載せた (docs/ext4-write.md)。ext2 の bh 基盤ではなく、カーネルと同じ意味の bh (`rt_kbh.c`) を ext4/jbd2 専用に足した。
- [x] FAT12/16/32 + vfat (fs/fat 無改変) 読み取り専用: ホスト 4 構成のテスト (8 イメージ x 約 331 ファイル、4Kn セクタ・フロッピー・断片化・日本語/サロゲートペア名・属性・2 秒時刻)、AVD (x86_64) の計装テスト 6 イメージ。docs/fat-port.md 参照。
- [ ] FAT の実機 USB ディスクでの確認 (手順は docs/fat-port.md の末尾)。FAT の書き込み、短い名前の codepage=932 (cp932 の表は登録済み。マウントオプションを渡す口が未実装)、隠し/システム属性の表示。
- [x] F2FS 読み取り専用 (docs/f2fs-port.md。**カーネルの fs/f2fs は取り込まず、オンディスク形式からの自前実装**。圧縮の展開のみ usbmem_kdecomp): インライン / 圧縮 (lz4・lzo・lzo-rle・zstd) / 二重間接 / roll-forward 検出 (EUCLEAN、確認のうえ無視して開ける)。検証は mkfs/sload/fsck.f2fs と、QEMU 上の本物のカーネルが書いたイメージ。実機 USB は未確認。
- [ ] F2FS: 実機での確認、圧縮の chksum 検証、xattr/ACL の公開、暗号化・casefold・verity・4K 以外のブロックサイズ。書き込みは実装済み (下、docs/f2fs-write.md): GC・roll-forward・quota・圧縮ファイルへの書き込みは未対応。
- [ ] 次: squashfs/erofs (進行中)、ntfs3 (NTFS の第二のバックエンドとして選択可能に)。見積もり (tools/fs_compat_probe.py): ntfs3 は 33k 行で iomap/folio 前提 (gcc エラー 618 件)。
- [x] ISO 9660 (fs/isofs: レベル 1〜3 / Joliet / Rock Ridge・relocation・シンボリックリンク。zisofs は EOPNOTSUPP で拒否) と UDF (fs/udf: 1.02〜2.01・Sparable・疎/断片化/ハードリンク/シンボリックリンク・ISO+UDF ブリッジは UDF 優先で開けなければ ISO) を読み取り専用で追加。docs/isofs-port.md / docs/udf-port.md。ホスト4構成 + AVD 通過、実機未確認。
- [ ] UDF の VAT / Metadata パーティション: 上流は読むが、ファイルの入った盤のフィクスチャを作れず未検証 (空の UDF 2.50 VAT のみ)。ISO: zisofs の展開・4 GiB 超・マルチセッション・実機の光学ドライブ。

## 開発要件書ロードマップ (2026-11-12 追加。順番は私 (Claude) に一任。要件書の章番号を併記)
並列に進められるものは worktree 付きの fork で同時に進める。各項目は「ホスト (PC) テスト → AVD → 実機は最後」。破壊的操作は必ず確認ダイアログ + 事前バックアップ。

進行中・近いもの (この順で着手):
1. [x] (core 実装済み・UI 未接続・実機書き込み未実施。docs/partition-edit.md) **パーティション管理の編集** (4 章): MBR / GPT / EBR の作成・削除・種別変更・名前変更・サイズ/位置変更、新規テーブル作成、パーティションテーブルのバックアップ/復元、GPT の CRC 検証 + バックアップ GPT の検査と修復計画 (読み取りは実装済み)、UUID / ラベル / 種別名の取得 (FS ラベルと UUID を含む)。編集は「計画 → 差分表示 → 確認トークン → 書き込み順序を守って適用 → 読み戻し検証」。マウント中は禁止。独立検証は sfdisk / parted / fdisk。UI は別段階。
2. [x] **BlockDevice の拡張** (2 章 P1/P2): NBD クライアント、QCOW2 / VHD / VHDX / VMDK の読み取り (qemu-img / qemu-nbd で検証)、COW オーバーレイ (イメージを書き換えずに書き込み可能にする)。※ NBD はネットワーク権限 (INTERNET) が要る。権限追加は別途明示してから。
   → core / C API / JNI / Kotlin 入口まで実装・ホスト検証済み (docs/blockdev-formats.md、docs/nbd.md)。**未了**: UI 統合 (イメージを開く導線・兄弟ファイル選択 `ImageResolver`)、AndroidManifest の INTERNET 権限 (NBD をネットワーク越しに使うとき)、差分 VHDX の実機 (Windows 製) での確認、VHDX ログ再生、QCOW2 の暗号化/zstd、NBD の TLS、AVD での実機相当テスト。
3. [x] (core 実装済み・UI 未接続) **暗号化ボリューム** (5 章): BlockTransform として BitLocker (読み取り専用: パスワード / 48 桁回復キー。実 Windows で作ったフィクスチャあり: tools/winvhd) → LUKS1 / LUKS2。ライセンス (libbde LGPL-3 / dislocker GPL-2 / cryptsetup) を比較して docs/licenses.md に記録。認証情報はログに出さない。

4. [~] **ネスト (ループバック) マウント** (2 章 P2): **読み取り専用は実装・検証済み** (docs/nested-mount.md)。ネイティブ層で親ボリュームのファイルを直接ブロックデバイスにする (SAF の fd は経由しない)。ボリューム単位のネイティブ mutex、閉じる順は 内 → 外 (VolumeRegistry が親子関係を持つ)、段数の上限 3、循環の拒否、パーティション表 (MBR / GPT / 論理) 付きイメージ、マウント中のイメージの書き換え禁止 (EBUSY)、自分のプロバイダ経由の URI はネストに振り替え。ホストテスト (5 種類の親 × 子) と AVD で検証。**残り**: 書き込みのネスト (外側が「フル」書き込みのとき。内側→外側の順に同期・アンマウント、外側の書き込みを内側の同期後にだけ許す)、実機での確認、ネストしたマウントの再起動後の復元 (今は保存しない)、親が書き込み可のときの別名 (ハードリンク) 経由の書き換え検出の強化 (今はネイティブ側のサイズ・更新時刻の照合で検出)。
5. [x] (ホスト・AVD で検証済み・実機未確認・`USBMEM_NTFS_WRITE` ビルドのみ。docs/ext4-write.md) **ext4 の書き込み (jbd2)** (3 章): fs/jbd2 を無改変で互換層に載せた
   (kjournald2 は作らず「待ち」で同期実行。カーネルと同じ意味の buffer_head `rt_kbh.c`)。① `Ext4Volume::readlink` ② 書き込み可のマウントだけでジャーナルを再生
   (debugfs が書いたトランザクションの再生が e2fsck とブロック一致、再生の途中の電源断も再実行で収束) ③ CreateOnly ④ Full (上書き・truncate・削除・rename・symlink・hardlink)。
   ホワイトリスト (extent / 64bit / flex_bg / metadata_csum / csum_seed / orphan_file / huge_file / dir_nlink / extra_isize / gdt_csum、ext3) 以外は読み取り専用。
   電源断の模擬 (R/E/S/A/B の 5 検査。満杯 + 解放ブロック再利用シナリオで 640 試行・80 カットポイント失敗 0) と突然変異 4 種 (flush・ダーティ印・ordered・解放ブロックの再利用) を検出。
   - [ ] 残り: 実機 USB、meta_bg / inline_data / bigalloc / quota / largedir / ea_inode の書き込み、delalloc、大きなファイルの書き込み性能、ACL/xattr の書き込み。
   - 続き: 自前実装リーダー (F2FS / Btrfs / XFS) の**書き込み**も対象 (ユーザー希望)。順は ext4 (カーネルの jbd2) → F2FS → Btrfs → XFS の見込み。各 FS とも「新規作成のみ (CreateOnly)」→ 完全書き込みの順に段階化し、書き込み前の必須条件は ext2/exFAT と同じ: ダーティフラグ先行、解放ブロックの再利用禁止、順序付き書き戻し、クラッシュ模擬 (接頭辞/キャッシュ × 途切れ書き込み)、独立ツール (fsck.f2fs / btrfs check / xfs_repair) と実カーネルで作ったイメージによる検証、ミューテーションテスト、AVD での自己テスト。
   - F2FS の書き込みは実装済み (ホスト・CI・AVD で検証、実機未確認、`USBMEM_NTFS_WRITE` ビルドのみ。docs/f2fs-write.md): チェックポイント方式 (新しいブロックへ書き、反対側の CP パックを 本体 → flush → フッタ の順に確定。roll-forward・orphan・fsync ログ・GC・discard なし)。CreateOnly と Full。`fsck.f2fs` と本物のカーネル (QEMU VM / CI のループマウント) で双方向に検証、電源断の全カットポイント、突然変異 7 種。
6. [ ] **ディスク管理 DiskTools** (6 章): 読み取り専用の整合性検査 (fsck 相当: ext / exFAT / FAT)、フォーマット (mkfs: FAT / exFAT / ext2-4 / NTFS)。NTFS の ntfsfix による自動修復は使わない (要件)。修復は明示承認 + バックアップ推奨。
7. [ ] **データ復旧 RecoveryEngine** (7 章): ddrescue 相当のレスキューコピーは core 実装済み (mapfile 互換・再開・読み取り専用ソース・安全弁。UI 未接続・実機/AVD 未検証。docs/recovery-backup.md)。**未着手**: TestDisk 相当のパーティション探索・ブートセクタ検査・ファイル救出、PhotoRec 相当のシグネチャ復元。
8. [x] (core 実装済み・UI 未接続・実機/AVD 未検証。docs/recovery-backup.md) **バックアップ・クローン BackupEngine** (8 章): Partclone 形式 (ext2/3/4・FAT・exFAT・NTFS の使用ブロックのみ、LZ4、チェックサム検証、計画→確認トークン→適用の復元、コピー元/先の取り違え防止、進捗・取り消し)。ディスク丸ごとのクローン(ブロック単位コピー)は救出エンジンで代替できるが、専用 UI は未実装。
9. [ ] **UI の統合** (11 章): デバイス一覧のパーティション構成グラフィカル表示、暗号化状態、検査/フォーマット/パーティション編集/バックアップ/救出の導線 (破壊的操作と読み取り専用の明確な区別)。
10. [ ] **公開準備** (13 章): GPL・依存ライセンス監査 (docs/licenses.md の更新)、ソース公開手順、16KB ページサイズ (実機/エミュレータで確認)、targetSdk 最新要件、プライバシーポリシーとデータセーフティ、exFAT 特許、Play ポリシー再確認。
11. [ ] **P2 以降**: SMB (9 章、libsmbclient / SMBJ)、ntfs3 (第二の NTFS バックエンド)、Btrfs / XFS / F2FS、UASP、QEMU 仮想 USB の AVD 接続実測 (14 章)。

- [x] XFS 読み取り専用 (v4/v5、独立実装のリーダ — カーネル fs/xfs は取り込まず形式を実装。docs/xfs-port.md): 18 イメージ + special_proto、ホスト検証、AVD 通過。ログ未再生は EUCLEAN + allow_noload。[ ] 実機確認、reflink 共有 extent の実イメージ、リアルタイム領域・外部ログ・xattr は未対応。

進行中のファイルシステム移植 (別の worktree): FAT/vfat・isofs・udf (統合済み)、squashfs・erofs (検証中)。

ホストに入れてもらうと検証が進むツール: gptfdisk (sgdisk)、cryptsetup、ddrescue、partclone、libbde-tools (bdeinfo / bdemount)、nbdkit。無くても自前の Python / qemu-nbd 等で代替して進めるが、独立検証の強さが落ちる。

暗号化ボリューム (上の項 3) の進捗: BitLocker / LUKS1 / LUKS2 の読み取り専用復号 (core + C API + JNI + Kotlin ラッパ) は実装・検証済み (docs/crypto-volumes.md)。未了: UI (パスワード入力・解錠状態の表示)、Android Keystore 連携 (設計メモのみ)、ARM 実機での速度測定と ARMv8 Crypto Extensions の高速パス、Windows 7/8 実機の diffuser 付きボリュームでの確認、LUKS2 4096 バイトセクタの実カーネル照合。

DiskTools (上の項 6) の進捗: 読み取り専用の整合性検査 (FAT12/16/32・exFAT・ext2/3/4・NTFS) と mkfs (ext2・exFAT・FAT32) は core + C API + JNI + Kotlin ラッパまで実装・検証済み (docs/disk-tools.md。fsck.fat / fsck.exfat / e2fsck / ntfsfix -n との突き合わせで見逃し 0・過検出 0、ASan/UBSan で破損入力 2000 件)。未了: UI、実機 USB メモリでの mkfs、NTFS の mkfs (対象外)、ext の htree 順序・bigalloc・meta_bg の検査、FAT16/FAT12 の mkfs、実 Windows / macOS での読み取り確認、修復 (明示承認つき) は未着手。

Btrfs (読み取り専用) の進捗: 単一デバイスの Btrfs を自前のリーダーで読み取り専用マウントできる (カーネルの fs/btrfs は載せていない。理由は docs/btrfs-port.md)。
チェックサム 4 種・zlib/lzo/zstd・ノード/セクタサイズ違い・サブボリューム・DUP・NODATASUM・tree-log 未再生の確認ダイアログまで host 4 構成と AVD で検証済み。
未了: 複数デバイス / RAID、サブボリューム選択 UI、xattr、実機確認、reflink の実イメージ。XFS / F2FS は別の worktree。
Btrfs の書き込み (実験的・`USBMEM_NTFS_WRITE` ビルドのみ・docs/btrfs-write.md): 読み取りリーダーの上に自前の COW エンジンを足した (カーネルの fs/btrfs は載せていない)。CreateOnly と Full (削除・上書き・truncate・rename・hardlink・symlink) を実装し、ホスト (btrfs check / btrfs restore / 実カーネル相互確認は CI) で検証。
ホワイトリスト外 (RAID・サブボリューム・quota・ログツリー未再生・未知フラグ・ミラー不一致など) は読み取り専用。未了: 実機 USB、システム chunk の確保、サブボリューム/スナップショット/reflink 共有 extent の書き込み、tree-log への書き込み、大量削除の分割トランザクション。

- [x] **実機 USB (TransMemory、A の p1_ntfs) への書き込み試験 (2026-11-13、Xperia XQ-FS44)**: `RealUsbWriteTest` (tools/run_android_real_usb_verify.sh --write) の全段階が合格 (事前の fsck/照合 → CreateOnly → 事後照合 → Full (WriteSelfTest + 32MiB 速度計測) → 最終照合)。PC 側の独立検証も合格: `ntfsfix -n` 正常、`make_real_usb_testset.py verify … --allow-extra` が「一致」(ファイル 3026・1.07GiB)。書き込み速度は約 1.8MB/s (この USB メモリの性能と思われる)。残り: A の ext4/exFAT、B の NTFS/exFAT/ext2/ext3 の同試験。
- [x] **実機 USB (TransMemory、A の p2_ext4) への書き込み試験 (2026-11-13、Xperia XQ-FS44)**: `RealUsbWriteTest` の全段階が合格 (jbd2 経由、クリーンアンマウント含む)。PC 側の独立検証も合格: `e2fsck -fn` がエラーなし (3089 ファイル、前後で同じ数 = 残骸なし)、`verify` が「一致」(ファイル 3029・シンボリックリンク 4・1.07GiB)。書き込み速度は約 2MB/s (USB メモリの性能と思われる)。残り: A の exFAT、B の NTFS/exFAT/ext2/ext3。
- [x] **実機 USB (A の p4_exfat) への書き込み試験 (2026-11-13、Xperia XQ-FS44)**: `RealUsbWriteTest` の全段階が合格 (約 70 秒)。書き込み速度は約 2MB/s。PC 側の独立検証も合格 (下の B の項に合わせて記録)。
- [x] **実機 USB (B = SD カード + USB 変換 1908:0226、p3_exfat / p5_ext2 / p6_ext3 / p2_ntfs) への書き込み試験 (2026-11-13、J9260)**: 4 パーティションとも `RealUsbWriteTest` の全段階が合格 (32MiB 書き込みは約 5〜8MB/s、NTFS は 4.5GiB のファイル入りでも約 40 秒)。p7_ext4 (needs_recovery) と p1_fat32 (書き込み未実装) は対象外。**PC 側の独立検証 (A・B 合わせて 7 パーティション) もすべて合格**: `ntfsfix -n` (A p1、B p2) 正常、`e2fsck -fn` (A p2_ext4、B p5_ext2、B p6_ext3) エラーなし、`fsck.exfat -n` (A p4: ファイル 3024、B p3: 3025) clean、`verify … --allow-extra` が 7 つとも「一致」(B の NTFS は 4.57GiB)。
- [x] **実機 USB (C = USB DISK 090c:2000、p3_f2fs / p2_btrfs) への書き込み試験 (2026-11-13、J9260、自前の書き込み実装として実機では初)**: 2 パーティションとも `RealUsbWriteTest` の全段階が合格 (F2FS: 32MiB 書き込み 2.8〜8.5 秒、Btrfs: 3.0〜8.5 秒)。Btrfs は小さいファイル 20 個の作成に約 18 秒かかる (F2FS は約 1 秒。1 ファイルごとにコミットしているためと思われる。性能の改善課題)。PC 側の `fsck.f2fs` / `btrfs check` / `verify` は、これから (未実施)。XFS は書き込み実装中。
- [ ] **USB マスストレージの複数 LUN 対応 (カードリーダーの SD / microSD スロット)**: 実装済み (core の GET MAX LUN・LUN 共有のバス・LUN 一覧、C API、JNI、UsbSessionManager のスロット (デバイス名 + LUN)、接続の参照カウント共有、LUN ごとのカード UI、「LUN 一覧を更新」「OS に返す」)。設計と制約は docs/multi-usb.md の 11 章。背景: 「USB3.0 Card Reader」(05e3:0749、LUN 2 つ) は SD カードが LUN 1 にあり、LUN 0 だけを開いて open failed: disconnected になっていた。残り: **実機のカードリーダーでの確認** (docs/multi-usb.md の 11.5 の手順。ユーザーの許可を得て)、UAS 接続の機器の LUN (BOT のみ対応)。 **実機確認 (2026-11-13、J9260 + ハブ内蔵リーダー 05e3:0749、B を SD スロットに挿入、読み取り専用)**: 「開く」で LUN 0 が「メディアなし」(灰色・開けない)、LUN 1 が「14.6 GiB」で自動接続、パーティション一覧が B と一致、exFAT を読み取り専用でマウントでき公開名は「… [1539] · LUN 1 #3」、LUN 1 を閉じると「メディアあり · 14.6 GiB」に戻り、「この USB を OS に返す」で OS が B のボリュームを再マウントした。開いた直後に OS が先に再マウントすることはなかった。未確認: 両スロットにカード、カード交換後の「LUN 一覧を更新」、LUN 1 つの USB メモリの回帰 (A/B/C はアダプタ経由でなく直接)。
