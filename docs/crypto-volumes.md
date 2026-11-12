# 暗号化ボリューム (BitLocker / LUKS1 / LUKS2) の読み取り — 設計・検証・制限

状態: 実装済み (読み取り専用)。UI なし (別担当)。`core/` の `BlockTransform` 層 + C API + JNI + Kotlin ラッパ。

```
BlockDevice (生の USB / パーティション / イメージ)
   └─ open_crypto_volume()      core/include/usbmem/transform.h
        └─ 復号済みの読み取り専用 BlockDevice ─→ probe_filesystem / mount_auto (NTFS・ext2/4・FAT …)
```

## 1. 方針と実装の選択 (ライセンス・移植性の比較と決定)

| 候補 | ライセンス | 内容 | 判断 |
|---|---|---|---|
| libbde | LGPL-3.0-or-later | BitLocker 読み取りの参照実装。libcdata/libcerror 等 20 前後のサブライブラリ依存 | **リンクしない**。外部の独立オラクル (`bdeinfo` / `bdemount`) としてテストだけに使う |
| dislocker | GPL-2.0-or-later | BitLocker 復号。FUSE 前提で Android では不可。OpenSSL / mbedTLS 依存 | 採用しない (GPL を新たに増やさない・FUSE 不可) |
| cryptsetup / libcrypt​setup | GPL-2.0-or-later (ライブラリは LGPL-2.1+) | LUKS。dm-crypt (カーネル) と root 前提、OpenSSL/gcrypt/nettle 依存 | **リンクしない**。`luksFormat` / `luksDump` でフィクスチャを作る正解側に使う |
| MbedTLS | Apache-2.0 | 汎用暗号。AES/SHA/PBKDF2 はあるが **Argon2 は無く**、AES-XTS は版によって有無が違う (同梱する版を固定する手間も増える) | 見送り: 足りない部分を自前で書くことになる |
| OpenSSL | Apache-2.0 (3.x) | 全部あるが NDK に無く、同梱ビルドが重い | 使わない (要件) |
| **自前実装 (採用)** | Apache-2.0 (本プロジェクト) | AES・SHA-1/256/512・HMAC・PBKDF2・AES-CBC/XTS/CCM・BLAKE2b・Argon2(d/i/id) を約 1,500 行で | 既知値テスト (KAT) と独立実装との突き合わせで担保 |

決定: **暗号プリミティブとフォーマット解析はすべて自前 (Apache-2.0)**。新しい依存は増えない (`docs/licenses.md` に追記)。
libbde / cryptsetup / nbdkit / qemu-img は「正解を知っている別実装」としてテストにだけ使い、配布物にもリンクにも入れない。
リスク: 自前暗号は実装誤りが怖いので、(a) 公開 KAT、(b) Python `hashlib` / `cryptography` / `argon2-cffi` / OpenSSL による生成ベクタ (`tools/gen_crypto_vectors.py`)、(c) 実フィクスチャを別実装で復号した結果とのバイト比較、の三重で確認した。
**サイドチャネル**: テーブル引き AES はキャッシュタイミング攻撃に対して定数時間ではない。本用途 (自分の USB を自分の端末で復号) の脅威モデルでは許容し、docs に明記する。

## 2. 共通層 (`transform.h` / `transform.cpp`)

- 書き込みは常に `Errc::ReadOnly`。`flush` は何もしない。`is_read_only() == true`。
- 復号済みキャッシュは 32 KiB チャンクの LRU (既定 32 個)。32 KiB 以上の読み出しはキャッシュを通さず直接復号。
- 鍵・パスワードのコピー・キャッシュはすべて `crypto::Secret` (破棄でゼロ化) か `Aes` (`clear()`・デストラクタでラウンド鍵をゼロ化)。`close()` で鍵とキャッシュを先にゼロ化し、以後の I/O は `Closed`。
- ログ・`describe()`・`VolumeInfo::note` に秘密を出さない。
- 資格情報違いは `Errc::AuthFailed (12)`。非対応の構成は `Errc::Unsupported (9)` + 理由 (note)。壊れたヘッダは `Corrupt`。
- サイズ・個数・KDF の計算量は上限付き (`OpenOptions`): Argon2 メモリ既定 1 GiB / PBKDF2 反復 2000 万 / Argon2 時間コスト 64 / 鍵 ≤ 128 B / stripes ≤ 10000 / JSON 深さ・ノード数 / LUKS2 ヘッダ ≤ 4 MiB / BitLocker メタデータ ≤ 64 KiB・エントリ ≤ 256。上限を超える keyslot は試さない。

