#!/usr/bin/env python3
"""パーティション編集の独立検証: 外部ツール (sfdisk / sgdisk / parted / fdisk / blkid / mkfs 各種) との双方向の照合。

使い方: check_partedit_tools.py <usbmem_partedit_cli のパス>

  ・外部ツールが作ったテーブルを自前のコードが正しく読む / 編集できる
  ・自前のコードが作った / 編集したテーブルを外部ツールが「問題なし」と判定する
  ・GPT の CRC・ヘッダ位置・配列の一致を Python で独立に再計算する
  ・バックアップファイルの CRC-32 / SHA-256 を Python で再計算する
  ・FS ラベル / UUID を blkid -p と比較する

作業ファイルは /dev/shm/usbmem_partedit_$USER 以下 (tmpfs)。実デバイスには触れない。
"""
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import zlib

CLI = sys.argv[1]
USER = os.environ.get("USER", "u")
W = f"/dev/shm/usbmem_partedit_{USER}/tools"
ENV = dict(os.environ)
ENV["PATH"] = ENV.get("PATH", "") + ":/sbin:/usr/sbin"
ENV["LC_ALL"] = "C.UTF-8"

passed = 0
failed = []
skipped = []


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


def mkimg(name, size):
    path = os.path.join(W, name)
    if os.path.exists(path):
        os.remove(path)
    with open(path, "wb") as f:
        f.truncate(size)
    return path


def cli(img, *args, sector=None, seed=7):
    cmd = [CLI, img]
    if sector:
        cmd += ["--sector", str(sector)]
    cmd += ["--seed", str(seed)] + list(args)
    rc, out, err = sh(cmd)
    last = None
    for line in out.strip().splitlines():
        try:
            last = json.loads(line)
        except ValueError:
            pass
    return rc, last, out, err


def sfdisk_json(img):
    rc, out, err = sh(["sfdisk", "--json", img])
    if rc != 0:
        return None
    return json.loads(out)["partitiontable"]


def skip(name, why):
    skipped.append(f"{name}: {why}")
    print(f"  SKIP {name}: {why}")


# ------------------------------------------------------------------ 独立した GPT / MBR 検査 (Python)

def check_gpt_raw(path, ss=512, label=""):
    """GPT をバイト列から再検証して問題の一覧を返す (外部ツールを使わない)。"""
    data = open(path, "rb").read()
    n = len(data) // ss
    probs = []

    def sec(i):
        return data[i * ss:(i + 1) * ss]

    mbr = sec(0)
    if mbr[510:512] != b"\x55\xaa":
        probs.append("no MBR signature")
    ent = [mbr[446 + 16 * i:446 + 16 * i + 16] for i in range(4)]
    prot = [e for e in ent if e[4] == 0xEE]
    if not prot:
        probs.append("no protective MBR")
    else:
        e = prot[0]
        start, size = struct.unpack("<II", e[8:16])
        if start != 1 or size != min(n - 1, 0xFFFFFFFF):
            probs.append(f"protective MBR spans {start}+{size}, expected 1+{min(n - 1, 0xFFFFFFFF)}")

    def header(lba):
        h = sec(lba)
        if h[:8] != b"EFI PART":
            return None
        (rev, hsz, crc, _res, my, alt, first, last) = struct.unpack("<IIIIQQQQ", h[8:56])
        guid = h[56:72]
        (elba, num, esz, ecrc) = struct.unpack("<QIII", h[72:92])
        body = bytearray(h[:hsz])
        body[16:20] = b"\0\0\0\0"
        ok = zlib.crc32(bytes(body)) & 0xFFFFFFFF == crc
        arr = data[elba * ss:elba * ss + num * esz]
        eok = len(arr) == num * esz and zlib.crc32(arr) & 0xFFFFFFFF == ecrc
        return dict(rev=rev, hsz=hsz, my=my, alt=alt, first=first, last=last, guid=guid, elba=elba, num=num, esz=esz,
                    hok=ok, eok=eok, arr=arr)

    p = header(1)
    b = header(n - 1)
    if not p or not p["hok"]:
        probs.append("primary header missing/bad CRC")
    if not b or not b["hok"]:
        probs.append("backup header missing/bad CRC")
    if p and p["hok"]:
        if not p["eok"]:
            probs.append("primary entries CRC")
        if p["my"] != 1 or p["alt"] != n - 1:
            probs.append(f"primary my/alt {p['my']}/{p['alt']}")
        if p["rev"] != 0x10000 or p["hsz"] != 92:
            probs.append("revision/header size")
    if b and b["hok"]:
        if not b["eok"]:
            probs.append("backup entries CRC")
        if b["my"] != n - 1 or b["alt"] != 1:
            probs.append(f"backup my/alt {b['my']}/{b['alt']}")
    if p and b and p["hok"] and b["hok"]:
        for k in ("guid", "first", "last", "num", "esz", "arr"):
            if p[k] != b[k]:
                probs.append(f"primary/backup differ in {k}")
        arr_secs = (b["num"] * b["esz"] + ss - 1) // ss
        if b["elba"] + arr_secs != n - 1:
            probs.append("backup entries not adjacent to backup header")
        if p["elba"] + arr_secs > p["first"]:
            probs.append("primary entries overlap usable range")
        if b["elba"] <= b["last"]:
            probs.append("backup entries overlap usable range")
        # エントリの検査
        rs = []
        for i in range(p["num"]):
            e = p["arr"][i * p["esz"]:(i + 1) * p["esz"]]
            if e[:16] == b"\0" * 16:
                continue
            fa, la = struct.unpack("<QQ", e[32:48])
            if fa > la or fa < p["first"] or la > p["last"]:
                probs.append(f"entry {i + 1} out of usable range")
            if e[16:32] == b"\0" * 16:
                probs.append(f"entry {i + 1} has zero unique GUID")
            rs.append((fa, la))
        rs.sort()
        for x, y in zip(rs, rs[1:]):
            if y[0] <= x[1]:
                probs.append("entries overlap")
    return probs


