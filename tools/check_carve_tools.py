#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""カービング (core の carve) の検証。

  1. 既知のファイル (tools/carve_samples.py。7z / PIL / ffmpeg があれば本物のエンコーダの出力も) をばらばらの位置に書いたイメージを走査し、
     全部が正しい位置・長さ・拡張子で切り出され、SHA-256 が元と一致する。壊れたもの (途中で切れた・1 バイト壊した) は出さない。
  2. ランダム・ゼロ・テキスト・「各形式の署名だけを散らしたランダム」の領域で、偽の検出が 0。
  3. 形式の選択・最小サイズ・align・取り消し → 再開 (続きから行って、通しで走査した結果と同じ)・未割り当てのみ (FAT / ext4 の削除済みファイルだけが出る)。
  4. 性能 (大きな疎イメージ + データの MB/s)。
  5. photorec (testdisk) があれば、同じイメージで検出数を比べ、自前が見落としているものを info として出す (失敗にはしない)。
使い方: check_carve_tools.py <usbmem_undelete_cli>   (SKIP は終了コード 77)
"""
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import carve_samples as cs  # noqa: E402
import ud_fslib  # noqa: E402

CLI = os.path.abspath(sys.argv[1])
ENV = dict(os.environ, PATH=os.environ.get("PATH", "") + ":/sbin:/usr/sbin:/usr/local/sbin")
WORK = tempfile.mkdtemp(prefix="usbmem_carve_", dir="/dev/shm" if os.path.isdir("/dev/shm") else None)
fails = checks = 0
infos = []


def check(cond, msg):
    global fails, checks
    checks += 1
    if not cond:
        fails += 1
        print("  FAIL:", msg, flush=True)
    return bool(cond)


def run(cmd, **kw):
    kw.setdefault("env", ENV)
    kw.setdefault("capture_output", True)
    kw.setdefault("timeout", 1800)
    return subprocess.run(cmd, **kw)


def carve(img, *args, out=None, state=None):
    cmd = [CLI, "carve", img] + list(args)
    if out:
        os.makedirs(out, exist_ok=True)
        cmd += ["--out", out]
    if state:
        cmd += ["--state", state]
    p = run(cmd)
    try:
        return json.loads(p.stdout.decode("utf-8", "replace")), p.returncode
    except Exception:
        return {"hits": [], "error": p.stdout.decode()[:200] + p.stderr.decode()[:200]}, p.returncode


def rand_text(r, n):
    words = ["alpha", "beta", "gamma", "delta", "%PDF-1.4", "PK", "ftyp", "RIFF", "GIF89a", "usb", "memory"]
    line = lambda: (" ".join(r.choice(words) for _ in range(10)) + "\n").encode()
    pool = [line() for _ in range(2000)]      # 行のプールを使い回す (24 MiB を 1 行ずつ作ると遅い)
    parts, total = [], 0
    while total < n:
        x = pool[r.randrange(len(pool))]
        parts.append(x)
        total += len(x)
    return b"".join(parts)[:n]


# ------------------------------------------------------------------------------------------------ 1. 配置したファイル
def build_layout(path, samples, decoys, size_mb=96, seed=5):
    r = random.Random(seed)
    placed = []  # (名前, offset, data)
    with open(path, "wb") as f:
        f.truncate(size_mb << 20)
        pos = 1 << 20
        items = [("good", n, v) for n, v in samples.items()] + [("decoy", n, v) for n, v in decoys.items()]
        r.shuffle(items)
        bg = [lambda n: r.randbytes(n), lambda n: bytes(n), lambda n: rand_text(r, n)]
        for kind, name, v in items:
            data = v[2]
            gap = 512 * r.randrange(1, 400)
            fill = bg[r.randrange(3)](gap)
            f.seek(pos)
            f.write(fill)
            pos += gap
            f.seek(pos)
            f.write(data)
            placed.append((kind, name, pos, data, v[0], v[1]))
            pos += (len(data) + 511) // 512 * 512
            # 直後に背景 (ファイルの終端の直後が偶然の署名にならないように)
            f.seek(pos)
            f.write(r.randbytes(512))
            pos += 512
        f.seek(pos)
        f.write(r.randbytes(4096))
    return placed


# 内容の検査 (CRC・チェックサム) を持たず、途中までしか無くても構造の連なりとして成り立つ形式。壊れたものを「短くなった・尻尾にごみ付き」で出すのは仕様なので、
# これらの壊れた見本の中に出た検出は失敗にしない (info に数える)。それ以外の形式は、壊れた見本を 1 つも出してはいけない
# riff / bmp は長さがヘッダに書いてあるだけなので、途中で切れたものの後ろに別のデータが続けば区別できない。7z は転送中の壊れ (パックされたデータの中) は圧縮を展開しないと分からない
SOFT = {"mp4", "mp3", "sqlite", "pe", "riff", "bmp"}
SOFT_NAMES = set()


def make_decoys(samples):
    r = random.Random(11)
    d = {}
    for n, (fid, ext, data) in samples.items():
        if fid in ("jpeg", "png", "gif", "pdf", "zip", "mp4", "mp3", "7z", "gzip", "riff", "bmp", "sqlite"):
            cut = max(64, int(len(data) * (0.4 + 0.3 * r.random())))
            d[n + "_trunc"] = (fid, ext, data[:cut])
            if fid in SOFT:
                SOFT_NAMES.add(n + "_trunc")
    # 1 バイト壊したもの (CRC・長さ・チェックサムのある形式)
    for n in ("png", "7z", "gzip", "sqlite", "exe"):
        if n in samples:
            fid, ext, data = samples[n]
            b = bytearray(data)
            i = len(b) // 2
            b[i] ^= 0x55
            d[n + "_corrupt"] = (fid, ext, bytes(b))
            if fid in SOFT or fid == "7z":
                SOFT_NAMES.add(n + "_corrupt")
    # 署名だけの偽物
    d["fake_pdf"] = ("pdf", "pdf", b"%PDF-1.4\n" + rand_text(r, 5000))
    d["fake_zip"] = ("zip", "zip", b"PK\x03\x04" + r.randbytes(3000))
    d["fake_riff"] = ("riff", "wav", b"RIFF\x10\x00\x00\x00WAVEfmt " + r.randbytes(100))
    d["fake_jpeg"] = ("jpeg", "jpg", b"\xff\xd8\xff\xe0\x00\x10JFIF" + r.randbytes(3000))
    return d


def test_layout():
    samples = cs.basic_samples()
    extra = cs.extra_samples()
    allsamples = dict(samples)
    allsamples.update(extra)
    decoys = make_decoys(samples)
    img = os.path.join(WORK, "layout.img")
    placed = build_layout(img, allsamples, decoys)
    out = os.path.join(WORK, "layout_out")
    j, rc = carve(img, out=out)
    check(rc == 0 and "hits" in j, "layout: carve failed: %s" % j.get("error"))
    hits = {h["offset"]: h for h in j["hits"]}
    good = [p for p in placed if p[0] == "good"]
    found = 0
    for kind, name, off, data, fid, ext in good:
        h = hits.get(off)
        if not h:
            check(False, "layout: not found: %s (%s, %d bytes, offset %d)" % (name, fid, len(data), off))
            continue
        ok = h["length"] == len(data) and h["format"] == fid and h["ext"] == ext
        if not check(ok, "layout: %s wrong: got len=%d format=%s ext=%s, want len=%d %s %s" % (name, h["length"], h["format"], h["ext"], len(data), fid, ext)):
            continue
        with open(os.path.join(out, h["name"]), "rb") as f:
            if check(cs.sha(f.read()) == cs.sha(data), "layout: %s extracted content differs" % name):
                found += 1
    # 偽の検出 (置いたもの以外) と、壊れたものの検出
    good_offs = {p[2] for p in good}
    soft_hits = 0
    for h in j["hits"]:
        if h["offset"] not in good_offs:
            owner = [p for p in placed if p[2] <= h["offset"] < p[2] + len(p[3])]
            if owner and all(p[1] in SOFT_NAMES for p in owner):
                soft_hits += 1
                continue
            check(False, "layout: unexpected hit at %d len=%d %s (inside %s)" % (h["offset"], h["length"], h["format"], [p[1] for p in owner]))
    print("  layout: %d samples placed (%d from real encoders), %d decoys, %d found exactly, hits=%d (%d are partial hits in damaged mp4/mp3/sqlite/pe/riff/bmp/7z), rejected=%d" %
          (len(good), len(extra), len(decoys), found, len(j["hits"]), soft_hits, j.get("rejected", 0)), flush=True)
    return img, j, placed


# ------------------------------------------------------------------------------------------------ 2. 偽の検出
def test_false_positives():
    r = random.Random(99)
    img = os.path.join(WORK, "noise.img")
    with open(img, "wb") as f:
        f.write(r.randbytes(24 << 20))                # ランダム
        f.write(bytes(24 << 20))                      # ゼロ
        f.write(rand_text(r, 24 << 20))               # テキスト
        f.write(bytes([0xFF]) * (4 << 20))            # 全 0xFF
        # 各形式の署名の先頭だけを 512 バイトごとに置いたランダム
        sigs = [b"\xff\xd8\xff\xe0", b"\x89PNG\r\n\x1a\n", b"GIF89a", b"BM", b"%PDF-1.4\n", b"PK\x03\x04", b"\x00\x00\x00\x18ftypisom", b"ID3\x03\x00\x00",
                b"\xff\xfb\x90\x00", b"RIFF\x24\x00\x00\x00WAVE", b"7z\xbc\xaf\x27\x1c\x00\x04", b"\x1f\x8b\x08\x00", b"SQLite format 3\x00\x10\x00\x01\x01\x00\x40\x20\x20",
                b"MZ" + bytes(58) + b"\x80\x00\x00\x00"]
        buf = bytearray(r.randbytes(16 << 20))
        for i in range(0, len(buf), 512):
            s = r.choice(sigs)
            buf[i:i + len(s)] = s
        f.write(bytes(buf))
    j, rc = carve(img)
    check(rc == 0, "noise: carve failed %s" % j.get("error"))
    check(len(j["hits"]) == 0, "noise: false detections: %s" % [(h["offset"], h["format"], h["length"]) for h in j["hits"][:5]])
    print("  noise (random/zero/text/0xFF/signature-seeded): %d MiB scanned, %d hits, %d candidates rejected" % (j["bytesScanned"] >> 20, len(j["hits"]), j.get("rejected", 0)), flush=True)
    os.unlink(img)


# ------------------------------------------------------------------------------------------------ 3. オプション
def test_options(img, placed):
    good = [p for p in placed if p[0] == "good"]
    goff = {p[2] for p in good}
    j, _ = carve(img, "--types", "jpeg,png")
    fmts = {h["format"] for h in j["hits"]}
    check(fmts <= {"jpeg", "png"} and len(j["hits"]) > 0, "types: only jpeg/png expected, got %s" % fmts)
    want = sum(1 for p in good if p[4] in ("jpeg", "png"))
    check(len(j["hits"]) == want, "types: %d hits, want %d" % (len(j["hits"]), want))
    j, _ = carve(img, "--min-size", "20000")
    check(all(h["length"] >= 20000 for h in j["hits"]), "min-size: a hit shorter than the minimum")
    check(sum(1 for h in j["hits"] if h["offset"] in goff) == sum(1 for p in good if len(p[3]) >= 20000), "min-size: placed files of >= 20000 bytes must all be found")
    full, _ = carve(img)
    # 取り消し → 再開
    st = os.path.join(WORK, "state.txt")
    if os.path.exists(st):
        os.unlink(st)
    j1, rc1 = carve(img, "--cancel-after-mb", "30", state=st)
    check(rc1 != 0 and not j1.get("finished"), "resume: the first run should be cancelled (rc=%s finished=%s)" % (rc1, j1.get("finished")))
    check(os.path.exists(st), "resume: state file missing")
    j2, rc2 = carve(img, state=st)
    check(rc2 == 0 and j2.get("finished"), "resume: second run should finish")
    key = lambda j: [(h["offset"], h["length"], h["format"], h["ext"]) for h in j["hits"]]
    check(key(j2) == key(full), "resume: hits differ from an uninterrupted run (%d vs %d)" % (len(j2["hits"]), len(full["hits"])))
    check(j2["bytesScanned"] < full["bytesScanned"], "resume: should scan less than a full run")
    # 別の設定の state は拒否
    j3, rc3 = carve(img, "--types", "png", state=st)
    check(rc3 != 0, "resume: a state from other options must be refused")
    # 壊れた state
    with open(st, "wb") as f:
        f.write(b"garbage")
    j4, rc4 = carve(img, state=st)
    check(rc4 != 0, "resume: a corrupt state must be refused")
    print("  options: types/min-size/resume OK (full run: %d hits, resumed: %d bytes scanned vs %d)" % (len(full["hits"]), j2["bytesScanned"], full["bytesScanned"]), flush=True)


def test_align():
    r = random.Random(3)
    s = cs.png(5)
    img = os.path.join(WORK, "align.img")
    with open(img, "wb") as f:
        f.write(r.randbytes(100))
        f.write(s)
        f.write(r.randbytes(1 << 20))
    j, _ = carve(img)
    check(len(j["hits"]) == 0, "align: a file at offset 100 must not be found with the default alignment")
    j, _ = carve(img, "--align", "1")
    check(len(j["hits"]) == 1 and j["hits"][0]["offset"] == 100 and j["hits"][0]["length"] == len(s), "align=1 should find the file at offset 100")
    os.unlink(img)


# ------------------------------------------------------------------------------------------------ 未割り当て
def test_unallocated():
    if not shutil.which("mkfs.fat", path=ENV["PATH"]):
        print("  skip unallocated (no mkfs.fat)")
        return
    img = os.path.join(WORK, "un_fat.img")
    with open(img, "wb") as f:
        f.truncate(24 << 20)
    run(["mkfs.fat", "-F", "16", "-s", "4", img])
    fs = ud_fslib.Fat(img)
    jp = {n: cs.jpeg(50 + i, 30000 + 2000 * i) for i, n in enumerate(["a", "b", "c", "d"])}
    pn = cs.png(60, 80, 60)
    for n in ("a", "b"):
        fs.add_file("/%s.jpg" % n, jp[n])
    fs.add_file("/p.png", pn)
    for n in ("c", "d"):
        fs.add_file("/%s.jpg" % n, jp[n])
    fs.close()
    fs = ud_fslib.Fat(img)
    fs.tree = {}
    fs.delete("/b.jpg")
    fs.delete("/d.jpg")
    fs.close()
    full, _ = carve(img)
    check(len(full["hits"]) == 5, "unalloc/fat: whole-device carve should find all 5 files, found %d" % len(full["hits"]))
    un, _ = carve(img, "--unalloc")
    shas = sorted(cs.sha(jp[n]) for n in ("b", "d"))
    outd = os.path.join(WORK, "un_out")
    un, _ = carve(img, "--unalloc", out=outd)
    got = sorted(cs.sha(open(os.path.join(outd, h["name"]), "rb").read()) for h in un["hits"])
    check(un.get("fs") == "fat16" and un.get("allocationKnown"), "unalloc/fat: FS should be recognised (%s)" % un.get("fs"))
    check(got == shas, "unalloc/fat: only the 2 deleted files expected, got %d hits" % len(un["hits"]))
    os.unlink(img)
    if shutil.which("mke2fs", path=ENV["PATH"]) and shutil.which("debugfs", path=ENV["PATH"]):
        src = os.path.join(WORK, "un_ext_src")
        os.makedirs(src, exist_ok=True)
        files = {"1.jpg": cs.jpeg(70, 40000), "2.jpg": cs.jpeg(71, 41000), "3.pdf": cs.pdf(72), "4.jpg": cs.jpeg(73, 42000)}
        for n, d in files.items():
            with open(os.path.join(src, n), "wb") as f:
                f.write(d)
        img = os.path.join(WORK, "un_ext.img")
        with open(img, "wb") as f:
            f.truncate(24 << 20)
        run(["mke2fs", "-q", "-F", "-t", "ext4", "-d", src, img])
        run(["debugfs", "-w", "-f", "-", img], input=b"rm /2.jpg\nrm /3.pdf\n")
        outd = os.path.join(WORK, "un_out2")
        un, _ = carve(img, "--unalloc", out=outd)
        got = sorted(cs.sha(open(os.path.join(outd, h["name"]), "rb").read()) for h in un["hits"])
        want = sorted(cs.sha(files[n]) for n in ("2.jpg", "3.pdf"))
        check(un.get("fs") == "ext4" and un.get("allocationKnown"), "unalloc/ext4: FS should be recognised")
        check(got == want, "unalloc/ext4: only the 2 deleted files expected, got %d hits" % len(un["hits"]))
        os.unlink(img)
        shutil.rmtree(src, ignore_errors=True)
    print("  unallocated-only: FAT16 / ext4 return just the deleted files", flush=True)


# ------------------------------------------------------------------------------------------------ 性能
def test_perf():
    img = os.path.join(WORK, "perf.img")
    r = random.Random(1)
    size = 3 << 30
    with open(img, "wb") as f:
        f.truncate(size)             # 疎 (ゼロ)。ゼロの読み取りもカービングの走査コストは同じ
        f.seek(0)
        for i in range(64):          # 先頭 256 MiB はランダム + サンプルの混合 (実データの読み出し・検査を含む)
            f.write(r.randbytes(4 << 20))
        samples = [v for v in cs.basic_samples().values()]
        pos = 100 << 20
        for k in range(200):
            v = samples[k % len(samples)]
            f.seek(pos)
            f.write(v[2])
            pos += (len(v[2]) + 511) // 512 * 512 + 512 * 20
    j, rc = carve(img)
    secs = j.get("seconds", 0)
    gb = j["bytesScanned"] / (1 << 30)
    rate = gb * 60 / secs if secs else 0
    check(rc == 0 and rate > 3.0, "perf: %.1f GiB/min is below 3 GiB/min" % rate)
    print("  perf (3 GiB, mostly zero = sparse + 256 MiB of data and samples): %.2f GiB in %.2f s = %.1f GiB/min (%.0f MiB/s), %d hits" %
          (gb, secs, rate, j["bytesScanned"] / secs / (1 << 20), len(j["hits"])), flush=True)
    os.unlink(img)
    # 中身がすべてランダム (読み出しと署名の照合が毎回要る最悪に近い入力) の 1 GiB。ゼロの穴に頼らない値
    blk = random.Random(2).randbytes(4 << 20)
    with open(img, "wb") as f:
        for _ in range(256):
            f.write(blk)
    j, rc = carve(img)
    secs = j.get("seconds", 0)
    gb = j["bytesScanned"] / (1 << 30)
    rate = gb * 60 / secs if secs else 0
    check(rc == 0 and rate > 3.0, "perf(random): %.1f GiB/min is below 3 GiB/min" % rate)
    check(len(j["hits"]) == 0, "perf(random): false detections in random data")
    print("  perf (1 GiB of random data, tmpfs, page cache): %.2f GiB in %.2f s = %.1f GiB/min (%.0f MiB/s)" % (gb, secs, rate, j["bytesScanned"] / secs / (1 << 20)), flush=True)
    os.unlink(img)


# ------------------------------------------------------------------------------------------------ photorec との比較 (info)
def test_photorec(img, j, placed):
    pr = shutil.which("photorec", path=ENV["PATH"])
    if not pr:
        infos.append("photorec: not installed (comparison skipped)")
        return
    outd = os.path.join(WORK, "pr_out")
    shutil.rmtree(outd, ignore_errors=True)
    os.makedirs(outd)
    p = run([pr, "/log", "/d", outd + "/", "/cmd", img, "partition_none,fileopt,everything,enable,search"], cwd=WORK, timeout=900)
    files = []
    for root, _, names in os.walk(outd):
        for n in names:
            if n.endswith(".log") or n.endswith(".xml"):
                continue
            files.append(os.path.join(root, n))
    if not files:
        infos.append("photorec produced no files (rc=%d): %s" % (p.returncode, (p.stdout + p.stderr).decode("utf-8", "replace")[-200:]))
        return
    ours = set()
    for kind, name, off, data, fid, ext in placed:
        if kind == "good":
            ours.add(cs.sha(data))
    mine_all = {}
    outc = os.path.join(WORK, "layout_out")
    for fn in os.listdir(outc):
        mine_all[cs.sha(open(os.path.join(outc, fn), "rb").read())] = fn
    pr_shas = {}
    for fn in files:
        pr_shas[cs.sha(open(fn, "rb").read())] = fn
    missed = [fn for h, fn in pr_shas.items() if h not in mine_all]
    # photorec が見つけて自前が見つけていないもの: 置いたファイルに一致するもの (本当の見落とし) と、それ以外 (photorec の誤検出・断片)
    real_miss = [os.path.basename(fn) for h, fn in pr_shas.items() if h in ours and h not in mine_all]
    extra = len(missed) - len(real_miss)
    ours_not_pr = [n for n in {p[1]: 1 for p in placed if p[0] == "good" and cs.sha(p[3]) not in pr_shas}]
    infos.append("photorec found %d files; ours %d (of %d placed). photorec-only placed files (our misses): %d %s; photorec-only other: %d; placed files photorec missed: %s" %
                 (len(pr_shas), len(mine_all), len(ours), len(real_miss), real_miss[:10], extra, ours_not_pr[:20]))


def main():
    if not os.path.exists(CLI):
        print("SKIP: no cli")
        return 77
    print("carve: layout")
    img, j, placed = test_layout()
    print("carve: noise")
    test_false_positives()
    print("carve: options")
    test_options(img, placed)
    test_align()
    print("carve: unallocated")
    test_unallocated()
    print("carve: performance")
    test_perf()
    test_photorec(img, j, placed)
    for i in infos:
        print("info:", i)
    print("== carve: %d checks, %d failed" % (checks, fails))
    return 1 if fails else 0


if __name__ == "__main__":
    try:
        rc = main()
    finally:
        shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(rc)
