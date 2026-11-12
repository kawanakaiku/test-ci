#!/usr/bin/env python3
"""F2FS のテスト用イメージを作る (core/compat/tests/test_f2fs.cpp 用)。
使い方: tools/gen_f2fs_fixture.py <outdir> [name ...]   (PATH に mkfs.f2fs / sload.f2fs / fsck.f2fs が必要。ルート権限は不要)
ツリーは tools/gen_squashfs_fixture.py (= gen_ext2_fixture.py の build() + ブロック境界のファイル + 乱数/繰り返しデータ + FIFO) と同じ。
mkfs.f2fs の機能 (extra_attr / inode_checksum / sb_checksum / flexible_inline_xattr / inode_crtime / compression / ...) と sload.f2fs の圧縮 (lz4 / lzo × クラスタの大きさ) の組み合わせで作る。
出力: <outdir>/<name>.img と <name>.manifest (gen_squashfs_fixture.py と同じ書式: D / F / L / S / R)。
独立した検証: 作った各イメージを fsck.f2fs で検査 (整合性。全項目 Ok)。内容の期待値は、イメージに入れる前のソースツリー (ドライバの出力ではない)。
カーネルが実際に書いたイメージ (tools/f2fs_vm/ の VM で作る) は別の名前 (k_*) で同じ出力先に置かれ、同じテストが検証する。"""
import importlib.util, os, shutil, socket, subprocess, sys, tempfile

os.environ["PATH"] = os.environ.get("PATH", "") + ":/sbin:/usr/sbin"
HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("gen_squashfs_fixture", os.path.join(HERE, "gen_squashfs_fixture.py"))
sq = importlib.util.module_from_spec(spec); spec.loader.exec_module(sq)
ext2 = sq.ext2
IMG_MB = 256

# name, mkfs の -O, sload の追加引数, イメージの MB (0 = 既定)
MODERN = "extra_attr,inode_checksum,sb_checksum,inode_crtime,lost_found"
# flexible_inline_xattr は、sload.f2fs 1.16 が一部のツリーで SIT / valid_block_count の食い違う (fsck.f2fs が NG の) イメージを作ることがある。圧縮のイメージでは問題なく通る。
# それ以外の flexible_inline_xattr の検証は、本物のカーネルが書いたイメージ (tools/f2fs_vm) で行う
FLEX = MODERN + ",flexible_inline_xattr"
VARIANTS = [
    ("plain",        "",                                   [], 0),                       # 機能なしの古い形式 (inode に extra 領域なし。インライン xattr 領域は 50 ワード)
    ("ext",          MODERN,                               [], 0),
    ("quota",        MODERN + ",quota,project_quota",      [], 0),
    # 圧縮 (compression) のイメージは、このマシンの sload.f2fs が圧縮に対応していない (the selected compression algorithm is not supported) ため、本物のカーネルが書く (tools/f2fs_vm)
    ("large",        MODERN,                               [], 6144),                    # 6 GiB (疎ファイル): NAT / SIT が複数セグメント
]

def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)

def make_tree_manifest(tree, outdir, name):
    sq.REFDIR[0] = outdir; sq.REFNAME[0] = name
    man = ext2.build(tree, 1024, extra=sq.extra_for(4096))
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    cwd = os.getcwd(); os.chdir(tree)
    try: s.bind("special_sock")
    finally: os.chdir(cwd); s.close()
    man.append("S\tspecial_sock\ts")
    return man

def fsck_clean(img, name):
    r = run(["fsck.f2fs", "--dry-run", img]) if False else run(["fsck.f2fs", img])
    bad = [l for l in (r.stdout + r.stderr).splitlines() if "[Fail" in l or "Fail" in l and "FSCK" in l]
    if r.returncode != 0 or bad:
        sys.exit("fsck.f2fs NOT clean for %s (rc %d):\n%s" % (name, r.returncode, "\n".join(bad) or (r.stdout + r.stderr)[-1500:]))

def make(outdir, name, feats, sload_args, mb):
    with tempfile.TemporaryDirectory(dir=outdir) as td:
        tree = os.path.join(td, "tree"); os.makedirs(tree)
        man = make_tree_manifest(tree, outdir, name)
        img = os.path.join(outdir, name + ".img")
        if os.path.exists(img): os.remove(img)
        size = (mb or IMG_MB) * 1024 * 1024
        with open(img, "wb") as f: f.truncate(size)
        cmd = ["mkfs.f2fs", "-f", "-q", "-l", "USBMEM"] + (["-O", feats] if feats else []) + [img]
        r = run(cmd)
        if r.returncode != 0: return "mkfs.f2fs failed: " + (r.stderr or r.stdout).strip()[-200:]
        r = run(["sload.f2fs", "-f", tree] + sload_args + [img])
        if r.returncode != 0 or "Error:" in (r.stdout + r.stderr): return "sload.f2fs failed: " + (r.stderr or r.stdout).strip()[-300:]   # sload は不正な引数でも rc 0 で何も入れないことがある
        fsck_clean(img, name)
        # sload.f2fs は FIFO / ソケットを入れない (読み飛ばす)。manifest からも外す (特殊ファイルは、本物のカーネルが書いたイメージで検証する)
        man = [l for l in man if not l.startswith("S\t")]
        with open(os.path.join(outdir, name + ".manifest"), "w", encoding="utf-8") as f: f.write("\n".join(man) + "\n")
        print("%s: %d manifest lines, fsck.f2fs clean" % (name, len(man)))
    return None

if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "/dev/shm/usbmem_f2fs"
    only = sys.argv[2:] if len(sys.argv) > 2 else None
    os.makedirs(out, exist_ok=True)
    skipped = []
    for name, feats, sargs, mb in VARIANTS:
        if only and name not in only: continue
        err = make(out, name, feats, sargs, mb)
        if err:
            print("%s: SKIPPED (%s)" % (name, err)); skipped.append(name)
    if skipped: print("skipped variants:", ", ".join(skipped))
