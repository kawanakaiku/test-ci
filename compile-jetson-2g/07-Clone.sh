git clone --depth=1 --recursive --branch=v1.11.0 https://github.com/pytorch/pytorch.git src_lower  # should be owned by root
mkdir -p src src_upper src_work
sudo mount -t overlay overlay -o lowerdir=src_lower,upperdir=src_upper,workdir=src_work src

sudo mkdir -p mnt/src
sudo mount -o bind src mnt/src
