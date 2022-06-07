import subprocess
import time

import logging
logging.basicConfig(level=logging.DEBUG, filename="log.txt")
logger = logging.getLogger(__name__)

import os
import errno

GUEST_IN = 'guest.in'
GUEST_OUT = 'guest.out'

os.mkfifo(GUEST_IN)
os.mkfifo(GUEST_OUT)

logger.info("starting qemu")
os.system(f"qemu-system-x86_64 -machine q35 -m 1024 -smp cpus=2 -cpu qemu64 -drive if=pflash,format=raw,read-only,file=edk2-x86_64-code.fd -netdev user,id=n1,hostfwd=tcp::2222-:22 -device virtio-net,netdev=n1 -cdrom alpine-virt-3.16.0-x86_64.iso -nographic <{GUEST_IN} >{GUEST_OUT} &")
logger.info("started qemu in background")

logger.info("Opening FIFO...")
with open(GUEST_IN, 'r', encoding='utf-8') as stdin, open(GUEST_OUT, 'w', encoding='utf-8') as stdout:
    logger.info("i/o opened")
    stdin.write('')
    while True:
        data = stdout.readline()
        if len(data) == 0:
            print("Writer closed")
            break
        logger.info('>>>' + data)
    
