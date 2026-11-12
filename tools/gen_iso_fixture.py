#!/usr/bin/env python3
"""ISO 9660 のテスト用イメージを作る (core/compat/tests/test_isofs.cpp 用)。
使い方: tools/gen_iso_fixture.py <outdir>   (PATH に genisoimage / xorriso / isoinfo が必要。/sbin は自動で足す)
Linux のドライバ・FUSE は使わない。genisoimage と xorriso (別実装) でツリーから作り、**独立のツール isoinfo** で作成直後に一覧 (名前・サイズ・種別・シンボリックリンク先) を照合する。
出力: <outdir>/<name>.img と <name>.manifest (タブ区切り。ext2 のフィクスチャと同じ書式):
  D <path> <mtime>
  F <path> <size> <mtime> <nlink> <spec> <mode>   spec = "d:<off>:<len>:<seed>" (パターン) / "z:<off>:<len>" (ゼロ)。mode は Rock Ridge のあるイメージだけ検査 (8 進)
  L <path> <target>
  O <path>                                        あってもなくてもよいエントリ (relocation 用の rr_moved など)
  V <key> <value>                                 イメージの性質 (rr=1/0, joliet=1/0, lower=1 など)
内容の規則 pattern: byte(i) = (i * 31 + seed * 7 + (i >> 8)) & 0xFF
バリアント: rr (genisoimage -R) / rr_xorriso (xorrisofs -R) / joliet (-J) / both (-R -J) / plain_l1 (レベル 1, 8.3) / plain_l2 (レベル 2) / joliet_xorriso (xorrisofs -J)"""
import os, sys, subprocess, tempfile, re, shutil

os.environ["PATH"] = os.environ.get("PATH", "") + ":/sbin:/usr/sbin"
os.environ["TZ"] = "UTC"
FIXED_MTIME = 1700000000

def pattern(seed, off, n):
    return bytes((((off + i) * 31 + seed * 7 + ((off + i) >> 8)) & 0xFF) for i in range(n))

