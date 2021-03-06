sudo chroot mnt /bin/bash <<'chroot_end' || true

python3.8 -m venv venv
source venv/bin/activate

_CFLAGS=" -mtune=cortex-a57 -L/lib -L/usr/local/cuda-10.2/lib64 -L/usr/lib -L/usr/lib/aarch64-linux-gnu -L/lib/aarch64-linux-gnu -L/usr/lib/aarch64-linux-gnu/tegra -Wl,-rpath,/usr/lib/aarch64-linux-gnu/tegra"
_CFLAGS+=" -I/usr/local/cuda-10.2/include -L/usr/local/cuda-10.2/lib64 -L/usr/local/cuda-10.2/lib64/stubs"
_CFLAGS+=" `pkg-config --cflags --libs python-3.8 libdrm_tegra $(pkg-config --list-all | awk '{print $1}' | grep -e '-10\.2$' | grep -v -e '^nppi')`"
export CFLAGS+="${_CFLAGS}"
export CXXFLAGS+="${_CFLAGS}"
#export MAKEFLAGS=-j1

# use native cross compiler
export CC=/usr/bin/aarch64-linux-gnu-gcc-7
export CXX=/usr/bin/aarch64-linux-gnu-g++-7
export PATH="/usr/aarch64-linux-gnu/bin:${PATH}"

export NVCC=/usr/local/cuda/bin/nvcc
export CUDACXX=/usr/local/cuda/bin/nvcc

export USE_OPENCV=1 USE_BLAS=openblas USE_CUDA=1 USE_CUDA_PATH=/usr/local/cuda USE_CUDNN=1
export USE_NUMPY=1
export CUDA_TOOLKIT_ROOT_DIR=/usr/local/cuda/targets/aarch64-linux
export CUDA_HOME=/usr/local/cuda

# https://qiita.com/tomoaki_teshima/items/294c9ffb03a3dcc4cfef
export TORCH_CUDA_ARCH_LIST="3.2;5.3;6.2;7.2"

export MAX_JOBS=2

cd /src

python3.8 -m pip install -U pip
python3.8 -m pip install -U setuptools wheel Cython cmake

python3.8 -m pip install -U -r ./requirements.txt
echo "testing numpy import" ; python3.8 -c 'import numpy;print(numpy)'
# python3.8 -m pip -vvv install ./
python3.8 setup.py bdist_wheel

mkdir -p build_wheel
cd build_wheel

python3.8 -m pip wheel -r ../requirements.txt
# python3.8 -m pip wheel ..
# python3.8 ../setup.py bdist_wheel
cp -a -v ../dist/*.whl .
python3.8 -m pip wheel pip setuptools wheel Cython cmake

chroot_end
