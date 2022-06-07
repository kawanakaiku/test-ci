from subprocess import Popen, PIPE, DEVNULL
from time import sleep

import logging
logging.basicConfig(level=logging.DEBUG)
logger = logging.getLogger(__name__)


# start qemu process
logger.info("starting qemu")

command = ["qemu-system-x86_64"]
command += "-machine q35 -m 2048 -smp cpus=2 -cpu qemu64 -nographic".split()
command += " -drive if=pflash,format=raw,read-only,file=edk2-x86_64-code.fd".split()
command += "-netdev user,id=n1,hostfwd=tcp::2222-:22".split()
command += "-device virtio-net,netdev=n1".split()
command += "-cdrom alpine-virt-3.16.0-x86_64.iso".split()
command += ["qemu.img"]

process = Popen(command, stdin=PIPE, stderr=DEVNULL, text=True, encoding="utf-8", errors="ignore")

logger.info("started qemu in background")


# communicate with qemu
def write(message):
    logger.info(f"writing '{message}'")
    process.stdin.write(message + "\n")
    process.stdin.flush()

def wait_shutdown():    
    process.wait()

write("")
sleep(100)

write("root")
sleep(3)

write("setup-interfaces")
sleep(3)
write("")
sleep(1)
write("")
sleep(1)
write("")
sleep(1)

write("ifup eth0")
sleep(10)

write(r"""sed -i -E 's/(local kernel_opts)=.*/\1="console=ttyS0"/' /sbin/setup-disk""")
sleep(3)

write(r"""sed -i -e 's/ext4) mkfs_args="$mkfs_args -O ^64bit";;/ext4) mkfs_args="$mkfs_args -O ^64bit,^has_journal";;/' /sbin/setup-disk""")
sleep(3)

write("wget https://github.com/kawanakaiku/test-ci/releases/download/alpine/answerfile")
sleep(10)

write("setup-alpine -e -f answerfile")
sleep(20)
# passwd
write("")
sleep(1)
write("")
sleep(5)
write("")
sleep(5)
write("y")
sleep(10)

write("poweroff")
wait_shutdown()