## 3. BitLocker (Windows 7 以降の FVE)

- 検出: 先頭セクタ `-FVE-FS-`。メタデータ 3 コピー (ブートセクタ +0xB0 の 3 つのオフセット) を読み、内容が同じコピーは 1 つにまとめる。壊れたコピーは飛ばして残りで読む。
- 暗号方式: **AES-XTS-128/256 (0x8004/0x8005)**、**AES-CBC-128/256 (0x8002/0x8003)**、**AES-CBC + Elephant diffuser 128/256 (0x8000/0x8001; Windows 7/8 方式)**。
- 保護 (VMK): パスワード、48 桁の回復パスワード、スタートアップキー (.BEK)、クリアキー (保護一時停止中は資格情報なしで開く)。パスワードは `SHA256(SHA256(UTF-16LE))`、回復パスワードは 8 グループ ÷ 11 → 16 バイト → `SHA256`、どちらも 0x100000 回の SHA-256 ストレッチ。VMK / FVEK は AES-CCM (12 バイト nonce・16 バイトタグ)。ストレッチ鍵エントリのメソッドは 0x1001 (パスワード) と 0x1000 (実 Windows 11 が回復パスワードに使う) の両方を受け付ける。
- データ: XTS の tweak = 物理セクタ番号。ボリューム先頭の 16 セクタ (NTFS ブートセクタ) は別の位置に退避されており、**退避先の物理位置を tweak にして**復号して先頭へ戻す。メタデータ領域と退避領域はゼロ埋めで見せる (libbde と同じ)。UsedSpaceOnly の未暗号化領域は暗号化されていないのでそのまま読む。ボリューム長はメタデータの「暗号化済みサイズ」でパーティション余りを切り落とす。
- 拒否/安全側: **TPM のみ**の保護 → `Unsupported` (note「TPM のみで保護されています。このアプリでは解錠できません」。回復パスワードがあれば入力できる旨)。**暗号化/復号が途中** → 既定は `Unsupported`。`allow_partial_encryption` で変換済み領域だけ復号して開く。パスワード/回復キー/BEK 違いは `AuthFailed`。入力形式が不正 (48 桁でない等) は `InvalidArgument`。BitLocker To Go (Vista 方式)・Win Vista の FVE は非対応。セクタサイズはボリュームの値を使う作りだが、4Kn (セクタ 4096) は**実フィクスチャ未検証**。
- 非対応: Windows Vista 形式 (メタデータ形式が違う)、eDrive / ハードウェア暗号 (OPAL)、BitLocker To Go のスマートカード / 証明書保護。

## 4. LUKS1 / LUKS2

- LUKS1: 592 バイト BE ヘッダ、8 keyslot、AF-split (anti-forensic) のマージ、mk-digest の PBKDF2 検証。
- LUKS2: 4096 バイトのバイナリヘッダ + JSON、**2 コピー (先頭と hdr_size 位置)** のチェックサム (sha256, フィールド 448 をゼロにして計算) 検査と seqid の新しい方の優先 (片方が壊れていればもう一方)、keyslot の `af`/`area`/`kdf` (pbkdf2 / argon2i / argon2id)、digest (pbkdf2)、segment (crypt)。
- 暗号: `aes-xts-plain64` (最優先), `aes-xts-plain`, `aes-cbc-plain64`, `aes-cbc-plain`, `aes-cbc-essiv:sha256`。IV は dm-crypt 同様 **512 バイト単位の番号** (LUKS2 の 4096 セクタでも)。
- 拒否 (すべて理由つき `Unsupported`): serpent / twofish 等の他の暗号、dm-integrity (`integrity`)、再暗号化途中 (`requirements`)、複数 segment、分離ヘッダ (detached header: 専用のオープン経路がなく、ヘッダを渡す API を作っていない)、未知の必須要件。
- keyslot の priority 0 (ignore) は試さない。複数 keyslot は優先度順に試し、パスフレーズ / キーファイルのどれが合うかを返す。
- Argon2 は 1 レーンずつ逐次実行 (並列化なし。ARM でも決定的で単純)。メモリ上限超えは試さず `Unsupported`。

## 5. API

