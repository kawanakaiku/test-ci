sudo chroot mnt /bin/bash <<'chroot_end'

export LDFLAGS+=" -L/lib -L/usr/local/cuda/lib64 -L/usr/lib -L/usr/lib/aarch64-linux-gnu -L/lib/aarch64-linux-gnu"
export CFLAGS+=" -I/include -I/usr/local/cuda/include -I/usr/include -I/usr/include/aarch64-linux-gnu"
export MAKEFLAGS=-j1

export NVCC=/usr/local/cuda/bin/nvcc
export CUDACXX=/usr/local/cuda/bin/nvcc

export USE_OPENCV=1 USE_BLAS=openblas USE_CUDA=1 USE_CUDA_PATH=/usr/local/cuda USE_CUDNN=1
export CUDA_TOOLKIT_ROOT_DIR=/usr/local/cuda/targets/aarch64-linux
export CUDA_HOME=/usr/local/cuda

export MAX_JOBS=8

cd /src

mkdir build
cd build

python3.8 -m pip install -r ../requirements.txt
python3.8 -m pip -vvv install ../

python3.8 -m pip wheel -r ../requirements.txt
python3.8 -m pip wheel ..
python3.8 -m pip wheel pip setuptools wheel Cython cmake

chroot_end
