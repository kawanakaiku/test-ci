echo 'path-exclude=/usr/share/man/*' | sudo tee /etc/dpkg/dpkg.cfg.d/01_nodoc
echo 'Acquire::Languages "none";' | sudo tee /etc/apt/apt.conf.d/99_translations

# sudo dpkg --add-architecture i386
echo sources.list: ; sudo cat /etc/apt/sources.list || true
echo sources.list.d ; sudo find /etc/apt/sources.list.d
echo 'deb http://archive.ubuntu.com/ubuntu/ jammy main restricted universe multiverse' | sudo tee /etc/apt/sources.listecho "path-exclude=/usr/share/man/*" | sudo tee /etc/dpkg/dpkg.cfg.d/01_nodoc

sudo apt-get update
sudo apt-get install -y --no-install-recommends gcc-multilib gcc-i686-linux-gnu

i686-linux-gnu-gcc -s -pipe -O3 $(dirname $0)/primes.c -o primes
