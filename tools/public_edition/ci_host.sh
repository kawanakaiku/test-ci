#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# 公開版のホストテスト (core のビルドと ctest)。公開版の .github/workflows/ci.yml と、個人版のリポジトリの public-edition.yml が同じ手順を使う。
#   使い方: tools/ci/host_tests.sh <plain|san>   (公開版のツリーの根で実行する。Ubuntu の runner を想定: sudo と apt が要る)
set -euo pipefail
mode=${1:?plain か san}
case "$mode" in plain|san) ;; *) echo "未知の構成: $mode" >&2; exit 2 ;; esac

sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  cmake ninja-build g++ python3 e2fsprogs dosfstools exfatprogs ntfs-3g mtools f2fs-tools \
  gdisk fdisk bc cryptsetup-bin nbdkit libnbd-bin qemu-utils partclone gddrescue zlib1g-dev \
  python3-cryptography python3-numpy squashfs-tools

# runner の /dev/shm はユーザークォータで約 6 GB に制限される。クォータなしの tmpfs を重ねる
sudo mount -t tmpfs -o size=10G,mode=1777,nosuid,nodev tmpfs /dev/shm

flags=""
[ "$mode" = san ] && flags="-DUSBMEM_SANITIZE=ON"
cmake -S core -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DUSBMEM_EDITION=public $flags
cmake --build build -j"$(nproc)"

export PATH="$PATH:/sbin:/usr/sbin"
export ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=1
cd build
ctest -j2 --output-on-failure
