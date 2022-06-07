from subprocess import Popen, PIPE, DEVNULL
from time import sleep

import logging
logging.basicConfig(level=logging.DEBUG)
logger = logging.getLogger(__name__)


def start_qemu(cdrom=True):
    global process
    logger.info("starting qemu")
    command = ["qemu-system-x86_64"]
    command += "-machine q35 -m 2048 -smp cpus=2 -cpu qemu64 -nographic".split()
    command += "-drive if=pflash,format=raw,read-only,file=edk2-x86_64-code.fd".split()
    command += "-netdev user,id=n1,hostfwd=tcp::2222-:22".split()
    command += "-device virtio-net,netdev=n1".split()
    if cdrom:
        command += "-cdrom archlinux-2022.06.01-x86_64.iso".split()
    command += ["qemu_arch.img"]
    process = Popen(command, stdin=PIPE, stderr=DEVNULL, text=True, encoding="utf-8", errors="ignore")
    logger.info("started qemu in background")

def write(message, end="\n"):
    logger.info(f"writing '{message}'")
    process.stdin.write(message + end)
    process.stdin.flush()

def wait_shutdown():    
    process.wait()


start_qemu(cdrom=True)
    
# edit boot param
write("")
sleep(10)
write("\t", end="")
sleep(3)
write(" console=tty0 console=ttyS0,115200")
sleep(100)

write("root")
sleep(30)

write("poweroff")
wait_shutdown()
