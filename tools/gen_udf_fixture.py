#!/usr/bin/env python3
"""UDF のテスト用イメージを作る (core/compat/tests/test_udf.cpp 用)。
使い方: tools/gen_udf_fixture.py <outdir>   (PATH に mkudffs / genisoimage / 7z / udfinfo が必要。/sbin は自動で足す。udisksctl が使えること)
作り方: mkudffs で空の UDF イメージを作り、udisksctl でループデバイスにして**ホスト Linux の UDF ドライバでマウント**し、そこへ木を書く (書き込み側の独立した実装)。
        genisoimage -udf (ISO 9660 + UDF 1.02 のブリッジ) は木から直接作る。
検証 (作成直後): 7z (独立実装。メタデータパーティションも読める) が見る名前・サイズ・種別が manifest と一致すること。
        さらにホストの UDF ドライバで読み取り専用に再マウントし、全ファイルの中身・mode・nlink・リンク先が manifest と一致すること (manifest 自体が、ディスクにあるものと合っているかの確認)。
出力: <outdir>/<name>.img と <name>.manifest (gen_iso_fixture.py / gen_ext2_fixture.py と同じ書式。V は rev/bs など)。
後始末: マウント・ループデバイスは必ず解除・削除する (例外時も)。"""
import os, sys, subprocess, tempfile, re, shutil, time, hashlib, importlib.util

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("gen_iso_fixture", os.path.join(HERE, "gen_iso_fixture.py"))
G = importlib.util.module_from_spec(spec); spec.loader.exec_module(G)
Tree, pattern, build_full, iso_list_7z, run = G.Tree, G.pattern, G.build_full, G.iso_list_7z, G.run
Tree.zfile = lambda self, rel, size: None   # zisofs は ISO 9660 固有。UDF のイメージには入れない
_link = Tree.link
def _link_tolerant(self, rel, target):
    """UDF のシンボリックリンクは 1 ブロックに収まる必要がある (512 バイトのブロックでは長いリンクが作れない)。作れなかったものは木にも manifest にも入れない。"""
    try: _link(self, rel, target)
    except OSError as e:
        if e.errno != 36: raise
        print("  (symlink %s not creatable on this block size -- skipped)" % rel)
Tree.link = _link_tolerant

def ud(*args, check=True):
    r = subprocess.run(["udisksctl"] + list(args) + ["--no-user-interaction"], capture_output=True, text=True, timeout=60)
    if check and r.returncode != 0: raise RuntimeError("udisksctl %s failed: %s%s" % (" ".join(args), r.stdout, r.stderr))
    return r

class Loop:
    """イメージをループデバイスにしてマウントする。with で抜けると必ずアンマウント・削除する。"""
    def __init__(self, img, ro=False):
        self.img, self.ro, self.dev, self.mnt = img, ro, None, None
    def __enter__(self):
        r = ud("loop-setup", "-f", self.img, *(["-r"] if self.ro else []))
        self.dev = re.search(r"as (/dev/loop\d+)", r.stdout).group(1)
        try:
            r = ud("mount", "-b", self.dev, *(["-o", "ro"] if self.ro else []))
            self.mnt = re.search(r" at (.*?)\.?$", r.stdout.strip()).group(1)
        except Exception:
            ud("loop-delete", "-b", self.dev, check=False); raise
        return self
    def __exit__(self, *a):
        os.sync()
        for _ in range(10):
            r = ud("unmount", "-b", self.dev, check=False)
            if r.returncode == 0 or "not mounted" in (r.stderr + r.stdout).lower(): break
            time.sleep(0.5)
        ud("loop-delete", "-b", self.dev, check=False)

def build_extras(t):
    """UDF 固有: ハードリンク・疎ファイル (穴)・断片化したファイル (複数エクステント)・小さいファイル (ICB 内データ)。"""
    t.dense("hl_orig.bin", 5000, 0o644, nlink=2)
    os.link(os.path.join(t.root, "hl_orig.bin"), os.path.join(t.root, "hl_copy.bin"))
    mt = [l for l in t.man if l.startswith("F\thl_orig.bin\t")][0].split("\t")
    t.man.append("\t".join(["F", "hl_copy.bin", mt[2], mt[3], mt[4], mt[5], mt[6]]))
    # 疎ファイル: 先頭 100 バイトと末尾 100 バイトだけに内容、間は穴
    p = os.path.join(t.root, "sparse.bin"); seed = t.s(); size = 3 * 1024 * 1024
    with open(p, "wb") as f:
        f.write(pattern(seed, 0, 100)); f.seek(size - 100); f.write(pattern(seed, size - 100, 100))
    os.chmod(p, 0o644); mt = t.stamp(p)
    t.man.append("F\tsparse.bin\t%d\t%d\t1\td:0:100:%d,d:%d:100:%d\t644" % (size, mt, seed, size - 100, seed))
    # 断片化: 2 つのファイルを 4096 バイトずつ交互に書く (割り当てが交互のエクステントになる)
    pa, pb = os.path.join(t.root, "frag_a.bin"), os.path.join(t.root, "frag_b.bin")
    sa, sb = t.s(), t.s(); n = 200 * 1024
    with open(pa, "wb") as fa, open(pb, "wb") as fb:
        for off in range(0, n, 4096):
            fa.write(pattern(sa, off, 4096)); fa.flush(); os.fsync(fa.fileno())
            fb.write(pattern(sb, off, 4096)); fb.flush(); os.fsync(fb.fileno())
    for nm, sd in (("frag_a.bin", sa), ("frag_b.bin", sb)):
        pth = os.path.join(t.root, nm); os.chmod(pth, 0o644)
        t.man.append("F\t%s\t%d\t%d\t1\td:0:%d:%d\t644" % (nm, n, t.stamp(pth), n, sd))
    # ICB 内に収まる小さいファイルと、ちょうど境界の大きさ
    for sz in (1, 100, 500):
        t.dense("tiny_%d.bin" % sz, sz)

