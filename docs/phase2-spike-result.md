# Phase 2 spike 結果: fs/ntfs を無改変でビルドするために必要な互換面

方法: 取り込んだ `third_party/ntfs-resurrection/fs-ntfs/*.c`(無改変, commit 067c5c24cef2)を、**空のスタブヘッダ + 基本型(u8/__le16/loff_t 等)と
byteorder/min/max マクロだけ**で `gcc -fsyntax-only` した。残ったエラーが「互換レイヤーが最低限埋めるべき面」の下限になる。

## 実測(段階 1)
- 基本型だけで総エラー 17,085 → 5,030 件に減少(大半は型欠落の連鎖だった)。
- 未定義の **型 12**(pgoff_t, atomic_t/atomic64_t, wait_queue_head_t, kuid_t/kgid_t, errseq_t, rwlock_t, vm_fault_t, blk_opf_t 等)。
- 未宣言の **識別子 204**(定数・マクロ。`docs/ntfs-spike-missing-identifiers.txt`)。
- 未宣言の **関数/マクロ 436**(うち ntfs 自身のものは 2 のみ。`docs/ntfs-spike-missing-functions.txt`)。
  内訳の多い系統: inode_* 27, folio_* 26, iomap_* 14, bio_* 12, generic_* 10, d_* 9, filemap_* 8, bdev_* 7, atomic_* 7, fsparam_* 7 …
- **不完全型のまま使われている構造体**(メンバ参照エラーの多い順): inode 484, super_block 100, iomap 62, dentry 43, file 30, kstat 28, kiocb 23, folio 16, bio 16, kstatfs 13, iattr 13, fs_context 13。

## 解釈(事実と推定を分ける)
- 事実: 必要面は「約 450 の関数/マクロ + 約 200 の定数 + 十数個の構造体定義」で、これは**下限**(構造体を定義すると、メンバ・次の層のエラーが新たに現れる)。
- 事実: 数は多いが、大半は inode/folio/iomap/bio/dentry の 5 系統に集中している。ext4 など他のカーネル FS にも共通で、`LinuxCompat` を共通化する要件書の方針と整合する。
- 推定(未検証): iomap の buffered write/読み出しヘルパ(カーネル側の実装は fs/ntfs の外)と folio のロック/dirty/writeback の意味論を
  最小限で正しく実装するのが最大の工数・リスク。書き込み系は読み取り系より桁違いに重い。

## 判断(暫定)
方式 A(最小 VFS エミュレーション)は**技術的に閉じた問題**で、実測上も不可能ではない。ただし工数は大きい。段階的に進める案:
1. **読み取り専用の最小面**(super_block/inode/address_space/folio/bio→BlockDevice、iomap は read のみ)で、`$MFT`→ルート列挙→ファイル読み出しまで。
2. 通ったら SAF 公開(Phase 2 完了条件)。書き込みは Phase 3 で iomap write/writeback を追加。
3. 1 が想定より膨らむ場合のみ B(部分移植)へ切替。切替理由は本書に追記する。

再現: `tools/ntfs_compat_spike.sh`。

## 進捗ログ: 互換レイヤー実装 (方式 A, 読み取り専用優先)

`tools/ntfs_compat_build.sh` で `fs/ntfs/*.c` を `-fsyntax-only` した総エラー数(少ないほど互換面が埋まっている)。

| 段階 | 内容 | 総エラー |
|---|---|---|
| 0 | 空スタブのみ | 17,085 |
| 1 | 基本型・エンディアン・マクロ | 5,030 |
| 2 | `compat/kernel.h`(型・属性・byteorder) | 4,347 |
| 3 | `compat/sync.h`(atomic/mutex/rwsem/list/bitops/alloc/ERR_PTR)+ `compat/fs.h`(inode/super_block/folio/bio/iomap 等の構造体) | 2,267 |
| 4 | include 経路の修正(`-iquote`)、static_assert/wait_event | **1,488** |

内訳(段階 4): 未宣言識別子 361 / 暗黙関数宣言 575 / 不完全型 49 / メンバ不足 171。
残りの大半は関数プロトタイプ(`inode_*`, `folio_*`, `iomap_*`, `bio_*`, `d_*`, `generic_*`)と、`DECLARE_IOMAP_ITER_NEXT` など iomap 補助マクロ。

