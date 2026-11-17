#!/bin/sh
# wimlib の固定バージョンのソースを取得 (sha256 検証)・展開し、libntfs-3g なし (--without-ntfs-3g)・FUSE ありで configure して libwim.a だけを作る。
# fuseshim の試作用 (docs/fuse-layer.md 6 章)。ソースはリポジトリに入れない。ネットワークと libfuse3-dev (ヘッダだけ。ライブラリは使わない)・autotools 不要 (tarball に configure 入り)。
# 使い方: tools/fuseshim/fetch_wimlib.sh DEST    → DEST/src、DEST/build/.libs/libwim.a
set -eu
DEST=${1:?DEST}
VER=1.14.4
SHA256=3633db2b6c8b255eb86d3bf3df3059796bd1f08e50b8c9728c7eb66662e51300
FILE=wimlib-$VER.tar.gz
URL=https://wimlib.net/downloads/$FILE
mkdir -p "$DEST"
curl -fsSL --retry 3 --connect-timeout 20 --max-time 300 -A "Mozilla/5.0 (X11; Linux x86_64)" -o "$DEST/$FILE" "$URL"
echo "$SHA256  $DEST/$FILE" | sha256sum -c - >/dev/null || { echo "wimlib $VER の sha256 が一致しない" >&2; exit 1; }
rm -rf "$DEST/src" "$DEST/build"; mkdir -p "$DEST/src" "$DEST/build"
tar xzf "$DEST/$FILE" -C "$DEST/src" --strip-components=1
# --without-ntfs-3g: libntfs-3g に依存しない (NTFS ボリュームの capture / apply だけが無くなる)。--with-fuse: wimmount (FUSE の 3.x 系 API)
( cd "$DEST/build" && CFLAGS="-O2 -fPIC" ../src/configure --without-ntfs-3g --with-fuse --disable-shared --enable-static >configure.log 2>&1 ) || { tail -30 "$DEST/build/configure.log" >&2; exit 1; }
grep -E "WITH_NTFS_3G|WITH_FUSE" "$DEST/build/config.h" || true
( cd "$DEST/build" && make -j"$(nproc)" libwim.la >make.log 2>&1 ) || { tail -30 "$DEST/build/make.log" >&2; exit 1; }
ls -l "$DEST/build/.libs/libwim.a"
# libntfs-3g に依存していないことの確認 (undefined シンボルに ntfs_ が無い)
if nm -u "$DEST/build/.libs/libwim.a" 2>/dev/null | grep -E " U ntfs_" ; then echo "libntfs-3g のシンボルが残っている" >&2; exit 1; else echo "libwim.a: libntfs-3g への参照なし (nm -u | grep ' U ntfs_' が空)"; fi
echo "fuse symbols used by libwim.a:"; nm -u "$DEST/build/.libs/libwim.a" 2>/dev/null | grep -E " U fuse_" | sort -u
