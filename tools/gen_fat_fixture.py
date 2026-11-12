#!/usr/bin/env python3
"""FAT12/16/32 (vfat) のテスト用イメージとマニフェストを作る (core/compat/tests/test_fat.cpp 用)。
使い方: gen_fat_fixture.py <出力ディレクトリ>
Linux の FAT ドライバとは**独立**の実装で作る:
  - mtools (mformat / mcopy / mattrib / mdel) でフォーマットと書き込みを行う。
  - mtools が作れない 1 つ (UTF-16 のサロゲートペアを含む名前) だけ、LFN エントリのバイト列を Python で直接書き換える。
  - できあがったイメージは、7-Zip (7z) の FAT リーダと mtools (mcopy の読み出し) の 2 つで読み戻し、元のツリーと一致することを確かめる (一致しなければ失敗)。
    (fsck.fat は環境に無いので、独立した 2 つのリーダによる全ファイル比較を整合性の根拠にしている。)
イメージの種類 (FAT の種別 x セクタサイズ x クラスタサイズ):
  fat12_f1440   FAT12  512 B  / 512 B     (1.44 MB フロッピー、ルート 224 エントリ固定)
  fat12_c4k     FAT12  512 B  / 4 KiB     (12 MiB)
  fat16_c2k     FAT16  512 B  / 2 KiB     (64 MiB、ルート 512 エントリ固定)
  fat16_c512    FAT16  512 B  / 512 B     (20 MB)
  fat32_c4k     FAT32  512 B  / 4 KiB     (128 MiB)
  fat32_c512    FAT32  512 B  / 512 B     (34 MB)
  fat32_s4k     FAT32  4096 B / 4 KiB     (4Kn セクタ、273 MiB)
  fat32_c8k     FAT32  512 B  / 8 KiB     (560 MB、疎ファイル)
マニフェスト (<name>.manifest): "D\\t<path>" / "F\\t<path>\\t<size>\\t<seed>\\t<mtime(UTC 秒)>\\t<attrs: r=読み取り専用 h=隠し s=システム、無ければ ->"
内容は pattern(seed, size) = ((i*31 + seed*7 + (i>>8)) & 255)。mtime は FAT の 2 秒精度に丸めた値 (UTC 解釈)。
"""
import hashlib, os, re, shutil, struct, subprocess, sys, tempfile

OUT = sys.argv[1] if len(sys.argv) > 1 else "/dev/shm/usbmem_fat_%s" % os.environ.get("USER", "u")
ENV = dict(os.environ, TZ="UTC", LC_ALL="C.UTF-8", MTOOLS_SKIP_CHECK="1")
BASE_T = 1715953530          # 2024-05-17 13:45:30 UTC
T_1980 = 315532800           # 1980-01-01 00:00:00 UTC
T_2099 = 4102444798          # 2099-12-31 23:59:58 UTC


def pattern(seed, n):
    return bytes(((i * 31 + seed * 7 + (i >> 8)) & 0xFF) for i in range(n))


def run(*a, check=True):
    r = subprocess.run(a, env=ENV, capture_output=True)
    if check and r.returncode != 0:
        sys.exit("command failed: %s\n%s\n%s" % (" ".join(a), r.stdout.decode(errors="replace")[-600:], r.stderr.decode(errors="replace")[-600:]))
    return r


