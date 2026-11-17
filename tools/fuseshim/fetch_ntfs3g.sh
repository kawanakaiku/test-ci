#!/bin/sh
# ntfs-3g の固定バージョンのソースを取得し (sha256 検証)、展開して configure する。fuseshim の試作用 (docs/fuse-layer.md)。
# ソースはリポジトリに入れない (GPL-2.0-or-later。取り込み方の方針は docs/fuse-layer.md 4 章)。ネットワークが要る。
# 使い方: tools/fuseshim/fetch_ntfs3g.sh DEST    → DEST/src (展開したソース)、DEST/build (configure 済み。config.h がある)
# 検証に失敗したら (ダウンロード失敗・sha256 不一致) skip ではなく失敗 (終了コード 1) にする。
set -eu
DEST=${1:?DEST}
VER=2022.10.3
# 取得元は 2 つ (どちらも固定の sha256 で検証する)。
#  A: 配布元の tgz (configure 入り)。GitHub の runner からは 403 になることがある (2026-11-18 に確認)。
#  B: Debian の orig tarball (中身は A と同じソースで、configure だけ無い → autoreconf が要る。diff で生成物以外の差が無いことを確認済み)。
A_FILE=ntfs-3g_ntfsprogs-$VER.tgz
A_SHA256=f20e36ee68074b845e3629e6bced4706ad053804cbaf062fbae60738f854170c
A_URL=https://tuxera.com/opensource/$A_FILE
B_FILE=ntfs-3g_$VER.orig.tar.gz
B_SHA256=8bd7749ea9d8534c9f0664d48b576e90b96d45ec8803c9427f6ffaa2f0dde299
B_URL=https://deb.debian.org/debian/pool/main/n/ntfs-3g/$B_FILE

mkdir -p "$DEST"
fetch() {  # url file sha256
  curl -fsSL --retry 3 --connect-timeout 20 --max-time 300 -A "Mozilla/5.0 (X11; Linux x86_64)" -o "$DEST/$2" "$1" \
    && echo "$3  $DEST/$2" | sha256sum -c - >/dev/null 2>&1
}
NEED_AUTORECONF=0
if fetch "$A_URL" "$A_FILE" "$A_SHA256"; then TGZ=$DEST/$A_FILE; echo "fetched $A_URL (sha256 ok)"
elif fetch "$B_URL" "$B_FILE" "$B_SHA256"; then TGZ=$DEST/$B_FILE; NEED_AUTORECONF=1; echo "fetched $B_URL (sha256 ok; autoreconf が必要)"
else echo "ntfs-3g $VER を取得できない / sha256 が一致しない" >&2; exit 1; fi

rm -rf "$DEST/src" "$DEST/build"
mkdir -p "$DEST/src" "$DEST/build"
tar xzf "$TGZ" -C "$DEST/src" --strip-components=1
[ "$NEED_AUTORECONF" = 0 ] || ( cd "$DEST/src" && autoreconf -fi >"$DEST/autoreconf.log" 2>&1 ) || { tail -20 "$DEST/autoreconf.log" >&2; exit 1; }
# configure は config.h を作るためだけに使う (ビルドは core/fuseshim.cmake)。内蔵 FUSE (FUSE_INTERNAL) の定義でヘッダ (fuse-lite) を使う。
# プラグイン (dlopen) と mtab・UUID・ディスクジオメトリは使わない。
( cd "$DEST/build" && ../src/configure --disable-ntfsprogs --disable-plugins --disable-mount-helper --disable-ldconfig --disable-mtab \
    --without-uuid --without-hd --disable-shared --enable-static --with-fuse=internal >configure.log 2>&1 ) || { tail -30 "$DEST/build/configure.log" >&2; exit 1; }
grep -E "FUSE_INTERNAL|DISABLE_PLUGINS|POSIXACLS|XATTR_MAPPINGS|HAVE_SETXATTR|HAVE_UTIMENSAT|IGNORE_MTAB|_FILE_OFFSET_BITS|^#define VERSION" "$DEST/build/config.h" || true
grep -q "define FUSE_INTERNAL" "$DEST/build/config.h" || { echo "config.h に FUSE_INTERNAL が無い" >&2; exit 1; }
echo "src=$DEST/src build=$DEST/build"
