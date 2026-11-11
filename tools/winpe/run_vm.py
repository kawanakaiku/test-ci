#!/usr/bin/env python3
"""WinPE を QEMU/KVM で起動し、autounattend.xml 経由でスクリプトを実行させ、終了 (シャットダウン) まで待つ。
使い方: run_vm.py <work_dir> <win_iso> [timeout_sec]
 work_dir に payload.iso (スクリプト) と target*.raw (空のディスク) がある前提。画面は screendump_*.ppm に定期保存 (診断用)。
"""
import json, os, socket, subprocess, sys, time

work, iso = sys.argv[1], sys.argv[2]
timeout = int(sys.argv[3]) if len(sys.argv) > 3 else 900
sock = os.path.join(work, "qmp.sock")
if os.path.exists(sock): os.unlink(sock)
disks = sorted(f for f in os.listdir(work) if f.startswith("target") and f.endswith(".raw"))
args = ["qemu-system-x86_64", "-enable-kvm", "-cpu", "host", "-smp", "4", "-m", "3072", "-machine", "pc",
        "-display", "none", "-vga", "std", "-qmp", f"unix:{sock},server=on,wait=off", "-no-reboot", "-boot", "order=d"]
for i, d in enumerate(disks[:2]):
    args += ["-drive", f"file={os.path.join(work, d)},format=raw,if=ide,index={i}"]
# usb4k*.raw: USB 接続の 4Kn ディスク (論理/物理セクタ 4096)。実際の USB メモリ/外付けドライブに近い経路。
for i, d in enumerate(sorted(f for f in os.listdir(work) if f.startswith("usb4k") and f.endswith(".raw"))):
    args += ["-device", "qemu-xhci,id=xhci%d" % i, "-drive", f"if=none,id=usbd{i},file={os.path.join(work, d)},format=raw",
             "-device", f"usb-storage,bus=xhci{i}.0,drive=usbd{i},logical_block_size=4096,physical_block_size=4096"]
args += ["-drive", f"file={iso},media=cdrom,if=ide,index=2,readonly=on", "-drive", f"file={os.path.join(work, 'payload.iso')},media=cdrom,if=ide,index=3,readonly=on"]
p = subprocess.Popen(args)
for _ in range(50):
    if os.path.exists(sock): break
    time.sleep(0.2)
q = socket.socket(socket.AF_UNIX); q.connect(sock); f = q.makefile("rw")
f.readline()
def cmd(c, **a):
    f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
    while True:
        r = json.loads(f.readline())
        if "return" in r or "error" in r: return r
cmd("qmp_capabilities")
t0 = time.time(); n = 0
while p.poll() is None and time.time() - t0 < timeout:
    el = time.time() - t0
    if el < 25:   # 「Press any key to boot from CD or DVD」を通す
        cmd("send-key", keys=[{"type": "qcode", "data": "spc"}])
    if int(el) // 15 != n:   # 15 秒ごとに画面保存 (診断用)
        n = int(el) // 15
        cmd("screendump", filename=os.path.join(work, f"screendump_{n:03d}.ppm"))
    time.sleep(1 if el < 25 else 2)
if p.poll() is None:
    cmd("screendump", filename=os.path.join(work, "screendump_timeout.ppm")); time.sleep(1)
    p.kill(); print("TIMEOUT", file=sys.stderr); sys.exit(1)
print("vm exited", p.returncode, f"after {int(time.time()-t0)}s")
