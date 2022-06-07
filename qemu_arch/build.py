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

def write(message):
    logger.info(f"writing '{message}'")
    process.stdin.write(message + "\n")
    process.stdin.flush()

def wait_shutdown():    
    process.wait()


start_qemu(cdrom=True)
    
write("")
sleep(120)

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



start_qemu(cdrom=False)

write("")
sleep(100)

write("root")
sleep(3)

write(r"""sed -i -E 's/relatime/noatime/g ; s/[0-9]+$/0/g ; s/(\s+?ext4\s+?\S+)/\1,barrier=0/g' /etc/fstab""")
sleep(3)

write("rm /etc/motd")
sleep(3)

write(r"""sed -i -E 's/GRUB_TIMEOUT=\S+?/GRUB_TIMEOUT=0/ ; s/(GRUB_CMDLINE_LINUX_DEFAULT=".*?)"/\1 mitigations=off fsck.mode=skip nowatchdog selinux=0 apparmor=0 lsm="/' /etc/default/grub""")

write("grub-mkconfig -o /boot/grub/grub.cfg")
sleep(10)

write("apk update && apk --no-cache add docker htop ncdu git ; service docker start ; rc-update add docker ; apk cache clean")
sleep(30)

write("reboot")
sleep(100)

write("root")
sleep(100)

write("poweroff")
wait_shutdown()
