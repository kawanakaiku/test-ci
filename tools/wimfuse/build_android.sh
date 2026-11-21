#!/bin/bash
# libwimfuse.so (wimlib の wimmount を載せた別プロセスの実行ファイル) を NDK でビルドする (docs/fuse-layer.md 10 章)。
#   1. wimlib の固定バージョン (tools/fuseshim/wimlib_pin.sh) を取得・sha256 検証・展開 (リポジトリにはソースを置かない)
#   2. ABI ごとに wimlib を NDK でクロスビルド (--without-ntfs-3g --with-fuse。ソースは無改変。configure の mq_open の確認だけ回避し、<mqueue.h> は代用ヘッダ)
#   3. core/fuseshim/wimfuse/android の CMake で libwimfuse.so (PIE の実行ファイル) をリンクして OUT/jniLibs/<abi>/ へ
# 使い方: tools/wimfuse/build_android.sh OUTDIR [abi ...]   (既定: arm64-v8a x86_64)
# 環境変数: ANDROID_NDK_HOME (必須)、WIMFUSE_API (既定 26)、FUSE3_INC (既定 /usr/include/fuse3 = libfuse3-dev のヘッダ。コンパイル時だけ)、
#           WIMFUSE_JOBS (make の並列。既定 2)
# 要るもの (apt): curl tar make cmake ninja-build libfuse3-dev (autotools は不要: tarball に configure がある)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=${1:?usage: build_android.sh OUTDIR [abi ...]}; shift
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
ABIS=("$@"); [ ${#ABIS[@]} -gt 0 ] || ABIS=(arm64-v8a x86_64)
NDK=${ANDROID_NDK_HOME:?ANDROID_NDK_HOME が要る}
API=${WIMFUSE_API:-26}
FUSE3_INC=${FUSE3_INC:-/usr/include/fuse3}
JOBS=${WIMFUSE_JOBS:-2}
[ -f "$FUSE3_INC/fuse.h" ] || { echo "libfuse3 のヘッダが無い: $FUSE3_INC/fuse.h (apt install libfuse3-dev)" >&2; exit 1; }
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
[ -x "$TC/clang" ] || { echo "NDK の clang が無い: $TC" >&2; exit 1; }
. "$ROOT/tools/fuseshim/wimlib_pin.sh"
W=$OUT/work; mkdir -p "$W"

# ---- 1. 取得 ----
TGZ=$W/wimlib-$WIMLIB_VER.tar.gz
if [ ! -f "$W/src/include/wimlib.h" ]; then
  curl -fsSL --retry 3 --connect-timeout 20 --max-time 300 -A "Mozilla/5.0 (X11; Linux x86_64)" -o "$TGZ" "$WIMLIB_URL"
  echo "$WIMLIB_SHA256  $TGZ" | sha256sum -c - >/dev/null || { echo "wimlib $WIMLIB_VER の sha256 が一致しない" >&2; exit 1; }
  rm -rf "$W/src"; mkdir -p "$W/src"
  tar xzf "$TGZ" -C "$W/src" --strip-components=1
fi
echo "wimlib $WIMLIB_VER (sha256 検証済み)"

for ABI in "${ABIS[@]}"; do
  case $ABI in
    arm64-v8a) TRIPLE=aarch64-linux-android ;;
    x86_64)    TRIPLE=x86_64-linux-android ;;
    *) echo "未対応の ABI: $ABI (64 ビットのみ)" >&2; exit 1 ;;
  esac
  echo "::group::wimlib ($ABI)"
  B=$W/build-$ABI; rm -rf "$B"; mkdir -p "$B/emptylib"
  # configure の AC_CHECK_LIB([rt], [mq_open]) が -lrt を LIBS に足す。bionic に librt は無いので、空のアーカイブで通す (mq_* は wimfuse_io.c の常に失敗する実装)
  "$TC/llvm-ar" rcs "$B/emptylib/librt.a"
  (
    cd "$B"
    CC="$TC/${TRIPLE}${API}-clang" AR="$TC/llvm-ar" RANLIB="$TC/llvm-ranlib" STRIP="$TC/llvm-strip" \
    CFLAGS="-O2 -fPIC -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -I$ROOT/core/fuseshim/wimfuse/stubs" \
    LDFLAGS="-L$B/emptylib" \
    ../src/configure --host="$TRIPLE" --without-ntfs-3g --with-fuse --disable-shared --enable-static \
      LIBFUSE_CFLAGS="-I$FUSE3_INC" LIBFUSE_LIBS="-lc" ac_cv_lib_rt_mq_open=yes >configure.log 2>&1
  ) || { tail -60 "$B/configure.log" >&2; exit 1; }
  grep -E "WITH_NTFS_3G|WITH_FUSE|HAVE_SYS_XATTR|HAVE_MQUEUE" "$B/config.h" || true
  ( cd "$B" && make -j"$JOBS" libwim.la >make.log 2>&1 ) || { grep -E "error|Error" "$B/make.log" | head -40 >&2; tail -30 "$B/make.log" >&2; exit 1; }
  ls -l "$B/.libs/libwim.a"
  # libntfs-3g に依存していないこと、mq_* の未解決が wimfuse_io.c で埋まる形であること
  if "$TC/llvm-nm" -u "$B/.libs/libwim.a" 2>/dev/null | grep -E " U ntfs_"; then echo "libntfs-3g のシンボルが残っている" >&2; exit 1; fi
  echo "libwim.a の fuse / mq の未解決シンボル:"; "$TC/llvm-nm" -u "$B/.libs/libwim.a" 2>/dev/null | grep -E " U (fuse_|mq_)" | sort -u
  echo "::endgroup::"

  echo "::group::libwimfuse.so ($ABI)"
  cmake -S "$ROOT/core/fuseshim/wimfuse/android" -B "$W/cmake-$ABI" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI="$ABI" -DANDROID_PLATFORM="android-$API" \
    -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release \
    -DWIMLIB_SRC="$W/src" -DWIMLIB_LIB="$B/.libs/libwim.a" -DFUSE3_INCLUDE="$FUSE3_INC" -DWIMFUSE_OUT="$OUT/jniLibs/$ABI"
  cmake --build "$W/cmake-$ABI" -j"$JOBS"
  F=$OUT/jniLibs/$ABI/libwimfuse.so
  ls -l "$F"
  file "$F" || true
  "$TC/llvm-readelf" -h -l "$F" | grep -E "Type:|Machine:|INTERP|Requesting program interpreter|LOAD" | head -8
  "$TC/llvm-readelf" -d "$F" | grep NEEDED || true
  echo "::endgroup::"
done
echo "出力: $OUT/jniLibs/<abi>/libwimfuse.so"