class Tree:
    def __init__(self, root, upper=False):
        self.root, self.man, self.clock, self.seed, self.upper = root, [], FIXED_MTIME, 1, upper
    def s(self):
        self.seed += 1; return self.seed
    def stamp(self, path, link=False):
        self.clock += 1
        os.utime(path, (self.clock, self.clock), follow_symlinks=not link)
        return self.clock
    def nm(self, rel):
        return rel.upper() if self.upper else rel
    def dense(self, rel, size, mode=0o644, nlink=1):
        rel = self.nm(rel)
        p = os.path.join(self.root, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        seed = self.s()
        with open(p, "wb") as f:
            f.write(pattern(seed, 0, size))
        os.chmod(p, mode)
        mt = self.stamp(p)
        self.man.append("F\t%s\t%d\t%d\t%d\t%s\t%o" % (rel, size, mt, nlink, ("d:0:%d:%d" % (size, seed)) if size else "", mode))
    def zfile(self, rel, size):
        """zisofs (Rock Ridge ZF) の圧縮ファイルに見えるもの: 先頭が固定のマジック。カーネルを圧縮非対応でビルドしているので、中身は返さず -EOPNOTSUPP を期待する。"""
        rel = self.nm(rel)
        p = os.path.join(self.root, rel)
        with open(p, "wb") as f:
            f.write(bytes([0x37, 0xE4, 0x53, 0x96, 0xC9, 0xDB, 0xD6, 0x07]) + pattern(self.s(), 8, size - 8))
        os.chmod(p, 0o644)
        self.stamp(p)
        self.man.append("Z\t%s\t%d" % (rel, size))
    def mkdir(self, rel, mode=0o755):
        rel = self.nm(rel)
        p = os.path.join(self.root, rel); os.makedirs(p, exist_ok=True); os.chmod(p, mode)
        return rel
    def dirstamp(self, rel):
        rel = self.nm(rel)
        self.man.append("D\t%s\t%d" % (rel, self.stamp(os.path.join(self.root, rel))))
    def link(self, rel, target):
        rel = self.nm(rel)
        p = os.path.join(self.root, rel); os.makedirs(os.path.dirname(p), exist_ok=True)
        os.symlink(target, p); self.stamp(p, True)
        self.man.append("L\t%s\t%s" % (rel, target))

def build_full(t, deep, emoji=True, rrfeat=True):
    """Rock Ridge 向け: 長い名前・日本語・空白・記号・大ディレクトリ・深い階層・シンボリックリンク。"""
    for sz in (0, 11, 2047, 2048, 2049, 4095, 4096, 4097, 100000, 3 * 1024 * 1024 + 123):
        t.dense("size_%d.bin" % sz, sz, 0o644)
    t.zfile("zisofs_like.bin", 5000)
    t.dense("exec.sh", 300, 0o755); t.dense("private.dat", 64, 0o600); t.dense("readonly.txt", 33, 0o444)
    t.dense("日本語ファイル名.txt", 300); t.dense("n" * (200 if rrfeat else 90), 5)
    t.dense("space and 'quote'.txt", 17); t.dense("MixedCase.TXT", 9); t.dense("dots.in.name.tar.gz", 21); (t.dense("semi;colon.txt", 7) if rrfeat else None)
    if emoji: t.dense("😀emoji.txt", 5)   # BMP 外の文字 (UTF-16 ではサロゲートペア)。Joliet (UCS-2) を作るツールが受け付けないときは入れない
    t.mkdir("emptydir"); t.dirstamp("emptydir")
    for k in range(1, deep + 1):
        pth = "/".join("d%d" % j for j in range(1, k + 1))
        t.mkdir(pth)
    t.dense("/".join("d%d" % j for j in range(1, deep + 1)) + "/leaf.txt", 1234)
    for k in range(1, deep + 1):
        t.dirstamp("/".join("d%d" % j for j in range(1, k + 1)))
    t.mkdir("many")
    for i in range(1000):
        t.dense("many/file_%04d.dat" % i, 1 + (i * 7) % 300)
    t.dirstamp("many")
    t.mkdir("sub/日本語ディレクトリ"); t.dense("sub/日本語ディレクトリ/中身.txt", 77)
    if rrfeat:   # シンボリックリンクは Rock Ridge でだけ表せる
        t.link("link_rel", "size_11.bin")
        t.link("link_up", "../" * 3 + "many/file_0001.dat")
        t.link("link_abs", "/usr/share/doc/readme")
        t.link("link_dangling", "no/such/target")
        t.link("link_long", "a/" * 80 + "target.txt")
        t.link("sub/link_in_sub", "../size_0.bin")
        t.link("link_to_dir", "many")
    t.dirstamp("sub/日本語ディレクトリ"); t.dirstamp("sub")   # 子を作り終えてから (追加するとディレクトリの mtime が変わるため)

def build_plain(t, level):
    """プレーン ISO 9660 (拡張なし): 大文字 8.3 (レベル 1) / 31 文字 (レベル 2)。カーネルは小文字化してバージョン ;1 を落とす。"""
    t.upper = True
    for sz in (0, 11, 2047, 2048, 2049, 4097, 100000, 1234567):
        t.dense("size_%d.bin" % sz if level >= 2 else "S%d.BIN" % sz if sz < 1000000 else "BIG.BIN", sz)
    t.dense("README.TXT", 100); t.dense("A.B", 3); t.dense("NODOT", 4)
    t.mkdir("DIR1"); t.mkdir("DIR1/DIR2"); t.dense("DIR1/DIR2/LEAF.TXT", 55)
    t.dirstamp("DIR1/DIR2"); t.dirstamp("DIR1")
    t.mkdir("EMPTY"); t.dirstamp("EMPTY")
    t.mkdir("MANY")
    for i in range(300):
        t.dense("MANY/F%04d.DAT" % i, 1 + (i * 7) % 300)
    t.dirstamp("MANY")
    if level >= 2:
        t.dense("A_MUCH_LONGER_NAME_31_CHARS_ABC.TXT"[:31 - 4] + ".TXT", 66)

def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw)