def check_backup_file(path, img, ss=512):
    f = open(path, "rb").read()
    probs = []
    if f[:8] != b"USBMTBK1":
        return ["bad magic"]
    body = f[:-36]
    crc = struct.unpack("<I", f[-36:-32])[0]
    if zlib.crc32(body) & 0xFFFFFFFF != crc:
        probs.append("CRC-32 mismatch")
    if hashlib.sha256(f[:-32]).digest() != f[-32:]:
        probs.append("SHA-256 mismatch")
    ver, hsz, bss, kind, disk_bytes, ts = struct.unpack("<IIIIQQ", f[8:40])
    cnt = struct.unpack("<I", f[56:60])[0]
    if ver != 1 or hsz != 64 or bss != ss:
        probs.append("header fields")
    if disk_bytes != os.path.getsize(img):
        probs.append("disk size field")
    pos = 64
    image = open(img, "rb")
    for _ in range(cnt):
        lba, k = struct.unpack("<QB", f[pos:pos + 9])
        ln = struct.unpack("<I", f[pos + 12:pos + 16])[0]
        data = f[pos + 16:pos + 16 + ln]
        image.seek(lba * ss)
        if image.read(ss) != data:
            probs.append(f"sector {lba} differs from the image")
        pos += 16 + ln
    if pos != len(f) - 36:
        probs.append("trailing bytes")
    return probs


# ------------------------------------------------------------------ 比較

def compare_with_sfdisk(img, ours, tag, partuuid=True):
    sj = sfdisk_json(img)
    if sj is None:
        check(False, f"{tag}: sfdisk --json failed")
        return
    ps = sj["partitions"]
    op = [p for p in ours["partitions"] if not p["protective"]]
    check(len(op) == len(ps), f"{tag}: partition count ours={len(op)} sfdisk={len(ps)}")
    if ours["kind"] == "gpt":
        check(ours["diskGuid"].upper() == sj["id"].upper(), f"{tag}: disk GUID {ours['diskGuid']} vs {sj['id']}")
        for a, b in zip(op, ps):
            check(int(a["startLba"]) == b["start"] and int(a["sectorCount"]) == b["size"], f"{tag}: range {a['index']}")
            check(a["typeGuid"].upper() == b["type"].upper(), f"{tag}: type GUID {a['index']}")
            check(a["uniqueGuid"].upper() == b["uuid"].upper(), f"{tag}: unique GUID {a['index']}")
            check(a["name"] == b.get("name", ""), f"{tag}: name {a['index']}: {a['name']!r} vs {b.get('name')!r}")
            if partuuid:
                check(a["partUuid"] == b["uuid"].lower(), f"{tag}: PARTUUID {a['index']}")
    else:
        check("0x" + ours["mbrDiskId"] == sj["id"], f"{tag}: disk signature {ours['mbrDiskId']} vs {sj['id']}")
        for a, b in zip(op, ps):
            check(int(a["startLba"]) == b["start"] and int(a["sectorCount"]) == b["size"], f"{tag}: range {a['index']}")
            check("%x" % a["mbrType"] == b["type"], f"{tag}: type {a['index']} {a['mbrType']:#x} vs {b['type']}")
            check(bool(a["bootable"]) == bool(b.get("bootable", False)), f"{tag}: active flag {a['index']}")
            if partuuid:
                check(a["partUuid"] == "%s-%02x" % (ours["mbrDiskId"], a["index"]), f"{tag}: PARTUUID {a['index']}")


