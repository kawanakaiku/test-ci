#!/usr/bin/env python3
"""実カーネルに XFS イメージのログを再生させて、結果のツリーを取り出す (docs/xfs-write.md)。
使い方: kreplay.py [--post] [--backend auto|loop|vm] <img>...    kreplay.py [--backend ...] --probe   (使えるバックエンドを調べる。無ければ 77)
  各イメージを実カーネルで rw マウントし (ここでカーネルがログを再生する)、ツリー全体を "種別 サイズ sha256 パス" (tools/ops_ref.py --tree と同じ書式) で <img>.ktree に書く。
  --post: そのあとカーネルに追加の書き込み (mkdir / ファイル / シンボリックリンク / fsync) をさせてからアンマウントする (自前が書いた空き領域・inode の木をカーネルが信用して使えるかの検査)。
  結果は <img>.kres (ok / mountfail <errno> / treefail ... など) と標準出力 "KRES <img> <結果>"。イメージはその場で変更される (再生・追加の書き込み)。呼び出し側がコピーを渡すこと。
バックエンド:
  loop = sudo -n mount -o loop (GitHub Actions など)。vm = QEMU の小さな VM (cachyos のカーネルパッケージから vmlinuz と xfs.ko を取り出す。USBMEM_XFS_KERNEL_PKG で .pkg.tar.zst を指定できる。KVM があれば使う)。
  auto = loop が使えれば loop、なければ vm。どちらも使えなければ終了コード 77 (テストは SKIP)。
"""
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(os.environ.get("XDG_CACHE_HOME", os.path.expanduser("~/.cache")), "usbmem_xfs_vm")
PKG_CANDIDATES = [
    "/run/media/u/Data/claude-code/qemu/linux-cachyos/linux-cachyos-7.1.8-1-x86_64.pkg.tar.zst",
    "/run/media/u/Data/claude-code/qemu/linux-cachyos/linux-cachyos-7.2.0-1-x86_64_v3.pkg.tar.zst",
]
DRIVES_PER_VM = 10


def tree_lines(root):
    out = []
    for dp, dns, fns in os.walk(root):
        dns.sort()
        for n in sorted(dns + fns):
            p = os.path.join(dp, n)
            r = os.path.relpath(p, root)
            if os.path.islink(p):
                out.append("l 0 %s %s" % (os.readlink(p), r))
            elif os.path.isdir(p):
                out.append("d 0 - " + r)
            else:
                try:
                    d = open(p, "rb").read()
                except OSError:
                    out.append("u 0 - " + r)
                    continue
                out.append("f %d %s %s" % (len(d), hashlib.sha256(d).hexdigest(), r))
    return sorted(out)


def post_writes(mnt):
    d = os.path.join(mnt, "kpost")
    os.makedirs(d, exist_ok=True)
    for i in range(6):
        n = min(5000 * (i + 1) * 2, 70000)
        with open(os.path.join(d, "k%d" % i), "wb") as f:
            f.write(bytes(((j * 7 + i) & 255) for j in range(n)))
            f.flush()
            os.fsync(f.fileno())
    if not os.path.lexists(os.path.join(d, "lnk")):
        os.symlink("k0", os.path.join(d, "lnk"))
    for i in range(100):
        open(os.path.join(d, "e%03d" % i), "w").close()
    os.sync()


def worker_loop(post, imgs):
    """root で実行される。loop マウントして ktree / kres を書く。"""
    for img in imgs:
        mnt = tempfile.mkdtemp(prefix="usbmem_xfsk.")
        res = "ok"
        try:
            r = subprocess.run(["mount", "-t", "xfs", "-o", "loop", img, mnt], capture_output=True, text=True)
            if r.returncode != 0:
                res = "mountfail " + (r.stderr.strip().replace("\n", " ")[:200] or str(r.returncode))
            else:
                try:
                    open(img + ".ktree", "w").write("\n".join(tree_lines(mnt)) + "\n")
                    if post:
                        post_writes(mnt)
                except Exception as e:  # noqa: BLE001
                    res = "treefail %s" % e
                u = subprocess.run(["umount", mnt], capture_output=True, text=True)
                if u.returncode != 0:
                    res = "umountfail " + u.stderr.strip()[:200]
        finally:
            try:
                os.rmdir(mnt)
            except OSError:
                pass
        open(img + ".kres", "w").write(res + "\n")
        os.chmod(img + ".kres", 0o666)
        if os.path.exists(img + ".ktree"):
            os.chmod(img + ".ktree", 0o666)
        os.chmod(img, 0o666)