- C: `core/include/usbmem/c_api_transform.h` — `usbmem_transform_probe(dev, &json)`、`usbmem_transform_open(dev, kind, password…, recovery…, key_file…, flags, &out, &info_json)`。返る `usbmem_device*` は `usbmem_close` で閉じる (元の `dev` より先に)。資格情報はバッファからコピーして使い、関数を出る前にゼロ化する。
- JNI: `android/app/src/main/cpp/jni_transform.cpp` (`CryptoNative.nativeProbe / nativeOpen`)。資格情報は `byte[]` (UTF-8)、ネイティブ側のコピーは関数を出る時にゼロ化。
- Kotlin: `CryptoVolume.kt` — `CryptoVolume.probe(handle): CryptoInfo?`、`CryptoVolume.open(handle, kind, password, recoveryKey, keyFile, allowPartialEncryption): Long`。渡された `ByteArray` は呼び出し後に `fill(0)`。失敗は `CryptoException(code)` (`authFailed` / `unsupported`)。得たハンドルを既存の `nativeNtfsMountDevice` 等へ渡す。
- 既存の共有ファイルの変更は最小: `android/app/src/main/cpp/CMakeLists.txt` に `jni_transform.cpp` と `usbmem_transform` の追加 2 箇所、`core/CMakeLists.txt` に `include(transform.cmake)` 1 行。
- **UI は未実装**。パスワード入力画面・Keystore 連携は別担当。

### Android Keystore (設計メモのみ・未実装)
資格情報の保存は**既定で行わない** (毎回入力)。利便性のために記憶させるなら: AndroidKeyStore に AES-256-GCM の鍵を `setUserAuthenticationRequired(true)` (生体/画面ロック) 付きで作り、パスフレーズをその鍵で暗号化してアプリ内ストレージに置く。ボリュームごとのキーは UUID (LUKS) / ボリューム GUID (BitLocker) で引く。VMK/マスターキー自体は保存しない (保存するのはパスフレーズだけ。盗まれてもデバイスと生体が無いと使えない)。`setIsStrongBoxBacked` は使えれば使う。復号後のキーを JVM に渡さない (C++ 内で完結) 設計は現状のまま維持する。

## 6. 検証結果

### 6.1 フィクスチャの由来 (実物か合成かを区別する)

| 区分 | 内容 | 作り方 |
|---|---|---|
| **実 Windows 11 製 BitLocker** | `win_bitlocker` (XTS-128・パスワード・hello.txt のみ)、`bl_xts256full` (XTS-256・UsedSpaceOnly・パスワード+回復パスワード)、`bl_cbc128` (AES-CBC-128・同上)、`bl_cbc256` (AES-CBC-256・同上)、`bl_startkey` (XTS-128・スタートアップキー+パスワード、実 .BEK)、`bl_suspended` (XTS-128・保護一時停止=クリアキー)、`bl_partial` (全領域暗号化を途中で止めたもの) | Windows 11 の VM で `tools/winvhd/make_bitlocker_matrix.sh` (VM は 1 台・`VM_DISPLAY=none`・終了後に停止確認済み)。167 ファイル・9 ディレクトリ (1 B〜20 MiB の乱数ファイル、150 個の小ファイル、日本語名、深い階層、空ファイル) を作ってから暗号化し、**Windows 側で `Get-FileHash` した SHA-256 の一覧 (manifest)** を付ける |
| **実イメージからの暗号方式差し替え (合成)** | AES-CBC + Elephant diffuser (0x8000/0x8001) と、XTS/CBC の別方式での確認 | `tools/bitlocker_retarget.py`: 実 Windows イメージのレイアウト (ブートセクタ・メタデータ・VMK) をそのままに、FVEK だけ新しい乱数にして全データを別方式で暗号化し直す。**Windows 7/8 実機の出力ではない**。正しさは独立実装 libbde が復号できることで確認 |
| 完全合成 (参考) | `tools/bitlocker_synth.py` | libbde が「ボリュームヘッダ検証」で拒否する原因を切り分けきれなかったため、**この合成方式は検証に使っていない**(上の retarget に切り替えた)。ツールは未解決の参考として残す |
| LUKS | 35 ケース | 実物の `cryptsetup luksFormat` (LUKS1/2、pbkdf2/argon2i/argon2id、各暗号、複数 keyslot、キーファイル) で作ったヘッダ。データ領域はヘッダのマスターキーで自前に暗号化し、`luksDump` / `--test-passphrase` でヘッダを検証。LUKS1 のペイロードは nbdkit luks フィルタ・qemu-img の luks ドライバ (essiv) でも独立に復号して一致を確認。破損/非対応ケースは実ヘッダを書き換えて作成 |

