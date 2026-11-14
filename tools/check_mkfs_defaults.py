#!/usr/bin/env python3
"""最新の mkfs の「既定」で作ったイメージの機能フラグを、自前実装の対応表と突き合わせる。

自前実装 (XFS / Btrfs / F2FS) は、知らない機能フラグを拒否する (黙って壊さない)。その代わり、
mkfs の既定が新しい機能を立てるようになると、その FS は読み取り専用で開く (または開けない) ことになる。
この検査は、それを早く見つけるためのもの。

  tools/check_mkfs_defaults.py [--json] [--matrix tools/fs_feature_matrix.json] [--only xfs,btrfs,f2fs]

終了コード: 0 = 既定のフラグがすべて表にあり、書き込めない機能は無い
            1 = 表に無い (= 新しい) フラグが立っていた → 対応方針を決めて tools/fs_feature_matrix.json と docs を直す
            2 = 表にはあるが、書き込みが拒否される (ro) / マウントを拒否する (refused) 機能が既定で立っていた (「既知の穴」)。
                --strict を付けたときだけ失敗として扱う (付けなければ 0 で、報告だけ)
            3 = 検査できなかった (mkfs が無い、など)。--only で絞ったとき以外は失敗

イメージは一時ディレクトリ (/dev/shm に 2 GiB 以上の空きがあればそこ、無ければ TMPDIR) の疎ファイルに作る。root は要らない。
"""
import argparse, json, os, shutil, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SBIN = ["/sbin", "/usr/sbin"]


def find_tool(name):
    p = shutil.which(name, path=os.pathsep.join([os.environ.get("PATH", "")] + SBIN))
    return p


def tool_version(cmd):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        return ((r.stdout or "") + (r.stderr or "")).strip().splitlines()[0]
    except Exception as e:  # noqa: BLE001
        return f"? ({e})"


def mk_image(workdir, name, size):
    p = os.path.join(workdir, name)
    with open(p, "wb") as f:
        f.truncate(size)
    return p


def read_at(path, off, n):
    with open(path, "rb") as f:
        f.seek(off)
        return f.read(n)


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"{' '.join(cmd)} -> {r.returncode}: {(r.stderr or r.stdout).strip()[:300]}")


def probe_xfs(workdir, m):
    mkfs = find_tool("mkfs.xfs")
    if not mkfs:
        return None
    img = mk_image(workdir, "xfs.img", 512 << 20)
    run([mkfs, "-q", "-f", img])  # 既定のまま (-m / -n の指定なし)
    sb = read_at(img, 0, 512)
    if sb[:4] != b"XFSB":
        raise RuntimeError("XFS のマジックが無い")
    o = m["offsets"]
    ver = struct.unpack(">H", sb[100:102])[0]
    vals = {k: struct.unpack(">I", sb[v:v + 4])[0] for k, v in o.items()}
    return {"version": tool_version([mkfs, "-V"]), "fields": vals, "extra": {"sb_version": ver & 0xf}}


def probe_btrfs(workdir, m):
    mkfs = find_tool("mkfs.btrfs")
    if not mkfs:
        return None
    img = mk_image(workdir, "btrfs.img", 256 << 20)
    run([mkfs, "-q", "-f", img])
    sb = read_at(img, 0x10000, 4096)
    if sb[0x40:0x48] != b"_BHRfS_M":
        raise RuntimeError("Btrfs のマジックが無い")
    vals = {k: struct.unpack("<Q", sb[v:v + 8])[0] for k, v in m["offsets"].items()}
    return {"version": tool_version([mkfs, "--version"]), "fields": vals, "extra": {}}


