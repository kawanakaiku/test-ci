# 暗号化ボリューム (BitLocker / LUKS1 / LUKS2) の読み取りと書き込み — 設計・検証・制限

状態: 読み取りは実装・検証済み。**書き込みは実験的に実装 (既定は無効。9 章)**。UI なし (別担当)。`core/` の `BlockTransform` 層 + C API + JNI + Kotlin ラッパ。

```
BlockDevice (生の USB / パーティション / イメージ)
   └─ open_crypto_volume()      core/include/usbmem/transform.h
        └─ 復号済みの BlockDevice (既定は読み取り専用。書き込みは 9 章の条件を全部満たしたときだけ) ─→ probe_filesystem / mount_auto (NTFS・ext2/4・FAT …)
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

- 書き込みは既定で `Errc::ReadOnly`、`flush` は何もしない、`is_read_only() == true`。書き込みを許すのは 9 章の条件 (ビルドフラグ `USBMEM_CRYPT_WRITE` + open 時の明示 `OpenOptions::writable` + 構成が対応 + 下位が書き込み可) を全部満たしたときだけ。
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
| 完全合成 | `tools/bitlocker_synth.py` | 2026-11-13 に**原因を特定して修正**: 「暗号化済みボリュームのサイズ」(メタデータのブロックヘッダ +16) が、ボリュームヘッダ退避領域を含まない長さ(平文長)になっていた。実 Windows のイメージでは退避領域が暗号化済みサイズの内側にあり、libbde も自前のリーダーも「退避領域が範囲内」を検査するため、どちらも拒否していた(自前: `corrupt: ボリュームヘッダの退避位置が不正です`)。完了時は全長(`total`)を書くよう直し、`bdeinfo` が開け、自前のリーダーでも解錠・libbde `bdemount` の出力と全バイト一致、NTFS を全ツリー比較で一致を確認した。リーダー側の検査は実 Windows 製に合っており修正不要。実 Windows 製・retarget 方式の検証は従来どおり。 |
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

**ARM での実測はしていない** (実機に触れない指示のため、ARM のエミュレータ/実機測定は未実施)。上の表は 6.4 の高速化の**前**の値 (表引き実装)。6.4 で AES / SHA-256 / CRC32C にハードウェア命令の経路を足したので、x86-64 ではこの表の数倍になる。ARM64 の命令版 (Crypto Extensions) はコードを書いて aarch64 向けに構文・命令選択まで確認したが、**手元でも CI でも実行はしていない** (CI の arm64 エミュレータで自己テストが通れば有効になる。通らなければ表引きに戻る)。Argon2 のメモリ上限は端末の RAM に合わせて `OpenOptions::max_kdf_memory_kib` で下げること (既定 1 GiB は PC 向け)。

### 6.4 実機での遅さ (BitLocker 0.5 MB/s・LUKS2 4.5 MB/s・Btrfs 9 MB/s) の原因調査と対策 (2026-11-14)

J9260 (USB 2.0 級) の実機測定で、BitLocker 約 0.5 MB/s、LUKS2 約 4.5 MB/s、Btrfs (小さいファイル・大きいファイルとも) 約 9 MB/s と遅かった (同じ USB の XFS は 28.8 MB/s)。**実機での再測定は未実施**。以下は「往復遅延 1 ms・帯域 30 MB/s・1 転送最大 512 KiB の模擬デバイス」を挟んだホストのマイクロベンチ (`core/tests/bench_transform.cpp`、`usbmem_transform_bench`) での切り分けと対策の結果で、実機の絶対値ではない。

**原因 (大きい順)**

1. **実機試験用 APK が `assembleDebug` で、ネイティブコードが最適化なし (-O0) でビルドされていた。** AGP の Debug は `CMAKE_BUILD_TYPE=Debug` かつ `CMAKE_CXX_FLAGS_DEBUG` が空で、`-O` が付かない (clang は -O0)。AES・SHA-256・Argon2・CRC32C・各 FS のコードがすべて 5〜8 倍遅くなる。ホストで同じ条件を再現すると、AES-XTS-256 復号は 23 MB/s (-O2 で 104 MB/s)、SHA-256 は 30 MB/s (同 146 MB/s)、BitLocker の鍵伸長 (SHA-256 × 2^20) を含む解錠は CPU 4.2 s (同 1.1 s)。ARM の実機はこれより遅いので、0.5 MB/s までの低下は -O0 + 小さい読みの増幅 (下の 2) で説明がつく。
2. **`DecryptingDevice` の 32 KiB チャンクキャッシュが小さい読みを 8 倍に増幅し、連続読みの検出もなかった。** 4 KiB を読むたびに 32 KiB を読んで復号していた (ランダム 4 KiB × 500 で下位 16 MB を転送・復号)。
3. **ハードウェア命令を使っていなかった。** AES・SHA-256 は表引きの C++ のみ。Btrfs のデータ検証用 CRC32C は 1 バイトずつの表引き (279 MB/s) で、XFS の slicing-by-8 (約 1.2 GB/s) より遅い。-O0 だとさらに数倍遅く、Btrfs 9 MB/s と XFS 28.8 MB/s の差の主因 (ほかの FS 共通の下位 I/O は同じ)。

**原因ではなかったもの**: 解錠時の下位 I/O (BitLocker 20 回・LUKS2 12 回、合計 20 ms 程度)、KDF の繰り返し (1 回だけ)。Btrfs の小ファイルは `mkfs --rootdir` で物理的にほぼ連続に置かれるため、既存の `ReadAheadDevice` で足りており (2400 ファイル 71 MB が下位 700 回、模擬デバイスで 20.6 MB/s)、先読みの追加は不要だった。

**対策**

- `android/app/src/main/cpp/CMakeLists.txt`: Debug (最適化指定なし) のときも `-O2` を付ける (`-DUSBMEM_DEBUG_NO_OPT=ON` で外せる。Release 系は従来どおり)。
- `DecryptingDevice` (`transform.cpp`): 4 KiB ページ単位のキャッシュ (ハッシュ表 + LRU、既定 4 MiB、メモリは `Secret` で閉じるときにゼロ化) に変更。連続読みは 4 本まで追跡し、16 KiB から 256 KiB まで倍々で先読みする。ランダムな 4 KiB 読みは 4 KiB しか読まない。32 KiB 以上でセクタ境界の読みはキャッシュを通さず直接復号する (最大 1 MiB ずつ)。
- AES (`crypto_aes_hw.cpp`): x86 は AES-NI、aarch64 は Crypto Extensions。ECB / CBC 復号 / XTS を 4 ブロック並列。復号は等価逆暗号の鍵スケジュール。実行時に CPU を判定し (x86 は cpuid、aarch64 Linux は `getauxval(AT_HWCAP)`)、**初回に FIPS-197 の既知答テストと表引き実装との一致確認 (AES-128/192/256 × ECB/CBC/XTS)** を行い、ひとつでも違えば表引きに戻る (フェイルクローズ)。
- SHA-256 (`crypto_sha_hw.cpp`): x86 は SHA-NI、aarch64 は SHA2 命令。同様に自己テスト付き。BitLocker の鍵伸長 (`stretch_key`) は圧縮関数を直接 2 ブロックずつ回す形にして、毎回の `HashCtx` 生成・パディングを省いた。
- CRC32C (`crc32.cpp`、Btrfs): x86 は SSE4.2 `crc32`、aarch64 は CRC32 命令、どちらもなければ slicing-by-8。自己テスト付き。`btrfs_volume.cpp` の 1 バイトずつの表引きを置き換えた。
- どの経路も関数単位の `target` 属性で、プロジェクト全体に `-maes` 等は付けない (対応していない CPU でも起動できる)。`crypto::Aes::set_hw_enabled(false)` / `sha256_set_hw_enabled(false)` で表引きに固定でき、テストが両者の一致を比べる。

**ホストのマイクロベンチ (x86-64、単一スレッド。-O0 は Debug の再現、-O2 は変更前の Release、後は変更後の Release)**

| 項目 | -O0 (変更前) | -O2 (変更前) | 変更後 (-O2 + 命令) |
|---|---|---|---|
| AES-XTS-256 復号 (512 B セクタ) | 23.0 MB/s | 104.2 MB/s | 993 MB/s (表引き強制 126 MB/s) |
| AES-CBC-128 復号 | 29.7 MB/s | 167.7 MB/s | 3968 MB/s |
| SHA-256 | 30.4 MB/s | 145.9 MB/s | 832 MB/s (表引き強制 153 MB/s) |
| PBKDF2-SHA256 20 万反復 | 0.993 s | 0.270 s | 0.110 s |
| CRC32C (Btrfs のデータ検証) | 未測定 (1 バイト表引きの -O2 は 279 MB/s) | 279 MB/s | 5802 MB/s (命令)、1199 MB/s (slicing-by-8) |
| BitLocker 解錠 CPU (鍵伸長込み) | 4.17 s | 1.14 s | 0.18 s |
| LUKS2 解錠 CPU (Argon2id 64 MiB) | 2.28 s | 0.57 s | 0.62 s (Argon2 が支配的で変化なし) |

模擬デバイス経由の読み取り (BitLocker。LUKS2 もほぼ同じ。MB/s は要求した量 / (CPU + I/O 時間)):

| 読み方 | -O0 (変更前) | -O2 (変更前) | 変更後 |
|---|---|---|---|
| 連続 1 MiB 要求 | 12.3 | 22.9 | 27.4 |
| 連続 64 KiB 要求 | 10.8 | 17.5 | 20.1 |
| 連続 4 KiB 要求 (4 MiB) | 9.2 (下位 129 回) | 13.7 (129 回) | 23.4 (22 回) |
| ランダム 4 KiB × 500 | 1.12 (下位 16.3 MB) | 1.72 | 3.61 (下位 2.0 MB) |
| 小ファイル (4K メタ + 4K × 8) | 3.58 | 5.35 | 6.20 |

- 変更後は CPU 時間がほぼ消え (連続 1 MiB で 0.77 s → 0.02 s)、残りは模擬した USB の遅延・帯域で、これは下位デバイスの限界。USB 2.0 級の実機でも、読み取り速度は復号ではなく USB の転送速度で決まるはず。
- Btrfs (全ファイル読み、2401 ファイル 71 MB、同じ模擬デバイス): -O0 は CPU 0.477 s で 18.3 MB/s、変更後は CPU 0.043 s で 20.6 MB/s。模擬デバイスの帯域 (30 MB/s) と往復が律速なので差が小さく見えるが、**CPU 時間は 11 分の 1**。実機の 9 MB/s が CPU 側の遅さ (-O0 の CRC32C など) だったかどうかは、実機で再測定しないと確定しない。
- 先読み無し (`--no-ra`) だと同じ Btrfs 読みが下位 2997 回・12.8 MB/s に落ちる。`ReadAheadDevice` が効いていることの確認。

**検証**: `usbmem_transform_tests` (13 件、先読み・キャッシュの境界・セクタ 512/4096 のテストを追加)、`crypto_unit` (AES / SHA-256 / CRC32C の命令版と表引きの一致テストを追加)、`crypto_volume_fixtures` (実 Windows の BitLocker イメージ・LUKS 35 ケース・libbde との全バイト比較を含む) が通る。Btrfs の `btrfs_read` / `btrfs_restore_xcheck` / `btrfs_corrupt` / `btrfs_ioerr` / `btrfs_fuzz` も通る (CRC32C の置き換え後)。Android 側は CI で確認する。

**未了・限界**: 実機での再測定、aarch64 命令版の実行確認 (構文と命令選択のみ確認。x86 の自己テストと同じ内容が起動時に走るので、誤りがあれば表引きに落ちるだけで復号結果は壊れないが、速度は上がらない)、`ReadAheadDevice` の複数ストリーム対応、直接復号経路での復号と I/O のパイプライン化は未実施。ホストの ctest のうち `ntfs_gen_fixture` / `udf_gen_fixture` / `blockdev_fixtures` とその依存の計 12 件は、この環境のフィクスチャ生成 (mkntfs の排他オープン競合、sgdisk のハング) が原因で失敗しており、本変更とは無関係 (他のテストは全部通った)。

## 7. 制限・未解決 (正直に)

- **Elephant diffuser (0x8000/0x8001)**: 実 Windows 7/8 のボリュームは手元になく、**実機製では未検証**。独立実装 libbde が復号できる形に作り替えた合成イメージとのバイト一致だけを根拠にしている。AES-128 + diffuser の鍵データは 64 バイト (FVEK 16 + 詰め物 + tweak 16 + 詰め物) で、32 バイトのみのときは 16+16 と解釈する — 実機の配置は libbde の解釈に依る。
- **BitLocker 4Kn / 大きなセクタ**、**BitLocker To Go (スマートカード・証明書)**、**Vista 形式**、**TPM+PIN / TPM+スタートアップキー** (TPM の封印を解けない) は非対応。
- **暗号化途中の BitLocker**: 既定は拒否。`allow_partial_encryption` の経路は合成イメージで libbde と一致を確認したが、**実 Windows が途中で止めたイメージでは検証できていない** (手元の `bl_partial` は先頭セクタの状態が libbde でも開けない形)。
- **LUKS2 の 4096 バイトセクタの IV**: 実カーネル (dm-crypt) での照合ができていない (root・カーネルモジュールが使えない環境)。cryptsetup の仕様どおり「IV = 512 バイト単位の番号」で実装し、cryptsetup 生成のヘッダ + 自前暗号データで往復するだけ。**ここは未検証の仮定**。LUKS1 は nbdkit / qemu-img でデータ層まで独立確認済み。
- LUKS: serpent / twofish、dm-integrity、再暗号化途中、分離ヘッダ、複数 segment は拒否 (要件どおり明示的に拒否する)。`aes-xts-plain64` 以外の `adiantum` / `lmk` / `tcw` / `eboiv` 等も非対応。
- 書き込み (9 章) は実験的で、既定のビルドでは無効。UI は未接続、実機 (USB) では未試験。9.7 に制限を列挙。
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

書き込みの検証 (9 章。`USBMEM_CRYPT_WRITE=ON` かつ `USBMEM_NTFS=ON` `USBMEM_NTFS_WRITE=ON` のビルド。cryptsetup・python3 cryptography/numpy が要る。bdemount・fuse2fs・ntfs-3g・mkfs 類があれば追加で使う):

```sh
cmake -S core -B build/cw -G Ninja -DUSBMEM_NTFS=ON -DUSBMEM_NTFS_WRITE=ON && cmake --build build/cw --target usbmem_transform_tests usbmem_cryptwrite_tool usbmem_cryptfs_write_tests
build/cw/usbmem_transform_tests                                              # 単体テスト (transform_write_*)
tools/run_crypt_write_tests.sh build/cw/usbmem_cryptwrite_tool               # LUKS / BitLocker の乱数書き込み・失敗注入・ファジング (USBMEM_KERNEL_DMCRYPT=1 は sudo が要る CI 用)
tools/run_crypt_fs_tests.sh build/cw/usbmem_cryptfs_write_tests              # 暗号層の上の ext4 / ext2 / NTFS / FAT
tools/run_crypt_mutation_tests.sh /tmp/mut                                   # 突然変異 (10 種のビルド)
```

## 9. 書き込み (実験的。ビルドフラグ `USBMEM_CRYPT_WRITE` + open 時の明示が必要)

「書き込みは本質的に暗号化」という見立てはほぼ正しい。ヘッダ・メタデータは変えず、論理セクタ範囲を暗号化して下位デバイスへ書くだけで済む。難所は暗号そのものではなく、(a) 部分セクタの読み出し-変更-書き込み、(b) ページキャッシュとの整合、(c) BitLocker の退避ヘッダ・メタデータ領域・変換途中、(d) 失敗・電源断の扱いで、いずれも下に記す。

### 9.1 有効にする条件 (全部必要。1 つでも欠ければ読み取り専用で開く)

1. ビルド: `-DUSBMEM_CRYPT_WRITE=ON` (既定は `USBMEM_NTFS_WRITE` の値。配布ビルドは OFF)。OFF のビルドでは `write_supported=false`、書き込み要求は読み取り専用で開く。
2. open 時の明示: C API は `USBMEM_TRANSFORM_WRITABLE` (flags = 2)、Kotlin は `CryptoVolume.open(..., writable = true)` (既定 false)、C++ は `OpenOptions::writable`。既定 false。
3. 構成が対応 (`VolumeInfo::write_supported`)。9.2 の表。
4. 下位の `BlockDevice` が書き込み可 (`is_read_only() == false`)。

(1)-(4) のどれかで書き込めないときは、エラーにせず**読み取り専用で開き**、`VolumeInfo::writable=false` と理由 `write_note` を返す (FS の `mount_rw` が読み取り専用に落ちるのと同じ流儀)。JSON (`usbmem_transform_open` の info) は `write_supported` / `writable` / `write_note` を持つ。UI は未接続。

### 9.2 方式 × 書き込み可否

| 形式 | 方式 | 書き込み | 根拠 / 理由 |
|---|---|---|---|
| LUKS1 / LUKS2 | aes-xts-plain64 (256/512 bit 鍵、論理セクタ 512 / 4096) | 可 | cryptsetup のマスターキー + Python cryptography、CI は実カーネル dm-crypt と一致 |
| LUKS1 / LUKS2 | aes-cbc-plain64、aes-cbc-essiv:sha256 | 可 | 同上 |
| LUKS1 | aes-xts-plain、aes-cbc-plain (32 ビット IV) | 不可 | 読み取りは対応だが書き込みの検証 (カーネルとの照合) をしていない。変更なし |
| LUKS2 | dm-integrity / AEAD、再暗号化の途中、複数 segment、未対応の暗号 | 不可 | 読み取りも非対応 (Unsupported) |
| BitLocker | XTS-AES-128/256、CBC-128/256 (Windows 8 以降 / 7) | 可 (論理セクタ 512 のみ) | libbde と Python (bitlocker_synth.py) でバイト一致 |
| BitLocker | CBC + Elephant diffuser (0x8000 / 0x8001) | 可 (注意あり) | 暗号化方向を実装し、libbde と bitlocker_synth.py の両方とバイト一致。**実 Windows 7/8 のイメージでは未確認** (7 章と同じ制限) |
| BitLocker | 変換途中 (暗号化 / 復号の途中) | 不可 | 境界が動き、未変換領域は平文のまま。`allow_partial_encryption` でも書き込まない |
| BitLocker | 論理セクタ 4096 (4Kn)、TPM のみ、メタデータ不一致 | 不可 | 4Kn は検証なし。TPM のみは読み取りも非対応。メタデータ 3 コピーが全部読めて一致していないとき、退避位置が信用できない (壊れた BitLocker の上に書かない。fuzz で見つかった) |

### 9.3 書き込み経路 (`DecryptingDevice::write_at`)

- **書き込みはその場で下位へ (write-through)**。遅延書き込みはない。要求を 1 MiB 以下・暗号セクタ境界で切って処理する。
- **部分セクタ**: 先頭・末尾が暗号セクタ (512 / 4096) に揃っていなければ、そのセクタを `read_plain` で復号して取り込み、新データを重ねて暗号化し、セクタ全体を書く (RMW)。読めなければ書かない。
- **キャッシュとの整合**: 4 KiB ページの平文キャッシュと先読みはそのまま使う。書いた範囲は成功後にキャッシュを**書き換え**、失敗時は**捨てる**。`io_` (shared_mutex) で読み出し (共有) と書き込み・flush (排他) を分け、読み出しが古い平文をキャッシュに入れ直すのを防ぐ。32 KiB 以上の読み出し (直接復号) も同じ `io_` の下。
- **flush**: 書き込み可なら下位の `flush()` へ素通し (FS のジャーナル順序・バリアは下位まで届く)。`close()` も最後に下位を flush する。
- **失敗**: 下位の write / flush が失敗したら、そのエラーを返し、`is_read_only()` を true にして以後の書き込みを `ReadOnly` で拒否する (読み出しは続く)。`flush()` は最初のエラーを返し続ける。範囲外・禁止範囲 (BitLocker のメタデータ領域など) の書き込みは**何も書かずに** `ReadOnly` で拒否し、読み取り専用には落とさない (要求全体を先に検査)。
- **電源断**: 暗号セクタ単位の書き込みは、下位の書き込み単位 (512 バイト) より大きいとき (LUKS2 の 4096 セクタを 512 バイト単位のデバイスに書く等) 途中で切れるとセクタ内が新旧混在し、復号すると**そのセクタ全体がゴミ**になる (暗号化後は 1 バイトの差が全体に拡散する)。FS のジャーナル・クラッシュ耐性は暗号層より上の性質だが、**暗号層の下でセクタが原子的でない**ぶん、電源断で壊れる範囲は平文より大きい (dm-crypt でも同じ性質)。実際の USB は多くが 4096 バイトで原子的に書くが、保証はない。
- **鍵・平文**: マスターキー・暗号化の作業バッファ (`crypto::Secret`) は使用後ゼロ化、`close()` で鍵とキャッシュをゼロ化。ログ・`describe()`・`note`・`write_note` に秘密を出さない。

### 9.4 LUKS / BitLocker の暗号化の細部

- LUKS: 復号と同じ IV で暗号化する。IV = 512 バイト単位のセクタ番号 (論理セクタが 4096 でも 512 単位) + LUKS2 の `iv_tweak`。ヘッダ・keyslot は一切書かない。
- BitLocker: XTS のトゥイーク / CBC の IV は**物理バイトオフセット**から作る。論理先頭 `vh_sectors` セクタは退避先 (`vh_off`) へ写す (読み出しと同じ写像)。メタデータ 3 コピー (各 0x10000) と退避先の物理領域は 0 に見えるが、**書き込みは拒否**する (`check_write_range`)。Elephant diffuser の暗号化は復号の逆: セクタ鍵と XOR → 逆 A (5 周、降順の引き算) → 逆 B (3 周) → CBC 暗号化。

### 9.5 検証 (独立実装との突き合わせ)

自前の暗号層で書いたイメージを、**自前ではない実装**で復号 / 暗号化し直して全バイト比較する。

| 対象 | 独立実装 | 場所 |
|---|---|---|
| LUKS1/2 の乱数書き込み (9 構成) | `cryptsetup luksDump --dump-master-key` + Python `cryptography` で復号 → モデルと cmp。CI は実カーネルの dm-crypt (`sudo cryptsetup open` on loop) でも復号して cmp | tools/run_crypt_write_tests.sh、tools/crypt_indep.py |
| BitLocker 6 方式 | `bitlocker_synth.py` (numpy / cryptography) でモデルを暗号化し直して全バイト一致 + libbde `bdemount` の復号結果 == モデル。メタデータ・ブートセクタ不変、メタデータ領域への書き込みは拒否 | 同上 |
| 暗号層の上の ext4 / ext2 / NTFS / FAT | 書いた後の暗号イメージを独立実装で復号 (LUKS: python / 実カーネル、BitLocker: synth での暗号化し直し一致 + NTFS は libbde) → e2fsck -fn / ntfsfix -n / fsck.fat -n が clean、自前ドライバと独立 FUSE (fuse2fs / ntfs-3g) のツリーがモデルと一致。CI は実カーネルの ext4 / vfat でも ro マウントして照合 | tools/run_crypt_fs_tests.sh、core/compat/tests/test_cryptfs_write.cpp |
| 部分セクタ・キャッシュ整合・失敗 | 単体テスト `transform_write_*` (位置依存 XOR 暗号 + 失敗注入デバイス)、`cryptwrite_tool failinj` (write 失敗 / flush 失敗 / 3 MiB 書き込みの 2 チャンク目で失敗 → エラーが返り、読み取り専用に落ち、以後下位へ書かない) | core/tests/test_transform.cpp、core/tests/cryptwrite_tool.cpp |
| ファジング | 壊れたヘッダで writable open → 書き込み → 保護範囲 (ヘッダ・メタデータ) が 1 バイトも変わらない (`fuzzw`) | 同上 |
| 突然変異 | 安全機構を 1 つ外した 10 種のビルドがすべて検出される (9.6) | tools/run_crypt_mutation_tests.sh |
| 既定 | 書き込みフラグ未指定・非対応構成・変換途中・下位が読み取り専用 → 読み取り専用で開き、イメージ不変 (`norw` / `ro`) | 同上 |

### 9.6 突然変異テスト

`-DUSBMEM_MUTATE_<名前>` で安全機構を 1 つだけ外す (通常ビルドには入らない)。単体テストかツール (独立検証) のどちらかが失敗すれば「検出」。`CW_NO_CACHE_UPDATE` (書いた後のキャッシュ更新なし)、`CW_NO_RMW_HEAD` (先頭の部分セクタを読まない)、`CW_NO_FLUSH` (flush を下位へ渡さない)、`CW_NO_LATCH` (書き込み失敗で読み取り専用に落とさない)、`CW_ALWAYS_WRITABLE` (open 時の明示を無視)、`CW_LUKS_IV` (IV が 1 ずれる)、`CW_BL_NO_VH_MAP` (退避先へ写さない)、`CW_BL_NO_HIDDEN_CHECK` (メタデータ領域を拒否しない)、`CW_BL_PARTIAL` (変換途中にも書く)、`CW_DIFF_ORDER` (diffuser の逆変換の順序)。

### 9.7 制限・注意 (正直に)

- 実機 (USB) では未試験。UI は未接続 (core + C API + JNI + Kotlin の入口まで)。
- 暗号層の上の FS の書き込みは、その FS 自身の書き込み対応 (ext4 / ext2 / NTFS / FAT / exFAT: `USBMEM_NTFS_WRITE`) に従う。下が読み取り専用の暗号層なら、ext2 / FAT / exFAT / NTFS / ext4 の `mount_rw` は読み取り専用でマウントする (ext4 は元からそうだった。他は今回追加)。XFS / Btrfs / F2FS は未確認 (F2FS は `is_read_only` を見る)。
- 上の「電源断」のとおり、4096 セクタを 512 バイト単位の下位へ書くと、切れたセクタ全体が壊れる。
- diffuser 付きの書き込みは実 Windows 7/8 のイメージでは未確認。
- BitLocker の 4Kn、dm-integrity、TPM のみ、複数 segment は書き込めない (拒否)。
- 書き込んだ後に Windows で開いたときの挙動 (BitLocker のメタデータ・TPM 検証値の更新が不要なことの確認) は未確認。書き込むのはデータ領域と退避ヘッダの暗号化セクタだけで、FVE メタデータ・ブートセクタは変えない設計。
- 負荷時に libbde の `bdemount` の読み出しが途中で切れる (EOF) のを 1 度観測した (ビルド実行中のローカル。再実行で消え、CI の結果は 9.8)。

### 9.8 結果

(CI の実行 ID と結果は下に記録する。)
