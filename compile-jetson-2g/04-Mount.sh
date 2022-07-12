offset=`sgdisk --print sd-blob.img | awk '{if($1==1){print $2*512}}'`

mkdir -p overlay/{lower,upper,work} mnt
sudo mount -t ext4 -o loop,offset=${offset},ro sd-blob.img overlay/lower

# create native ubuntu rootfs with cross compiler
sudo mmdebstrap --components=main,multiverse,restricted,universe --include=gcc-7-aarch64-linux-gnu,g++-7-aarch64-linux-gnu --variant=essential --architecture=amd64 bionic bionic-amd64 http://archive.ubuntu.com/ubuntu

sudo mount -t overlay -o lowerdir=overlay/lower:bionic-amd64,upperdir=overlay/upper,workdir=overlay/work overlay mnt

sudo mount -t proc /proc mnt/proc
sudo mount -t sysfs /sys mnt/sys
sudo mount -o bind /dev mnt/dev
sudo mount -o bind /run mnt/run