### 次にやること(未着手)
1. 残りの関数プロトタイプと定数(`docs/ntfs-spike-missing-*.txt` を参照)を宣言し、全 `.c` が構文通過する状態にする。
2. 実行時実装(`core/compat/src/`): ページキャッシュ(address_space→folio)、`bio`→`BlockDevice` ブリッジ、inode ハッシュ(`iget5_locked`)、`iomap_read_folio`。
3. 書き込み・xattr・DIO・swapfile・workqueue 系は読み取り専用段階ではスタブ(`-ENOSYS`)にする。
4. 最初のリンク可能な到達点: `runlist.c` `mst.c` `unistr.c` `upcase.c` `collate.c` と LZX/XPRESS を単体テスト付きでリンク。
5. その後 `$MFT` → ルート列挙 → ファイル読み出し。

### 現時点の状態(正直な記録)
- **NTFS はまだマウントも読み取りもできない。** `core/compat` は CMake に未接続で、アプリ・テストには影響しない。
- 互換面の下限見積り(関数 約 430 / 定数 約 200 / 構造体 十数個)は変わらず、実装量は数千行規模。

### 更新 (互換ヘッダー完成)
- 段階 5〜: `compat/consts.h`, `vfs_api.h`, `misc.h`, `ops.h`(`tools/gen_ops_structs.py` が上流の演算テーブルから自動生成)を追加。
  iomap は上流 `include/linux/iomap.h`(commit 067c5c24cef2)の `iomap_iter_next` / `DEFINE_IOMAP_ITER_NEXT*` を同等に移植(GPL-2.0)。
- **結果: `fs/ntfs/*.c` と `lib/*.c` の全 31 ファイルが無改変のままコンパイル通過(構文エラー 0、`-c` でオブジェクト生成まで成功)。**
  総エラーの推移: 17,085 → 0。`tools/ntfs_compat_build.sh` で再現。
- ただしこれは**ヘッダーが埋まっただけ**で、動作する実装はまだ無い。オブジェクトをリンクするために実装が必要な外部シンボルは
  **224 個**(`docs/ntfs-compat-runtime-todo.txt`)。内訳の傾向: folio_* / inode・dentry 系 / bio・bdev 系 / iomap_* / generic_* / filemap_*。
  うち読み取り専用の最小到達点で本当に必要なのは一部(書き込み・VFS 公開・DIO・swapfile 系は `-ENOSYS` スタブで可)と見込むが、**未検証**。
- 次: `core/compat/src/` に最小実装(pagecache / inode hash / bio→BlockDevice / iomap_read_folio / iomap_iter_continue)を作り、
  CMake で `ntfs_fs` ターゲットとしてリンク → `runlist` 等の単体テスト → `$MFT` 読み出し。

### 更新 (最初の読み取り成功)
- `core/compat/src/` に実行時を実装: `rt_core.c`(メモリ/同期/inode キャッシュ/dentry/マウント)、`rt_pagecache.c`(folio・bio・iomap 読み取り・UTF 変換)、
  `rt_vfs.c`(ホスト向け mount/lookup/readdir/read)、`bdev_bridge.cpp`(BlockDevice ブリッジ)、`ntfs_volume.cpp`(C++ ラッパ)。
  未実装の 63 シンボルは `tools/gen_compat_stubs.py` が生成するスタブ(`rt_stubs.c`)。呼ばれると stderr に名前を出して -ENOSYS を返す。
- **結果: 無改変の `fs/ntfs` で、`mkntfs` + `ntfs-3g` が作った実 NTFS イメージ(32MiB, 512B セクタ, 4KiB クラスタ)を読み取り専用でマウントし、
  ルート列挙・入れ子ディレクトリ・日本語名・3MiB+777B ファイル・空ファイルを sha256 付きで検証して一致。** 失敗 0。
  通常ビルドと ASan/UBSan(リーク検出あり)の両方で `ctest` 通過(`ntfs_gen_fixture` + `ntfs_read`)。イメージは tmpfs (/dev/shm)。