def sgdisk_ok(img, tag):
    rc, out, err = sh(["sgdisk", "-v", img])
    check(rc == 0 and "No problems found" in out, f"{tag}: sgdisk -v: {out.strip()[:200]!r}")


def sfdisk_ok(img, tag):
    rc, out, err = sh(["sfdisk", "--verify", img])
    check(rc == 0, f"{tag}: sfdisk --verify: {(out + err).strip()[:200]!r}")


def parted_lists(img, tag, count):
    rc, out, err = sh(["parted", "-s", img, "unit", "s", "print"])
    check(rc == 0, f"{tag}: parted print rc={rc} {err.strip()[:120]!r}")
    lines = [l for l in out.splitlines() if l.strip() and l.strip()[0].isdigit()]
    check(len(lines) == count, f"{tag}: parted lists {len(lines)} partitions, expected {count}")
    return out


def fdisk_label(img, tag, kind, ident):
    rc, out, err = sh(["fdisk", "-l", img])
    check(rc == 0, f"{tag}: fdisk -l rc={rc}")
    check(f"Disklabel type: {kind}" in out, f"{tag}: fdisk label type")
    check(ident.lower() in out.lower(), f"{tag}: fdisk disk identifier {ident}")
    check("warning" not in out.lower() and "overlap" not in out.lower(), f"{tag}: fdisk warnings: {out.strip()[-200:]!r}")


# ------------------------------------------------------------------ シナリオ

MIB = 1 << 20


def scenario_external_to_ours():
    print("[A] 外部ツールが作った表を自前のコードが読む")
    if have("sgdisk"):
        img = mkimg("a_sgdisk.img", 64 * MIB)
        rc, out, err = sh(["sgdisk", "-o", "-n", "1:2048:+4M", "-t", "1:EF00", "-c", "1:EFI sys", "-n", "2:0:+10M", "-t", "2:8300", "-c", "2:日本語名",
                           "-u", "2:12345678-1234-1234-1234-123456789ABC", "-n", "3:0:0", "-t", "3:0700", "-A", "3:set:2",
                           "-U", "DEADBEEF-0000-4000-8000-0123456789AB", img])
        check(rc == 0, f"sgdisk create: {err.strip()[:100]}")
        rc, info, _, _ = cli(img, "info")
        check(info is not None and info["kind"] == "gpt", "A1: kind gpt")
        compare_with_sfdisk(img, info, "A1 sgdisk-made GPT")
        check(info["partitions"][2]["gptAttributes"] == str(1 << 2), "A1: attribute bit 2")
        check(not [d for d in info["diagnostics"] if d["severity"] == "error"], "A1: no error diagnostics")
        rc, ins, _, _ = cli(img, "inspect")
        check(ins["copiesAgree"] and ins["backupAtEnd"] and ins["mbrProtectiveOk"] and not ins["suggestedRepairs"], "A1: inspect clean")
        check(check_gpt_raw(img) == [], f"A1: raw GPT check {check_gpt_raw(img)}")
    else:
        skip("A1", "sgdisk not available")
    if have("sfdisk"):
        img = mkimg("a_dos.img", 64 * MIB)
        script = "label: dos\nlabel-id: 0x1234abcd\nstart=2048, size=8192, type=83, bootable\nstart=16384, size=100000, type=5\nstart=18432, size=8192, type=83\nstart=28672, size=8192, type=c\nstart=38912, size=2048, type=82\n"
        rc, out, err = sh(["sfdisk", img], input=script)
        check(rc == 0, f"sfdisk create dos: {err.strip()[:100]}")
        rc, info, _, _ = cli(img, "info")
        check(info["kind"] == "mbr", "A2: kind mbr")
        compare_with_sfdisk(img, info, "A2 sfdisk-made dos with 3 logicals")
        check(not [d for d in info["diagnostics"] if d["severity"] != "info"], f"A2: diagnostics {info['diagnostics']}")
    else:
        skip("A2", "sfdisk not available")
    if have("parted"):
        for label, cmds in (("gpt", ["mklabel", "gpt", "mkpart", "p1", "ext4", "1MiB", "10MiB", "mkpart", "p2", "fat32", "10MiB", "30MiB"]),
                            ("msdos", ["mklabel", "msdos", "mkpart", "primary", "ext4", "1MiB", "10MiB", "mkpart", "extended", "10MiB", "60MiB",
                                       "mkpart", "logical", "ext4", "11MiB", "20MiB", "mkpart", "logical", "fat32", "21MiB", "30MiB"])):
            img = mkimg(f"a_parted_{label}.img", 64 * MIB)
            rc, out, err = sh(["parted", "-s", img] + cmds)
            check(rc == 0, f"parted {label}: {err.strip()[:100]}")
            rc, info, _, _ = cli(img, "info")
            compare_with_sfdisk(img, info, f"A3 parted-made {label}")
    else:
        skip("A3", "parted not available")


