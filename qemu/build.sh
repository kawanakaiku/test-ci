mkfifo guest.in guest.out

echo starting qemu
qemu-system-x86_64 -machine q35 -m 1024 -smp cpus=2 -cpu qemu64 -drive if=pflash,format=raw,read-only,file=edk2-x86_64-code.fd -netdev user,id=n1,hostfwd=tcp::2222-:22 -device virtio-net,netdev=n1 -cdrom alpine-virt-3.16.0-x86_64.iso -nographic < guest.in > guest.out &
echo starting qemu finished

echo reading stdout
( while read line ; do echo ">>> $line" ; done ) < guest.out