- 読み取り中に実際に呼ばれた未実装 API: `super_set_uuid` のみ(影響なし)。
- ビルド上の注意: 互換シム(`<linux/errno.h>` 等)は **C ソース専用**にする(C++ に見せると glibc の `<errno.h>` が奪われ std ヘッダと衝突)。
  `project()` に C を含め忘れると C ソースが黙って無視される(一度ハマった)。

### 制約・未検証(正直な記録)
- 検証したのは **mkntfs/ntfs-3g が作った小さな正常イメージ 1 種**のみ。圧縮ファイル・暗号化・スパース・ADS・reparse・巨大ファイル・
  破損/敵対的イメージは未検証。Windows 作成イメージとの照合も未実施(Phase 2 完了条件)。
- folio は 4KiB 固定(large folio 未対応)。`iomap_readahead`、書き込み系、DIO、xattr/ACL は未実装(スタブ)。
- マウント/アンマウントの完全な後始末(inode 退避、メモリ解放)は未実装。読み取り専用固定。
- 単一スレッド前提(inode hash の I_NEW 待ちなし)。
- Android(NDK)ビルドへの接続と SAF DocumentsProvider への接続は未着手。

### Codex 2 回目レビューの反映 (2026-11-11)
修正済み: bdev ブリッジの戻り値を負の errno に統一 / UTF-8 の厳密検証 (overlong・サロゲート・U+10FFFF 超を拒否) /
マウント失敗時の sb・fc 解放 / lookup の dentry 破棄時に inode 参照を返す。
追加修正: アンマウント実装 (evict_inodes → put_super → 残り退避 → sb/dentry/bdev 解放)、folio の参照規約を Linux と同じに
(キャッシュ 1 + get 系で +1、最後の put で解放)。アンマウント時の ASan 検出で実際に folio UAF を再現して修正。
20 回のマウント/読み取り/アンマウント反復を ASan(リーク検出あり)で通過。
未対応 (記録): fill_super 失敗時の s_fs_info 解放(小規模リーク)。

### Android への接続 (2026-11-11)
- `usbmem_ntfs` を NDK ビルド (arm64-v8a / x86_64) に組み込み、JNI (`jni_ntfs.cpp`) と Kotlin `NtfsVolume` (list / read / close、読み取り専用) を追加。
  `libusbmem_jni.so` は GPL-2.0-or-later の NTFS 部分を含むため共有ライブラリ全体が GPL 扱い。16KB ページ整列 (LOAD align 0x4000) を両 ABI で確認。
- 互換シムの Android 対応: bionic 内部ヘッダが `<linux/types.h>` 等を引くため、低レベル uapi ヘッダは実物を素通しにし (`include_next`)、
  `u64` 等は bionic の `__u64` (unsigned long long) に揃えた。`EFSCORRUPTED` を定義、`<strings.h>` を追加。
- **AVD (API 37, x86_64) の計装テスト 5 件成功**: 実 NTFS イメージ (64MiB) を 全ファイル sha256 で照合 (20MiB スパース・日本語名・800 件ディレクトリ含む)、
  NTFS でないデータは NativeException で失敗。arm64-v8a は **ビルドのみ**で実行は未検証 (実機/エミュレータ未使用)。
- 次: SAF DocumentsProvider (別アプリから NTFS イメージを読めること = Phase 2 完了条件)。

### SAF DocumentsProvider (2026-11-11)
- `NtfsDocumentsProvider` (authority `org.usbmem.app.documents`, `MANAGE_DOCUMENTS` 必須 = システムの Documents UI のみ呼べる) + `VolumeRegistry`。
  documentId = `<volumeId>:<絶対パス>`、`..`/`.` を含むパスは拒否。`openDocument` は `"r"` のみ許可し、`StorageManager.openProxyFileDescriptor` でシーク可能な読み取り fd を返す。
  書き込み・削除・名前変更は一切許可しない (Document flags = 0)。SAF の URI は private な SharedPreferences に保存し、次回起動時に再マウント。
- UI: 「NTFS イメージを他アプリに公開」ボタン (takePersistableUriPermission → マウント → roots 更新通知)。
- AVD 計装テスト **7 件成功**: ContentResolver (バインダ経由) で roots / 子一覧 / 3MiB ファイルの sha256 / 書き込みモード拒否 / `..` 拒否 / isChildDocument。
- **未検証**: 実際のシステム Documents UI / 他社ファイルマネージャからの操作 (テストは自アプリからの ContentResolver 呼び出し)、
  選択 UI のエンドツーエンド (SAF ピッカー操作)、arm64 実行、USB 経由 (Phase 3)。

