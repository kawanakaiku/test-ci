#!/usr/bin/env python3
"""本物の Linux カーネルに F2FS を書かせたテスト用イメージ (k_*) を作る。ルート権限は不要。
使い方: tools/f2fs_vm/gen_k_fixtures.py <outdir> [name ...]
仕組み: ホストで mkfs.f2fs したイメージを、QEMU (TCG。KVM は不要) の小さな VM にディスクとして渡す。VM は cachyos のカーネル (パッケージから取り出した vmlinuz と f2fs.ko。
  環境変数 USBMEM_F2FS_KERNEL_PKG で .pkg.tar.zst を指定できる) を起動し、static な /init (init.c) が f2fs をマウントして、ツリーのコピー・圧縮の書き込み・疎ファイル・fsync などを行い、電源を切る。
  圧縮 (compress_algorithm=lz4/lzo/lzo-rle/zstd)・インライン・ノード木の深いところ (間接 / 二重間接)・チェックポイント後の fsync (roll-forward が必要な状態) のイメージは、
  mkfs.f2fs / sload.f2fs では作れないので、カーネルに作らせる。
出力: <outdir>/k_*.img と k_*.manifest (tools/gen_f2fs_fixture.py と同じ書式)。カーネル / qemu / gcc / cpio が無ければ何も作らずに終了コード 77 (テストは SKIP)。
独立した検証: きれいにアンマウントしたイメージは fsck.f2fs が全項目 Ok (内容の期待値はコピー元のツリー)。k_unclean は roll-forward が必要な状態 (fsck は検査しない)。"""
import glob, importlib.util, os, shutil, subprocess, sys, tarfile, tempfile

os.environ["PATH"] = os.environ.get("PATH", "") + ":/sbin:/usr/sbin:" + os.path.expanduser("~/.local/bin")
HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
spec = importlib.util.spec_from_file_location("gen_squashfs_fixture", os.path.join(TOOLS, "gen_squashfs_fixture.py"))
sq = importlib.util.module_from_spec(spec); spec.loader.exec_module(sq)
ext2 = sq.ext2
MTIME_BASE = 1700000000
DEFAULT_PKGS = [
    "/run/media/u/Data/claude-code/qemu/linux-cachyos/linux-cachyos-7.1.8-1-x86_64.pkg.tar.zst",   # 汎用 (x86-64 v1) のカーネル: TCG で確実に動く
    "/run/media/u/Data/claude-code/qemu/linux-cachyos/linux-cachyos-7.2.0-1-x86_64_v3.pkg.tar.zst",
]
CACHE = os.path.join(tempfile.gettempdir(), "usbmem_f2fs_vm_cache_%s" % os.environ.get("USER", "u"))

MODERN = "extra_attr,inode_checksum,sb_checksum,inode_crtime,lost_found"
FLEX = MODERN + ",flexible_inline_xattr"
GIB = 1 << 30
# name, mkfs -O, mount options, image bytes, ジョブの種類
SCENARIOS = [
    ("k_basic",         FLEX,                       "",                                                         12 * GIB, "basic_deep"),
    ("k_nofeat",        "",                         "",                                                         512 << 20, "basic"),
    ("k_ext",           MODERN,                     "",                                                         512 << 20, "basic"),
    ("k_comp_lz4_c2",   FLEX + ",compression",     "compress_algorithm=lz4,compress_log_size=2,compress_extension=*",   512 << 20, "basic"),
    ("k_comp_lz4_c4",   FLEX + ",compression",     "compress_algorithm=lz4,compress_log_size=4,compress_extension=*",   512 << 20, "basic"),
    ("k_comp_lz4_chk",  FLEX + ",compression",     "compress_algorithm=lz4,compress_log_size=2,compress_extension=*,compress_chksum", 512 << 20, "basic"),
    ("k_comp_lzo_c2",   FLEX + ",compression",     "compress_algorithm=lzo,compress_log_size=2,compress_extension=*",   512 << 20, "basic"),
    ("k_comp_lzorle_c3", FLEX + ",compression",    "compress_algorithm=lzo-rle,compress_log_size=3,compress_extension=*", 512 << 20, "basic"),
    ("k_comp_zstd_c2",  FLEX + ",compression",     "compress_algorithm=zstd:3,compress_log_size=2,compress_extension=*", 512 << 20, "basic"),
    ("k_unclean",       FLEX,                       "",                                                         512 << 20, "unclean"),
    ("k_dirty",         FLEX,                       "",                                                         512 << 20, "dirty"),
]

def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)

def find_kernel():
    pkgs = [os.environ["USBMEM_F2FS_KERNEL_PKG"]] if os.environ.get("USBMEM_F2FS_KERNEL_PKG") else DEFAULT_PKGS
    for p in pkgs:
        if os.path.exists(p): return p
    return None