### 6.2 結果

- 既知値テスト (KAT): ハッシュ 51・HMAC 24・PBKDF2 8・AES 9・CBC 6・XTS 32・CCM 16・BLAKE2b 24・Argon2 24 (`ctest crypto_unit`)。
- **BitLocker (実 Windows 製 7 イメージ)**: パスワード / 回復パスワード / .BEK / クリアキーのそれぞれで開き、**(a) 復号デバイスを `bdemount` (libbde) の出力と全バイト比較 → 全イメージ一致、(b) NTFS (LinuxCompat 版 fs/ntfs) でマウントして Windows 側 manifest と照合 → 167 ファイル・9 ディレクトリ、SHA-256 不一致 0**。違うパスワード → `AuthFailed`。途中暗号化 (`bl_partial`) → `Unsupported`。
- **TPM のみ** (実イメージの VMK をTPM 保護に差し替えた合成): `tpm_only=1`・`Unsupported` + 理由。回復パスワードを渡しても同じ (その保護が無いので)。
- **暗号化途中の BitLocker (合成: 実イメージを 50% 変換済み・状態 2/4 に作り替え)**: 既定で `Unsupported`、`allow_partial_encryption` で開くと libbde の `bdemount` 出力と全バイト一致 (先頭 267321344 バイト。ただし暗号化途中のときはパーティション余りを切り落とさないので、デバイス長は元のパーティション全体になる)。
- **暗号方式マトリクス (retarget、libbde との全バイト比較)**: 0x8000, 0x8001 (diffuser), 0x8002, 0x8003 (CBC), 0x8004, 0x8005 (XTS) × {パスワード, 回復パスワード, クリアキー} = 全一致。
- **LUKS 35 ケース**: 解錠 → 平文イメージ (ext2/ext4) と全バイト比較一致、FS をマウントして全ツリー比較一致、違うパスフレーズ `AuthFailed`、2 本目の keyslot (キーファイル)、ヘッダ片側破損 → もう一方で解錠、両側破損 `Corrupt`、digest 破損 `AuthFailed`、serpent / dm-integrity / 再暗号化 / 複数 segment は理由つき `Unsupported`。
- **破壊・切り詰め**: ヘッダ/メタデータの破壊 fuzz (BitLocker 400 回、LUKS2 argon2id 600 回、LUKS2 essiv 800 回、LUKS1 1500 回) と、あらゆる長さでの切り詰め (BitLocker 38 通り・LUKS 36〜37 通り)。クラッシュ・ハング・無制限確保なし。**fuzz で LUKS1 の keyslot の PBKDF2 反復回数に上限が掛かっておらず、反復回数を 12 億に書き換えられると数分固まることを発見 → 修正済み (上限超過の keyslot は試さない)**。
- **鍵・平文のゼロ化**: `close()` 後にヒープに復号済み平文の 32 バイト目印が残らないことをヒープ走査で確認 (BitLocker・LUKS とも「閉じる前 1 件 → 閉じた後 0 件」)。`Secret` / `Aes` のゼロ化は単体テスト (`crypto_secret_zeroization`)。
- ASan/UBSan: 単体テスト 9 件、フィクスチャ検証 (LUKS 35 ケース全部 + BitLocker 実イメージ 2 つ。簡易モード = Debug+サニタイザは BitLocker のストレッチが遅いため)、fuzz (LUKS1 300 回・LUKS2 argon2id 150 回・BitLocker 25 回) を実行、検出なし。fuzz の回数が多いのは Release 版 (上の行)。

### 6.3 性能 (x86-64, Release, 単一スレッド, 参考値)

| 処理 | 速度 |
|---|---|
| AES-XTS-256 復号 (単体) | 117 MB/s |
| AES-XTS (BitLocker・ベースの読み出し込み) | 92 MB/s |
| AES-CBC-128 (BitLocker) | 172 MB/s |
| LUKS2 aes-xts-plain64 / aes-cbc-essiv | 95 / 111 MB/s |
| SHA-256 | 150 MB/s |
| BitLocker パスワード/回復パスワードのストレッチ (SHA-256 × 0x100000) | 約 1.0 s |
| LUKS2 argon2id (64 MiB・2 パス・4 レーン、逐次) | 約 0.5 s |
| LUKS PBKDF2 (1000 反復) | 約 0.01 s |

