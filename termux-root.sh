#/usr/bin/bash

set -eu

TERMUX_PREFIX=${TERMUX_PREFIX:-/data/data/com.termux/files/usr}
TERMUX_HOME=${TERMUX_HOME:-/data/data/com.termux/files/home}
TERMUX_ARCH=${TERMUX_ARCH:-aarch64}
VERSION=${VERSION:-30_r01}

run_wait() {(
  pids=
  for func do
    log() { echo "$0: $func: $*"; }
    echo "running $func..."
    "$func" &
    pids+=" $!"
  done
  for pid in ${pids[*]}; do
    wait $pid || exit 1
  done
)}

get_system() {
  export LPUNPACK=otatools/bin/lpunpack
  
  log "download system.img"
  ABI=$(
    case $TERMUX_ARCH in
      aarch64) echo arm64-v8a;;
      arm) echo armeabi-v7a;;
      i686) echo x86;;
      x86_64) echo x86_64;;
    esac
  )
  wget -nv https://dl.google.com/android/repository/sys-img/android/${ABI}-${VERSION}.zip -O system.zip
  7z e system.zip ${ABI}/system.img
  7z e system.img super.img
  rm system.zip system.img
  
  log "download lpunpack"
  python -m pip install -U gdown
  python -m gdown.cli 1h4sGXNI1Al5Y_Te9T8xMWrp3mHw1tke- -O otatools.zip
  unzip -q otatools.zip
  chmod u+x ${LPUNPACK}
  rm otatools.zip
  
  log "extract system"
  mkdir super
  ${LPUNPACK} --slot=0 super.img super
  7z x super/system.img -otermux-root
  rm -r super super.img
  
  log "extract apex"
  cd termux-root/system/apex
  ls
  rm -f *.capex
  for APEX in *.apex; do
    NAME=${APEX%.*}
    7z e -aoa ${APEX} apex_payload.img
    7z x apex_payload.img -o${NAME}
  done
  rm *.apex apex_payload.img
  cd -
  
  log "link"
  cd termux-root
  rm -r apex
  ln -s /system/apex apex
  grep -rIl -e . . | xargs file | sed 's|:\s*|:|' | awk -F: '{if($2=="ASCII text, with no line terminators"){print $1}}' | while read f; do
    TO=$(cat $f)
    ln -sf ${TO} ${f}
  done
  cd -
  
  log "chmod"
  cd termux-root
  find system/bin -type f -print0 | xargs -0 chmod 0755
  chmod 0755 system/xbin/su system/apex/*/bin/*
  cd -
  
  log "remove unneeded"
  cd termux-root
  rm -r \
    system/fonts \
    system/app \
    system/priv-app \
    system/framework
  cd -
}

get_bootstrap() {
  log "download bootstrap"
  wget -nv https://github.com/termux/termux-packages/releases/latest/download/bootstrap-${TERMUX_ARCH}.zip -O bootstrap.zip
  
  log "extract"
  install -d -m 700 usr home
  unzip -q bootstrap.zip -d usr
  rm bootstrap.zip
  
  log "link"
  cd usr
  awk -F← '{system("ln -s "$1" "$2)}' SYMLINKS.txt
  rm SYMLINKS.txt
  cd -
  
  log "chmod"
  cd usr
  find -type f | sed -e 's|^\./||' | while read f; do
    if [[ "$f" == bin/* ]] ||
      [[ "$f" == libexec* ]] ||
      [[ "$f" == lib/apt/apt-helper* ]] ||
      [[ "$f" == lib/apt/methods* ]]
    then
      chmod 0700 "$f"
    fi
  done
  cd -
}

merge_dirs() {
  log "install termux"
  cd termux-root
  install -d -m 700 data/data/com.termux/{files,cache}
  mv ../usr ../home data/data/com.termux/files
}

install_pkgs() {
  log "install qemu"
  sudo apt-get install -y --no-install-recommends qemu-user-static
  
  log "chroot termux"
  cd termux-root
  mkdir -p proc dev dev/pts sys
  sudo mount -t proc proc proc
  sudo mount -t devtmpfs udev dev
  sudo mount -t devpts devpts dev/pts
  sudo mount -t sysfs sysfs sys
  # set host file
  mkdir -p system/etc
  for host in pypi.python.org files.pythonhosted.org pypi.org \
    github.com objects.githubusercontent.com \
    packages.termux.dev
  do
    echo $( nslookup $host | awk '/^Address: / { print $2; exit }' ) " $host" >> system/etc/hosts
  done
  # suppress linker: Warning: failed to find generated linker configuration from "/linkerconfig/ld.config.txt"
  mkdir -p linkerconfig
  touch linkerconfig/ld.config.txt
  TERMUX_USER=system
  printf '%s\n' * | grep -v -e ^proc$ -e ^dev$ -e ^sys$ | xargs sudo chroot . /system/xbin/su 0 chown -R ${TERMUX_USER}:${TERMUX_USER}
cat <<SH > ./chroot
#!/usr/bin/sh
exec sudo chroot ${PWD} \
  ${TERMUX_PREFIX}/bin/su ${TERMUX_USER} \
  ${TERMUX_PREFIX}/bin/env \
  PATH=${TERMUX_PREFIX}/bin \
  LD_LIBRARY_PATH=${TERMUX_PREFIX}/lib \
  HOME=${TERMUX_HOME} \
  LANG=en_US.UTF-8 \
  ANDROID_ROOT=/system \
  ANDROID_DATA=/data \
  ANDROID_ART_ROOT=/apex/com.android.art \
  ANDROID_I18N_ROOT=/apex/com.android.i18n \
  ANDROID_TZDATA_ROOT=/apex/com.android.tzdata \
  PIP_DISABLE_PIP_VERSION_CHECK=1 \
  "\$@"
SH
  chmod +x ./chroot
  cd -
  
  log "update"
  cd termux-root
./chroot bash --rcfile ${TERMUX_PREFIX}/etc/profile <<BASH
apt-get update
apt-get upgrade -y --auto-remove --purge build-essential jq termux-exec python gnupg file git wget curl ndk-sysroot ninja nodejs rust squid
apt-get clean
python -m pip install -U --no-cache-dir pip
BASH
  # reset hosts
  echo | sudo tee system/etc/hosts
  rm ./chroot
  cd -
  
  log "unmount"
  cd termux-root
  sudo umount -lf sys
  sudo umount -lf dev/pts
  sudo umount -lf dev
  sudo umount -lf proc
  cd -
  
  log "archive"
  cd termux-root
  sudo tar -c -I 'xz -9 -T0' -f - * > ${WORKDIR}/termux-${TERMUX_ARCH}.tar.xz
  cd -
}

export PIP_DISABLE_PIP_VERSION_CHECK=1
WORKDIR=${PWD}
TMPDIR=${PWD}/${RANDOM}
mkdir ${TMPDIR}
cd ${TMPDIR}

run_wait get_system get_bootstrap
run_wait merge_dirs
run_wait install_pkgs

cd -
rm -r ${TMPDIR}

