cd mnt

sudo wget -nv https://bootstrap.pypa.io/get-pip.py

sudo chroot . /bin/bash <<'chroot_end'
  python3.8 /get-pip.py
  python3.8 -m pip install -U pip
  python3.8 -m pip install -U setuptools wheel Cython cmake
chroot_end

sudo rm get-pip.py