# (name, 総バイト数, セクタ長, mformat の引数, ルートに置けるエントリ数の目安, 大きいファイルを置くか)
CONFIGS = [
    ("fat12_f1440", 1440 * 1024, 512, ["-f", "1440"], 200, False),
    ("fat12_c4k", 12 << 20, 512, ["-T", "24576", "-c", "8", "-r", "32"], 400, True),
    ("fat16_c2k", 64 << 20, 512, ["-T", "131072", "-c", "4", "-r", "32"], 400, True),
    ("fat16_c512", 40000 * 512, 512, ["-T", "40000", "-c", "1", "-r", "32"], 400, True),
    ("fat32_c4k", 128 << 20, 512, ["-F", "-T", "262144", "-c", "8"], 1000, True),
    ("fat32_c512", 70000 * 512, 512, ["-F", "-T", "70000", "-c", "1"], 1000, True),
    ("fat32_s4k", 70000 * 4096, 4096, ["-F", "-S", "5", "-T", "70000", "-c", "1"], 1000, True),
    ("fat32_c8k", 1100000 * 512, 512, ["-F", "-T", "1100000", "-c", "16"], 1000, True),
]


class Tree:
    def __init__(self, root):
        self.root = root
        self.dirs = []      # 相対パス (mcopy -s でまとめて入れるもの)
        self.files = {}     # path -> (size, seed, mtime, attrs)

    def mkdir(self, rel):
        os.makedirs(os.path.join(self.root, rel), exist_ok=True)
        self.dirs.append(rel)

    def add(self, rel, size, seed, mtime=BASE_T, attrs="-"):
        p = os.path.join(self.root, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(pattern(seed, size))
        os.utime(p, (mtime, mtime))
        self.files[rel] = (size, seed, mtime, attrs)


def build_tree(tree, cluster, big, root_budget):
    seed = [1]

    def nxt():
        seed[0] += 1
        return seed[0]
    add = lambda rel, size, **k: tree.add(rel, size, nxt(), **k)
    add("SMALL.TXT", 13)                                   # 短い名前だけ (大文字の 8.3)
    add("lower.txt", 29)                                   # 小文字の 8.3 (NT の小文字フラグ)
    add("empty.dat", 0)
    add("MiXeD.CaSe", 5)
    add("one_cluster.bin", cluster)
    add("cluster_minus1.bin", cluster - 1)
    add("cluster_plus1.bin", cluster + 1)
    add("multi.bin", 5 * cluster + 17)
    add("日本語ファイル名.txt", 100)
    add("ＦＵＬＬ　ｗｉｄｔｈ.txt", 60)                     # 全角英数・全角スペース
    add("emoji_XX.txt", 31)                                # 後で LFN を書き換えて U+1F600 (サロゲートペア) にする
    add("a name with spaces.txt", 40)
    add("dots.in.name.tar.gz", 41)
    add("plus+comma,semi;eq=bracket[x].txt", 42)           # 8.3 に使えない文字 (LFN で保持)
    add("x" * 100 + ".bin", 20)
    add(("y" * 251) + ".txt", 21)                           # 255 文字
    add("UPPER.TXT", 11)
    add("odd_second.txt", 15, mtime=BASE_T + 1)            # 奇数秒は 2 秒単位に切り捨て
    add("old1980.txt", 16, mtime=T_1980)
    add("future2099.txt", 17, mtime=T_2099)
    add("readonly.txt", 18, attrs="r")
    add("hidden.txt", 19, attrs="h")
    add("system.bin", 22, attrs="s")
    add("all_attrs.txt", 23, attrs="rhs")
    tree.mkdir("sub")
    add("sub/leaf.txt", 24)
    tree.mkdir("sub/deep")
    add("sub/deep/leaf.bin", 3 * cluster + 5)
    tree.mkdir("sub/deep/deeper")
    add("sub/deep/deeper/ほげ.txt", 25)
    tree.mkdir("emptydir")
    tree.mkdir("日本語ディレクトリ")
    add("日本語ディレクトリ/中身.txt", 26)
    # 複数クラスタにまたがるディレクトリ (長い名前は 1 つあたり 3 スロット)。ルートの上限を超えないようにする
    tree.mkdir("many")
    n_many = 120 if root_budget < 400 else 300
    for i in range(n_many):
        add("many/file_with_long_name_%04d.dat" % i, (i * 37) % 700 + (1 if i % 11 else 0))
    if big:
        add("big.bin", 3 << 20)
        add("sub/deep/medium.bin", 300 * 1024 + 7)


def mtools_copy_tree(img, tree, cluster):
    root = tree.root
    tops = sorted(os.listdir(root))
    # 断片化したファイル: 1 クラスタのファイルを並べて 1 つおきに消し、できた穴に大きめのファイルを入れる
    frag = os.path.join(root, "..", "frag_src")
    os.makedirs(frag, exist_ok=True)
    tmpnames = []
    for i in range(24):
        p = os.path.join(frag, "tmp%02d.bin" % i)
        with open(p, "wb") as f:
            f.write(b"\xAA" * cluster)
        tmpnames.append(p)
    run("mcopy", "-i", img, *tmpnames, "::")
    for i in range(0, 24, 2):
        run("mdel", "-i", img, "::/tmp%02d.bin" % i)
    fsize = 16 * cluster + 100
    fsrc = os.path.join(root, "fragmented.bin")
    with open(fsrc, "wb") as f:
        f.write(pattern(99, fsize))
    os.utime(fsrc, (BASE_T, BASE_T))
    tree.files["fragmented.bin"] = (fsize, 99, BASE_T, "-")
    run("mcopy", "-m", "-i", img, fsrc, "::")
    for i in range(1, 24, 2):
        run("mdel", "-i", img, "::/tmp%02d.bin" % i)
    # 残り全部 (fragmented.bin は入れ済みなので除く)
    rest = [os.path.join(root, t) for t in tops if t != "fragmented.bin"]
    # mcopy は 1 回の引数が長すぎないよう分割する
    for k in range(0, len(rest), 40):
        run("mcopy", "-s", "-m", "-i", img, *rest[k:k + 40], "::")
    for rel, (size, seed, mtime, attrs) in tree.files.items():
        if attrs == "-":
            continue
        for ch, flag in (("r", "+r"), ("h", "+h"), ("s", "+s")):
            if ch in attrs:
                run("mattrib", "-i", img, flag, "::/" + rel)


def patch_emoji(img):
    """LFN エントリ 'emoji_XX.txt' の 'XX' (UTF-16 で 2 文字) を U+1F600 のサロゲートペア D83D DE00 に書き換える (mtools は BMP 外を扱えない)。"""
    data = bytearray(open(img, "rb").read())
    m = re.search(rb"\x41e\x00m\x00o\x00j\x00i\x00\x0f\x00", bytes(data))
    if not m:
        sys.exit("emoji LFN slot not found")
    s = m.start()
    # スロット内の文字位置: 1..10 (5 文字), 14..25 (6 文字), 28..31 (2 文字)。'emoji_XX.txt' = e m o j i | _ X X . t x | t -> 7,8 番目 (0 始まり 6,7) は 14.. の組の 2,3 番目
    assert data[s + 14:s + 16] == b"_\x00" and data[s + 16:s + 20] == b"X\x00X\x00", bytes(data[s + 14:s + 20])
    data[s + 16:s + 20] = struct.pack("<HH", 0xD83D, 0xDE00)
    # FAT が 2 つあってもディレクトリは 1 か所 (データ領域) にしか無い。最初に見つかった所だけ書き換える
    open(img, "wb").write(data)


def expected_files(tree):
    out = {}
    for rel, (size, seed, mtime, attrs) in tree.files.items():
        name = rel.replace("emoji_XX.txt", "emoji_\U0001F600.txt")
        out[name] = (size, seed, (mtime // 2) * 2, attrs)
    return out


def verify_with_7z(img, exp):
    d = tempfile.mkdtemp(prefix="fat7z_")
    try:
        r = run("7z", "x", "-y", "-o" + d, img, check=False)
        if r.returncode not in (0, 1, 2):
            print("   (7z で展開できなかったので 7z による照合は省略: rc=%d)" % r.returncode)
            return False
        got = {}
        for dp, dn, fn in os.walk(d):
            for f in fn:
                p = os.path.join(dp, f)
                got[os.path.relpath(p, d)] = open(p, "rb").read()
        bad = 0
        for rel, (size, seed, mt, at) in exp.items():
            g = got.get(rel)
            if g is None:
                print("   7z: missing %s" % rel); bad += 1
            elif g != pattern(seed, size):
                print("   7z: content mismatch %s" % rel); bad += 1
        extra = set(got) - set(exp)
        # tmpNN (奇数番号は消し済み、偶数も消し済み) は残らない。何か余分があれば失敗
        for e in sorted(extra):
            print("   7z: extra %s" % e); bad += 1
        if bad:
            sys.exit("7z readback mismatch in %s" % img)
        return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def verify_with_mtools(img, exp):
    # mtools の読み出し (mcopy) で、emoji 以外の全ファイルを比較 (mtools は BMP 外の名前を扱えないので除く)
    bad = 0
    for rel, (size, seed, mt, at) in exp.items():
        if "\U0001F600" in rel or "[" in rel:   # mtools は BMP 外の名前を扱えず、[ ] はワイルドカードとして解釈する (7z の照合は全ファイルに行う)
            continue
        r = run("mcopy", "-n", "-i", img, "::/" + rel, "-", check=False)
        if r.returncode != 0 or r.stdout != pattern(seed, size):
            print("   mtools: mismatch %s (rc=%d)" % (rel, r.returncode)); bad += 1
    if bad:
        sys.exit("mtools readback mismatch in %s" % img)


def write_manifest(path, tree, exp, meta):
    dirs = set()
    for rel in list(tree.dirs):
        p = rel
        while p:
            dirs.add(p)
            p = p.rpartition("/")[0]
    with open(path, "w", encoding="utf-8") as f:
        f.write("# %s\n" % meta)
        for d in sorted(dirs):
            f.write("D\t%s\n" % d)
        for rel in sorted(exp):
            size, seed, mt, at = exp[rel]
            f.write("F\t%s\t%d\t%d\t%d\t%s\n" % (rel, size, seed, mt, at))


def main():
    for tool in ("mformat", "mcopy", "mattrib", "mdel", "7z"):
        if shutil.which(tool) is None:
            print("skip: %s not found" % tool, file=sys.stderr)
            sys.exit(77)
    os.makedirs(OUT, exist_ok=True)
    for name, total, sector, fmt_args, root_budget, big in CONFIGS:
        img = os.path.join(OUT, name + ".img")
        print("== %s" % name, flush=True)
        if os.path.exists(img):
            os.unlink(img)
        with open(img, "wb") as f:
            f.truncate(total)
        run("mformat", "-i", img, *fmt_args, "::")
        info = run("minfo", "-i", img, "::").stdout.decode()
        cluster = int(re.search(r"cluster size: (\d+) sectors", info).group(1)) * sector
        kind = re.search(r'disk type="(FAT\d+)', info)
        kind = kind.group(1) if kind else "FAT?"
        work = tempfile.mkdtemp(prefix="fatsrc_")
        try:
            src = os.path.join(work, "src")
            os.makedirs(src)
            tree = Tree(src)
            build_tree(tree, cluster, big, root_budget)
            mtools_copy_tree(img, tree, cluster)
            patch_emoji(img)
            exp = expected_files(tree)
            seven = verify_with_7z(img, exp)
            verify_with_mtools(img, exp)
            write_manifest(os.path.join(OUT, name + ".manifest"), tree, exp, "%s sector=%d cluster=%d total=%d 7z=%s" % (kind, sector, cluster, total, "ok" if seven else "skipped"))
            print("   %s sector=%d cluster=%d: %d files, 7z=%s" % (kind, sector, cluster, len(exp), "ok" if seven else "skipped"))
        finally:
            shutil.rmtree(work, ignore_errors=True)
    print("fixtures in", OUT)


if __name__ == "__main__":
    main()
