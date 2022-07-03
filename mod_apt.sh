VERSION_CODENAME=$( . /etc/os-release ; echo $VERSION_CODENAME )

sudo rm -f /etc/apt/apt.conf.d/*

echo 'path-exclude=/usr/share/man/*' | sudo tee /etc/dpkg/dpkg.cfg.d/99_nodoc >/dev/null
echo 'Acquire::Languages "none";' | sudo tee /etc/apt/apt.conf.d/99_translations >/dev/null

sudo rm -f /etc/apt/sources.list.d/*

cat <<LIST | sudo tee /etc/apt/sources.list >/dev/null
$(
  for dist in ${VERSION_CODENAME}{,-{backports,security,updates}} ; do
    echo "deb http://azure.archive.ubuntu.com/ubuntu/ ${dist} main restricted universe multiverse"
  done
)
LIST