def parse_man(man):
    """manifest 行 → 期待する {パス: 情報}。F の spec は ',' 区切りで複数領域 (疎ファイル用)。"""
    out = {}
    for l in man:
        c = l.split("\t")
        if c[0] in "DFL" and len(c[0]) == 1: out[c[1]] = c
    return out

def verify_tree(root, man, rrfeat):
    """マウントされた木が manifest と一致すること (全ファイルの内容・mode・nlink・リンク先・mtime)。"""
    bad = 0
    want = parse_man(man)
    seen = set()
    for dp, dn, fn in os.walk(root):
        for n in dn + fn:
            seen.add(os.path.relpath(os.path.join(dp, n), root))
    missing = [p for p in want if p not in seen]
    extra = [p for p in seen if p not in want and not p.startswith("lost+found")]
    for p in missing[:10]: print("  readback missing", p); bad += 1
    for p in extra[:10]: print("  readback extra", p); bad += 1
    for p, c in want.items():
        full = os.path.join(root, p)
        try: st = os.lstat(full)
        except OSError: continue
        if c[0] == "F":
            sz, mt, nl, spec, mode = int(c[2]), int(c[3]), int(c[4]), c[5], int(c[6], 8)
            if st.st_size != sz: print("  size", p, st.st_size, sz); bad += 1; continue
            if st.st_mtime != mt: print("  mtime", p, st.st_mtime, mt); bad += 1
            if st.st_nlink != nl: print("  nlink", p, st.st_nlink, nl); bad += 1
            if (st.st_mode & 0o7777) != mode: print("  mode %s %o vs %o" % (p, st.st_mode & 0o7777, mode)); bad += 1
            exp = bytearray(sz)
            for part in [s for s in spec.split(",") if s]:
                q = part.split(":")
                if q[0] == "d": exp[int(q[1]):int(q[1]) + int(q[2])] = pattern(int(q[3]), int(q[1]), int(q[2]))
            if open(full, "rb").read() != bytes(exp): print("  content", p); bad += 1
        elif c[0] == "L":
            if os.readlink(full) != c[2]: print("  link", p, os.readlink(full), c[2]); bad += 1
        elif c[0] == "D":
            if int(c[2]) != int(st.st_mtime): print("  dir mtime", p, int(st.st_mtime), c[2]); bad += 1
    return bad

def check_7z(img, man, rr_deep_ok=True):
    """7z (独立実装) による照合。7z はシンボリックリンクや大きなディレクトリなどを含む UDF を "Unsupported feature" で開けないことがあるので、開けたときだけ照合する。"""
    r = subprocess.run(["7z", "l", "-slt", img], capture_output=True, text=True, env=dict(os.environ, LC_ALL="C.UTF-8"))
    if r.returncode != 0:
        print("  (7z cannot open this image: %s -- skipped)" % (r.stdout.strip().splitlines()[-1] if r.stdout.strip() else "?"))
        return 0
    got = iso_list_7z(img)
    bad = 0
    for p, c in parse_man(man).items():
        g = got.get("/" + p)
        if g is None: print("  7z missing", p); bad += 1; continue
        if c[0] == "F" and (g[0] != "-" or g[1] != int(c[2])): print("  7z mismatch", p, g, c[2]); bad += 1
        if c[0] == "D" and g[0] != "d": print("  7z dir mismatch", p); bad += 1
    return bad