def prepare_kernel(pkg):
    """パッケージから vmlinuz と f2fs.ko / lz4hc_compress.ko / lz4_compress.ko を取り出して .ko に展開する。{'vmlinuz':..., 'mods':[...]} を返す。"""
    os.makedirs(CACHE, exist_ok=True)
    stamp = os.path.join(CACHE, "stamp")
    key = pkg + ":" + str(os.path.getmtime(pkg))
    mods = ["lz4_compress", "lz4hc_compress", "f2fs"]   # 読み込み順
    if os.path.exists(stamp) and open(stamp).read() == key and all(os.path.exists(os.path.join(CACHE, m + ".ko")) for m in mods) and os.path.exists(os.path.join(CACHE, "vmlinuz")):
        return {"vmlinuz": os.path.join(CACHE, "vmlinuz"), "mods": [os.path.join(CACHE, m + ".ko") for m in mods]}
    names = run(["tar", "--zstd", "-tf", pkg]).stdout.split()
    want = {}
    for n in names:
        if n.endswith("/vmlinuz"): want["vmlinuz"] = n
        for m in mods:
            if n.endswith("/" + m + ".ko.zst") or n.endswith("/" + m + ".ko"): want[m] = n
    if "vmlinuz" not in want or "f2fs" not in want:
        return None
    ex = os.path.join(CACHE, "x"); shutil.rmtree(ex, ignore_errors=True); os.makedirs(ex)
    r = run(["tar", "--zstd", "-xf", pkg, "-C", ex] + list(want.values()))
    if r.returncode != 0: return None
    shutil.copy(os.path.join(ex, want["vmlinuz"]), os.path.join(CACHE, "vmlinuz"))
    out = []
    for m in mods:
        if m not in want: continue   # 組み込みの場合は不要
        src = os.path.join(ex, want[m]); dst = os.path.join(CACHE, m + ".ko")
        if src.endswith(".zst"):
            if run(["zstd", "-dq", "-f", src, "-o", dst]).returncode != 0: return None
        else: shutil.copy(src, dst)
        out.append(dst)
    open(stamp, "w").write(key)
    shutil.rmtree(ex, ignore_errors=True)
    return {"vmlinuz": os.path.join(CACHE, "vmlinuz"), "mods": out}

def need(tool):
    return shutil.which(tool) is not None

def build_tree(td, outdir, name):
    tree = os.path.join(td, "tree"); os.makedirs(tree)
    sq.REFDIR[0] = outdir; sq.REFNAME[0] = name
    man = ext2.build(tree, 512, extra=sq.extra_for(4096))
    fifo = os.path.join(tree, "special_fifo")
    if os.path.exists(fifo): os.remove(fifo)
    man = [l for l in man if not l.startswith("S\tspecial_fifo")]
    return tree, man

