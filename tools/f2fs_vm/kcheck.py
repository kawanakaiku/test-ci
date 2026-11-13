#!/usr/bin/env python3
"""本物の Linux カーネル (QEMU/TCG の小さな VM。tools/f2fs_vm/gen_k_fixtures.py と同じ仕組み) に F2FS イメージをマウントさせ、ツリーをダンプする。ルート権限は不要。
使い方: kcheck.py [--ops <ops.txt>] [--ro] [--mntopts <opts>] <img> <out.tree>
  既定: img を書き込み可でマウント → (--ops があれば操作列を POSIX 操作として実行) → ツリーを <out.tree> に書く (tools/ops_ref.py --tree と同じ書式、ソート済み) → アンマウント。img はカーネルが書き換える。
  --ro: 読み取り専用でマウントし、ダンプだけ (img は変わらない)。
終了コード: 0 = 成功 (カーネルがマウントでき、F2FS-fs のエラー / 警告ログが無い) / 1 = 失敗 (ログを標準エラーへ) / 77 = カーネル・qemu などが無い (SKIP)。
用途 (docs/f2fs-write.md): (A) 自前ドライバが書いたイメージをカーネルが読めて、ツリーが期待と一致する / (B) そのうえでカーネルが書き足しても fsck.f2fs が clean /
  (C) カーネルが書いたイメージ (--ops) を自前ドライバが読めて、カーネルのツリーと一致する。"""
import importlib.util, os, re, shutil, subprocess, sys, tempfile
sys.dont_write_bytecode = True   # tools/__pycache__ はリポジトリに入っている: 実行のたびに書き換えない

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("gen_k_fixtures", os.path.join(HERE, "gen_k_fixtures.py"))
gk = importlib.util.module_from_spec(spec); spec.loader.exec_module(gk)

BAD = re.compile(r"inconsistent|corrupt|invalid|error|fail|unsupported|mismatch|unusual|abnormal|stopped|readonly|read-only", re.I)
BENIGN = re.compile(r"Mounted with checkpoint version|Mounting|mount.*quota|segment|lazytime|discard", re.I)

def main():
    a = sys.argv[1:]
    probe = bool(a) and a[0] == "--probe"   # 使えるか (カーネル・qemu・gcc などが揃っているか) だけ調べる: 0 / 77
    if probe: a = a[1:]
    ops = None; ro = False; mntopts = "-"
    while a and a[0].startswith("--"):
        if a[0] == "--ops": ops = a[1]; a = a[2:]
        elif a[0] == "--ro": ro = True; a = a[1:]
        elif a[0] == "--mntopts": mntopts = a[1]; a = a[2:]
        else: print("bad option", a[0]); return 2
    if len(a) != 2 and not probe: print(__doc__); return 2
    img, out = (a + [None, None])[:2]
    for t in ("qemu-system-x86_64", "gcc", "cpio", "gzip", "zstd", "tar"):
        if not gk.need(t): print("SKIP: %s not found" % t); return 77
    pkg = gk.find_kernel()
    if not pkg: print("SKIP: no kernel package (set USBMEM_F2FS_KERNEL_PKG)"); return 77
    kern = gk.prepare_kernel(pkg)
    if not kern: print("SKIP: could not extract vmlinuz/f2fs.ko from", pkg); return 77
    if probe: return 0
    os.makedirs(gk.CACHE, exist_ok=True)
    initbin = os.path.join(gk.CACHE, "init_w")
    src = os.path.join(HERE, "init.c")
    if not os.path.exists(initbin) or os.path.getmtime(initbin) < os.path.getmtime(src):
        if gk.run(["gcc", "-static", "-O2", "-o", initbin, src]).returncode != 0: print("SKIP: static gcc failed"); return 77
    with tempfile.TemporaryDirectory(dir=os.environ.get("USBMEM_WORK_DIR", "/dev/shm")) as td:
        job = ["LOADMOD /modules/%s" % os.path.basename(m) for m in kern["mods"]]
        job.append("MOUNT /dev/vda %s" % ("ro" if ro else mntopts))
        if ops: job.append("OPS /ops.txt")
        job.append("DUMPTREE")
        if not ro: job.append("UMOUNT")
        root = os.path.join(td, "initrd"); os.makedirs(os.path.join(root, "modules"))
        shutil.copy(initbin, os.path.join(root, "init")); os.chmod(os.path.join(root, "init"), 0o755)
        for m in kern["mods"]: shutil.copy(m, os.path.join(root, "modules"))
        for d in ("proc", "sys", "dev", "mnt"): os.makedirs(os.path.join(root, d), exist_ok=True)
        if ops: shutil.copy(ops, os.path.join(root, "ops.txt"))
        open(os.path.join(root, "job.txt"), "w").write("\n".join(job) + "\n")
        initrd = os.path.join(td, "initrd.gz")
        r = subprocess.run("cd %s && find . | cpio -o -H newc -R 0:0 --quiet | gzip -1 > %s" % (root, initrd), shell=True, capture_output=True, text=True)
        if r.returncode != 0: print("cpio failed", r.stderr[-200:]); return 1
        log = os.path.join(td, "serial.log")
        cmd = ["qemu-system-x86_64", "-machine", "q35", "-cpu", "max", "-m", "1024", "-smp", "2", "-display", "none", "-no-reboot",
               "-kernel", kern["vmlinuz"], "-initrd", initrd, "-append", "console=ttyS0 panic=-1 loglevel=6 rdinit=/init",
               "-drive", "file=%s,format=raw,if=virtio,cache=unsafe%s" % (img, ",readonly=on" if ro else ""), "-serial", "file:" + log, "-monitor", "none"]
        try: subprocess.run(cmd, capture_output=True, text=True, timeout=3000)
        except subprocess.TimeoutExpired: print("qemu timed out"); return 1
        txt = open(log, errors="replace").read() if os.path.exists(log) else ""
        if "INIT-DONE" not in txt or "INIT-FAIL" in txt or "TREE-END" not in txt:
            sys.stderr.write(txt[-4000:] + "\n"); print("VM job failed"); return 1
        lines = sorted(l[5:].rstrip("\r") for l in txt.splitlines() if l.startswith("TREE|"))
        with open(out, "w", encoding="utf-8", errors="surrogateescape") as f:
            f.write("\n".join(lines) + ("\n" if lines else ""))
        bad = [l for l in txt.splitlines() if "F2FS-fs" in l and BAD.search(l) and not BENIGN.search(l)]
        failed_ops = [l for l in txt.splitlines() if l.startswith("OPS-FAIL")]
        if bad or failed_ops:
            sys.stderr.write("\n".join(bad + failed_ops) + "\n"); print("kernel reported problems"); return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
