#!/usr/bin/env python3
# usbmem_fuse を xfstests (fstests) で試す実行スクリプト (GitHub Actions / ローカルの root 環境用)。docs/xfstests.md
#
#   sudo tools/fuse/run_xfstests.py --fs ext4 --fstests /path/to/fstests --out /path/to/out [--group quick | --tests generic/001,generic/002]
#
# やること: (1) イメージを mkfs で作る (TEST / SCRATCH) (2) /sbin/mount.fuse.usbmem と /etc/usbmem_fuse.env を置く
#           (3) fstests の local.config を書く (FSTYP=fuse / FUSE_SUBTYP=.usbmem) (4) テストを 1 件ずつ `./check` で実行 (タイムアウト付き。ハングしても次へ進む)
#           (5) 結果 (pass / fail / notrun / hang と、アンマウント時の fsck の結果) を out/results.json と out/summary.md に書く。
# 1 件ずつ実行するのは、アダプターがハング・クラッシュしても全体が止まらないようにするため (check の起動のたびに 1 秒ほどかかるが許容する)。
import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))

# FS ごと: mkfs と、アンマウントのたびに走らせる検査 (USBMEM_FUSE_FSCK。$1 = イメージ)
FS = {
    "ext4": dict(mkfs=["mkfs.ext4", "-q", "-F", "-b", "4096"], fsck='e2fsck -fn "$1"'),
    "ext2": dict(mkfs=["mkfs.ext2", "-q", "-F", "-b", "4096"], fsck='e2fsck -fn "$1"'),
    "fat": dict(mkfs=["mkfs.fat", "-F", "32", "-s", "8"], fsck='fsck.fat -n "$1"'),
    "exfat": dict(mkfs=["mkfs.exfat"], fsck='fsck.exfat -n "$1"'),
    "ntfs": dict(mkfs=["mkntfs", "-F", "-Q", "-q"], fsck=None),
    "xfs": dict(mkfs=["mkfs.xfs", "-q", "-f", "-m", "rmapbt=0", "-n", "parent=0"], fsck='xfs_repair -n "$1"'),
    "btrfs": dict(mkfs=["mkfs.btrfs", "-q", "-f"], fsck='btrfs check --readonly "$1"'),
    "f2fs": dict(mkfs=["mkfs.f2fs", "-q", "-f"], fsck='fsck.f2fs --dry-run -f "$1"'),
}


def sh(cmd, **kw):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **kw)


# notrun の理由 → 分類 (正規表現は小文字化した理由に当てる)。
#   saf_unusable   : SAF (Android の Storage Access Framework) 経由では使えない操作 (シンボリックリンク / ハードリンク / fallocate / 開いたままの ftruncate / 直接 I/O など)。core が対応しても
#                    アプリからは届かないので、対応を急がない
#   core_unsupported: 自前実装 (core の FS API) に無い操作 (権限・所有者・ACL・xattr・時刻の細かい制御・クォータ・SEEK_DATA / HOLE など)。FUSE アダプターは正直に ENOSYS / EOPNOTSUPP を返す
#   environment    : 前提 (実デバイス・別のデバイス・カーネルの機能・ツール) が FUSE / この runner に無い
NOTRUN_RULES = [
    ("saf_unusable", r"symlink|hardlink|hard link|fallocate|falloc|punch|zero.?range|collapse|insert.?range|o_direct|direct.?io|dio|reflink|clone|dedupe|copy_file_range|mknod|fifo|device special|swap|exchange|whiteout"),
    ("core_unsupported", r"xattr|attr|acl|chown|chmod|permission|setuid|uid|gid|quota|seek_data|seek_hole|fiemap|fsync|utimes|timestamp|atime|ctime|btime|birth|mtime|casefold|encrypt|verity|immutable|chattr|lsattr|project|idmap|fs-verity|ioctl|extent|sparse|hole"),
    ("environment", r"scratch|logwrites|dm-|flakey|loop|device|kernel|require|not supported by|does not support|need|must|root|mount option|journal|log"),
]


def classify_notrun(reason):
    r = reason.lower()
    for cat, pat in NOTRUN_RULES:
        if re.search(pat, r):
            return cat
    return "other"


