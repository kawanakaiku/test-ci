#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# 出所の機械照合 (tools/provenance_scan.py、docs/provenance-btrfs-xfs.md) のために、Linux カーネルの該当部分を GitHub のタグの tarball から取り出す。
#   使い方: tools/provenance_fetch.sh <出力ディレクトリ (tmpfs の下など。終わったら削除する)> [タグ ...]   (既定のタグ: v7.3 v6.12)
# 出力: <出力>/k-<タグ>/ (fs/btrfs fs/xfs fs/f2fs include/uapi/linux include/linux lib/{lzo,lz4,zstd,zlib_inflate} ほか) と、<出力>/k-<タグ>.sha256 (tarball の SHA-256)。
# カーネルのソースはリポジトリに入れない (GPL-2.0)。取得したものは照合のためだけに読み、照合が済んだら消す。
set -euo pipefail
out=${1:?出力ディレクトリ}; shift || true
tags=("$@"); [ ${#tags[@]} -gt 0 ] || tags=(v7.3 v6.12)
mkdir -p "$out"
for t in "${tags[@]}"; do
  v=${t#v}
  mkdir -p "$out/k-$t"
  curl -sfL --retry 3 "https://github.com/torvalds/linux/archive/refs/tags/$t.tar.gz" -o "$out/k-$t.tar.gz"
  (cd "$out" && sha256sum "k-$t.tar.gz" > "k-$t.sha256")
  tar -xzf "$out/k-$t.tar.gz" -C "$out/k-$t" --strip-components=1 --wildcards \
    "linux-$v/fs/btrfs" "linux-$v/fs/xfs" "linux-$v/fs/f2fs" "linux-$v/include/uapi/linux" "linux-$v/include/linux" \
    "linux-$v/lib/lzo" "linux-$v/lib/zstd" "linux-$v/lib/lz4" "linux-$v/lib/zlib_inflate"
  rm -f "$out/k-$t.tar.gz"
  # xxhash64 / crc32c の実装 (Btrfs のチェックサム) も比べる
  mkdir -p "$out/k-$t/lib/extra"
  base=https://raw.githubusercontent.com/torvalds/linux/$t
  for f in lib/xxhash.c lib/crc/crc32-main.c lib/crc/crc32c.c lib/crc/crc32.h lib/crc/crc32table.h lib/crc/gen_crc32table.c; do
    curl -sfL "$base/$f" -o "$out/k-$t/lib/extra/$(basename "$f")" || echo "取得できなかった (その版に無い): $f" >&2
  done
  echo "取得: $t ($(cat "$out/k-$t.sha256"))"
done