def make(outdir, name, feats, mnt, size, kind, kern, initbin):
    with tempfile.TemporaryDirectory() as td:
        tree, man = build_tree(td, outdir, name)
        job = ["LOADMOD /modules/%s" % os.path.basename(m) for m in kern["mods"]]
        job.append("MOUNT /dev/vda %s" % (mnt if mnt else "-"))
        job.append("COPY")
        pieces = os.path.join(td, "pieces"); os.makedirs(pieces)
        n_piece = [0]
        def piece(seed, off, n):
            n_piece[0] += 1
            p = os.path.join(pieces, "p%d" % n_piece[0])
            open(p, "wb").write(ext2.pattern(seed, off, n))
            return "/pieces/p%d" % n_piece[0]
        # 特殊ファイル
        for i, (k, nm) in enumerate((("p", "special_fifo"), ("s", "special_sock"), ("c", "special_chr"), ("b", "special_blk"))):
            job.append("SPECIAL %s %s %d" % (k, nm, MTIME_BASE + 900 + i))
            man.append("S\t%s\t%s" % (nm, k))
        if kind == "basic_deep":
            # ノード木の深いところ: 直接 (inode 内 923 + 2 ノード) / 単間接 / 二重間接 / 三重 (二重間接ノード経由) に、疎にデータを置く
            regions = [(0, 8192, 11), (12 * (1 << 20) + 123, 10000, 12), (int(4.1 * GIB) + 7, 5000, 13), (int(8.3 * GIB), 4096, 14), (int(8.3 * GIB) + (5 << 20), 7777, 15)]
            total = 9 * GIB + 777
            spec = []; pos = 0
            for off, n, seed in regions:
                job.append("W deep_sparse.bin %d %s" % (off, piece(seed, off, n)))
                if off > pos: spec.append("z:%d:%d" % (pos, off - pos))
                spec.append("d:%d:%d:%d" % (off, n, seed)); pos = off + n
            job.append("T deep_sparse.bin %d" % total)
            if total > pos: spec.append("z:%d:%d" % (pos, total - pos))
            job.append("MT deep_sparse.bin %d" % (MTIME_BASE + 950))
            man.append("F\tdeep_sparse.bin\t%d\t%d\t1\t%s" % (total, MTIME_BASE + 950, ",".join(spec)))
        if kind == "unclean":
            # チェックポイント (SYNC) のあとに fsync だけして電源を切る: fsync マークのノードがチェックポイントの後に残る
            job.append("SYNC")
            job.append("APPENDFSYNC small.txt %s" % piece(40, 0, 600))
            job.append("APPENDFSYNC direct_max.bin %s" % piece(41, 0, 3000))   # (新しいファイルの fsync は、親ディレクトリが未記録のため完全なチェックポイントになってしまい、roll-forward が不要になる)
            job.append("POWEROFF")
        elif kind == "dirty":
            job.append("SYNC")
            job.append("W unsynced.bin 0 %s" % piece(42, 0, 9000))   # fsync も sync もしない: チェックポイントの内容がそのまま見える (回復は不要)
            job.append("POWEROFF")
        else:
            job.append("UMOUNT")
        # initramfs
        root = os.path.join(td, "initrd"); os.makedirs(os.path.join(root, "modules"))
        shutil.copy(initbin, os.path.join(root, "init")); os.chmod(os.path.join(root, "init"), 0o755)
        shutil.copytree(tree, os.path.join(root, "tree"), symlinks=True)
        shutil.copytree(pieces, os.path.join(root, "pieces"))
        for m in kern["mods"]: shutil.copy(m, os.path.join(root, "modules"))
        for d in ("proc", "sys", "dev", "mnt"): os.makedirs(os.path.join(root, d), exist_ok=True)
        open(os.path.join(root, "job.txt"), "w").write("\n".join(job) + "\n")
        initrd = os.path.join(td, "initrd.gz")
        r = subprocess.run("cd %s && find . | cpio -o -H newc -R 0:0 --quiet | gzip -1 > %s" % (root, initrd), shell=True, capture_output=True, text=True)
        if r.returncode != 0: return "cpio failed: " + r.stderr[-200:]
        # イメージ
        img = os.path.join(outdir, name + ".img")
        if os.path.exists(img): os.remove(img)
        with open(img, "wb") as f: f.truncate(size)
        r = run(["mkfs.f2fs", "-f", "-q", "-l", "USBMEM"] + (["-O", feats] if feats else []) + [img])
        if r.returncode != 0: return "mkfs.f2fs failed: " + (r.stderr or r.stdout)[-200:]
        # QEMU (TCG)
        log = os.path.join(td, "serial.log")
        cmd = ["qemu-system-x86_64", "-machine", "q35", "-cpu", "max", "-m", "1024", "-smp", "2", "-display", "none", "-no-reboot",
               "-kernel", kern["vmlinuz"], "-initrd", initrd, "-append", "console=ttyS0 panic=-1 loglevel=4 rdinit=/init",
               "-drive", "file=%s,format=raw,if=virtio,cache=unsafe" % img, "-serial", "file:" + log, "-monitor", "none"]
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=3000)
        except subprocess.TimeoutExpired:
            return "qemu timed out (see %s)" % log
        txt = open(log, errors="replace").read() if os.path.exists(log) else ""
        if "INIT-DONE" not in txt or "INIT-FAIL" in txt:
            sys.stderr.write(txt[-3000:] + "\n")
            return "VM job failed"
        if kind not in ("unclean", "dirty"):
            r = run(["fsck.f2fs", img])
            bad = [l for l in (r.stdout + r.stderr).splitlines() if "[Fail" in l]
            if r.returncode != 0 or bad:
                return "fsck.f2fs NOT clean (rc %d): %s" % (r.returncode, "; ".join(bad) or (r.stdout + r.stderr)[-300:])
        with open(os.path.join(outdir, name + ".manifest"), "w", encoding="utf-8") as f: f.write("\n".join(man) + "\n")
        print("%s: %d manifest lines, kernel-written%s" % (name, len(man), "" if kind in ("unclean", "dirty") else ", fsck.f2fs clean"))
    return None

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "/dev/shm/usbmem_f2fs"
    only = sys.argv[2:]
    for t in ("mkfs.f2fs", "fsck.f2fs", "qemu-system-x86_64", "gcc", "cpio", "gzip", "zstd", "tar"):
        if not need(t): print("SKIP: %s not found" % t); return 77
    pkg = find_kernel()
    if not pkg: print("SKIP: no kernel package (set USBMEM_F2FS_KERNEL_PKG)"); return 77
    kern = prepare_kernel(pkg)
    if not kern: print("SKIP: could not extract vmlinuz/f2fs.ko from", pkg); return 77
    os.makedirs(out, exist_ok=True)
    initbin = os.path.join(CACHE, "init")
    if run(["gcc", "-static", "-O2", "-o", initbin, os.path.join(HERE, "init.c")]).returncode != 0:
        print("SKIP: static gcc failed"); return 77
    failed = []
    for name, feats, mnt, size, kind in SCENARIOS:
        if only and name not in only: continue
        err = make(out, name, feats, mnt, size, kind, kern, initbin)
        if err:
            print("%s: FAILED (%s)" % (name, err)); failed.append(name)
    if failed:
        print("failed:", ", ".join(failed)); return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