def scenario_ours_to_external():
    print("[B] 自前のコードが作った表を外部ツールが検証する")
    # B1: 新規 GPT
    img = mkimg("b_gpt.img", 64 * MIB)
    rc, res, out, err = cli(img, "edit", "new-gpt", "+", "create", "start=2048", "count=8192", "type=esp", "name=EFI System", "+",
                            "create", "count=20000", "type=linux", "name=root \"quoted\"", "+", "create", "count=0", "type=msdata", "name=データ😀", "attrs=4")
    check(rc == 0 and res["result"]["ok"] and res["result"]["verified"], f"B1: apply {res and res.get('result')}")
    check(check_gpt_raw(img) == [], f"B1: raw GPT check {check_gpt_raw(img)}")
    if have("sgdisk"):
        sgdisk_ok(img, "B1")
        rc, out, err = sh(["sgdisk", "-p", img])
        check("EFI System" in out and "EF00" in out and "8300" in out and "0700" in out, f"B1: sgdisk -p {out[-300:]!r}")
        rc, out, err = sh(["sgdisk", "-i", "3", img])
        check("Attribute flags: 0000000000000004" in out and "Partition name: 'データ" in out, f"B1: sgdisk -i 3 {out!r}")
    rc, info, _, _ = cli(img, "info")
    if have("sfdisk"):
        sfdisk_ok(img, "B1")
        compare_with_sfdisk(img, info, "B1 our GPT")
    if have("parted"):
        parted_lists(img, "B1", 3)
    if have("fdisk"):
        fdisk_label(img, "B1", "gpt", info["diskGuid"])

    # B2: 新規 MBR (主 2 + 拡張 + 論理 3)
    img2 = mkimg("b_mbr.img", 64 * MIB)
    rc, res, out, err = cli(img2, "edit", "new-mbr", "+", "create", "role=p", "count=8192", "type=83", "boot=1", "+", "create", "role=p", "count=8192", "type=c",
                            "+", "create", "role=e", "count=0", "+", "create", "role=l", "count=4096", "type=83", "+", "create", "role=l", "count=4096", "type=82",
                            "+", "create", "role=l", "count=0", "type=7")
    check(rc == 0 and res["result"]["ok"], f"B2: apply {res and res.get('result', res)}")
    rc, info2, _, _ = cli(img2, "info")
    if have("sfdisk"):
        sfdisk_ok(img2, "B2")
        compare_with_sfdisk(img2, info2, "B2 our MBR with logicals")
    if have("parted"):
        parted_lists(img2, "B2", 6)
    if have("fdisk"):
        fdisk_label(img2, "B2", "dos", "0x" + info2["mbrDiskId"])

    # B3: sgdisk が作った表を自前で編集 → sgdisk が検証
    if have("sgdisk"):
        img3 = mkimg("b_edit_sgdisk.img", 64 * MIB)
        sh(["sgdisk", "-o", "-n", "1:2048:+4M", "-t", "1:EF00", "-c", "1:one", "-n", "2:0:+10M", "-c", "2:two", "-n", "3:0:+5M", "-c", "3:three", img3])
        sh(["sgdisk", f"--backup={W}/b3.sgd", img3])
        rc, before, _, _ = cli(img3, "info")
        rc, res, _, _ = cli(img3, "edit", "name", "1", "renamed", "+", "remove", "2", "+", "attrs", "3", "5", "+", "type", "3", "swap", "+",
                            "create", "count=4096", "type=lvm", "name=new", "+", "resize", "3", "4096", "+", "diskid", "01234567-89ab-cdef-0123-456789abcdef")
        check(rc == 0 and res["result"]["ok"], f"B3: apply {res.get('result') if res else res}")
        sgdisk_ok(img3, "B3")
        rc, out, _ = sh(["sgdisk", "-p", img3])
        check("renamed" in out and "8200" in out and "8E00" in out and "01234567-89AB-CDEF-0123-456789ABCDEF" in out, f"B3: sgdisk -p {out[-400:]!r}")
        rc, after, _, _ = cli(img3, "info")
        compare_with_sfdisk(img3, after, "B3 sgdisk-made then edited")
        # 逆方向: sgdisk が取ったバックアップで元に戻せる (sgdisk の復元 → 自前の読み取りが元と一致)
        rc, out, err = sh(["sgdisk", f"--load-backup={W}/b3.sgd", img3])
        check(rc == 0, f"B3: sgdisk --load-backup rc={rc} {err.strip()[:100]}")
        rc, restored, _, _ = cli(img3, "info")
        key = lambda t: [(p["startLba"], p["sectorCount"], p["typeGuid"], p["uniqueGuid"], p["name"]) for p in t["partitions"]]
        check(key(restored) == key(before) and restored["diskGuid"] == before["diskGuid"], "B3: sgdisk --load-backup restores the original table as we read it")

    # B4: sfdisk が作った MBR (論理あり) を自前で編集
    if have("sfdisk"):
        img4 = mkimg("b_edit_dos.img", 64 * MIB)
        script = "label: dos\nlabel-id: 0xcafe0001\nstart=2048, size=8192, type=83\nstart=16384, size=100000, type=5\nstart=18432, size=8192, type=83\nstart=28672, size=8192, type=c\nstart=38912, size=2048, type=82\n"
        sh(["sfdisk", img4], input=script)
        rc, res, _, _ = cli(img4, "edit", "remove", "6", "+", "type", "5", "7", "+", "boot", "1", "1", "+", "create", "role=l", "count=2048", "type=83", "+", "resize", "1", "4096", "+", "diskid", "0xdead0002")
        check(rc == 0 and res["result"]["ok"], f"B4: apply {res.get('result', res) if res else res}")
        sfdisk_ok(img4, "B4")
        rc, info4, _, _ = cli(img4, "info")
        compare_with_sfdisk(img4, info4, "B4 sfdisk-made dos then edited")

    # B5: 修復
    if have("sgdisk"):
        base = mkimg("b5_base.img", 32 * MIB)
        sh(["sgdisk", "-o", "-n", "1:2048:+4M", "-n", "2:0:+8M", "-c", "2:data", base])
        orig = open(base, "rb").read()
        # ディスクが大きくなった
        g = mkimg("b5_grow.img", 64 * MIB)
        open(g, "r+b").write(orig)
        rc, out, _ = sh(["sgdisk", "-v", g])
        check("doesn't reside" in out or "end of the disk" in out, f"B5: sgdisk notices a grown disk: {out[:200]!r}")
        rc, ins, _, _ = cli(g, "inspect")
        check(ins["diskGrew"] and "move-backup-to-end" in ins["suggestedRepairs"], "B5: inspect suggests move-backup-to-end")
        rc, res, _, _ = cli(g, "edit", "repair", "move-backup-to-end")
        check(rc == 0 and res["result"]["ok"], f"B5: move apply {res.get('result') if res else res}")
        sgdisk_ok(g, "B5 grown")
        check(check_gpt_raw(g) == [], f"B5 grown: raw {check_gpt_raw(g)}")
        # プライマリ破損 → バックアップから
        p = mkimg("b5_prim.img", 32 * MIB)
        open(p, "r+b").write(orig)
        with open(p, "r+b") as f:
            f.seek(512 + 40)
            f.write(b"\xff\xff")
        rc, out, _ = sh(["sgdisk", "-v", p])
        check("problem" in out.lower() or "corrupt" in out.lower() or "invalid" in out.lower(), f"B5: sgdisk sees the damaged primary: {out[:160]!r}")
        rc, res, _, _ = cli(p, "edit", "repair", "copy-backup-to-primary")
        check(rc == 0 and res["result"]["ok"], "B5: copy-backup-to-primary applied")
        sgdisk_ok(p, "B5 primary")
        check(open(p, "rb").read() == orig, "B5: primary repair reproduces the original bytes exactly")
        # バックアップ破損 → プライマリから
        q = mkimg("b5_back.img", 32 * MIB)
        open(q, "r+b").write(orig)
        with open(q, "r+b") as f:
            f.seek(32 * MIB - 512)
            f.write(b"\0" * 512)
        rc, res, _, _ = cli(q, "edit", "repair", "copy-primary-to-backup")
        check(rc == 0 and res["result"]["ok"], "B5: copy-primary-to-backup applied")
        sgdisk_ok(q, "B5 backup")
        check(open(q, "rb").read() == orig, "B5: backup repair reproduces the original bytes exactly")
        # 縮んだイメージ
        s = mkimg("b5_trunc.img", 20 * MIB)
        open(s, "wb").write(orig[:20 * MIB])
        rc, ins, _, _ = cli(s, "inspect")
        check(ins["backupBeyondDisk"] and "move-backup-to-end" in ins["suggestedRepairs"], "B5: truncated image is repairable")
        rc, res, _, _ = cli(s, "edit", "repair", "move-backup-to-end")
        check(rc == 0 and res["result"]["ok"], f"B5: truncated repair {res.get('result', res) if res else res}")
        sgdisk_ok(s, "B5 truncated")
        # 保護 MBR
        r = mkimg("b5_mbr.img", 32 * MIB)
        open(r, "r+b").write(orig)
        with open(r, "r+b") as f:
            f.seek(446 + 12)
            f.write(b"\x10\x00\x00\x00")
        rc, res, _, _ = cli(r, "edit", "repair", "rebuild-protective-mbr")
        check(rc == 0 and res["result"]["ok"], "B5: protective MBR rebuilt")
        check(open(r, "rb").read() == orig, "B5: protective MBR rebuild reproduces the original bytes")


