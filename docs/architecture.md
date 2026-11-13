# アーキテクチャ(Phase 0/1 時点)

```
android/                Kotlin (Compose M3) — SAF / USB / UI のみ。Android 固有処理はここに隔離
  app/src/main/cpp/     JNI 薄層(例外を出さない、文字列は UTF-8 byte[])
core/                   C++17 ネイティブエンジン — PC 単体でビルド・テスト可能(Android 非依存)
  include/usbmem/       status / block_device / devices / partition / fs_probe / c_api
  tests/                単体・故障注入・ミューテーションテスト、Python 製の独立フィクスチャ検証
tools/                  gen_fixtures.py(独立実装), run_host_tests.sh, analyze_ntfs_deps.sh
third_party/            (Phase 2 以降) NTFS Resurrection 等の取り込み先。出典コミットを記録する
docs/
```

## BlockDevice 契約(`block_device.h`)

- `read_at/write_at` は全量成功かエラー。短い I/O は実装内で継続。範囲外は `OutOfRange`(拡張しない、64bit オーバーフロー安全)。
- `flush`, `close`(冪等), `size`, `logical_block_size`, `is_read_only`, `CancelToken`/`timeout_ms`, `Errc`+`errno`。
- 実装済み: `RawImageDevice`(pread/pwrite, 読み取り専用が既定, 書き込みは排他ロックで直列化), `MemoryDevice`, `SubRangeDevice`(パーティション/オフセット変換の土台)。
- 予定: USB BOT(Phase 3), BitLocker 等の `BlockTransform`(Phase 4), NBD(Phase 4)。未実装の timeout は現状無視される(RAW 実装)。

## パーティション(`partition.h`)

MBR / EBR(ループ・範囲外・256 超を打ち切り)/ GPT(ヘッダ+配列 CRC、バックアップ検査、プライマリ破損時のバックアップ採用、
ハイブリッド MBR 検出、512/4096 セクタ自動判定)/ パーティションなしボリューム(FAT/NTFS/exFAT/BitLocker は VBR、ext/ISO 等は全体プローブ)。
不正テーブルはエラーでなく `diagnostics`(安定した `code`)に記録。I/O エラーのみ `Status` で返す。
FS プローブは「検出」のみで、マウント対応を意味しない。

## 境界

Kotlin ⇄ JNI: `nativeOpenFd(fd, readOnly)`(fd 所有権はネイティブへ), `nativeListPartitions`(JSON), `nativeClose`。
64bit 値は JSON 文字列で受け渡し(Kotlin 側 `toULong`)。Phase 1 は常に読み取り専用。

## 複数 USB デバイス(Android アプリ)

USB は `UsbSessionManager`(プロセスに 1 つ)が `deviceName` をキーに 1 本ずつ管理し、ネイティブハンドル・ボリューム・SAF ルート・書き込みモード・アイドル停止はデバイスごとに独立。
ネイティブ側のプロセス共通の状態は互換層(ミューテックス保護、マウントは直列化)と診断トレース(`thread_local`)のみ。設計・状態遷移・制約は docs/multi-usb.md。