def lowernorm(man_lines, lower):
    """プレーン ISO はカーネルが小文字にする。manifest のパスを小文字化する。"""
    if not lower: return man_lines
    out = []
    for l in man_lines:
        c = l.split("\t")
        c[1] = c[1].lower()
        if c[0] == "L": pass
        out.append("\t".join(c))
    return out

def iso_list_7z(img):
    """7z (独立実装。Joliet の UTF-16 名を UTF-8 で出せる): {絶対パス: (種別, サイズ, None)}。"""
    r = subprocess.run(["7z", "l", "-slt", img], capture_output=True, text=True, env=dict(os.environ, LC_ALL="C.UTF-8"))
    if r.returncode != 0: sys.exit("7z failed: " + r.stdout[-500:] + r.stderr)
    out, cur, body = {}, {}, False
    for line in r.stdout.splitlines() + [""]:
        if line.startswith("----------"): body = True; continue
        if not body: continue
        if line.strip() == "":
            if "Path" in cur:
                k = "d" if cur.get("Folder") == "+" else "-"
                out["/" + cur["Path"]] = (k, int(cur["Size"]) if cur.get("Size", "") != "" else None, None)
            cur = {}
            continue
        if " = " in line:
            a, b = line.split(" = ", 1); cur[a] = b
    return out

def iso_list(img, mode):
    if mode == "7z": return iso_list_7z(img)
    """isoinfo -l: {絶対パス: (種別, サイズ)}。mode = '' / '-R' / '-J'。シンボリックリンクは -R のとき 'name -> target' で出る。"""
    r = run(["isoinfo", "-l", "-i", img] + ([mode] if mode else []))
    out, cur = {}, None
    for line in r.stdout.splitlines():
        m = re.match(r"^Directory listing of (.*)$", line)
        if m: cur = m.group(1); continue
        m = re.match(r"^([-dlrwxsStT]{10})\s+\d+\s+\d+\s+\d+\s+(\d+)\s+\w+\s+\d+\s+\d+\s+\[\s*\d+\s*(?:\d+)?\]\s+(.*)$", line)
        if m and cur is not None:
            nm = m.group(3).rstrip()
            if nm in (".", ".."): continue
            tgt = None
            if " -> " in nm: nm, tgt = nm.split(" -> ", 1)
            out[cur.rstrip("/") + "/" + nm] = (m.group(1)[0], int(m.group(2)), tgt)
    return out

def check_with_isoinfo(img, man, mode, lower, rr):
    """isoinfo (独立実装) が見る名前・サイズ・種別・リンク先が manifest と一致すること。余分なもの (rr_moved) は O 扱い。"""
    got = iso_list(img, mode)
    want = {}
    for l in man:
        c = l.split("\t")
        if c[0] == "D": want["/" + c[1]] = ("d", None, None)
        elif c[0] == "F": want["/" + c[1]] = ("-", int(c[2]), None)
        elif c[0] == "L": want["/" + c[1]] = ("l", None, c[2])
    bad = 0
    for p, (k, sz, tgt) in want.items():
        if p.count("/") > 8 and rr: continue   # 8 階層より深いものは Rock Ridge の relocation (rr_moved へ移される) で、isoinfo は元の場所では辿れない。カーネル側のテストで検証する
        # 名前が ;1 で終わるプレーンは isoinfo が ;1 付きで出す
        cand = [p] + [p + ";1", p.upper() + ";1", p.upper(), p.upper() + ".;1"]
        g = next((got[x] for x in cand if x in got), None)
        if g is None:
            print("  isoinfo(%s) missing %s" % (mode or "plain", p)); bad += 1; continue
        if k == "-" and (g[0] != "-" or g[1] != sz): print("  isoinfo mismatch %s: %s vs %s" % (p, g, (k, sz))); bad += 1
        if k == "d" and g[0] != "d": print("  isoinfo mismatch dir %s" % p); bad += 1
        if k == "l" and (g[0] != "l" or (rr and g[2] != tgt)): print("  isoinfo mismatch link %s: %s vs %s" % (p, g, tgt)); bad += 1
    return bad, got

