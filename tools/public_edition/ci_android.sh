#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# 公開版の Android アプリ: assemble と JVM 単体テスト (-PusbmemEdition=public)、APK のネイティブライブラリの検査。
# 公開版の .github/workflows/ci.yml と、個人版のリポジトリの public-edition.yml が同じ手順を使う。
#   使い方: tools/ci/android_build.sh   (公開版のツリーの根で実行する。JDK 17 と Android SDK (SDK_ROOT) がある runner を想定)
set -euo pipefail
NDK_VERSION=${NDK_VERSION:-30.0.16248370}       # android/app/build.gradle.kts の ndkVersion と合わせる
CMAKE_VERSION=${CMAKE_VERSION:-4.1.2}
SDK_ROOT=${SDK_ROOT:-/usr/local/lib/android/sdk}

python3 -I tools/check_manifest_permissions.py
bash tools/test_retry_gradle.sh

SM="$SDK_ROOT/cmdline-tools/latest/bin/sdkmanager"
yes | "$SM" --licenses >/dev/null || true
"$SM" "platforms;android-36" "build-tools;36.0.0" "ndk;$NDK_VERSION" "cmake;$CMAKE_VERSION"

(cd android && ../tools/ci/retry_gradle.sh ./gradlew --no-daemon :app:assembleDebug :app:testDebugUnitTest -PusbmemEdition=public)
find android/app/build/outputs/apk -name '*.apk' -exec ls -l {} +

# 公開版の APK のネイティブライブラリに、個人版の FS ドライバ (カーネル由来) のシンボルが無く、公開版の F2FS / Btrfs / XFS だけがあること
apk=$(ls android/app/build/outputs/apk/debug/*.apk | head -1)
x=$(mktemp -d)
(cd "$x" && unzip -o -q "$OLDPWD/$apk" 'lib/x86_64/*.so')
so="$x/lib/x86_64/libusbmem_jni.so"
for v in F2fsVolume BtrfsVolume XfsVolume; do nm -D "$so" | grep -E "$v" >/dev/null || { echo "$v のシンボルが無い"; exit 1; }; done
if nm -D "$so" | grep -E 'NtfsVolume|ExfatVolume|Ext2Volume|Ext4Volume|FatVolume|IsofsVolume|UdfVolume|SquashfsVolume|ErofsVolume|rt_vfs|lzo1x_decompress_safe|ZSTD_decompress|xz_dec'; then
  echo "個人版のシンボルが公開版の APK にある"; exit 1
fi
echo "OK: 公開版の APK のネイティブライブラリは F2FS / Btrfs / XFS だけ"
