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

## 他のファイルシステム (LinuxCompat に載せる)
- [x] exFAT (fs/exfat 無改変) 読み取り専用: ホスト 4 構成のテスト、AVD (x86_64) と F-51A (arm64) の計装テスト (3 イメージ・314 ファイルをバイト一致)。アプリは USB パーティションの FS を判別して NTFS / exFAT を自動で切り替える (exFAT は書き込み許可スイッチが ON でも読み取り専用でマウント)。docs/exfat-port.md 参照。
- [ ] exFAT の実機 USB ディスクでの確認 (数 TiB のボリュームの mount 時間: exfat_load_bitmap が全ビットマップを読む)。
- [x] exFAT の書き込み (fs/exfat 無改変、実験的・`USBMEM_NTFS_WRITE` ビルドのみ): VolumeDirty の先行確定、確保系/解放系の同期順序、電源断の模擬 (突然変異テストで検出力を確認)、fsck.exfat + exfat-fuse での独立検証、AVD。docs/exfat-write.md。
- [ ] exFAT の書き込み: 実機 USB での確認 (flush の効き方)、UI の書き込み許可スイッチ (親側)、ビットマップ走査の差分化、確保系/解放系が混在する rename の電源断検証。
- [ ] ピン表 (ino → inode) の上限 / LRU (巨大ディレクトリでメモリが増える)。
- [x] ext2 (読み取り専用、docs/ext2-port.md。ホスト4構成+AVD通過、実機未確認) / [x] ext3・ext4 (読み取り専用・ジャーナル非再生、needs_recovery は既定で拒否。docs/ext4-port.md。ホスト4構成+AVD通過、実機未確認)
- [ ] ext4: 実機 USB での確認、書き込み(ジャーナル実装が必要)、ACL/xattr の公開、ブロックサイズ 8K 以上。
- [x] ext2 の書き込み (実験的、`USBMEM_NTFS_WRITE` ビルドのみ。docs/ext2-write.md): bh の dirty/writeback 基盤、作成・書き込み・truncate・削除・rename・symlink・hardlink。ホスト (e2fsck + fuse2fs 参照との全ツリー比較、モデル、3000 ファイル、電源断 320 試行/構成) と AVD で検証。ダーティ印を先に確定し、汚れた/エラー/未知機能のボリュームは読み取り専用。
- [ ] ext2 の書き込み: 実機 USB での確認 (flush の効き方)、ディレクトリ再帰削除の性能 (解放のたびに同期)、予約ブロックの扱い (uid 0 扱いで 100% まで使える)、inode キャッシュの上限。
- [x] リンク: DocumentsProvider はボリューム内でシンボリックリンクを辿って見せる (`LinkResolver.kt`: 絶対パスはボリュームのルート基準、".." はルートで止める、辿りは 40 回まで、祖先/自分を指すディレクトリリンク・壊れたリンク・循環はリンク先の文字列を中身とする小さなファイルとして見せる、削除/名前変更はリンク自身、再帰削除はリンクを辿らない)。アプリ内の「リンクを作る…」ダイアログ (ext2 のシンボリック/ハード。ハードは「すべて許可」のみ)。AVD の `LinkProviderTest` で検証。実機未確認。
- [x] 書き込み自己診断 (診断タブ。「すべて許可」でマウント中のみ。専用の一時フォルダだけを使い、全部消す)。AVD で検証。**実機の RAID ではまだ実行していない (ユーザーが任意のタイミングで実行する)**。
- [ ] NTFS のリパースポイント (シンボリックリンク/ジャンクション) の表示は未対応。
- [ ] ext4 の書き込みは ext2 の書き込み基盤の上に載せる (jbd2 の移植が必要。exFAT は完了: docs/exfat-write.md)。
- [x] FAT12/16/32 + vfat (fs/fat 無改変) 読み取り専用: ホスト 4 構成のテスト (8 イメージ x 約 331 ファイル、4Kn セクタ・フロッピー・断片化・日本語/サロゲートペア名・属性・2 秒時刻)、AVD (x86_64) の計装テスト 6 イメージ。docs/fat-port.md 参照。
- [ ] FAT の実機 USB ディスクでの確認 (手順は docs/fat-port.md の末尾)。FAT の書き込み、短い名前の codepage=932 (cp932 の表は登録済み。マウントオプションを渡す口が未実装)、隠し/システム属性の表示。
- [ ] 次: squashfs/erofs (進行中)、ntfs3 (NTFS の第二のバックエンドとして選択可能に)。見積もり (tools/fs_compat_probe.py): ntfs3 は 33k 行で iomap/folio 前提 (gcc エラー 618 件)。
- [x] ISO 9660 (fs/isofs: レベル 1〜3 / Joliet / Rock Ridge・relocation・シンボリックリンク。zisofs は EOPNOTSUPP で拒否) と UDF (fs/udf: 1.02〜2.01・Sparable・疎/断片化/ハードリンク/シンボリックリンク・ISO+UDF ブリッジは UDF 優先で開けなければ ISO) を読み取り専用で追加。docs/isofs-port.md / docs/udf-port.md。ホスト4構成 + AVD 通過、実機未確認。
- [ ] UDF の VAT / Metadata パーティション: 上流は読むが、ファイルの入った盤のフィクスチャを作れず未検証 (空の UDF 2.50 VAT のみ)。ISO: zisofs の展開・4 GiB 超・マルチセッション・実機の光学ドライブ。

