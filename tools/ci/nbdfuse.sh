# nbdfuse で「疎の大きなイメージ」をメモリ上に作る共通の関数 (source して使う)。
#
#   . tools/ci/nbdfuse.sh
#   nbdfuse_install          # (CI) nbdkit / nbdfuse / fuse3 を入れる。手元では不要
#   nbdfuse_apparmor         # (CI) runner の AppArmor が fusermount3 の FUSE マウントを拒否するのを許可する
#   nbdfuse_start DIR 31G    # DIR/nbd が 31 GiB の疎ファイルとして見える。触れたページだけメモリを使う
#   ... mkfs.xfs DIR/nbd / dd / テスト ...
#   nbdfuse_stop DIR
#
# なぜ: 大きなイメージをディスクや /dev/shm に疎ファイルとして置くと、(1) runner のディスクが足りない、
# (2) /dev/shm のユーザークォータ (EDQUOT) に当たる、(3) 手元では Data (NTFS) に大きな書き込みをしてしまう、のどれかになる。
# nbdfuse はファイルシステムに書かず、触れたページだけプロセスのメモリに持つ。後始末は nbdfuse_stop (または fusermount -u)。
# ルート権限は要らない (FUSE を使う)。マウントポイントは NBDFUSE_ROOT (既定 /dev/shm/nbdfuse) の下に作ると、AppArmor の許可がそのまま効く。

NBDFUSE_ROOT=${NBDFUSE_ROOT:-/dev/shm/nbdfuse}

nbdfuse_install() {
  sudo apt-get update -qq
  sudo apt-get install -y --no-install-recommends nbdkit libnbd-bin fuse3
}

# ubuntu-latest (26.04) の AppArmor は、fusermount3 が /dev/shm 以下へ FUSE をマウントするのを拒否する (host-tests.yml と同じ対処)。
nbdfuse_apparmor() {
  if [ -f /etc/apparmor.d/fusermount3 ] && command -v apparmor_parser >/dev/null; then
    sudo tee -a /etc/apparmor.d/local/fusermount3 >/dev/null <<EOT
mount fstype=@{fuse_types} options=(nosuid,nodev) options in (ro,rw,noatime,dirsync,nodiratime,noexec,sync) -> ${NBDFUSE_ROOT}/**/,
umount ${NBDFUSE_ROOT}/**/,
EOT
    sudo apparmor_parser -r /etc/apparmor.d/fusermount3
  fi
}

# nbdfuse_start DIR SIZE [nbdkit memory の追加引数...]   例: nbdfuse_start "$NBDFUSE_ROOT/c" 31G
nbdfuse_start() {
  local dir=$1 size=$2; shift 2
  mkdir -p "$dir"
  nohup nbdfuse "$dir" --command nbdkit -s memory "$size" allocator=sparse "$@" >"$dir.log" 2>&1 &
  local i
  for i in $(seq 1 100); do
    [ -e "$dir/nbd" ] && return 0
    sleep 0.2
  done
  echo "nbdfuse_start: $dir/nbd が現れない" >&2; cat "$dir.log" >&2 || true
  return 1
}

# nbdfuse_stop DIR: アンマウントして、メモリを返す。
nbdfuse_stop() {
  local dir=$1 i
  fusermount3 -u "$dir" 2>/dev/null || fusermount -u "$dir" 2>/dev/null || true
  for i in $(seq 1 50); do
    mountpoint -q "$dir" || break
    sleep 0.2
  done
  rmdir "$dir" 2>/dev/null || true
  rm -f "$dir.log"
}
