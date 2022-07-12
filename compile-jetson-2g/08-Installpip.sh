cd mnt

sudo wget -nv https://bootstrap.pypa.io/get-pip.py

sudo chroot . /bin/bash <<'chroot_end'
  python3.8 /get-pip.py
chroot_end

sudo rm get-pip.py