def make_mkudffs(out, name, mk_args, size_mib, bs, builder, rrfeat=True, emoji=True, extras=True, label=None):
    img = os.path.join(out, name + ".img")
    if os.path.exists(img): os.remove(img)
    label = label or ("USBMEM_" + name.upper())[:30]
    blocks = size_mib * 1024 * 1024 // bs
    run(["mkudffs", "--new-file", "--label=" + label, "--blocksize=%d" % bs] + mk_args + [img, str(blocks)])
    info = run(["udfinfo", img]).stdout
    with Loop(img) as lp:
        t = Tree(lp.mnt)
        builder(t)
        if extras: build_extras(t)
        man = list(t.man)
    # 一度アンマウントしてから読み戻し (ページキャッシュではなくディスクの内容を見る) と 7z の照合
    with Loop(img, ro=True) as lp:
        bad = verify_tree(lp.mnt, man, rrfeat)
    bad += check_7z(img, man)
    if bad: sys.exit("cross-check FAILED for %s (%d)" % (name, bad))
    rev = re.search(r"udfrev=([\d.]+)", info); rev = rev.group(1) if rev else "?"
    man.append("V\tbs\t%d" % bs)
    open(os.path.join(out, name + ".manifest"), "w", encoding="utf-8").write("\n".join(man) + "\n")
    print("%s: %d bytes, bs %d, udfrev %s, %d manifest lines, host-driver readback ok (7z listing used when it can open the image)" % (name, os.path.getsize(img), bs, rev, len(man)))

def make_vat_empty(out, name, media="cdr", rev="1.50"):
    """VAT (仮想パーティション、追記型メディア = UDF 1.50 の CD-R) の空ボリューム。ホストの UDF ドライバは VAT のボリュームを書けないので、中身は空のみ (mkudffs --vat)。"""
    img = os.path.join(out, name + ".img")
    if os.path.exists(img): os.remove(img)
    run(["mkudffs", "--new-file", "--label=USBMEM_VAT", "--blocksize=2048", "--media-type=" + media, "--vat", "--udfrev=" + rev, img, "8192"])
    info = run(["udfinfo", img]).stdout
    open(os.path.join(out, name + ".manifest"), "w", encoding="utf-8").write("V\tbs\t2048\nV\tvat\t1\n")
    print("%s: %d bytes, VAT (empty root only), udfinfo: %s" % (name, os.path.getsize(img), [l for l in info.splitlines() if l.startswith(("numfiles", "udfrev", "blocksize"))]))

def make_bridge(out, name, opts, builder):
    """genisoimage -udf (ISO 9660 + UDF 1.02 ブリッジ)。木から直接作る。"""
    with tempfile.TemporaryDirectory(dir=out) as td:
        tree = os.path.join(td, "tree"); os.makedirs(tree)
        t = Tree(tree); builder(t); man = list(t.man)
        img = os.path.join(out, name + ".img")
        if os.path.exists(img): os.remove(img)
        r = subprocess.run(["genisoimage", "-udf"] + opts + ["-quiet", "-V", "USBMEM_BRIDGE", "-o", img, tree], capture_output=True, text=True)
        if r.returncode != 0: sys.exit("genisoimage failed: " + r.stdout + r.stderr)
        bad = check_7z(img, man)
        if bad: sys.exit("7z cross-check FAILED for %s" % name)
        man.append("V\tbs\t2048"); man.append("V\tbridge\t1")
        open(os.path.join(out, name + ".manifest"), "w", encoding="utf-8").write("\n".join(man) + "\n")
        print("%s: %d bytes, ISO+UDF bridge, %d manifest lines, 7z cross-check ok" % (name, os.path.getsize(img), len(man)))

if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "/dev/shm/usbmem_udf"
    only = set(sys.argv[2:])
    os.makedirs(out, exist_ok=True)
    full = lambda t: build_full(t, 8, True, True)
    variants = [
        ("hd512", ["--media-type=hd", "--udfrev=2.01"], 40, 512),
        ("hd2048", ["--media-type=hd", "--udfrev=2.01"], 32, 2048),
        ("hd4096", ["--media-type=hd", "--udfrev=2.01"], 32, 4096),
        ("hd102", ["--media-type=hd", "--udfrev=1.02"], 40, 512),
        ("spar150", ["--media-type=dvdram", "--udfrev=1.50", "--spartable=2"], 32, 2048),
        ("cdrw150", ["--media-type=cdrw", "--udfrev=1.50"], 32, 2048),
    ]
    for name, args, mib, bs in variants:
        if only and name not in only: continue
        make_mkudffs(out, name, args, mib, bs, full)
    if not only or "vat250_empty" in only:
        # UDF 1.50 の CD-R (--media-type=cdr) の VAT は mkudffs 2.3 で作ってもホストの Linux も開けない (can't read superblock) ので使わない。UDF 2.50 の VAT (bdr) はホストでマウントできる
        make_vat_empty(out, "vat250_empty", "bdr", "2.50")
    if not only or "bridge" in only:
        make_bridge(out, "bridge", ["-R", "-J", "-joliet-long", "-iso-level", "3", "-input-charset", "utf-8"], lambda t: build_full(t, 6, False, False))