def loop_ok():
    if not shutil.which("sudo") or subprocess.run(["sudo", "-n", "true"], capture_output=True).returncode != 0:
        return False
    return os.path.exists("/dev/loop-control") and shutil.which("mount") is not None


def find_pkg():
    p = os.environ.get("USBMEM_XFS_KERNEL_PKG")
    if p:
        return p if os.path.exists(p) else None
    for c in PKG_CANDIDATES:
        if os.path.exists(c):
            return c
    return None


def prepare_kernel(pkg):
    os.makedirs(CACHE, exist_ok=True)
    vm, ko = os.path.join(CACHE, "vmlinuz"), os.path.join(CACHE, "xfs.ko")
    stamp = os.path.join(CACHE, "stamp")
    key = "%s %d" % (pkg, os.path.getmtime(pkg))
    if os.path.exists(vm) and os.path.exists(ko) and os.path.exists(stamp) and open(stamp).read() == key:
        return vm, ko
    names = subprocess.run(["tar", "--zstd", "-tf", pkg], capture_output=True, text=True).stdout.split()
    want = {}
    for n in names:
        if n.endswith("/vmlinuz"):
            want["vmlinuz"] = n
        if n.endswith("/fs/xfs/xfs.ko.zst") or n.endswith("/fs/xfs/xfs.ko"):
            want["xfs"] = n
    if len(want) != 2:
        return None
    with tempfile.TemporaryDirectory() as ex:
        if subprocess.run(["tar", "--zstd", "-xf", pkg, "-C", ex] + list(want.values()), capture_output=True).returncode != 0:
            return None
        shutil.copy(os.path.join(ex, want["vmlinuz"]), vm)
        src = os.path.join(ex, want["xfs"])
        if src.endswith(".zst"):
            if subprocess.run(["zstd", "-dq", "-f", src, "-o", ko], capture_output=True).returncode != 0:
                return None
        else:
            shutil.copy(src, ko)
    open(stamp, "w").write(key)
    return vm, ko


def vm_ok():
    for t in ("qemu-system-x86_64", "gcc", "cpio", "gzip", "zstd", "tar"):
        if not shutil.which(t):
            return None
    pkg = find_pkg()
    if not pkg:
        return None
    return prepare_kernel(pkg)


