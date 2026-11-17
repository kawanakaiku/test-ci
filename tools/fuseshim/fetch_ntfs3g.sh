#!/bin/sh
# ntfs-3g の固定バージョンのソースを取得し (sha256 検証)、展開して configure する。fuseshim の試作用 (docs/fuse-layer.md)。
# ソースはリポジトリに入れない (GPL-2.0-or-later。取り込み方の方針は docs/fuse-layer.md 4 章)。ネットワークが要る。
# 使い方: tools/fuseshim/fetch_ntfs3g.sh DEST    → DEST/src (展開したソース)、DEST/build (configure 済み。config.h がある)
# 検証に失敗したら (ダウンロード失敗・sha256 不一致) skip ではなく失敗 (終了コード 1) にする。
set -eu
DEST=${1:?DEST}
VER=2022.10.3
SHA256=f20e36ee68074b845e3629e6bced4706ad053804cbaf062fbae60738f854170c
FILE=ntfs-3g_ntfsprogs-$VER.tgz
URLS="https://tuxera.com/opensource/$FILE https://github.com/tuxera/ntfs-3g/releases/download/$VER/$FILE"

mkdir -p "$DEST"
TGZ=$DEST/$FILE
ok=0
for u in $URLS; do
  if curl -fsSL --retry 3 --connect-timeout 20 --max-time 300 -o "$TGZ" "$u" && echo "$SHA256  $TGZ" | sha256sum -c - >/dev/null 2>&1; then
    echo "fetched $u (sha256 ok)"; ok=1; break
  fi
  echo "failed: $u" >&2
done
[ "$ok" = 1 ] || { echo "ntfs-3g $VER を取得できない / sha256 が一致しない" >&2; exit 1; }

rm -rf "$DEST/src" "$DEST/build"
mkdir -p "$DEST/src" "$DEST/build"
tar xzf "$TGZ" -C "$DEST/src" --strip-components=1
# configure は config.h を作るためだけに使う (ビルドは core/fuseshim.cmake)。内蔵 FUSE (FUSE_INTERNAL) の定義でヘッダ (fuse-lite) を使う。
# プラグイン (dlopen) と mtab・UUID・ディスクジオメトリは使わない。
( cd "$DEST/build" && ../src/configure --disable-ntfsprogs --disable-plugins --disable-mount-helper --disable-ldconfig --disable-mtab \
    --without-uuid --without-hd --disable-shared --enable-static --with-fuse=internal >configure.log 2>&1 ) || { tail -30 "$DEST/build/configure.log" >&2; exit 1; }
grep -E "FUSE_INTERNAL|DISABLE_PLUGINS|POSIXACLS|XATTR_MAPPINGS|HAVE_SETXATTR|HAVE_UTIMENSAT|IGNORE_MTAB|_FILE_OFFSET_BITS|^#define VERSION" "$DEST/build/config.h" || true
grep -q "define FUSE_INTERNAL" "$DEST/build/config.h" || { echo "config.h に FUSE_INTERNAL が無い" >&2; exit 1; }
echo "src=$DEST/src build=$DEST/build"
