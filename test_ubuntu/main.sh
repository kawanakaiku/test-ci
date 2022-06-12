echo apt.conf.d ; find /etc/apt/apt.conf.d
sudo rm /etc/apt/apt.conf.d/*
echo 'path-exclude=/usr/share/man/*' | sudo tee /etc/dpkg/dpkg.cfg.d/01_nodoc
echo 'Acquire::Languages "none";' | sudo tee /etc/apt/apt.conf.d/99_translations

echo sources.list: ; sudo cat /etc/apt/sources.list || true
echo sources.list.d ; sudo find /etc/apt/sources.list.d
echo 'deb http://archive.ubuntu.com/ubuntu/ jammy main restricted universe multiverse' | sudo tee /etc/apt/sources.list
sudo rm -rf /etc/apt/sources.list.d

sudo apt-get update
sudo apt-get install -y gcc-multilib

gcc -static -s -pipe -O3 $(dirname $0)/primes.c -o primes
