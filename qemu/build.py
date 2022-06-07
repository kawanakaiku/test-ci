from subprocess import Popen, PIPE, DEVNULL
import time

import logging
logging.basicConfig(level=logging.DEBUG)
logger = logging.getLogger(__name__)


# start qemu process
logger.info("starting qemu")

command = ["qemu-system-x86_64"]
command += "-machine q35 -m 1024 -smp cpus=2 -cpu qemu64 -nographic".split()
command += " -drive if=pflash,format=raw,read-only,file=edk2-x86_64-code.fd".split()
command += "-netdev user,id=n1,hostfwd=tcp::2222-:22".split()
command += "-device virtio-net,netdev=n1".split()
command += "-cdrom alpine-virt-3.16.0-x86_64.iso".split()

process = Popen(command, stdin=PIPE, stdout=PIPE, stderr=DEVNULL, text=True, encoding="utf-8", errors="ignore")

logger.info("started qemu in background")


# communicate with qemu
def read():
    return process.stdout.readline()

def write(message):
    process.stdin.write(message + "\n")
    process.stdin.flush()

def answer(pattern, message):    
    while True:
        data = read()
        if data == "":
            print("Writer closed")
            break
        data = data.strip()
        if data != "":
            logger.info('<<< ' + data)
        if pattern in data:
            time.sleep(1)
            write(message)
            logger.info('>>> ' + message)
            break

logger.info("Opening FIFO...")
write("")
    
answer("Welcome to Alpine Linux", "root")
time.sleep(3)