def scenario_backup_restore():
    print("[C] バックアップ / 復元 / sfdisk 互換出力")
    if not have("sgdisk"):
        skip("C", "sgdisk not available")
        return
    img = mkimg("c.img", 32 * MIB)
    sh(["sgdisk", "-o", "-n", "1:2048:+4M", "-c", "1:alpha", "-n", "2:0:+8M", "-c", "2:beta", img])
    orig = open(img, "rb").read()
    bk = os.path.join(W, "c.bak")
    rc, _, _, _ = cli(img, "backup", bk)
    check(rc == 0 and check_backup_file(bk, img) == [], f"C1: backup file independent check {check_backup_file(bk, img)}")
    # sgdisk で壊してから自前の復元で戻す (テーブル領域のバイト列が完全に元通り)
    sh(["sgdisk", "-d", "1", "-n", "3:0:+2M", img])
    check(open(img, "rb").read() != orig, "C2: image changed by sgdisk")
    pre = os.path.join(W, "c.pre")
    rc, res, _, _ = cli(img, "--pre-backup", pre, "restore", bk)
    check(rc == 0 and res["result"]["ok"] and res["result"]["verified"], f"C2: restore {res.get('result') if res else res}")
    check(open(img, "rb").read() == orig, "C2: restore reproduces the original image byte for byte")
    check(os.path.exists(pre) and check_backup_file(pre, img) != None, "C2: automatic pre-restore backup written")
    # 自動バックアップから元の(壊れた)状態にも戻せる
    rc, res, _, _ = cli(img, "restore", pre)
    check(rc == 0 and res["result"]["ok"], "C2: the automatic backup restores the pre-restore table")
    rc, res, _, _ = cli(img, "restore", bk)
    check(open(img, "rb").read() == orig, "C2: and back again")
    # サイズ違いは force が必要
    big = mkimg("c_big.img", 64 * MIB)
    sh(["sgdisk", "-o", "-n", "1:2048:+1M", big])
    rc, res, _, _ = cli(big, "--dry", "restore", bk)
    check(res["plan"]["requiresForce"] and res["plan"]["confirmPhrase"].startswith("FORCE "), "C3: size mismatch requires FORCE")
    rc, res, _, _ = cli(big, "restore", bk)
    check(rc == 4 and res["result"]["code"] == "force-required", "C3: refused without --force")
    rc, res, _, _ = cli(big, "--force", "restore", bk)
    check(rc == 0 and res["result"]["ok"], "C3: applied with --force")
    # 壊れたバックアップは拒否
    data = bytearray(open(bk, "rb").read())
    data[100] ^= 1
    bad = os.path.join(W, "c.bad")
    open(bad, "wb").write(data)
    rc, res, out, _ = cli(img, "restore", bad)
    check(rc == 2 and res["code"] == "backup-corrupt", "C4: corrupt backup is rejected")

    # sfdisk --dump 互換のテキスト: sfdisk が読み込め、同じ表になる
    for src in (img, os.path.join(W, "b_mbr.img"), os.path.join(W, "a_dos.img")):
        if not os.path.exists(src):
            continue
        rc, out, err = sh([CLI, src, "sfdisk", src])
        check(rc == 0 and out.startswith("label:"), f"C5: export {os.path.basename(src)}")
        dst = mkimg("c_export.img", os.path.getsize(src))
        rc, o2, e2 = sh(["sfdisk", dst], input=out)
        check(rc == 0, f"C5: sfdisk accepts our export of {os.path.basename(src)}: {e2.strip()[:160]}")
        a, b = sfdisk_json(src), sfdisk_json(dst)
        strip = lambda t: ([{k: v for k, v in p.items() if k != "node"} for p in t["partitions"]], t["id"], t["label"])
        check(strip(a) == strip(b), f"C5: re-created table equals the original ({os.path.basename(src)})")
        # sfdisk -d の出力と、項目レベルで一致 (空白の幅は無視)
        rc, d1, _ = sh(["sfdisk", "-d", src])
        import re
        norm = lambda s: [re.sub(r"=\s+", "=", " ".join(l.split())).replace(src, "DEV") for l in s.splitlines() if l and not l.startswith("device:")]
        n1, n2 = norm(d1), norm(out)
        check(n1 == n2, f"C5: export equals sfdisk -d for {os.path.basename(src)}:\n    {n1}\n    {n2}")