def probe_f2fs(workdir, m):
    mkfs = find_tool("mkfs.f2fs")
    if not mkfs:
        return None
    img = mk_image(workdir, "f2fs.img", 128 << 20)
    run([mkfs, "-q", "-f", img])
    base = m["offsets"]["superblock"]
    sb = read_at(img, base, 4096)
    if struct.unpack("<I", sb[:4])[0] != 0xF2F52010:
        raise RuntimeError("F2FS のマジックが無い")
    return {"version": tool_version([mkfs, "-V"]), "fields": {"feature": struct.unpack("<I", sb[m["offsets"]["feature"]:m["offsets"]["feature"] + 4])[0]},
            "extra": {}}


PROBES = {"xfs": probe_xfs, "btrfs": probe_btrfs, "f2fs": probe_f2fs}


def classify(fs, probe, m):
    """立っているビットを表に当てはめる。戻り値: [(field, bit, name, status, note)]"""
    out = []
    for field, val in probe["fields"].items():
        table = m.get(field, {})
        bit = 0
        while val >> bit:
            if (val >> bit) & 1:
                e = table.get(str(bit))
                if e is None:
                    out.append((field, bit, f"bit{bit}", "NEW", "表に無い機能"))
                else:
                    out.append((field, bit, e["name"], e["status"], e.get("note", "")))
            bit += 1
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--matrix", default=os.path.join(HERE, "fs_feature_matrix.json"))
    ap.add_argument("--only", default="")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--strict", action="store_true", help="既知の穴 (ro / refused) も失敗にする")
    args = ap.parse_args()
    matrix = json.load(open(args.matrix, encoding="utf-8"))
    want = [x for x in args.only.split(",") if x] or list(PROBES)
    tmp_root = None  # 既定は TMPDIR / /tmp。メモリ上 (/dev/shm) に十分な空きがあるときだけ、そちらを使う
    if os.path.isdir("/dev/shm") and os.access("/dev/shm", os.W_OK):
        st = os.statvfs("/dev/shm")
        if st.f_bavail * st.f_frsize >= 2 << 30:
            tmp_root = "/dev/shm"
    report, rc = {}, 0
    with tempfile.TemporaryDirectory(prefix="mkfsdef.", dir=tmp_root) as wd:
        for fs in want:
            try:
                p = PROBES[fs](wd, matrix[fs])
            except Exception as e:  # noqa: BLE001
                report[fs] = {"error": str(e)}
                rc = max(rc, 3)
                continue
            if p is None:
                report[fs] = {"error": "mkfs が見つからない"}
                if args.only == "":
                    rc = max(rc, 3)
                continue
            items = classify(fs, p, matrix[fs])
            new = [i for i in items if i[3] == "NEW"]
            gaps = [i for i in items if i[3] in ("ro", "refused")]
            verdict = "NEW" if new else ("GAP" if gaps else "OK")
            report[fs] = {"version": p["version"], "raw": {k: hex(v) for k, v in p["fields"].items()}, "flags": items, "verdict": verdict}
            if new:
                rc = max(rc, 1)
            elif gaps and args.strict:
                rc = max(rc, 2)
    if args.json:
        print(json.dumps(report, ensure_ascii=False, indent=1))
    else:
        for fs, r in report.items():
            print(f"== {fs}: {r.get('version', '')}")
            if "error" in r:
                print(f"   検査できなかった: {r['error']}")
                continue
            print(f"   既定のフラグ (生の値): " + ", ".join(f"{k}={v}" for k, v in r["raw"].items()))
            for field, bit, name, status, note in r["flags"]:
                mark = {"rw": "  ok ", "ro": " 穴  ", "refused": " 穴  ", "NEW": " NEW "}[status]
                print(f"   [{mark}] {field}[{bit}] {name}: {status}" + (f"  ({note})" if note else ""))
            msg = {"OK": "既定で作ったイメージは、すべて読み書きできる",
                   "GAP": "既定で作ったイメージは、読み取り専用になる / 開けない機能を含む (既知の穴)",
                   "NEW": "表に無い機能が既定で立っている → 対応方針を決めること"}[r["verdict"]]
            print(f"   結論: {r['verdict']} - {msg}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
