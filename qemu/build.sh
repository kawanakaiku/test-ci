sudo apt-get install -y --no-install-recommends qemu-system-x86 qemu-utils
qemu-system-x86_64 --version
wget -nc -nv https://github.com/kawanakaiku/test-ci/releases/download/files/edk2-x86_64-code.fd
wget -nc -nv https://dl-cdn.alpinelinux.org/alpine/v3.16/releases/x86_64/alpine-virt-3.16.0-x86_64.iso

exit 0





mkfifo guest.in guest.out

echo starting qemu
qemu-system-x86_64 -machine q35 -m 1024 -smp cpus=2 -cpu qemu64 -drive if=pflash,format=raw,read-only,file=edk2-x86_64-code.fd -netdev user,id=n1,hostfwd=tcp::2222-:22 -device virtio-net,netdev=n1 -cdrom alpine-virt-3.16.0-x86_64.iso -nographic < guest.in > guest.out &
#bash -c 'for i in {1..10} ; do sleep 1 ; echo "counting $i..." ; done' < guest.in > guest.out &
echo starting qemu finished

echo reading stdout
# have to trigger program by writing zero byte to stdin
echo -n '' > guest.in
cat guest.out | while read line ; do
  echo ">>> $line"
done
