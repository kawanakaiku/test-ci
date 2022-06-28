sudo cp /usr/bin/qemu-aarch64-static mnt/usr/bin/qemu-aarch64-static
sudo cp --remove-destination /etc/resolv.conf mnt/etc/resolv.conf
sudo sed -i -e 's/<SOC>/t210/g' mnt/etc/apt/sources.list.d/nvidia-l4t-apt-source.list
sudo rm mnt/etc/apt/apt.conf.d/50appstream  # no icons update
