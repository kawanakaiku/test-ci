# android-usbmem (Android Universal Storage Manager)

root 不要で USB ストレージ/ディスクイメージを扱う Android アプリ。現在 Phase 1(RAW イメージ + MBR/GPT 列挙、読み取り専用)。
要件は開発要件書、進捗は `docs/`。

- PC テスト: `tools/run_host_tests.sh`
- Android: `cd android && android-sdk-exec ./gradlew :app:assembleDebug`
  (計装テスト: `ANDROID_SERIAL=emulator-5556 android-sdk-exec ./gradlew :app:connectedDebugAndroidTest`)
- 設計: `docs/architecture.md`, NTFS 移植分析: `docs/phase0-ntfs-port-analysis.md`