## 開発要件書ロードマップ (2026-11-12 追加。順番は私 (Claude) に一任。要件書の章番号を併記)
並列に進められるものは worktree 付きの fork で同時に進める。各項目は「ホスト (PC) テスト → AVD → 実機は最後」。破壊的操作は必ず確認ダイアログ + 事前バックアップ。

進行中・近いもの (この順で着手):
1. [x] (core 実装済み・UI 未接続・実機書き込み未実施。docs/partition-edit.md) **パーティション管理の編集** (4 章): MBR / GPT / EBR の作成・削除・種別変更・名前変更・サイズ/位置変更、新規テーブル作成、パーティションテーブルのバックアップ/復元、GPT の CRC 検証 + バックアップ GPT の検査と修復計画 (読み取りは実装済み)、UUID / ラベル / 種別名の取得 (FS ラベルと UUID を含む)。編集は「計画 → 差分表示 → 確認トークン → 書き込み順序を守って適用 → 読み戻し検証」。マウント中は禁止。独立検証は sfdisk / parted / fdisk。UI は別段階。
2. [ ] **BlockDevice の拡張** (2 章 P1/P2): NBD クライアント、QCOW2 / VHD / VHDX / VMDK の読み取り (qemu-img / qemu-nbd で検証)、COW オーバーレイ (イメージを書き換えずに書き込み可能にする)。※ NBD はネットワーク権限 (INTERNET) が要る。権限追加は別途明示してから。
3. [ ] **暗号化ボリューム** (5 章): BlockTransform として BitLocker (読み取り専用: パスワード / 48 桁回復キー。実 Windows で作ったフィクスチャあり: tools/winvhd) → LUKS1 / LUKS2。ライセンス (libbde LGPL-3 / dislocker GPL-2 / cryptsetup) を比較して docs/licenses.md に記録。認証情報はログに出さない。
4. [ ] **ネスト (ループバック) マウント** (2 章 P2): SAF / 他ボリューム上のイメージファイルを開く。ネイティブ層で親ボリュームのファイルを直接ブロックデバイスにする (SAF の fd 経由はプロバイダ専用スレッドの行き詰まりを起こすため不可)。まず読み取り専用 → 書き込み (内側→外側の順に同期・アンマウント、依存関係の管理)。パーティション表付きイメージも対応。
5. [ ] **ext4 の書き込み (jbd2)** (3 章): jbd2 を無改変で互換層に載せる、電源断の模擬。ext4 の symlink (`Ext4Volume::readlink`) も未実装 (LinkResolver は ext4 を対象にしているが読めない)。
6. [ ] **ディスク管理 DiskTools** (6 章): 読み取り専用の整合性検査 (fsck 相当: ext / exFAT / FAT)、フォーマット (mkfs: FAT / exFAT / ext2-4 / NTFS)。NTFS の ntfsfix による自動修復は使わない (要件)。修復は明示承認 + バックアップ推奨。
7. [ ] **データ復旧 RecoveryEngine** (7 章): ddrescue 相当のレスキューコピー (mapfile 互換、再開、読み取り専用ソース)、TestDisk 相当のパーティション探索・ブートセクタ検査・ファイル救出、PhotoRec 相当のシグネチャ復元。
8. [ ] **バックアップ・クローン BackupEngine** (8 章): Partclone 方式 (使用ブロックのみ、検証、復元、コピー元/先の取り違え防止、進捗・再開)。
9. [ ] **UI の統合** (11 章): デバイス一覧のパーティション構成グラフィカル表示、暗号化状態、検査/フォーマット/パーティション編集/バックアップ/救出の導線 (破壊的操作と読み取り専用の明確な区別)。
10. [ ] **公開準備** (13 章): GPL・依存ライセンス監査 (docs/licenses.md の更新)、ソース公開手順、16KB ページサイズ (実機/エミュレータで確認)、targetSdk 最新要件、プライバシーポリシーとデータセーフティ、exFAT 特許、Play ポリシー再確認。
11. [ ] **P2 以降**: SMB (9 章、libsmbclient / SMBJ)、ntfs3 (第二の NTFS バックエンド)、Btrfs / XFS / F2FS、UASP、QEMU 仮想 USB の AVD 接続実測 (14 章)。

進行中のファイルシステム移植 (別の worktree): FAT/vfat・isofs・udf (統合済み)、squashfs・erofs (検証中)。

ホストに入れてもらうと検証が進むツール: gptfdisk (sgdisk)、cryptsetup、ddrescue、partclone、libbde-tools (bdeinfo / bdemount)、nbdkit。無くても自前の Python / qemu-nbd 等で代替して進めるが、独立検証の強さが落ちる。

暗号化ボリューム (上の項 3) の進捗: BitLocker / LUKS1 / LUKS2 の読み取り専用復号 (core + C API + JNI + Kotlin ラッパ) は実装・検証済み (docs/crypto-volumes.md)。未了: UI (パスワード入力・解錠状態の表示)、Android Keystore 連携 (設計メモのみ)、ARM 実機での速度測定と ARMv8 Crypto Extensions の高速パス、Windows 7/8 実機の diffuser 付きボリュームでの確認、LUKS2 4096 バイトセクタの実カーネル照合。