def load_expected(fsname):
    """tools/fuse/xfstests_expected.json: {"<fs>" or "*": {"generic/NNN": {"cat": "...", "why": "..."}}}。cat は saf_unusable / core_unsupported / premise_mismatch / core_bug / adapter_bug。"""
    path = os.path.join(HERE, "xfstests_expected.json")
    if not os.path.exists(path):
        return {}
    d = json.load(open(path))
    out = dict(d.get("*", {}))
    out.update(d.get(fsname, {}))
    return {k: v for k, v in out.items() if not k.startswith("_")}


def mk_one(fsname, p, size):
    if os.path.exists(p):
        os.unlink(p)
    with open(p, "wb") as f:
        f.truncate(size)
    return sh(FS[fsname]["mkfs"] + [p])


def mk_images(fsname, work, size):
    imgs = {}
    for role in ("test", "scratch"):
        p = os.path.join(work, role + ".img")
        r = mk_one(fsname, p, size)
        if r.returncode != 0:
            sys.exit("mkfs failed for %s: %s" % (p, r.stdout))
        imgs[role] = p
    return imgs


def group_tests(fstests, group):
    """tests/generic と tests/shared から、_begin_fstest の行に group を持つものを返す (check -g と同じ選び方)。"""
    out = []
    for d in ("generic", "shared"):
        base = os.path.join(fstests, "tests", d)
        if not os.path.isdir(base):
            continue
        for n in sorted(os.listdir(base)):
            if not re.fullmatch(r"\d+", n):
                continue
            try:
                txt = open(os.path.join(base, n), errors="replace").read(4000)
            except OSError:
                continue
            m = re.search(r"^_begin_fstest\s+(.*)$", txt, re.M)
            if m and group in m.group(1).split():
                out.append("%s/%s" % (d, n))
    return out


def umount_all(dirs):
    for d in dirs:
        sh(["umount", "-l", d])
    sh(["pkill", "-9", "-f", "usbmem_fuse"])


