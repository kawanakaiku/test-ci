import subprocess
import time

import logging
logging.basicConfig(level=logging.DEBUG, filename="log.txt")
logger = logging.getLogger(__name__)

class process:
    def __init__(self, command):
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, encoding="utf-8", bufsize=1)
        
    def read(self):
        self.process.stdout.flush()
        output = self.process.stdout.readline().rstrip('\n')
        logger.info(f">>> {output}")
        return output

    def write(self, input):
        self.process.stdin.write(input+"\n")
        self.process.stdin.flush()
        logger.info(f"<<< {input}")

    def terminate(self):
        self.process.stdin.close()
        self.process.terminate()
        self.process.wait(timeout=1)

    def close(self):
        self.process.stdin.close()
        self.process.wait()
        returncode = self.process.returncode
        logger.info(f"returncode={returncode}")
        
    def answer(self, match, input):
        while True:
            output = self.read()
            if match in output:
                self.write(input)
                return

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        self.close()
        
        
with process("qemu-system-x86_64 -machine q35 -m 1024 -smp cpus=2 -cpu qemu64 -drive if=pflash,format=raw,read-only,file=edk2-x86_64-code.fd -netdev user,id=n1,hostfwd=tcp::2222-:22 -device virtio-net,netdev=n1 -cdrom alpine-virt-3.16.0-x86_64.iso -nographic".split()) as p:
    while True:
        p.read()