FS_SPECS = []


def build_fs_images():
    out = []
    mb = MIB

    def add(name, make, expect_type):
        path = mkimg(f"fs_{name}.img", 0)
        out.append((name, path, make, expect_type))

    def mk(name, size, cmd, expect_type, post=None):
        path = mkimg(f"fs_{name}.img", size)
        rc, o, e = sh([c.replace("{img}", path) for c in cmd])
        if rc != 0:
            skip(f"label {name}", f"mkfs failed: {e.strip()[:100] or o.strip()[:100]}")
            return
        out.append((name, path, expect_type))

    if have("mkfs.fat"):
        mk("fat12", 2 * mb, ["mkfs.fat", "-F", "12", "-n", "FAT12LBL", "-i", "A1B2C3D4", "{img}"], "vfat")
        mk("fat16", 20 * mb, ["mkfs.fat", "-F", "16", "-n", "FAT16LBL", "-i", "0BADF00D", "{img}"], "vfat")
        mk("fat32", 40 * mb, ["mkfs.fat", "-F", "32", "-n", "Fat 32 Lbl", "-i", "CAFEBABE", "{img}"], "vfat")
        mk("fat32_nolabel", 40 * mb, ["mkfs.fat", "-F", "32", "-i", "12345678", "{img}"], "vfat")
    if have("mkfs.exfat"):
        mk("exfat", 16 * mb, ["mkfs.exfat", "-L", "ExFAT ラベル", "-U", "11223344-5566-7788-99aa-bbccddeeff00", "{img}"], "exfat")
        mk("exfat_nolabel", 16 * mb, ["mkfs.exfat", "{img}"], "exfat")
    if have("mke2fs"):
        mk("ext2", 8 * mb, ["mke2fs", "-q", "-t", "ext2", "-L", "ext2lbl", "-U", "11111111-2222-3333-4444-555555555555", "{img}"], "ext2")
        mk("ext4", 16 * mb, ["mke2fs", "-q", "-t", "ext4", "-L", "ext4 label", "-U", "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", "{img}"], "ext4")
    if have("mkntfs"):
        mk("ntfs", 16 * mb, ["mkntfs", "-F", "-Q", "-q", "-L", "NTFS ラベル", "-s", "512", "{img}"], "ntfs")
    if have("genisoimage"):
        d = os.path.join(W, "isoroot")
        os.makedirs(d, exist_ok=True)
        open(os.path.join(d, "f.txt"), "w").write("x")
        mk("iso", 0, ["genisoimage", "-quiet", "-V", "ISOLBL", "-o", "{img}", d], "iso9660")
        # genisoimage はファイルを作るので mkimg(0) の後に上書きされる。サイズは結果のまま
    if have("mkudffs"):
        mk("udf", 8 * mb, ["mkudffs", "--label=UDFLabel", "--vsid=Vol123", "--media-type=hd", "{img}"], "udf")
    if have("mksquashfs"):
        d = os.path.join(W, "isoroot")
        os.makedirs(d, exist_ok=True)
        path = os.path.join(W, "fs_squashfs.img")
        if os.path.exists(path):
            os.remove(path)
        rc, o, e = sh(["mksquashfs", d, path, "-quiet", "-no-progress"])
        if rc == 0:
            out.append(("squashfs", path, "squashfs"))
    if have("mkfs.erofs"):
        d = os.path.join(W, "isoroot")
        path = os.path.join(W, "fs_erofs.img")
        if os.path.exists(path):
            os.remove(path)
        rc, o, e = sh(["mkfs.erofs", "-L", "erofslbl", "-U", "77777777-8888-9999-aaaa-bbbbbbbbbbbb", path, d])
        if rc == 0:
            out.append(("erofs", path, "erofs"))
    if have("mkfs.btrfs"):
        mk("btrfs", 128 * mb, ["mkfs.btrfs", "-q", "-L", "btrfslbl", "-U", "99999999-aaaa-bbbb-cccc-dddddddddddd", "{img}"], "btrfs")
    if have("mkfs.xfs"):
        mk("xfs", 320 * mb, ["mkfs.xfs", "-q", "-L", "xfslbl", "-m", "uuid=88888888-9999-aaaa-bbbb-cccccccccccc", "{img}"], "xfs")
    if have("cryptsetup"):
        path = mkimg("fs_luks2.img", 32 * mb)
        key = os.path.join(W, "luks.key")
        open(key, "wb").write(b"k" * 32)
        rc, o, e = sh(["cryptsetup", "luksFormat", "--type", "luks2", "--label", "lukslbl", "--uuid", "66666666-7777-8888-9999-aaaaaaaaaaaa", "--batch-mode",
                       "--pbkdf", "pbkdf2", "--pbkdf-force-iterations", "1000", "--key-file", key, path])
        if rc == 0:
            out.append(("luks2", path, "crypto_LUKS"))
        else:
            skip("label luks2", e.strip()[:100])
    return out