def read_tail(path, n=40):
    try:
        with open(path, errors="replace") as f:
            return "".join(f.readlines()[-n:])
    except OSError:
        return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fs", required=True, choices=sorted(FS))
    ap.add_argument("--fstests", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--work", default="/mnt/usbmem_xfstests")
    ap.add_argument("--bin", default="/usr/local/bin/usbmem_fuse")
    ap.add_argument("--group", default="quick")
    ap.add_argument("--tests", default="", help="カンマ区切りのテスト名 (指定すると --group は使わない)")
    ap.add_argument("--exclude", default="", help="除外するテスト名 (カンマ区切り)")
    ap.add_argument("--timeout", type=int, default=300, help="1 件あたりの秒数")
    ap.add_argument("--size", default="", help="イメージの大きさ。既定は 1G、疎ファイルを持てない FAT / exFAT は 6G (fsstress の穴のあるファイルが実際に領域を食うため。generic/013 で ENOSPC になる)")
    ap.add_argument("--mount-opts", default="allow_other,default_permissions", help="usbmem_fuse の -o (quiet_setattr など)")
    ap.add_argument("--no-remkfs", action="store_true", help="テストごとにイメージを作り直さない (既定は作り直す。FSTYP=fuse の fstests は mkfs をしないので、前のテストの残りや破損が次へ持ち越される)")
    ap.add_argument("--max-seconds", type=int, default=0, help="全体の上限 (0 = 無制限)。超えたら残りは skipped-by-budget")
    a = ap.parse_args()

    if not a.size:
        a.size = "6G" if a.fs in ("fat", "exfat") else "1G"
    size = int(re.fullmatch(r"(\d+)([KMG]?)", a.size).group(1)) * {"": 1, "K": 1 << 10, "M": 1 << 20, "G": 1 << 30}[re.fullmatch(r"(\d+)([KMG]?)", a.size).group(2)]
    os.makedirs(a.out, exist_ok=True)
    os.makedirs(a.work, exist_ok=True)
    test_mnt, scratch_mnt = os.path.join(a.work, "mnt_test"), os.path.join(a.work, "mnt_scratch")
    os.makedirs(test_mnt, exist_ok=True)
    os.makedirs(scratch_mnt, exist_ok=True)
    imgs = mk_images(a.fs, a.work, size)

    fsck_log = os.path.join(a.out, "fsck.log")
    fuse_log = os.path.join(a.out, "usbmem_fuse.log")
    for p in (fsck_log, fuse_log, fsck_log + ".out"):
        open(p, "w").close()
    fsck = FS[a.fs]["fsck"]
    with open("/etc/usbmem_fuse.env", "w") as f:
        f.write("USBMEM_FUSE_BIN=%s\n" % a.bin)
        f.write("USBMEM_FUSE_LOG=%s\n" % fuse_log)
        if fsck:
            f.write("USBMEM_FUSE_FSCK='%s >>%s.out 2>&1'\n" % (fsck, fsck_log))
            f.write("USBMEM_FUSE_FSCK_LOG=%s\n" % fsck_log)
        if os.environ.get("USBMEM_FUSE_TRACE") == "1":
            f.write("USBMEM_FUSE_TRACE=1\n")
    shutil.copy(os.path.join(HERE, "mount.fuse.usbmem"), "/sbin/mount.fuse.usbmem")
    os.chmod("/sbin/mount.fuse.usbmem", 0o755)

    mopts = "-o fstype=%s,%s" % (a.fs, a.mount_opts) if a.mount_opts else "-o fstype=%s" % a.fs
    with open(os.path.join(a.fstests, "local.config"), "w") as f:
        f.write("export FSTYP=fuse\nexport FUSE_SUBTYP=.usbmem\n")
        f.write("export TEST_DEV=%s\nexport TEST_DIR=%s\n" % (imgs["test"], test_mnt))
        f.write("export SCRATCH_DEV=%s\nexport SCRATCH_MNT=%s\n" % (imgs["scratch"], scratch_mnt))
        f.write('export TEST_FS_MOUNT_OPTS="%s"\nexport MOUNT_OPTIONS="%s"\n' % (mopts, mopts))

    if a.tests:
        tests = [t.strip() for t in a.tests.split(",") if t.strip()]
    else:
        tests = group_tests(a.fstests, a.group)
    excl = set(t.strip() for t in a.exclude.split(",") if t.strip())
    tests = [t for t in tests if t not in excl]
    print("fs=%s tests=%d" % (a.fs, len(tests)), flush=True)

    results = {}
    t_start = time.time()
    results_dir = os.path.join(a.fstests, "results")
    env = dict(os.environ)
    env["PATH"] = env.get("PATH", "") + ":/sbin:/usr/sbin"
    for i, t in enumerate(tests):
        if a.max_seconds and time.time() - t_start > a.max_seconds:
            results[t] = dict(status="skipped-by-budget")
            continue
        shutil.rmtree(results_dir, ignore_errors=True)
        if not a.no_remkfs and i > 0:
            for role in ("test", "scratch"):
                r = mk_one(a.fs, imgs[role], size)
                if r.returncode != 0:
                    print("remkfs failed %s: %s" % (role, r.stdout[:300]), flush=True)
        fsck_before = len(read_tail(fsck_log, 100000).splitlines())
        t0 = time.time()
        proc = subprocess.Popen(["./check", t], cwd=a.fstests, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, start_new_session=True)
        try:
            out, _ = proc.communicate(timeout=a.timeout)
            rc, hung = proc.returncode, False
        except subprocess.TimeoutExpired:
            hung, rc = True, -1
            try:
                os.killpg(proc.pid, signal.SIGKILL)   # check とテストのプロセスグループごと止める
            except OSError:
                pass
            umount_all([test_mnt, scratch_mnt])
            try:
                out, _ = proc.communicate(timeout=20)
            except subprocess.TimeoutExpired:
                out = ""
        dt = time.time() - t0
        base = os.path.join(results_dir, t)
        r = dict(seconds=round(dt, 1))
        if a.fs in ("ext2", "ext4"):
            r["state_after"] = {k: re.sub(r"\s+", " ", sh("dumpe2fs -h %s 2>/dev/null | grep -i 'state\\|features'" % v, shell=True).stdout.strip()) for k, v in imgs.items()}
        if hung:
            r["status"] = "hang"
            r["detail"] = out[-1500:]
        elif rc == 0:
            nr = base + ".notrun"
            if os.path.exists(nr):
                r["status"] = "notrun"
                r["reason"] = open(nr, errors="replace").read().strip()[:300]
            else:
                r["status"] = "pass"
        else:
            r["status"] = "fail"
            bad = base + ".out.bad"
            full = base + ".full"
            det = ""
            if os.path.exists(bad):
                exp = [x for x in (os.path.join(a.fstests, "tests", t + sfx) for sfx in (".out", ".out.default", ".out.nfs")) if os.path.exists(x)]
                d = sh(["diff", "-u", exp[0], bad]).stdout if exp else ""
                det += "".join(d.splitlines(True)[:60])
            if not det:
                det = out[-1500:]
            r["detail"] = det[:4000]
            r["procs"] = sh("pgrep -a usbmem_fuse; mount | grep -c fuse", shell=True).stdout[:500]
            r["full_tail"] = read_tail(full, 25)[-2500:]
        # 後始末 (sync / 閉じる / fsck) をしているデーモンが終わるのを待つ。umount は戻っても、デーモンはまだ動いている
        for _ in range(1200):
            if sh("pgrep -f '[u]sbmem_fuse -o' >/dev/null", shell=True).returncode != 0:
                break
            time.sleep(0.1)
        # アンマウント時の fsck の失敗 (テストが pass でも記録する)
        newlog = read_tail(fsck_log, 100000).splitlines()[fsck_before:]
        if newlog:
            r["fsck_fail"] = len(newlog)
        results[t] = r
        # 状態をリセット (マウントが残っていたら外す)
        for d in (test_mnt, scratch_mnt):
            if os.path.ismount(d):
                sh(["umount", d])
                if os.path.ismount(d):
                    sh(["umount", "-l", d])
        if (i + 1) % 25 == 0:
            print("[%d/%d] %.0fs" % (i + 1, len(tests), time.time() - t_start), flush=True)
            json.dump(results, open(os.path.join(a.out, "results.json"), "w"), indent=1, ensure_ascii=False)

    cnt = {}
    for r in results.values():
        cnt[r["status"]] = cnt.get(r["status"], 0) + 1
    fsck_fails = sum(1 for r in results.values() if r.get("fsck_fail"))
    lines = ["# xfstests on usbmem_fuse: fs=%s" % a.fs, "",
             "total %d: %s; tests with fsck failure after unmount: %d" % (len(results), ", ".join("%s=%d" % kv for kv in sorted(cnt.items())), fsck_fails), ""]
    expected = load_expected(a.fs)
    for st in ("fail", "hang"):
        names = [t for t, r in results.items() if r["status"] == st]
        if names:
            exp = [t for t in names if t in expected]
            unexp = [t for t in names if t not in expected]
            lines += ["## %s (%d: expected %d, unexpected %d)" % (st, len(names), len(exp), len(unexp))]
            if unexp:
                lines += ["unexpected: " + " ".join(unexp)]
            for cat in sorted({expected[t]["cat"] for t in exp}):
                lines += ["expected (%s): " % cat + " ".join(t for t in exp if expected[t]["cat"] == cat)]
            lines += [""]
    fixed = [t for t, r in results.items() if r["status"] == "pass" and t in expected]
    if fixed:
        lines += ["## expected-failure list entries that now pass (%d)" % len(fixed), " ".join(fixed), ""]
    ff = [t for t, r in results.items() if r.get("fsck_fail")]
    if ff:
        lines += ["## fsck failed after unmount (%d)" % len(ff), " ".join(ff), ""]
    reasons = {}
    bycat = {}
    for t, r in results.items():
        if r["status"] == "notrun":
            r["notrun_cat"] = classify_notrun(r.get("reason", ""))
            bycat[r["notrun_cat"]] = bycat.get(r["notrun_cat"], 0) + 1
            k = "[%s] %s" % (r["notrun_cat"], re.sub(r"\d+", "N", r.get("reason", ""))[:100])
            reasons.setdefault(k, []).append(t)
    if reasons:
        lines.append("## notrun reasons (by category: %s)" % ", ".join("%s=%d" % kv for kv in sorted(bycat.items())))
        for k, v in sorted(reasons.items(), key=lambda kv: -len(kv[1])):
            lines.append("- (%d) %s" % (len(v), k))
    json.dump(results, open(os.path.join(a.out, "results.json"), "w"), indent=1, ensure_ascii=False)
    open(os.path.join(a.out, "summary.md"), "w").write("\n".join(lines) + "\n")
    print("\n".join(lines))
    umount_all([test_mnt, scratch_mnt])


if __name__ == "__main__":
    main()
