sudo rm mnt/usr/bin/qemu-aarch64-static

sudo umount -lf mnt/src

sudo umount -lf mnt/run
sudo umount -lf mnt/dev
sudo umount -lf mnt/sys
sudo umount -lf mnt/proc
sudo umount -lf mnt
sudo umount -lf overlay/lower
