import subprocess
import time

import logging
logging.basicConfig(level=logging.DEBUG, filename="log.txt")
logger = logging.getLogger(__name__)

import os
import errno

GUEST_IN = 'guest.in'
GUEST_OUT = 'guest.out'

os.mkfifo(FIFO)

os.system("qemu-system-x86_64 -serial pipe:guest -machine q35 -m 1024 -smp cpus=2 -cpu qemu64 -drive if=pflash,format=raw,read-only,file=edk2-x86_64-code.fd -netdev user,id=n1,hostfwd=tcp::2222-:22 -device virtio-net,netdev=n1 -cdrom alpine-virt-3.16.0-x86_64.iso -nographic &")

print("Opening FIFO...")
with open(GUEST_IN, 'rb') as in, open(GUEST_OUT, 'rb') as out:
    print("in/out opened")
    while True:
        data = out.readline()
        if len(data) == 0:
            print("Writer closed")
            break
        print(data.decode('utf-8', errors='ignore'))
    
