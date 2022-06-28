# unzip image.zip  # extracts "sd-blob.img"
# rm image.zip

sudo apt-get -o Acquire::Languages=none update
sudo apt-get install -y avfs

mountavfs

ln -s ${HOME}/.avfs/${PWD}/image.zip#/sd-blob.img sd-blob.img