### 破損イメージ fuzz (2026-11-11)
- `usbmem_ntfs_tests --fuzz <dir> <回数> <seed>`: 実イメージを読み取り専用のまま、メモリ上で 1〜24 バイト変異 (ブート領域・MFT 周辺・全域) して
  マウント → 走査 (最大 1200 エントリ、各ファイル先頭 32KiB) を繰り返す。ASan/UBSan、ハング検出 (30 秒 alarm)。`--replay` で変異を再現。CTest に 150 回を組込み。
- **実際に見つかった問題**: マウント失敗後に sb アドレスが再利用され、前回の inode がハッシュに残っていて UAF (seed 2/5)。失敗時に当該 sb の inode を強制破棄して修正。
- 修正後: seed 2,5,6,7 × 1500 回、seed 11–16 × 2000 回 (MFT 重点) で crash/ハングなし。
- 限界: 失敗したマウントの後始末で inode 内部の確保物 (runlist 等) がリークしうる (fuzz テストは detect_leaks=0)。画像が 64MiB でほぼ空のため、
  実使用規模・Windows 作成イメージ・圧縮/暗号化属性に対するカバレッジは限定的。コード網羅 (coverage) は未計測。

### 圧縮・巨大ボリューム・後始末 (2026-11-11)
- **LZNT1 圧縮**: ntfs-3g (`-o compression` + ディレクトリ属性 0x800) で圧縮ファイル (テキスト / 非圧縮性 / 圧縮可・不可・全ゼロ混在) を作成し、
  `fs/ntfs` 経由の読み出しを sha256 で照合。圧縮読み出しがブロックデバイス自身のページキャッシュ (`bd_mapping`) を使うため、その互換実装を追加。
  ASan/UBSan 通過。上流 C には `-fwrapv`、UBSan の alignment/shift-base 検査のみ除外 (上流は HAVE_EFFICIENT_UNALIGNED_ACCESS と -fno-strict-overflow 前提。互換層には適用しない)。
- **1TiB ボリューム** (`tools/ntfs_large_test.sh`): qcow2 + `nbdfuse -o allow_other` で、見かけ 1TiB / 実使用 ≈70MB の NTFS を tmpfs 上に作成。
  `ntfs-3g` は setuid root で動くため nbdfuse に `allow_other` が必要 (/etc/fuse.conf に user_allow_other)。同じ内容 (809 ファイル、圧縮・スパース含む) を ASan で照合し、
  破損 fuzz 300 回も通過。所要約 7 分 (ASan + FUSE) のため CTest には入れず手動実行。MFT ゾーン (≈1/8) の後ろの高位 LCN を実際に読む経路の検証になる。
- printk 互換 (`%pV` 展開) を実装し、エラーログが読めるようになった。
- 失敗したマウントの後始末: ページキャッシュ (folio) は解放するよう修正。**inode 内部の確保物 (MFT レコードのコピー、runlist) は失敗経路で残る** (解放には既に解放済みの vol が必要で、
  UAF になるため安全側で許容)。早期に失敗する非 NTFS データでは発生しない。
- **未検証のまま**: Windows が作成した NTFS (この環境に Windows がない)、暗号化 (EFS)、reparse/WOF 圧縮、システムの Documents UI。

### Windows が作成した NTFS での検証 (2026-11-11)
Phase 2 完了条件の「Windows で作成した NTFS イメージとの検証」を実施。Win11 25H2 インストール ISO の **WinPE を QEMU/KVM で起動**し、`autounattend.xml` の
`windowsPE` パス `RunSynchronous` から、同梱 CD のスクリプトを実行させて NTFS を作らせた (キー操作なし、約 100 秒/回)。ISO は同梱・配布しない。
手順: `tools/winpe/make_windows_fixtures.sh <Win11.iso>` (`make_payload.py` が内容と期待値 (sha256) をホスト側で生成、`run_vm.py` が VM を起動)。

