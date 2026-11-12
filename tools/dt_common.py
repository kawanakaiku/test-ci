# SPDX-License-Identifier: Apache-2.0
"""DiskTools 検証用の共通部品: 自前 CLI と外部ツール (fsck.fat / fsck.exfat / e2fsck / ntfsfix) の呼び出し、結果比較。

外部ツールは必ず「検査のみ」の引数で呼ぶ (fsck.fat -n / fsck.exfat -n / e2fsck -fn / ntfsfix -n)。修復は一切しない。
"""
import json
import os
import shutil
import subprocess
import sys

PATH_EXTRA = "/sbin:/usr/sbin:/usr/local/sbin"
os.environ["PATH"] = os.environ.get("PATH", "") + ":" + PATH_EXTRA

WORK = "/dev/shm/usbmem_dt_%s" % os.environ.get("USER", "u")


def work_dir(name):
    d = os.path.join(WORK, name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    return d


def run(cmd, **kw):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, **kw)


def ours(cli, img, off=None, length=None, fs=None):
    """自前の検査。戻り値 (verdict_code, dict)。verdict: 0 clean / 1 warnings / 2 errors / 3 unsupported / 4 incomplete"""
    cmd = [cli, "check", img]
    if fs:
        cmd += ["--fs", fs]
    if off is not None:
        cmd += ["--range", str(off), str(length)]
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=180)
    except subprocess.TimeoutExpired:
        return 99, {"verdict": "crash", "findings": [], "stderr": "timeout"}
    try:
        j = json.loads(p.stdout.decode("utf-8", "replace"))
    except Exception:
        j = {"verdict": "crash", "findings": [], "stderr": p.stderr.decode("utf-8", "replace")}
    return p.returncode, j


def codes(j):
    return sorted({f["code"] for f in j.get("findings", []) if f["severity"] != "info"})


class Result:
    def __init__(self):
        self.rows = []
        self.bad = 0

    def add(self, name, ref_problem, our_problem, ref_txt="", our_codes=None, allow=None):
        """ref_problem: 外部ツールが問題ありと言ったか。our_problem: 自前が警告/エラーを出したか。
        allow: 'fp' / 'fn' で既知の差を許容 (理由は ref_txt に)。"""
        status = "ok"
        if ref_problem and not our_problem:
            status = "FN"      # 見逃し (最重視)
        elif our_problem and not ref_problem:
            status = "FP"      # 過検出
        if status != "ok" and allow and status.lower() in allow:
            status = status + "(allowed)"
        elif status != "ok":
            self.bad += 1
        self.rows.append((name, status, ref_problem, our_problem, our_codes or [], ref_txt))
        print("%-9s %-34s ref=%-5s ours=%-5s %s" % (status, name, "BAD" if ref_problem else "ok", "BAD" if our_problem else "ok", ",".join(our_codes or [])))
        if status in ("FN", "FP") and ref_txt:
            print("          ref: " + ref_txt.strip().replace("\n", " | ")[:300])
        sys.stdout.flush()

    def summary(self, title):
        n = len(self.rows)
        fn = sum(1 for r in self.rows if r[1] == "FN")
        fp = sum(1 for r in self.rows if r[1] == "FP")
        print("== %s: %d cases, %d unexpected (FN=%d FP=%d)" % (title, n, self.bad, fn, fp))
        return self.bad == 0
