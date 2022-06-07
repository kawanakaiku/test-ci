import subprocess
import time

import logging
logging.basicConfig(level=logging.DEBUG, filename="log.txt")
logger = logging.getLogger(__name__)

import os
import errno

FIFO = 'guest'

os.mkfifo(FIFO)

os.system("qemu-system-x86_64 -serial pipe:guest -machine q35 -m 1024 -smp cpus=2 -cpu qemu64 -drive if=pflash,format=raw,read-only,file=edk2-x86_64-code.fd -netdev user,id=n1,hostfwd=tcp::2222-:22 -device virtio-net,netdev=n1 -cdrom alpine-virt-3.16.0-x86_64.iso -nographic &")

print("Opening FIFO...")
with open(FIFO) as fifo:
    print("FIFO opened")
    while True:
        data = fifo.readline()
        if len(data) == 0:
            print("Writer closed")
            break
