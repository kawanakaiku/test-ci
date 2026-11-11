# Phase 1 報告

## 完了条件
「テスト用 RAW イメージを選択し、パーティションを正確に列挙できる」

## 実施内容・結果
- ネイティブコア(PC でビルド可): BlockDevice / RAW / SubRange / Memory、MBR・EBR・GPT、FS 検出、C API。
- PC テスト: `tools/run_host_tests.sh`(通常 + ASan/UBSan)。37 件の単体テスト + 6 件の独立フィクスチャ照合、全通過。
  内容: 範囲外・オーバーフロー・読み取り専用・キャンセル・並行書き込み、GPT 主/備破損・巨大/敵対的ヘッダ・4Kn・重複・ハイブリッド、
  EBR ループ/範囲外、I/O 故障と切断の注入、1,800 回のランダム破壊(クラッシュ・ハングなし)。
- フィクスチャ(`tools/gen_fixtures.py`)は C++ と別実装(Python)で作成し、仕様の読み違いの共有を避けた。
- テスト用ディスクイメージは tmpfs(`/dev/shm`)に生成(`USBMEM_TEST_TMPDIR` で変更可、tmpfs でなければ警告)。
- Android: assembleDebug 成功(arm64-v8a, x86_64)。JVM 単体テスト成功。AVD(API 37, x86_64)で計装テスト 3 件成功
  (JNI 経由の MBR 列挙、不正 fd のエラー、空イメージの診断)。.so は LOAD 0x4000 アラインで `zipalign -P 16` も OK(16KB ページ対応を確認)。

## 残る制約
- SAF ピッカー経由の UI 操作は未自動化(計装テストは同じ fd 経路を直接使用)。
- AVD の計装テスト用イメージはエミュレータ内 `/data`(ホストの tmpfs ではない)。
- USB / 書き込み / 各ファイルシステム / BitLocker は未実装(UI にも出していない)。timeout は RAW 実装で未対応。
- ASan は `/etc/ld.so.preload` のある環境で `verify_asan_link_order=0` が必要(スクリプトで設定済み)。
- AGP 8.11.1 は Gradle 9.0.0 で動作。システムの Gradle 9.7.1 は AGP 8.x と非互換のため `./gradlew` を使うこと。
- 実機の USB メモリ等での検証は未実施。ビルドやテストは必ずエミュレータ(`ANDROID_SERIAL`)を指定する(別の実機が接続されていることがある)。