def scenario_labels():
    print("[D] FS ラベル / UUID / シリアル (blkid -p と比較。FS は GPT のパーティション内に置く)")
    if not have("blkid"):
        skip("D", "blkid not available")
        return
    for name, path, expect_type in build_fs_images():
        bl = {}
        for tag in ("TYPE", "LABEL", "UUID"):
            rc, out, err = sh(["blkid", "-p", "-s", tag, "-o", "value", path])
            if rc == 0:
                bl[tag] = out.rstrip("\n")
        if bl.get("TYPE") != expect_type:
            skip(f"label {name}", f"blkid TYPE={bl.get('TYPE')} (expected {expect_type})")
            continue
        size = os.path.getsize(path)
        secs = (size + 511) // 512
        cont = mkimg(f"d_{name}.img", (secs + 4096 + 2048) * 512)
        rc, res, _, _ = cli(cont, "edit", "new-gpt", "+", "create", "start=2048", f"count={secs}", "type=msdata")
        check(rc == 0 and res["result"]["ok"], f"D {name}: container created")
        with open(cont, "r+b") as c, open(path, "rb") as f:
            c.seek(2048 * 512)
            shutil.copyfileobj(f, c)
        rc, info, _, _ = cli(cont, "labels")
        p = info["partitions"][0]
        exp_label = bl.get("LABEL", "")
        check(p["fsLabel"] == exp_label, f"D {name}: label ours={p['fsLabel']!r} blkid={exp_label!r}")
        exp_uuid = bl.get("UUID", "")
        if name == "udf":
            print(f"    (udf uuid: ours={p['fsUuid']!r} blkid={exp_uuid!r} — not compared)")
        elif name == "exfat":
            check(p["fsUuid"] == exp_uuid or p["fsSerial"] == exp_uuid, f"D {name}: serial/uuid ours={p['fsUuid']!r}/{p['fsSerial']!r} blkid={exp_uuid!r}")
            check(p.get("fsGuid", "").lower() == "11223344-5566-7788-99aa-bbccddeeff00", f"D {name}: volume GUID {p.get('fsGuid')!r}")
        elif name in ("squashfs",):
            pass
        else:
            check(p["fsUuid"].lower() == exp_uuid.lower(), f"D {name}: uuid ours={p['fsUuid']!r} blkid={exp_uuid!r}")
        if name.startswith("fat") or name.startswith("exfat") or name == "ntfs":
            check(p["fsSerial"] != "", f"D {name}: serial present")
        if name in ("squashfs", "erofs", "luks2"):
            check(p["fsDetected"] == ("luks" if name == "luks2" else name) or p["fs"] != "unknown", f"D {name}: detected={p['fsDetected']!r} fs={p['fs']!r}")


def main():
    os.makedirs(W, exist_ok=True)
    for t in ("sfdisk", "sgdisk", "parted", "fdisk", "blkid"):
        print(f"  tool {t}: {'yes' if have(t) else 'NO'}")
    scenario_external_to_ours()
    scenario_ours_to_external()
    scenario_backup_restore()
    scenario_labels()
    print(f"\n{passed} checks passed, {len(failed)} failed, {len(skipped)} skipped")
    for s in skipped:
        print("  skipped:", s)
    shutil.rmtree(W, ignore_errors=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
