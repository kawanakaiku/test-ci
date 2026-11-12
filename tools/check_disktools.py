#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""DiskTools の独立検証。実イメージを壊し、外部ツール (検査のみ) と自前の検出結果を比べる。

使い方: tools/check_disktools.py <usbmem_disktools_cli> [fat|exfat|ext|ntfs|mkfs|fuzz|all] [--quick]
  fuzz: 各 FS の実イメージの無作為な破損で検査が落ちない・止まることの確認 (ASan/UBSan 付きの CLI と組み合わせる)
外部ツールは読み取り専用の引数 (fsck.fat -n / fsck.exfat -n / e2fsck -fn / ntfsfix -n) でだけ呼ぶ。
終了コード: 0 = 見逃し (FN) も過検出 (FP) も許容外が無い、1 = ある、77 = 外部ツールが無い (SKIP)
"""
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.dont_write_bytecode = True
from dt_common import Result  # noqa: E402


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 64
    cli = sys.argv[1]
    which = [a for a in sys.argv[2:] if not a.startswith("--")] or ["all"]
    quick = "--quick" in sys.argv
    ok = True
    ran = 0
    res = Result()
    mods = {"fat": ("dt_fat", "run_fat", ["fsck.fat", "mkfs.fat", "mcopy"]),
            "exfat": ("dt_exfat", "run_exfat", ["fsck.exfat", "mkfs.exfat"]),
            "ext": ("dt_ext", "run_ext", ["e2fsck", "mke2fs", "debugfs"]),
            "ntfs": ("dt_ntfs", "run_ntfs", ["ntfsfix", "mkntfs"]),
            "mkfs": ("dt_mkfs", "run_mkfs", ["fsck.fat", "fsck.exfat", "e2fsck"]),
            "fuzz": ("dt_fuzz", "run_fuzz", ["mkfs.fat", "mke2fs"])}
    for k, (mod, fn, tools) in mods.items():
        if "all" not in which and k not in which:
            continue
        missing = [t for t in tools if not shutil.which(t)]
        if missing:
            print("SKIP %s: missing tools %s" % (k, missing))
            continue
        ran += 1
        m = __import__(mod)
        r = Result()
        getattr(m, fn)(cli, r, quick)
        ok = r.summary(k) and ok
    if ran == 0:
        return 77
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
