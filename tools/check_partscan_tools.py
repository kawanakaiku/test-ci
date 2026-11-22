#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""パーティション探索 / 復元案 / ブートセクタ修復の独立検証: 本物の sfdisk / mkfs.* / fsck.* / ntfsfix / testdisk との照合。

使い方: check_partscan_tools.py <usbmem_partscan_cli のパス>

  ・sfdisk で GPT / MBR (拡張 + 論理) のディスクを作り、各区画に本物の mkfs.* で作った FS を置く。
  ・表を壊す (表の消去 / 先頭のゼロ埋め / 表だけの破損 / 末尾の切り詰め) → 探索が真の区画をすべて見つけ、偽の候補が 0 であること。
  ・復元案を適用 → sfdisk が読めて、真の表と一致する。FS のデータ領域は 1 バイトも変わらない。
  ・FS ごとにブートセクタを壊す → 検査が見つけ、修復後に fsck.fat / fsck.exfat / e2fsck -fn / ntfsfix -n が通る。
    書き込み前のバックアップを書き戻すと修復前のバイト列に戻る。
  ・testdisk (入っていれば) と同じイメージで「analyse,search」の結果を比べる (差は表示するが、testdisk が真の位置に見つけたものを
    こちらが見落としたときだけ失敗)。testdisk が無い / 動かない / 時間切れなら、その部分だけ飛ばす (真の表との照合は必ず行う)。

