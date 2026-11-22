#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# 公開版のホストテスト (core のビルドと ctest)。公開版の .github/workflows/ci.yml と、個人版のリポジトリの public-edition.yml が同じ手順を使う。
#   使い方: tools/ci/host_tests.sh <plain|san|write>   (公開版のツリーの根で実行する。Ubuntu の runner を想定: sudo と apt が要る)
#     plain = Debug、san = ASan + UBSan、write = FS の書き込み経路を有効にした Release (-DUSBMEM_NTFS_WRITE=ON。Btrfs / XFS / F2FS の書き込みと展開器の試験だけを回す。実験的な書き込みの検証)
set -euo pipefail
mode=${1:?plain か san}
case "$mode" in plain|san|write) ;; *) echo "未知の構成: $mode" >&2; exit 2 ;; esac

sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  cmake ninja-build g++ python3 e2fsprogs dosfstools exfatprogs ntfs-3g mtools f2fs-tools \
  gdisk fdisk bc cryptsetup-bin nbdkit libnbd-bin qemu-utils partclone gddrescue zlib1g-dev \
  python3-cryptography python3-numpy squashfs-tools fuse2fs fuse3 exfat-fuse \
  btrfs-progs xfsprogs zstd lzop testdisk
echo user_allow_other | sudo tee -a /etc/fuse.conf >/dev/null

# runner の /dev/shm はユーザークォータで約 6 GB に制限される。クォータなしの tmpfs を重ねる
sudo mount -t tmpfs -o size=10G,mode=1777,nosuid,nodev tmpfs /dev/shm

# ubuntu 26.04 の runner は AppArmor の fusermount3 プロファイルが FUSE のマウント / アンマウントを /home・/mnt・/media・/tmp・/run/user の下に限る。
# テストの作業場所は /dev/shm なので (NTFS のフィクスチャは ntfs-3g を使う)、ローカル上書きで /dev/shm を足す。
if [ -f /etc/apparmor.d/fusermount3 ] && command -v apparmor_parser >/dev/null; then
  sudo tee -a /etc/apparmor.d/local/fusermount3 >/dev/null <<'EOT'
mount fstype=@{fuse_types} options=(nosuid,nodev) options in (ro,rw,noatime,dirsync,nodiratime,noexec,sync) -> /dev/shm/**/,
umount /dev/shm/**/,
EOT
  sudo apparmor_parser -r /etc/apparmor.d/fusermount3
fi

flags=""; bt=Debug
[ "$mode" = san ] && flags="-DUSBMEM_SANITIZE=ON"
[ "$mode" = write ] && { flags="-DUSBMEM_NTFS_WRITE=ON"; bt=Release; }
cmake -S core -B build -G Ninja -DCMAKE_BUILD_TYPE=$bt -DUSBMEM_EDITION=public $flags
cmake --build build -j"$(nproc)"

export PATH="$PATH:/sbin:/usr/sbin"
export ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=1
cd build
if [ "$mode" = write ]; then
  # 書き込みの試験 (btrfs check / xfs_repair との突き合わせ、電源断の模擬、ファジング) と、実カーネルとの相互確認 (loop マウントに sudo を使う。使えなければ SKIP)。
  # 展開器と読み取り系もここで Release で回す (plain / san と同じ試験で、書き込みを有効にしたビルドでも読み取りが変わらないことの確認)
  export USBMEM_BTRFS_KERNEL=1
  ctest -j2 --output-on-failure -R '^(decomp_|btrfs_|xfs_|f2fs_)'
else
  ctest -j2 --output-on-failure
fi