**ARM での実測はしていない** (実機に触れない指示のため、ARM のエミュレータ/実機測定は未実施)。テーブル引きの素の C++ なので、ARM64 は一般に x86-64 デスクトップの 1/2〜1/4 程度になると見積もられ、BitLocker のストレッチは 2〜4 秒、XTS 復号は 25〜60 MB/s 程度が目安 (推定であり実測ではない)。USB3 のHDD/SSD の速度を律速しうるので、実機で測って必要なら ARMv8 Crypto Extensions (`vaeseq_u8`) を使った高速パスを追加する (現状は未実装)。Argon2 のメモリ上限は端末の RAM に合わせて `OpenOptions::max_kdf_memory_kib` で下げること (既定 1 GiB は PC 向け)。

## 7. 制限・未解決 (正直に)

- **Elephant diffuser (0x8000/0x8001)**: 実 Windows 7/8 のボリュームは手元になく、**実機製では未検証**。独立実装 libbde が復号できる形に作り替えた合成イメージとのバイト一致だけを根拠にしている。AES-128 + diffuser の鍵データは 64 バイト (FVEK 16 + 詰め物 + tweak 16 + 詰め物) で、32 バイトのみのときは 16+16 と解釈する — 実機の配置は libbde の解釈に依る。
- **BitLocker 4Kn / 大きなセクタ**、**BitLocker To Go (スマートカード・証明書)**、**Vista 形式**、**TPM+PIN / TPM+スタートアップキー** (TPM の封印を解けない) は非対応。
- **暗号化途中の BitLocker**: 既定は拒否。`allow_partial_encryption` の経路は合成イメージで libbde と一致を確認したが、**実 Windows が途中で止めたイメージでは検証できていない** (手元の `bl_partial` は先頭セクタの状態が libbde でも開けない形)。
- **LUKS2 の 4096 バイトセクタの IV**: 実カーネル (dm-crypt) での照合ができていない (root・カーネルモジュールが使えない環境)。cryptsetup の仕様どおり「IV = 512 バイト単位の番号」で実装し、cryptsetup 生成のヘッダ + 自前暗号データで往復するだけ。**ここは未検証の仮定**。LUKS1 は nbdkit / qemu-img でデータ層まで独立確認済み。
- LUKS: serpent / twofish、dm-integrity、再暗号化途中、分離ヘッダ、複数 segment は拒否 (要件どおり明示的に拒否する)。`aes-xts-plain64` 以外の `adiantum` / `lmk` / `tcw` / `eboiv` 等も非対応。
- 暗号化されたボリュームを**書き込む**機能はない (読み取り専用)。
- NTFS ドライバ (LinuxCompat 版 fs/ntfs, v1.1 ログ) は Windows 11 の NTFS ログ (LogFile 2.0) を「サポートしない」と警告するが、読み取りは成功し manifest は全件一致した (警告は読み取りには影響しないが、書き込みを有効にする将来は要確認)。
- JVM 側の `ByteArray` はゼロ埋めするが、GC のコピーやスワップに残る可能性は排除できない。パスワードを `String` にしない設計で露出を減らしている。
- 副産物: `main` の `core/compat/include/compat/ops.h` に `void *direct_IO;` の重複メンバがありビルドが通らなかったため、重複 1 行を削除した (本機能とは無関係の既存不具合)。

## 8. 再現手順

```sh
# 単体テスト (KAT + 共通層)
cmake -S core -B build/t -DUSBMEM_NTFS=ON && cmake --build build/t -j4 && ctest --test-dir build/t -R "crypto_unit|crypto_volume"
# LUKS フィクスチャ (cryptsetup が要る) → /dev/shm/usbmem_crypt_$USER/luks
python3 -I tools/gen_luks_fixtures.py /dev/shm/usbmem_crypt_$USER/luks
# BitLocker フィクスチャ (Windows 11 VM が要る)
tools/winvhd/make_bitlocker_matrix.sh /dev/shm/usbmem_crypt_$USER/winbl <w11 のパス> [variant …]
# 全部の結合検証 (libbde-utils があれば bdemount 比較も)
tools/run_crypto_volume_tests.sh build/t/usbmem_cryptovol_tests
# 暗号方式差し替え (Windows 7/8 方式を含む) と libbde との全バイト比較
tools/bitlocker_retarget_matrix.sh build/t/usbmem_cryptovol_tests <実 BitLocker イメージ> <パーティション先頭> <パスワード>
```