作業ファイルは /dev/shm/usbmem_partscan_$USER 以下 (tmpfs)。実デバイスには触れない。終了コード 77 = 必要な道具が無く何も検証できなかった。
"""
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys

CLI = sys.argv[1]
USER = os.environ.get("USER", "u")
W = f"/dev/shm/usbmem_partscan_{USER}"
ENV = dict(os.environ)
ENV["PATH"] = ENV.get("PATH", "") + ":/sbin:/usr/sbin"
ENV["LC_ALL"] = "C.UTF-8"
MiB = 1 << 20

passed = 0
failed = []
skipped = []
notes = []


def check(cond, msg):
    global passed
    if cond:
        passed += 1
    else:
        failed.append(msg)
        print("  FAIL:", msg)


def have(tool):
    return shutil.which(tool, path=ENV["PATH"]) is not None


def sh(args, **kw):
    p = subprocess.run(args, env=ENV, capture_output=True, text=True, errors="replace", **kw)
    return p.returncode, p.stdout, p.stderr


def cli(img, *args, sector=None):
    cmd = [CLI, img] + (["--sector", str(sector)] if sector else []) + list(args)
    rc, out, err = sh(cmd)
    last = None
    for line in out.strip().splitlines():
        try:
            last = json.loads(line)
        except ValueError:
            pass
    return rc, last, err


def sha(path, off=0, n=None):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        f.seek(off)
        left = n if n is not None else os.path.getsize(path) - off
        while left > 0:
            b = f.read(min(1 << 20, left))
            if not b:
                break
            h.update(b)
            left -= len(b)
    return h.hexdigest()


def mkimg(name, size):
    path = os.path.join(W, name)
    if os.path.exists(path):
        os.remove(path)
    with open(path, "wb") as f:
        f.truncate(size)
    return path


def zero(path, off, n):
    with open(path, "r+b") as f:
        f.seek(off)
        f.write(bytes(n))


def patch(path, off, data):
    with open(path, "r+b") as f:
        f.seek(off)
        f.write(data)


def copy(src, dst):
    if os.path.exists(dst):
        os.remove(dst)
    rc, _, _ = sh(["cp", "--sparse=always", src, dst])
    assert rc == 0


# ------------------------------------------------------------------ FS の作成

MKFS = {
    "ext4": (["mkfs.ext4", "-q", "-F"], "mkfs.ext4"),
    "ext2": (["mkfs.ext2", "-q", "-F"], "mkfs.ext2"),
    "fat32": (["mkfs.fat", "-F", "32"], "mkfs.fat"),
    "fat16": (["mkfs.fat", "-F", "16"], "mkfs.fat"),
    "exfat": (["mkfs.exfat"], "mkfs.exfat"),
    "ntfs": (["mkfs.ntfs", "-q", "-F", "-Q"], "mkfs.ntfs"),
    "xfs": (["mkfs.xfs", "-q"], "mkfs.xfs"),
    "btrfs": (["mkfs.btrfs", "-q"], "mkfs.btrfs"),
    "f2fs": (["mkfs.f2fs", "-q"], "mkfs.f2fs"),
}
FS_NAME = {"ext4": "ext4", "ext2": "ext2", "fat32": "fat32", "fat16": "fat16", "exfat": "exfat", "ntfs": "ntfs", "xfs": "xfs", "btrfs": "btrfs", "f2fs": "f2fs",
           "squashfs": "squashfs", "iso": "iso9660", "udf": "udf", "luks1": "luks", "luks2": "luks", "erofs": "erofs"}
_fs_cache = {}


def make_fs(kind, size):
    """kind の FS を size バイトのイメージとして作る (キャッシュ)。作れなければ None"""
    key = (kind, size)
    if key in _fs_cache:
        return _fs_cache[key]
    path = os.path.join(W, f"fs_{kind}_{size}.img")
    res = None
    if kind in MKFS:
        cmd, tool = MKFS[kind]
        if have(tool):
            mkimg(os.path.basename(path), size)
            rc, out, err = sh(cmd + [path])
            if rc == 0:
                res = path
            else:
                print(f"  note: {kind}: mkfs failed ({err.strip()[:100]})")
    elif kind == "squashfs" and have("mksquashfs"):
        d = os.path.join(W, "sqsrc")
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "f"), "wb") as f:
            f.write(os.urandom(300000))
        if os.path.exists(path):
            os.remove(path)
        rc, _, _ = sh(["mksquashfs", d, path, "-quiet", "-noappend", "-no-progress"])
        if rc == 0:
            res = path
    elif kind == "udf" and have("mkfs.udf"):
        mkimg(os.path.basename(path), size)
        rc, _, _ = sh(["mkfs.udf", "--media-type=hd", path])
        if rc == 0:
            res = path
    elif kind in ("luks1", "luks2") and have("cryptsetup"):
        mkimg(os.path.basename(path), size)
        key = os.path.join(W, "luks.key")
        with open(key, "wb") as f:
            f.write(b"usbmem-partscan-test-key")
        rc, out, err = sh(["cryptsetup", "luksFormat", "--type", kind, "--batch-mode", "--pbkdf", "pbkdf2", "--pbkdf-force-iterations", "1000",
                           "--key-file", key, path])
        if rc == 0:
            res = path
        else:
            print(f"  note: {kind}: cryptsetup luksFormat failed ({(out + err).strip()[:100]})")
    elif kind == "erofs" and have("mkfs.erofs"):
        d = os.path.join(W, "erosrc")
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "f"), "wb") as f:
            f.write(os.urandom(300000))
        if os.path.exists(path):
            os.remove(path)
        rc, _, _ = sh(["mkfs.erofs", path, d])
        if rc == 0:
            res = path
    elif kind == "iso" and have("genisoimage"):
        d = os.path.join(W, "isosrc")
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "f"), "wb") as f:
            f.write(os.urandom(300000))
        if os.path.exists(path):
            os.remove(path)
        rc, _, _ = sh(["genisoimage", "-quiet", "-o", path, d])
        if rc == 0:
            res = path
    _fs_cache[key] = res
    return res


def place_fs(disk, fs_path, start_lba, ss=512):
    with open(fs_path, "rb") as f, open(disk, "r+b") as g:
        g.seek(start_lba * ss)
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            g.write(b)


# ------------------------------------------------------------------ ディスクの作成 (sfdisk)

def sfdisk_json(img):
    rc, out, err = sh(["sfdisk", "--json", img])
    if rc != 0:
        print("  sfdisk --json failed:", (out + err).strip()[:200])
        return None
    return json.loads(out)["partitiontable"]


def truth_of(img):
    t = sfdisk_json(img)
    if t is None:
        return "?", []
    parts = []
    for p in t["partitions"]:
        if p.get("type") in ("5", "f", "85", "0x5", "0xf"):  # 拡張コンテナ
            continue
        parts.append({"start": p["start"], "size": p["size"], "node": p["node"], "type": p["type"], "uuid": p.get("uuid", "")})
    return t["label"], parts


class Spec:
    """ディスクの設計: (名前, FS 種別, 開始 LBA, セクタ数)"""

    def __init__(self, name, label, total_sectors, parts, script):
        self.name, self.label, self.total, self.parts, self.script = name, label, total_sectors, parts, script


def build(spec):
    path = mkimg(f"disk_{spec.name}.img", spec.total * 512)
    p = subprocess.run(["sfdisk", "-q", path], input=spec.script, env=ENV, capture_output=True, text=True)
    if p.returncode != 0:
        print("  sfdisk failed:", p.stderr.strip()[:200])
        return None
    expect = {}
    for kind, start, count in spec.parts:
        fs = make_fs(kind, count * 512)
        if fs is None:
            return None
        place_fs(path, fs, start)
        expect[start] = kind
    return path, expect


MB = 2048  # 1 MiB = 2048 セクタ

# GPT: 小さい FS 5 つ。起点は 1 MiB 境界
GPT_SMALL = Spec(
    "gpt_small", "gpt", 230 * MB,
    [("ext4", 1 * MB, 32 * MB), ("fat32", 40 * MB, 40 * MB), ("ntfs", 90 * MB, 24 * MB), ("exfat", 120 * MB, 24 * MB), ("f2fs", 150 * MB, 64 * MB)],
    "label: gpt\n" + "".join(f"start={s}, size={c}, type=L\n" for s, c in [(1 * MB, 32 * MB), (40 * MB, 40 * MB), (90 * MB, 24 * MB), (120 * MB, 24 * MB), (150 * MB, 64 * MB)]),
)
# GPT: 大きい FS (xfs の最小サイズ 300 MiB)
GPT_BIG = Spec(
    "gpt_big", "gpt", 620 * MB,
    [("xfs", 1 * MB, 320 * MB), ("btrfs", 330 * MB, 128 * MB), ("squashfs", 470 * MB, 8 * MB), ("iso", 490 * MB, 8 * MB)],
    "label: gpt\n" + "".join(f"start={s}, size={c}, type=L\n" for s, c in [(1 * MB, 320 * MB), (330 * MB, 128 * MB), (470 * MB, 8 * MB), (490 * MB, 8 * MB)]),
)
# MBR: 主 2 + 拡張 (論理 3)
MBR_EXT = Spec(
    "mbr_ext", "dos", 200 * MB,
    [("ext4", 1 * MB, 32 * MB), ("fat32", 40 * MB, 40 * MB), ("fat16", 90 * MB, 20 * MB), ("ext2", 115 * MB, 16 * MB), ("ntfs", 140 * MB, 24 * MB)],
    "label: dos\n"
    f"start={1 * MB}, size={32 * MB}, type=83\n"
    f"start={40 * MB}, size={40 * MB}, type=c\n"
    f"start={85 * MB}, size={115 * MB - 1}, type=5\n"
    f"start={90 * MB}, size={20 * MB}, type=6\n"
    f"start={115 * MB}, size={16 * MB}, type=83\n"
    f"start={140 * MB}, size={24 * MB}, type=7\n",
)


def misc_spec():
    """UDF / LUKS1 / LUKS2 / erofs: 作れるものだけでシナリオを組む (無ければ None)"""
    items = [("udf", 16), ("luks1", 40), ("luks2", 40), ("erofs", 8)]
    parts, pos = [], 1 * MB
    for kind, mb in items:
        if make_fs(kind, mb * MiB) is not None:
            parts.append((kind, pos, mb * MB))
            pos += (mb + 1) * MB
    if not parts:
        return None
    return Spec("gpt_misc", "gpt", pos + 2 * MB, parts, "label: gpt\n" + "".join(f"start={s_}, size={c}, type=L\n" for _, s_, c in parts))


# ------------------------------------------------------------------ 壊し方

def dmg_erase_table(img, label):
    size = os.path.getsize(img)
    if label == "gpt":
        zero(img, 0, 34 * 512)
        zero(img, size - 33 * 512, 33 * 512)
    else:
        zero(img, 0, 512)
        t = sfdisk_json_for_ebr(img)
        for lba in t:
            zero(img, lba * 512, 512)


EBR_LBAS = []


def sfdisk_json_for_ebr(img):
    return list(EBR_LBAS)


def dmg_zero_head(img, label):
    zero(img, 0, 1 * MiB if label == "dos" else 34 * 512)


def dmg_table_only(img, label):
    size = os.path.getsize(img)
    if label == "gpt":
        # プライマリのヘッダの CRC を壊す + 配列の先頭エントリを書き換える。バックアップは無傷
        patch(img, 512 + 16, b"\xde\xad\xbe\xef")
        patch(img, 1024 + 40, b"\x55\x55")
    else:
        zero(img, 446, 64)  # パーティションエントリだけ消す (署名・ブートコードは残る)


def dmg_truncate(img, label):
    size = os.path.getsize(img)
    with open(img, "r+b") as f:
        f.truncate(size * 3 // 4 // 512 * 512)


DAMAGES = {
    "erase-table": dmg_erase_table,
    "zero-head": dmg_zero_head,
    "table-only": dmg_table_only,
    "truncate": dmg_truncate,
}


# ------------------------------------------------------------------ 探索の照合

def scan(img, full=False, sector=None):
    rc, js, err = cli(img, "scan", *(["--full"] if full else []), sector=sector)
    check(rc == 0 and js is not None, f"scan failed rc={rc} {err[:100]}")
    return js


def candidates_of(js, min_conf=40):
    out = []
    for c in js["candidates"]:
        if c["confidence"] >= min_conf:
            out.append({"fs": c["fs"], "start": int(c["startLba"]), "bytes": int(c["sizeBytes"]), "known": c["sizeKnown"], "min": c["sizeMinimum"],
                        "fits": c["fitsDevice"], "conf": c["confidence"], "rel": c["relation"], "notes": c["notes"]})
    return out


def check_scan(tag, js, expect, disk_size, truth_sizes):
    """expect: {start_lba: fs_kind}。偽の候補が 0、見つけるべきものが全部ある"""
    cands = candidates_of(js)
    by_start = {}
    for c in cands:
        by_start.setdefault(c["start"], []).append(c)
    for start, kind in sorted(expect.items()):
        got = by_start.get(start, [])
        want = FS_NAME[kind]
        ok = any(c["fs"] == want or (want in ("ext2", "ext4") and c["fs"] in ("ext2", "ext3", "ext4")) for c in got)
        check(ok, f"{tag}: not found {kind} at {start} (candidates there: {[c['fs'] for c in got]})")
        for c in got:
            if c["fs"] == want and c["known"] and not c["min"] and start in truth_sizes:
                tsz = truth_sizes[start] * 512
                # iso / squashfs は区画より小さいイメージを置くのが普通 (下限は無い)
                low_ok = kind in ("iso", "squashfs", "udf", "luks1", "luks2", "erofs") or tsz - c["bytes"] < 1 * MiB
                check(c["bytes"] <= tsz and low_ok, f"{tag}: {kind} at {start}: size {c['bytes']} vs partition {tsz}")
    extra = [c for c in cands if c["start"] not in expect]
    check(not extra, f"{tag}: false candidates {[(c['fs'], c['start'], c['conf'], c['notes']) for c in extra]}")
    check(len([c for c in cands if c["start"] in expect]) == len(expect), f"{tag}: duplicate candidates for one start {[(c['fs'], c['start']) for c in cands]}")


# ------------------------------------------------------------------ testdisk との照合

def testdisk_found(img):
    """testdisk /cmd <img> analyse,search の結果 → {(start_lba, sectors)}。動かなければ None"""
    if not have("testdisk"):
        return None
    d = os.path.join(W, "td")
    os.makedirs(d, exist_ok=True)
    log = os.path.join(d, "testdisk.log")
    if os.path.exists(log):
        os.remove(log)
    env = dict(ENV)
    env["TERM"] = "xterm"
    try:
        p = subprocess.run(["testdisk", "/log", "/cmd", img, "analyse,search"], cwd=d, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=240)
    except subprocess.TimeoutExpired:
        return None
    if not os.path.exists(log):
        return None
    text = open(log, errors="replace").read()
    m = re.search(r"CHS (\d+) (\d+) (\d+)", text)
    if not m:
        return None
    _, H, S = int(m.group(1)), int(m.group(2)), int(m.group(3))
    # search_part() の後ろに出る検出結果の行 (種別 + 開始 CHS + 終了 CHS + セクタ数) をすべて集める。
    # 最終的な推奨 (interface_write の一覧) は重なりなどで検出の一部を落とすので、検出そのものを比べる
    parts = set()
    pos = text.find("search_part()")
    if pos < 0:
        return parts
    for line in text[pos:].splitlines():
        mm = re.match(r"^\s+(\S(?:.*?\S)?)\s{2,}(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)", line)
        if mm and not re.match(r"^\s*\d+ [*PLEDXpl ] ", line):
            c, h, s_ = int(mm.group(2)), int(mm.group(3)), int(mm.group(4))
            parts.add(((c * H + h) * S + s_ - 1, int(mm.group(8))))
    return parts


def differential(tag, img, js, truth):
    td = testdisk_found(img)
    if td is None:
        skipped.append(f"{tag}: testdisk")
        return
    ours = {(c["start"], (c["bytes"] + 511) // 512) for c in candidates_of(js)}
    truth_set = {(t["start"], t["size"]) for t in truth}
    td_starts = {s for s, _ in td}
    our_starts = {s for s, _ in ours}
    missed = [(s, z) for s, z in td if s in {t["start"] for t in truth} and s not in our_starts]
    check(not missed, f"{tag}: testdisk found partitions at true positions that we missed: {missed}")
    notes.append(f"{tag}: ours {len(our_starts)} / testdisk {len(td_starts)} / truth {len(truth_set)}; only-ours {sorted(our_starts - td_starts)}; only-testdisk {sorted(td_starts - our_starts)}")


# ------------------------------------------------------------------ 復元案

def proposals(img, full=False, sector=None):
    rc, js, err = cli(img, "proposals", *(["--full"] if full else []), sector=sector)
    check(rc == 0 and isinstance(js, list), f"proposals failed rc={rc} {err[:100]}")
    return js or []


def regions_changed(a, b, allowed):
    """a と b でバイト列が違う範囲が、allowed ([(off, len)]) の中だけか"""
    sa, sb = os.path.getsize(a), os.path.getsize(b)
    if sa != sb:
        return False
    pos = 0
    allowed = sorted(allowed)
    with open(a, "rb") as fa, open(b, "rb") as fb:
        while pos < sa:
            n = min(1 << 20, sa - pos)
            x, y = fa.read(n), fb.read(n)
            if x != y:
                for i in range(0, n, 512):
                    if x[i:i + 512] != y[i:i + 512]:
                        off = pos + i
                        if not any(o <= off < o + l for o, l in allowed):
                            return False
            pos += n
    return True


def run_scenario(spec, name_filter=None):
    built = build(spec)
    if built is None:
        skipped.append(f"{spec.name}: needed mkfs tool or sfdisk missing")
        return
    pristine, expect = built
    label, truth = truth_of(pristine)
    global EBR_LBAS
    EBR_LBAS = []
    if spec.label == "dos":
        # 拡張の先頭と各論理の直前 (EBR) の位置
        tp = sfdisk_json(pristine)
        ext = [p for p in tp["partitions"] if p["type"] in ("5", "f", "85")]
        EBR_LBAS = [ext[0]["start"]] + [p["start"] - 1 for p in tp["partitions"] if p["type"] not in ("5", "f", "85") and p["start"] > ext[0]["start"]]
        EBR_LBAS = sorted(set(EBR_LBAS))
    truth_sizes = {t["start"]: t["size"] for t in truth}
    check(set(truth_sizes) == set(expect), f"{spec.name}: truth/expect mismatch")

    # 無傷のディスクでも偽の候補が 0 で全部 Match
    for full in (False, True):
        js = scan(pristine, full)
        check_scan(f"{spec.name} intact {'full' if full else 'quick'}", js, expect, os.path.getsize(pristine), truth_sizes)
        check(all(c["rel"] == "match" for c in candidates_of(js)), f"{spec.name} intact: relation not match: {[(c['fs'], c['rel']) for c in candidates_of(js)]}")
    differential(f"{spec.name} intact", pristine, scan(pristine), truth)

    for dname, dfn in DAMAGES.items():
        if name_filter and name_filter not in dname:
            continue
        img = os.path.join(W, f"dmg_{spec.name}_{dname}.img")
        copy(pristine, img)
        dfn(img, spec.label)
        size = os.path.getsize(img)
        if dname == "truncate":
            exp = {s: k for s, k in expect.items() if s * 512 + 64 * 1024 <= size}
        else:
            exp = dict(expect)
        for full in (False, True):
            js = scan(img, full)
            check_scan(f"{spec.name} {dname} {'full' if full else 'quick'}", js, exp, size, truth_sizes)
        js = scan(img)
        differential(f"{spec.name} {dname}", img, js, [t for t in truth if t["start"] in exp])

        # 復元案を適用 (切り詰めは対象外: ディスクが小さくなっていて、真の表とは一致しない)
        if dname == "truncate":
            continue
        props = proposals(img)
        ok_props = [p for p in props if p["planOk"]]
        check(bool(ok_props), f"{spec.name} {dname}: no applicable proposal ({[p['id'] for p in props]})")
        if not ok_props:
            continue
        # 信頼度の高い順
        confs = [p["confidence"] for p in ok_props]
        check(confs == sorted(confs, reverse=True), f"{spec.name} {dname}: proposals not in confidence order {confs}")
        want_ids = ["gpt-copy-backup-to-primary", "gpt-repair-primary", "rebuild-gpt"] if spec.label == "gpt" else ["rebuild-mbr"]
        pick = next((p for p in ok_props if p["id"] in want_ids), ok_props[0])
        res = os.path.join(W, f"fix_{spec.name}_{dname}.img")
        copy(img, res)
        bk = os.path.join(W, f"fix_{spec.name}_{dname}.bak")
        rc, js2, err = cli(res, "apply-proposal", pick["id"], "--backup", bk)
        check(rc == 0 and js2 and js2.get("ok") and js2.get("verified"), f"{spec.name} {dname}: apply {pick['id']} -> {js2} {err[:100]}")
        if rc != 0:
            continue
        check(os.path.exists(bk) and os.path.getsize(bk) > 0, f"{spec.name} {dname}: no pre-write backup")
        label2, truth2 = truth_of(res)
        check(label2 == spec.label, f"{spec.name} {dname}: label {label2}")
        got = {(t["start"], t["size"]) for t in truth2}
        for t in truth:
            near = [g for g in got if g[0] == t["start"]]
            check(bool(near), f"{spec.name} {dname}: partition at {t['start']} not restored (got {sorted(got)})")
            if near:
                if expect.get(t["start"]) in ("udf", "luks1", "luks2"):
                    # FS の大きさがヘッダから分からない種別は、次の区画の手前 / ディスクの終わりまで広げる (真の大きさ以上)
                    check(near[0][1] >= t["size"], f"{spec.name} {dname}: size at {t['start']}: {near[0][1]} < {t['size']}")
                else:
                    check(near[0][1] <= t["size"] and t["size"] - near[0][1] < 2 * MiB, f"{spec.name} {dname}: size at {t['start']}: {near[0][1]} vs {t['size']}")
        check(len(truth2) == len(truth), f"{spec.name} {dname}: {len(truth2)} partitions restored, expected {len(truth)}")
        rc, out, err = sh(["sfdisk", "-V", res])
        check(rc == 0, f"{spec.name} {dname}: sfdisk -V: {out.strip()[:150]} {err.strip()[:100]}")
        if "backup" in pick["id"] or dname == "table-only" and spec.label == "gpt":
            # バックアップのコピーなら、真の表と GUID まで一致する
            check({(t["start"], t["size"], t["uuid"]) for t in truth2} == {(t["start"], t["size"], t["uuid"]) for t in truth}, f"{spec.name} {dname}: not identical to the true table")
        # 適用後の探索: すべて Match
        js3 = scan(res)
        check(all(c["rel"] == "match" for c in candidates_of(js3)), f"{spec.name} {dname}: after fix, relation: {[(c['fs'], c['rel']) for c in candidates_of(js3)]}")
        # 表以外のバイトは変わっていない
        allowed = [(0, 34 * 512), (size - 33 * 512, 33 * 512)] if spec.label == "gpt" else [(0, 512)] + [(l * 512, 512) for l in EBR_LBAS]
        check(regions_changed(img, res, allowed), f"{spec.name} {dname}: bytes outside the partition table changed")
        # バックアップから元の (壊れた) 表に戻せる
        # (partedit のバックアップは usbmem_partedit_cli で復元する形式。ここでは存在とサイズだけを確かめる)


# ------------------------------------------------------------------ ブートセクタの修復

def fsck(kind, img, lenient_ext=False):
    """修復後の独立検査 (読み取りのみ)。(通ったか, 出力)"""
    if kind in ("fat32", "fat16"):
        if not have("fsck.fat"):
            return None, ""
        rc, out, err = sh(["fsck.fat", "-n", img])
    elif kind == "exfat":
        if not have("fsck.exfat"):
            return None, ""
        rc, out, err = sh(["fsck.exfat", "-n", img])
    elif kind in ("ext2", "ext4"):
        if not have("e2fsck"):
            return None, ""
        rc, out, err = sh(["e2fsck", "-fn", img])
        if rc != 0 and lenient_ext:
            # 予備のスーパーブロック / GDT から戻したあとは、空きブロック数・空き inode 数・ディレクトリ数 (集計) が古いので e2fsck -b と同じく指摘される。
            # 集計以外 (スーパーブロック・グループ記述子・ビットマップ・inode テーブルの位置など) の指摘が無ければ通ったとみなす
            bad = ("uperblock", "Bad magic", "Group descriptors", "Corruption", "Illegal", "Inode table for group", "Block bitmap for group", "Inode bitmap for group", "short read")
            lines = [l for l in (out + err).splitlines() if any(b in l for b in bad)]
            return not lines, (out + err)[-300:]
    elif kind == "ntfs":
        if not have("ntfsfix"):
            return None, ""
        rc, out, err = sh(["ntfsfix", "-n", img])
    else:
        return None, ""
    return rc == 0, (out + err)[-300:]


def ext_geometry(kind):
    return ["mkfs.ext4", "-q", "-F", "-b", "1024"] if kind == "ext4" else ["mkfs.ext2", "-q", "-F", "-b", "1024"]


def boot_cases():
    # (kind, size, [(damage name, fn, expected repair id)])
    def z(off, n):
        return lambda img, base=0: zero(img, base + off, n)

    return [
        ("fat32", 40 * MiB, [
            ("zero-sector0", z(0, 512), "fat32-restore-primary-from-backup"),
            ("zero-sector0-2", z(0, 3 * 512), "fat32-restore-primary-from-backup"),
            ("bad-signature", lambda img, base=0: patch(img, base + 510, b"\x00\x00"), "fat32-restore-primary-from-backup"),
            ("zero-backup", z(6 * 512, 3 * 512), "fat32-refresh-backup-from-primary"),
            ("fsinfo-signature", z(512, 4), "fat32-rebuild-fsinfo"),
        ]),
        ("exfat", 24 * MiB, [
            ("zero-main", z(0, 12 * 512), "exfat-restore-main-from-backup"),
            ("zero-sector0", z(0, 512), "exfat-restore-main-from-backup"),
            ("checksum-sector", lambda img, base=0: patch(img, base + 11 * 512, b"\x00\x00\x00\x00" * 4), "exfat-restore-main-from-backup"),
            ("zero-backup", z(12 * 512, 12 * 512), "exfat-refresh-backup-from-main"),
        ]),
        ("ntfs", 24 * MiB, [
            ("zero-sector0", z(0, 512), "ntfs-restore-primary-from-backup"),
            ("zero-last-sector", lambda img, base=0: zero(img, base + 24 * MiB - 512, 512), "ntfs-refresh-backup-from-primary"),
        ]),
        ("ext2", 48 * MiB, [
            ("zero-superblock", z(1024, 1024), "ext-restore-superblock-from-backup"),
            ("zero-first-8k", z(0, 8192), "ext-restore-superblock-from-backup"),
            ("bad-magic", lambda img, base=0: patch(img, base + 1024 + 56, b"\x00\x00"), "ext-restore-superblock-from-backup"),
        ]),
        ("ext4", 48 * MiB, [
            ("zero-superblock", z(1024, 1024), "ext-restore-superblock-from-backup"),
            ("zero-first-8k", z(0, 8192), "ext-restore-superblock-from-backup"),
            ("bad-checksum", lambda img, base=0: patch(img, base + 1024 + 0x3FC, b"\x00\x00\x00\x00"), "ext-restore-superblock-from-backup"),
        ]),
    ]


def make_boot_fs(kind, size):
    if kind in ("ext2", "ext4"):
        tool = "mkfs.ext4" if kind == "ext4" else "mkfs.ext2"
        if not have(tool):
            return None
        path = os.path.join(W, f"bootfs_{kind}.img")
        mkimg(os.path.basename(path), size)
        rc, _, _ = sh(ext_geometry(kind) + [path])
        return path if rc == 0 else None
    return make_fs(kind, size)


def run_boot():
    for kind, size, cases in boot_cases():
        base = make_boot_fs(kind, size)
        if base is None:
            skipped.append(f"boot {kind}: mkfs tool missing")
            continue
        ok0, _ = fsck(kind, base)
        if ok0 is False:
            print(f"  note: {kind}: fsck does not pass on the pristine image; skipping the post-repair fsck for it")
        rc, js, err = cli(base, "boot-inspect")
        check(rc == 0 and js and js["verdict"] in ("ok", "warnings") and js["recognized"], f"boot {kind}: pristine image verdict {js and js['verdict']}")
        for dname, dfn, rid in cases:
            tag = f"boot {kind} {dname}"
            img = os.path.join(W, f"boot_{kind}_{dname}.img")
            copy(base, img)
            dfn(img)
            before_sha = sha(img)
            fsck_before, _ = fsck(kind, img)
            rc, js, err = cli(img, "boot-inspect", "--fs", "ext4" if kind == "ext4" else ("ext2" if kind == "ext2" else kind))
            check(rc == 0 and js is not None, f"{tag}: inspect failed")
            if js is None:
                continue
            check(js["verdict"] in ("repairable", "warnings") and (not (js["bootOk"] and js["backupOk"]) or dname.startswith("fsinfo")),
                  f"{tag}: inspection verdict {js['verdict']} bootOk={js['bootOk']} backupOk={js['backupOk']}")
            ids = [r["id"] for r in js["repairs"]]
            check(rid in ids, f"{tag}: expected repair {rid}, offered {ids}")
            if rid not in ids:
                continue
            if fsck_before is True and kind != "fat32":
                notes.append(f"{tag}: fsck did not notice this damage (inspection did)")
            bk = os.path.join(W, f"boot_{kind}_{dname}.bak")
            rc, js2, err = cli(img, "boot-repair", rid, "--backup", bk, "--fs", kind)
            check(rc == 0 and js2 and js2.get("ok") and js2.get("verified"), f"{tag}: repair -> {js2} {err[:100]}")
            if rc != 0:
                continue
            ok1, out1 = fsck(kind, img, lenient_ext=dname == "zero-first-8k")
            if ok1 is not None and ok0 is not False:
                check(ok1, f"{tag}: fsck after repair: {out1}")
            rc, js3, err = cli(img, "boot-inspect", "--fs", kind)
            check(js3 and js3["verdict"] in ("ok", "warnings") and js3["bootOk"], f"{tag}: re-inspection verdict {js3 and js3['verdict']}")
            # 書き込み前のバックアップを書き戻すと修復前のバイト列に戻る
            rc, js4, err = cli(img, "boot-restore", bk)
            check(rc == 0 and sha(img) == before_sha, f"{tag}: restoring the backup did not reproduce the pre-repair bytes")
        # 大きさを変えずに: 修復の計画は書かない (--dry) ことも確かめる
        img = os.path.join(W, f"boot_{kind}_dry.img")
        copy(base, img)
        cases[0][1](img)
        h = sha(img)
        cli(img, "boot-repair", cases[0][2], "--dry", "--fs", kind)
        check(sha(img) == h, f"boot {kind}: --dry changed the image")

    # FAT16: 検査と BPB の再構築の提案だけ (書かない)
    if have("mkfs.fat"):
        base = make_fs("fat16", 20 * MiB)
        if base:
            img = os.path.join(W, "boot_fat16_zero.img")
            copy(base, img)
            orig = open(img, "rb").read(512)
            zero(img, 0, 512)
            rc, js, err = cli(img, "boot-inspect", "--fs", "fat16")
            check(js and js["repairs"] == [] and js["proposedBpb"], f"boot fat16: inspection {js and js['verdict']}")
            if js and js["proposedBpb"]:
                pb = js["proposedBpb"]
                le = lambda o, n: int.from_bytes(orig[o:o + n], "little")
                check(int(pb["bytesPerSector"]) == le(11, 2) and int(pb["sectorsPerCluster"]) == orig[13] and int(pb["reservedSectors"]) == le(14, 2) and
                      int(pb["numFats"]) == orig[16] and int(pb["rootEntries"]) == le(17, 2) and int(pb["sectorsPerFat"]) == le(22, 2) and
                      int(pb["totalSectors"]) == (le(19, 2) or le(32, 4)), f"boot fat16: proposed BPB {pb} differs from the original")
            h = sha(img)
            rc, js2, err = cli(img, "boot-repair", "fat32-restore-primary-from-backup", "--dry", "--fs", "fat16")
            check(sha(img) == h and (js2 is None or not js2.get("ok")), "boot fat16: a write plan was made for FAT16")

    # パーティションの途中にある FAT32 (オフセット・長さ指定)
    if have("mkfs.fat") and have("sfdisk"):
        base = make_fs("fat32", 40 * MiB)
        if base:
            img = mkimg("boot_offset.img", 64 * MiB)
            place_fs(img, base, 4 * MB)
            zero(img, 4 * MB * 512, 512)
            before_tail = sha(img, 4 * MB * 512 + 40 * MiB)
            before_head = sha(img, 0, 4 * MB * 512)
            rc, js, err = cli(img, "boot-inspect", "--offset", str(4 * MB * 512), "--length", str(40 * MiB))
            check(js and js["fs"] == "fat32" and js["verdict"] == "repairable", f"boot offset: {js and js['verdict']}")
            rc, js2, err = cli(img, "boot-repair", "fat32-restore-primary-from-backup", "--offset", str(4 * MB * 512), "--length", str(40 * MiB), "--backup", os.path.join(W, "off.bak"))
            check(js2 and js2.get("ok"), f"boot offset: repair {js2}")
            check(sha(img, 4 * MB * 512 + 40 * MiB) == before_tail and sha(img, 0, 4 * MB * 512) == before_head, "boot offset: wrote outside the volume")


# ------------------------------------------------------------------ 本体

def main():
    global passed
    if not have("sfdisk") and not have("mkfs.fat"):
        print("SKIP: sfdisk / mkfs.* がありません")
        return 77
    shutil.rmtree(W, ignore_errors=True)
    os.makedirs(W, exist_ok=True)
    try:
        if have("sfdisk"):
            for spec in (GPT_SMALL, MBR_EXT, GPT_BIG, misc_spec()):
                if spec is None:
                    skipped.append("gpt_misc: udf / luks / erofs tools missing")
                    continue
                print(f"== {spec.name}")
                run_scenario(spec)
                # 次のシナリオに備えて、作業イメージを消す (tmpfs の節約)
                for f in os.listdir(W):
                    if f.startswith(("dmg_", "fix_", "disk_")):
                        os.remove(os.path.join(W, f))
        else:
            skipped.append("partition scenarios: sfdisk missing")
        print("== boot sectors")
        run_boot()
    finally:
        shutil.rmtree(W, ignore_errors=True)
    for n in notes:
        print("  info:", n)
    for s in skipped:
        print("  skipped:", s)
    print(f"{passed} checks passed, {len(failed)} failed")
    if failed:
        return 1
    if passed == 0:
        return 77
    return 0


if __name__ == "__main__":
    sys.exit(main())
