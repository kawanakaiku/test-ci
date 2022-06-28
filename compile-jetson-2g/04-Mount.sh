offset=`sgdisk --print sd-blob.img | awk '{if($1==1){print $2*512}}'`

mkdir -p overlay/{lower,upper,work} mnt
sudo mount -t ext4 -o loop,offset=${offset},ro sd-blob.img overlay/lower
sudo mount -t overlay -o lowerdir=overlay/lower,upperdir=overlay/upper,workdir=overlay/work overlay mnt

sudo mount -t proc /proc mnt/proc
sudo mount -t sysfs /sys mnt/sys
sudo mount -o bind /dev mnt/dev
sudo mount -o bind /run mnt/run