def make(out, name, tool, opts, builder, mode, lower, rr, joliet, deep=0, extra_o=()):
    with tempfile.TemporaryDirectory(dir=out) as td:
        tree = os.path.join(td, "tree"); os.makedirs(tree)
        t = Tree(tree)
        builder(t)
        man = lowernorm(t.man, lower)
        img = os.path.join(out, name + ".img")
        if os.path.exists(img): os.remove(img)
        cmd = (tool if isinstance(tool, list) else [tool]) + opts + ["-quiet", "-V", "USBMEM_" + name.upper()[:20], "-o", img, tree]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0: sys.exit("%s failed for %s:\n%s%s" % (tool, name, r.stdout, r.stderr))
        bad, got = check_with_isoinfo(img, man, mode, lower, rr)
        if bad: sys.exit("isoinfo cross-check FAILED for %s (%d)" % (name, bad))
        # isoinfo が見た、manifest に無いエントリ (relocation の rr_moved など) は O として許す
        known = set("/" + l.split("\t")[1] for l in man if l[0] in "DFL")
        for p in sorted(got):
            q = re.sub(r";\d+$", "", p)            # バージョン ;1
            if lower: q = q.rstrip(".").lower()      # プレーン ISO: 末尾の "." も落ち、小文字になる
            if q not in known and q.count("/") == 1:
                man.append("O\t%s" % q.lstrip("/"))
        man.append("V\trr\t%d" % rr); man.append("V\tjoliet\t%d" % joliet); man.append("V\tlower\t%d" % lower)
        open(os.path.join(out, name + ".manifest"), "w", encoding="utf-8").write("\n".join(man) + "\n")
        print("%s: %s, %d manifest lines, isoinfo(%s) cross-check ok" % (name, os.path.getsize(img), len(man), mode or "plain"))

if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "/dev/shm/usbmem_iso"
    os.makedirs(out, exist_ok=True)
    G = "genisoimage"
    X = "xorrisofs"
    full = lambda deep, emoji=True, rrfeat=True: (lambda t: build_full(t, deep, emoji, rrfeat))
    make(out, "rr", G, ["-R", "-iso-level", "3", "-input-charset", "utf-8"], full(12), "-R", False, 1, 0)
    make(out, "rr_xorriso", X, ["-R", "-iso-level", "3", "-input-charset", "utf-8", "-rr_reloc_dir", "rr_moved"], full(12), "-R", False, 1, 0)
    make(out, "both", G, ["-R", "-J", "-joliet-long", "-iso-level", "3", "-input-charset", "utf-8"], full(12, False), "-R", False, 1, 1)
    make(out, "joliet", G, ["-J", "-joliet-long", "-iso-level", "3", "-input-charset", "utf-8"], full(6, False, False), "7z", False, 0, 1)
    make(out, "joliet_xorriso", ["xorriso", "-rockridge", "off", "-as", "mkisofs"], ["-J", "-joliet-long", "-iso-level", "3", "-input-charset", "utf-8"], full(6, False, False), "7z", False, 0, 1)
    make(out, "plain_l1", G, ["-iso-level", "1"], lambda t: build_plain(t, 1), "", True, 0, 0)
    make(out, "plain_l2", G, ["-iso-level", "2"], lambda t: build_plain(t, 2), "", True, 0, 0)
