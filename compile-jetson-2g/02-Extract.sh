unzip image.zip  # extracts "sd-blob.img"
rm image.zip

exit 0


# Error 38 when determining sector size! Setting sector size to 512

sudo apt-get -o Acquire::Languages=none update
sudo apt-get install -y avfs

mountavfs

ln -s ${HOME}/.avfs/${PWD}/image.zip#/sd-blob.img sd-blob.img
