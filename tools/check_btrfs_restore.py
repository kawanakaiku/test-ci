#!/usr/bin/env python3
"""Btrfs リーダー (usbmem_btrfs_tests tree) の出力を、参照実装 `btrfs restore` の展開結果と突き合わせる (ドライバ自身の manifest ではない独立した比較)。
使い方: tools/check_btrfs_restore.py <fixture_dir> <usbmem_btrfs_tests>
対象: <fixture_dir> の *.img のうち manifest のあるもの (セクタサイズ != ページサイズのものは btrfs restore が abort するので除く) と、サブボリュームのイメージ (--subvol)。
比較: 通常ファイルの SHA-256 とサイズ、ディレクトリ、シンボリックリンクのリンク先 (FIFO / ソケットは restore が作らないので除く)。"""
import hashlib, os, subprocess, sys, tempfile

os.environ["PATH"] = os.environ.get("PATH", "") + ":/sbin:/usr/sbin"

def digest_dir(root):
    out = set()
    for dp, dn, fn in os.walk(root):
        for n in fn + [d for d in dn if os.path.islink(os.path.join(dp, d))]:
            p = os.path.join(dp, n); rel = os.path.relpath(p, root)
            if os.path.islink(p): out.add((rel, "l", os.readlink(p)))
            elif os.path.isfile(p):
                h = hashlib.sha256()
                with open(p, "rb") as f:
                    for ch in iter(lambda: f.read(1 << 20), b""): h.update(ch)
                out.add((rel, "f", h.hexdigest()))
        for d in dn:
            if not os.path.islink(os.path.join(dp, d)): out.add((os.path.relpath(os.path.join(dp, d), root), "d", ""))
    return out

def digest_ours(binary, img, subvol=None):
    cmd = [binary, "tree", img] + ([str(subvol)] if subvol else [])
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("usbmem_btrfs_tests tree failed for %s (rc %d):\n%s" % (img, r.returncode, r.stderr[-800:]))
    out = set()
    for line in r.stdout.splitlines():
        p = line.split("\t")
        if len(p) < 5: continue
        path, t, size, mtime, extra = p[0], p[1], p[2], p[3], "\t".join(p[4:])
        if t == "d": out.add((path, "d", ""))
        elif t == "f": out.add((path, "f", extra))
        elif t == "l": out.add((path, "l", extra))
    return out

def main():
    fx, binary = sys.argv[1], sys.argv[2]
    names = sorted(f[:-4] for f in os.listdir(fx) if f.endswith(".img") and os.path.exists(os.path.join(fx, f[:-4] + ".manifest")) and not f.startswith("sector"))
    names += [n for n in ("subvols_default", "nodatasum") if os.path.exists(os.path.join(fx, n + ".img"))]
    bad = 0
    for name in names:
        img = os.path.join(fx, name + ".img")
        with tempfile.TemporaryDirectory() as td:
            # サブボリュームのイメージ: restore は既定サブボリュームではなくトップレベル (ID 5) を展開する (-s でスナップショットも)。こちらは subvol_id=5 で比べる
            ex = os.path.join(td, "ex")
            os.makedirs(ex)
            r = subprocess.run(["btrfs", "restore", "-S", "-i", "-s", img, ex], capture_output=True, text=True)
            ref = digest_dir(ex)
            ours = digest_ours(binary, img, 5 if name.startswith("subvols") else None)
            if name == "subvols_default":
                # restore (-s) は、サブボリュームの中身をスナップショット名のディレクトリにも展開することがあるので、こちらの全項目が参照に含まれることだけを見る
                missing = ours - ref
            else:
                missing = ours ^ ref
            if missing:
                bad += 1
                print("MISMATCH %s: %d differing entries, e.g. %s" % (name, len(missing), sorted(missing)[:4]))
            else:
                print("%s: %d entries identical to btrfs restore" % (name, len(ours)))
    if bad: sys.exit(1)
    print("OK: %d images agree with btrfs restore" % len(names))

main()
