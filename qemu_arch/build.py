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
    command += "-drive if=pflash,format=raw,read-only,file=edk2-x86_64-code.fd,index=0".split()
    command += "-net nic,model=virtio -net user,hostfwd=tcp::2222-:22".split()
    if cdrom:
        command += "-cdrom archlinux-2022.06.01-x86_64.iso".split()
    command += "-drive file=qemu_arch.img,if=virtio,media=disk".split()
    process = Popen(command, stdin=PIPE, stderr=DEVNULL, text=True, encoding="utf-8", errors="ignore")
    logger.info("started qemu in background")

def write(message="", end="\n"):
    logger.info(f"writing '{message}'")
    process.stdin.write(message + end)
    process.stdin.flush()

def wait_shutdown():    
    process.wait()


start_qemu(cdrom=True)
    
# edit boot param
write(); sleep(10)
write("\t", end=""); sleep(3)
write(" console=tty0 console=ttyS0,115200"); sleep(100)

write("root"); sleep(30)

write("pacman -Sy --noconfirm ncdu"); sleep(100)

write(r"""echo -en 'g\nn\n\n\n+100M\nt\n1\nn\n\n\n\nw\n' | fdisk /dev/vda"""); sleep(5)

write(r"""mkfs.fat -F 32 /dev/vda1 && mkfs.ext4 /dev/vda2"""); sleep(5)

write(r"""mount -t ext4 -o noatime,nobarrier /dev/vda2 /mnt && mkdir -p /mnt/boot/efi && mount -t vfat -o noatime /dev/vda1 /mnt/boot/efi"""); sleep(5)

write(r"""pacstrap /mnt base linux && genfstab -U /mnt >> /mnt/etc/fstab"""); sleep(300)

write("arch-chroot /mnt"); sleep(3)

write("sudo pacman -Scc"); sleep(3)

write("exit"); sleep(3)

write("poweroff")
wait_shutdown()