| イメージ | 構成 |
|---|---|
| `win_mbr` | IDE、MBR、512B セクタ、4KiB クラスタ |
| `win_gpt64k` | IDE、GPT、64KiB クラスタ |
| `win_usb4kn` | **USB 接続 4Kn** (論理/物理セクタ 4096)、GPT、4KiB クラスタ |
| `win_dirty` | `fsutil dirty set` でダーティフラグ付き (`$Volume` flags = 0x0181 を生イメージで確認) |

内容 (全イメージ共通、1020 ファイル): 日本語名/ディレクトリ、254 文字の名前、空白・括弧入り名、5MiB+777B、**1000 件のディレクトリ**、
**Windows 自身の LZNT1 圧縮** (`compact /c`: テキスト 5.3:1 / 非圧縮性 1:1 / 混在 4:1)、**WOF 圧縮 (xpress4k / 8k / 16k / lzx)**、
10MiB 疎ファイル (`fsutil sparse`)、代替データストリーム (主ストリームを検証)、hidden/system/readonly 属性、シンボリックリンク・ジャンクション、削除済み MFT スロット。

**結果: 4 イメージとも manifest 全件 (サイズ + sha256) が一致、失敗 0。** ASan/UBSan・リーク検出あり。さらに各イメージに fuzz (計 6000 回) を当て、crash/ハングなし。
4Kn はデバイスの論理ブロックサイズを 512 / 4096 のどちらで開いても読めた。ダーティなボリュームも読み取り専用なら問題なくマウントできた。
CTest `ntfs_windows_images` (イメージが無ければ SKIP)。

観測した点:
- `win_gpt64k` で `$MFT and $MFTMirr record 24 do not match` の警告。64KiB クラスタでは上流が `$MFTMirr` を 1 クラスタ分 (64 レコード) 比較するが、Windows が同期するのは
  先頭 4 レコードだけで、後続は古い。**上流の偽陽性**で、読み取り専用では無害。書き込み有効化時に上流がこれを理由に読み取り専用へ落とす可能性があるので、Phase 3 の書き込みでは扱いを決める必要がある。
- 圧縮ファイルのクラスタ数: Windows は圧縮ユニットに満たない部分を疎にする。読み出しは問題なし。
- 未検証: EFS 暗号化 (WinPE では作れない)、BitLocker (Phase 4)、Windows が書き込み中に電源断した本物の「ログ未再生」ボリューム (ダーティフラグのみで $LogFile は静止状態)、巨大 (TB 級) ボリューム上の Windows 製データ。

### 暗号化 NTFS・Windows 11 本体で作ったボリューム (2026-11-11)
実 Windows 11 (Education 25H2, `w11` ツールの `--ephemeral` VM、ネットワークなし) でボリュームを作成: `tools/winvhd/make_encrypted_fixtures.sh` / ゲスト側 `guest_efs.ps1`, `guest_bitlocker.ps1`。
- **EFS**: 暗号化したファイル・ディレクトリ配下のファイル 3 件は、いずれも読み出しが **-EOPNOTSUPP (95) で安全に拒否**。平文ファイルは正常に読める。fuzz 200 回 crash/ハングなし。
  (EFS の作成は対話セッションの鍵が必要で、ssh 経由 (セッション 0) では「アクセス拒否」。`./w11 desk` で実行する。)
- **BitLocker** (XTS-AES-128、パスワード保護、`-UsedSpaceOnly`): パーティションは `FsType::BitLocker` と判別され、NTFS としてはマウント対象外。復号は Phase 4。
- **LogFile v2.0 (訂正あり)**: ダーティのままデタッチした Windows 11 製イメージでは `$LogFile` の restart page が **v2.0** で、上流 (1.1 のみ対応) が書き込みマウントを自動的に読み取り専用に落とす
  (読み取りは全データ可、イメージ無変更)。当初「最近の Windows 11 でフォーマットした USB には書き込めない」と書いたが**誤り**: Windows は**クリーンにディスマウントすると restart page を v1.1 に戻し**、
  その状態のボリュームには `ntfs-3g` も自前ドライバも書き込めた (書いた範囲の内容と `ntfsfix -n` を確認)。v2.0 が問題になるのはダーティ (未再生ログあり) のボリュームだけで、
  そこは元々書き込まない方針。`ntfs-3g` もダーティなこのイメージでは「unclean file system, refused to mount」と読み取り専用に落ちる。
