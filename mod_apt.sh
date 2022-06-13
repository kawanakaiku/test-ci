sudo rm -f /etc/apt/apt.conf.d/*
echo 'path-exclude=/usr/share/man/*' | sudo tee /etc/dpkg/dpkg.cfg.d/01_nodoc
echo 'Acquire::Languages "none";' | sudo tee /etc/apt/apt.conf.d/99_translations

sudo rm -f /etc/apt/sources.list.d/*
echo "deb http://azure.archive.ubuntu.com/ubuntu/ $( . /etc/os-release ; echo $VERSION_CODENAME ) main restricted universe multiverse" | sudo tee /etc/apt/sources.list
