if false ; then

git clone --depth=1 --recursive --branch=v1.11.0 https://github.com/pytorch/pytorch.git src_lower  # should be owned by root

else

mkdir src_lower
wget -nv https://github.com/kawanakaiku/test-ci/releases/download/archive/pytorch-v1.11.0.squashfs
sudo mount -t squashfs pytorch-v1.11.0.squashfs ./src_lower

fi

mkdir -p src src_upper src_work
sudo mount -t overlay overlay -o lowerdir=src_lower,upperdir=src_upper,workdir=src_work src

sudo mkdir -p mnt/src
sudo mount -o bind src mnt/src