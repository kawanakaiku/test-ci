cd mnt

sudo chroot . /bin/bash <<'chroot_end'
  apt-get -o Acquire::Languages=none update
  apt-get install --download-only -y \
    apt-transport-https build-essential ca-certificates \
    cmake curl git \
    libatlas-base-dev libcurl4-openssl-dev libjemalloc-dev \
    liblapack-dev libopencv-dev \
    libzmq3-dev ninja-build \
    python3.8-dev software-properties-common sudo \
    unzip virtualenv wget \
    libjpeg-dev libopenblas-dev libopenmpi-dev libomp-dev ccache \
    libblas3 liblapack3
chroot_end


# apt in chroot hungs
shopt -s nullglob
for deb in var/cache/apt/archives/*.deb ; do
  echo "extracting $deb ..."
  ar x $deb data.tar.xz
  [ -f data.tar.xz ] || continue
  sudo tar -xf data.tar.xz --no-overwrite-dir -C ./
  rm data.tar.xz
done