def run_vm(kern, post, imgs):
    vmlinuz, ko = kern
    initbin = os.path.join(CACHE, "init")
    src = os.path.join(HERE, "init.c")
    if not os.path.exists(initbin) or os.path.getmtime(initbin) < os.path.getmtime(src):
        if subprocess.run(["gcc", "-static", "-O2", "-o", initbin, src], capture_output=True).returncode != 0:
            raise RuntimeError("static gcc failed")
    kvm = os.access("/dev/kvm", os.R_OK | os.W_OK)
    for s in range(0, len(imgs), DRIVES_PER_VM):
        batch = imgs[s:s + DRIVES_PER_VM]
        with tempfile.TemporaryDirectory(dir=os.environ.get("USBMEM_WORK_DIR", "/dev/shm") if os.path.isdir("/dev/shm") else None) as td:
            root = os.path.join(td, "initrd")
            os.makedirs(os.path.join(root, "modules"))
            for d in ("proc", "sys", "dev", "mnt"):
                os.makedirs(os.path.join(root, d))
            shutil.copy(initbin, os.path.join(root, "init"))
            os.chmod(os.path.join(root, "init"), 0o755)
            shutil.copy(ko, os.path.join(root, "modules", "xfs.ko"))
            lines = ["/modules/xfs.ko", "1" if post else "0"] + ["/dev/vd" + chr(ord("a") + i) for i in range(len(batch))]
            open(os.path.join(root, "job.txt"), "w").write("\n".join(lines) + "\n")
            initrd = os.path.join(td, "initrd.gz")
            r = subprocess.run("cd %s && find . | cpio -o -H newc -R 0:0 --quiet | gzip -1 > %s" % (root, initrd), shell=True, capture_output=True, text=True)
            if r.returncode != 0:
                raise RuntimeError("cpio failed: " + r.stderr[-200:])
            log = os.path.join(td, "serial.log")
            cmd = ["qemu-system-x86_64", "-machine", "q35", "-m", "768", "-smp", "2", "-display", "none", "-no-reboot",
                   "-kernel", vmlinuz, "-initrd", initrd, "-append", "console=ttyS0 panic=-1 loglevel=4 rdinit=/init",
                   "-serial", "file:" + log, "-monitor", "none"]
            cmd += ["-enable-kvm", "-cpu", "host"] if kvm else ["-cpu", "max"]
            for i, im in enumerate(batch):
                cmd += ["-drive", "file=%s,format=raw,if=none,id=d%d,cache=unsafe" % (im, i), "-device", "virtio-blk-pci,drive=d%d" % i]
            try:
                subprocess.run(cmd, capture_output=True, text=True, timeout=1800)
            except subprocess.TimeoutExpired:
                pass
            txt = open(log, errors="replace").read() if os.path.exists(log) else ""
            trees = {i: [] for i in range(len(batch))}
            res = {}
            other = {i: [] for i in range(len(batch))}
            done = False
            for ln in txt.splitlines():
                if ln.startswith("K|"):
                    parts = ln.split("|", 2)
                    if len(parts) < 3:
                        continue
                    try:
                        i = int(parts[1])
                    except ValueError:
                        continue
                    body = parts[2]
                    if body == "INIT-DONE":
                        done = True
                    elif body.startswith("RESULT "):
                        res[i] = body[7:]
                    elif body.startswith("treefail") or body.startswith("postfail"):
                        other.setdefault(i, []).append(body)
                    elif i in trees:
                        trees[i].append(body)
                elif "XFS" in ln or "xfs" in ln:
                    for i in other:
                        pass
            kern_msgs = [l for l in txt.splitlines() if ("XFS" in l or "xfs" in l) and not l.startswith("K|")]
            for i, im in enumerate(batch):
                r = res.get(i, "vm-incomplete" if not done else "no-result")
                if other.get(i) and r == "ok":
                    r = other[i][0]
                if r != "ok" and kern_msgs:
                    r += " [" + "; ".join(kern_msgs[-3:])[:300] + "]"
                open(im + ".kres", "w").write(r + "\n")
                if trees[i] or r.startswith("ok"):
                    open(im + ".ktree", "w").write("\n".join(sorted(trees[i])) + ("\n" if trees[i] else ""))
                elif os.path.exists(im + ".ktree"):
                    os.remove(im + ".ktree")


def main():
    args = sys.argv[1:]
    post = False
    backend = "auto"
    if args and args[0] == "--worker-loop":
        worker_loop(args[1] == "1", args[2:])
        return 0
    while args and args[0].startswith("--"):
        a = args.pop(0)
        if a == "--probe":   # 使えるバックエンドがあれば 0 (名前を標準出力)、無ければ 77
            if backend in ("auto", "loop") and loop_ok():
                print("loop")
                return 0
            if backend in ("auto", "vm") and vm_ok():
                print("vm")
                return 0
            return 77
        if a == "--post":
            post = True
        elif a == "--backend":
            backend = args.pop(0)
        else:
            print("unknown option", a, file=sys.stderr)
            return 2
    imgs = [os.path.abspath(a) for a in args]
    for im in imgs:
        for suf in (".ktree", ".kres"):
            if os.path.exists(im + suf):
                os.remove(im + suf)
    use = backend
    kern = None
    if use in ("auto", "loop") and loop_ok():
        use = "loop"
    elif use in ("auto", "vm"):
        kern = vm_ok()
        use = "vm" if kern else "none"
    else:
        use = "none"
    if use == "none":
        print("SKIP: no kernel backend (need sudo loop mount, or QEMU + a kernel package)", file=sys.stderr)
        return 77
    if use == "loop":
        for s in range(0, len(imgs), 40):
            r = subprocess.run(["sudo", "-n", "python3", "-I", os.path.abspath(__file__), "--worker-loop", "1" if post else "0"] + imgs[s:s + 40])
            if r.returncode != 0:
                print("worker failed", r.returncode, file=sys.stderr)
                return 1
    else:
        run_vm(kern, post, imgs)
    bad = 0
    for im in imgs:
        r = open(im + ".kres").read().strip() if os.path.exists(im + ".kres") else "missing"
        print("KRES %s %s" % (im, r))
        if not r.startswith("ok"):
            bad += 1
    print("backend: %s" % use, file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
