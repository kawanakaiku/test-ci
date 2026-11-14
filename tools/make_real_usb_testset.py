
def _xfs_supports(opt_args):
    """この mkfs.xfs が opt_args (例 ["-m", "rmapbt=0"]) を受け付けるか (-N: 何も書かない)。"""
    import subprocess, tempfile
    try:
        with tempfile.NamedTemporaryFile(prefix="xfsprobe") as f:
            f.truncate(300 * 1024 * 1024)
            r = subprocess.run(["mkfs.xfs", "-N", "-q"] + opt_args + [f.name], capture_output=True)
            return r.returncode == 0
    except OSError:
        return False


def _xfs_mkfs_m_opts():
    return ",rmapbt=0" if _xfs_supports(["-m", "rmapbt=0"]) else ""


def _xfs_mkfs_n_opts():
    return ["-n", "parent=0"] if _xfs_supports(["-n", "parent=0"]) else []

#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""実機 USB メモリ 3 本 (プロファイル A / B / C) に、検証用のパーティション構成と決定的なテストデータを作る**ユーザー実行用**スクリプト。

  方式: イメージ経由 (build-then-flash)。
    (a) デバイスに触らず、ローカルディスク (--work-dir / --keep-images) の疎ファイルに、パーティション表と各パーティションのイメージを全部作る。
        ここで失敗してもデバイスは無傷。root 不要の部分が多い (NTFS=ntfs-3g FUSE、exFAT=exfat-fuse、LUKS2=Python の XTS、F2FS=sload.f2fs など)。
    (b) デバイスのパスを手入力して確認 → (c) バックアップ → (d) nbdcopy (または dd) で**一度だけ順次**流し込む
    → (e) 読み戻して SHA-256 を照合 (f) マニフェスト照合はイメージ側で済み、デバイスはイメージとのバイト一致で担保する。
  既定は dry-run (計画を表示するだけ)。実デバイスへの書き込みは --execute --profile X --device /dev/sdX が全部そろったときだけ。
  デバイスは厳密に検証し (USB・サイズ ±8%・未マウント・システムディスクでない)、デバイスパスを手入力して一致しないと進まない。
  --keep-images DIR: 作ったイメージを残す。--flash-only DIR: 再生成せず流し込みだけ (容量が違うデバイスは拒否)。--build-only: イメージだけ作る (root 不要)。
  --image FILE --profile X --execute: 実デバイスの代わりに**ファイル**へ同じ構成を作る (イメージモード。root 不要・検証用)。
  verify サブコマンド: 作ったマニフェストと、マウントした/取り出したツリーを照合する (PC に戻した後・Android から取り出した後の確認用)。

標準ライブラリのみ (LUKS2 の root なし投入と BitLocker 風は python3 の cryptography / numpy)。外部コマンド (sgdisk / sfdisk / mkfs.* / mkntfs / cryptsetup /
mksquashfs / mkfs.erofs / genisoimage / mkudffs / nbdcopy / nbdkit ...) は事前に確認し、足りなければ一覧して中止する。詳しくは docs/real-usb-testset.md。
"""
import argparse
import dataclasses
import datetime
import hashlib
import json
import mmap
import os
import random
import re
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import unicodedata
import uuid

# ----------------------------------------------------------------------------------------------------------------------
# 文字コード (ファイル名に日本語・濁点の結合文字を使うので、Python のファイルシステム文字コードは UTF-8 が必須)
# ----------------------------------------------------------------------------------------------------------------------
if __name__ == "__main__" and sys.getfilesystemencoding().lower().replace("-", "") != "utf8" and not os.environ.get("_USBMEM_REEXEC"):
    _env = dict(os.environ, PYTHONUTF8="1", _USBMEM_REEXEC="1")
    os.execve(sys.executable, [sys.executable] + sys.argv, _env)

HERE = os.path.dirname(os.path.abspath(__file__))
KiB = 1024
MiB = KiB * KiB
GiB = MiB * KiB

# sgdisk / mkfs などは sbin にある。PATH に無い環境 (一般ユーザーのシェル) でも見つかるようにする
for _d in ("/usr/sbin", "/sbin", "/usr/local/sbin"):
    if _d not in os.environ.get("PATH", "").split(":"):
        os.environ["PATH"] = os.environ.get("PATH", "") + ":" + _d

SEED = "usbmem-testset-v1"           # 乱数・UUID の種 (固定)。変えるとデータも UUID も全部変わる
ALIGN = MiB                          # パーティションは 1 MiB 整列
LEAD = MiB                           # 先頭の空き (先頭 1 MiB 整列)
TAIL_RESERVE = MiB                   # 末尾に残す分 (GPT バックアップ 33 セクタ分より十分大きい)
SIZE_TOLERANCE = 0.08                # デバイス容量の許容誤差
MIN_REST = 256 * MiB                 # 「残り全部」のパーティションの最小サイズ
FIXED_TIME = 1704067200              # 2024-01-01T00:00:00Z。全ファイルの mtime
LUKS_PASSPHRASE = "usbmem-test-pass"
BITLOCKER_PASSWORD = "synth-Password-1"
WORK_MNT_PREFIX = "usbmem_testset_mnt_"
IMAGESET_FORMAT = 1                  # イメージ一式 (--keep-images / --flash-only) のメタデータ形式。互換が切れたら上げる
META_NAME = "imageset.json"
TABLE_IMG = "disk_table.img"         # パーティション表だけを持つ、デバイスと同じ大きさの疎ファイル (先頭 / 拡張パーティションの EBR / 末尾の領域だけが非ゼロ)
LUKS_SECTOR = 512
IO_BLOCK = 4 * MiB

# ----------------------------------------------------------------------------------------------------------------------
# プロファイル定義 (サイズはここの定数。デバイス容量に合わせて調整してよい)
# ----------------------------------------------------------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class PartSpec:
    key: str                  # 'ntfs' など。パーティション名 p<番号>_<key> になる
    fs: str                   # ntfs ext2 ext3 ext4 fat32 exfat xfs btrfs f2fs luks bitlocker squashfs erofs udf iso
    size: object              # バイト数。None = 残り全部
    kind: str = "primary"     # primary / logical (MBR のみ)
    big: bool = False         # 4.5 GiB のファイルを入れる (FAT32 不可。B の NTFS と C の XFS / Btrfs だけ)
    dirty_opt: bool = False   # --dirty-ntfs の対象
    needs_recovery: bool = False
    desc: str = ""


@dataclasses.dataclass(frozen=True)
class Profile:
    name: str
    table: str                # gpt / mbr
    expected: int             # 期待するデバイス容量 (バイト)。±8% を許す
    parts: tuple
    min_tail: int = 0         # 末尾に未割り当てで残す最小サイズ (C は 約 10 GiB)
    note: str = ""


def _gib(x):
    return int(x * GiB)


PROFILES = {
    "A": Profile("A", "gpt", _gib(7.8), (
        PartSpec("ntfs", "ntfs", 3 * GiB, dirty_opt=True, desc="書き込み検証用。--dirty-ntfs でダーティフラグ付きにできる"),
        PartSpec("ext4", "ext4", 2 * GiB, desc="ジャーナル付き (クリーン)"),
        PartSpec("fat32", "fat32", 1 * GiB, desc="FAT32 (4 GiB 超ファイルなし)"),
        PartSpec("exfat", "exfat", None, desc="残り全部 (約 1.5 GiB)"),
    ), note="書き込み検証用 (GPT)"),
    "B": Profile("B", "mbr", _gib(16), (
        PartSpec("fat32", "fat32", 1 * GiB),
        PartSpec("ntfs", "ntfs", 5 * GiB, big=True, desc="4.5 GiB のファイルを入れる"),
        PartSpec("exfat", "exfat", 3 * GiB),
        PartSpec("ext2", "ext2", 1 * GiB, kind="logical"),
        PartSpec("ext3", "ext3", 1 * GiB, kind="logical"),
        PartSpec("ext4", "ext4", 1 * GiB, kind="logical", needs_recovery=True, desc="ジャーナル付き・needs_recovery を立てる"),
    ), note="MBR + 拡張パーティション"),
    "C": Profile("C", "gpt", _gib(31), (
        PartSpec("xfs", "xfs", 5 * GiB, big=True),
        PartSpec("btrfs", "btrfs", 6 * GiB, big=True),
        PartSpec("f2fs", "f2fs", 3 * GiB),
        PartSpec("luks", "luks", 2 * GiB, desc="LUKS2 (中に ext4)"),
        PartSpec("bitlocker", "bitlocker", 1 * GiB, desc="BitLocker 風の合成イメージ (bitlocker_synth.py)"),
        PartSpec("squashfs", "squashfs", 256 * MiB),
        PartSpec("erofs", "erofs", 256 * MiB),
        PartSpec("udf", "udf", 512 * MiB),
        PartSpec("iso", "iso", 512 * MiB),
    ), min_tail=_gib(9.5), note="末尾 約 10 GiB 以上は未割り当て (パーティション編集の検証用)"),
}

# ファイルシステムごとの性質 (テストデータの生成に使う)
#   case: 大文字小文字を区別する / symlink / hardlink / sparse(穴を作れる) / name_chars: 1 要素の最大文字数 / nonbmp: BMP 外の文字
FS_CAPS = {
    "ntfs": dict(case=False, symlink=False, hardlink=True, sparse=True, name_chars=83, nonbmp=True),
    "ext2": dict(case=True, symlink=True, hardlink=True, sparse=True, name_chars=83, nonbmp=True),
    "ext3": dict(case=True, symlink=True, hardlink=True, sparse=True, name_chars=83, nonbmp=True),
    "ext4": dict(case=True, symlink=True, hardlink=True, sparse=True, name_chars=83, nonbmp=True),
    "fat32": dict(case=False, symlink=False, hardlink=False, sparse=False, name_chars=83, nonbmp=False),   # mtools は BMP 外の文字を落とす
    "exfat": dict(case=False, symlink=False, hardlink=False, sparse=False, name_chars=83, nonbmp=True),
    "xfs": dict(case=True, symlink=True, hardlink=True, sparse=True, name_chars=83, nonbmp=True),
    "btrfs": dict(case=True, symlink=True, hardlink=True, sparse=True, name_chars=83, nonbmp=True),
    "f2fs": dict(case=True, symlink=True, hardlink=True, sparse=True, name_chars=83, nonbmp=True),
    "luks": dict(case=True, symlink=True, hardlink=True, sparse=True, name_chars=83, nonbmp=True),     # 中は ext4
    "bitlocker": dict(case=False, symlink=False, hardlink=True, sparse=False, name_chars=83, nonbmp=True),  # 中は NTFS
    "squashfs": dict(case=True, symlink=True, hardlink=True, sparse=True, name_chars=83, nonbmp=True),
    "erofs": dict(case=True, symlink=True, hardlink=True, sparse=False, name_chars=83, nonbmp=True),
    "udf": dict(case=True, symlink=True, hardlink=True, sparse=False, name_chars=83, nonbmp=True),
    "iso": dict(case=False, symlink=True, hardlink=False, sparse=False, name_chars=60, nonbmp=False),  # Joliet は 64 文字・UCS-2
}
# マウントして書き込む必要があり、root が要る FS (kernel mount / cryptsetup)。NTFS は ntfs-3g (FUSE) なので root 不要。
# exFAT は --exfat-method fuse (exfat-fuse)、LUKS2 は --luks-method xts (Python で XTS 暗号化) なら root 不要。
ROOT_POPULATE = {"exfat", "udf", "luks"}


def root_needed(fs, opts):
    """FS の中身の投入に root が要るか (opts の方式選択を見る)。"""
    if fs == "exfat":
        return opts.exfat_method == "mount"
    if fs == "luks":
        return opts.luks_method == "open"
    if fs == "udf":
        return opts.udf_method == "mkudffs"
    if fs == "f2fs":
        return opts.f2fs_method == "mount"
    return False


def root_needed_parts(profile, opts):
    return [spec.key for spec in profile.parts if root_needed(spec.fs, opts)]
FS_DISPLAY = {"ntfs": "NTFS", "ext2": "ext2", "ext3": "ext3", "ext4": "ext4", "fat32": "FAT32", "exfat": "exFAT", "xfs": "XFS",
              "btrfs": "Btrfs", "f2fs": "F2FS", "luks": "LUKS2(ext4)", "bitlocker": "BitLocker風", "squashfs": "squashfs",
              "erofs": "erofs", "udf": "UDF", "iso": "ISO9660"}


def human(n):
    n = float(n)
    for u in ("B", "KiB", "MiB", "GiB", "TiB"):
        if n < 1024 or u == "TiB":
            return ("%d %s" % (n, u)) if u == "B" else ("%.2f %s" % (n, u))
        n /= 1024


def align_up(x, a=ALIGN):
    return (x + a - 1) // a * a


def align_down(x, a=ALIGN):
    return x // a * a


def derive_uuid(*parts):
    return str(uuid.uuid5(uuid.NAMESPACE_URL, SEED + "/" + "/".join(parts)))


def derive_hex32(*parts):
    return derive_uuid(*parts).replace("-", "")[:8].upper()


# ----------------------------------------------------------------------------------------------------------------------
# パーティション配置
# ----------------------------------------------------------------------------------------------------------------------


@dataclasses.dataclass
class Placed:
    index: int
    spec: PartSpec
    start: int
    size: int
    name: str
    profile: str = ""

    @property
    def end(self):
        return self.start + self.size

    @property
    def fs(self):
        return self.spec.fs

    @property
    def label(self):
        return "TS_" + self.spec.key.upper()


@dataclasses.dataclass
class Layout:
    profile: Profile
    dev_size: int
    ss: int
    parts: list
    extended: object          # (index, start, size) または None
    unallocated_tail: int     # 最後のパーティション (拡張パーティション) の終わりからデバイス末尾まで
    used_end: int


def compute_layout(profile, dev_size, ss=512):
    """先頭 1 MiB 整列・末尾 1 MiB を残して配置する。入りきらなければ ValueError。"""
    if dev_size % ss:
        dev_size = align_down(dev_size, ss)
    limit = align_down(dev_size - TAIL_RESERVE)
    pos = LEAD
    placed = []
    prim = [s for s in profile.parts if s.kind == "primary"]
    logi = [s for s in profile.parts if s.kind == "logical"]
    idx = 0
    extended = None

    def size_of(spec, start, is_last):
        if spec.size is None:
            sz = limit - start
            if sz < MIN_REST:
                raise ValueError("プロファイル %s: 「残り全部」の %s が %s しか取れない (最小 %s)" % (profile.name, spec.key, human(max(sz, 0)), human(MIN_REST)))
            return sz
        return align_up(spec.size)

    for spec in prim:
        idx += 1
        sz = size_of(spec, pos, True)
        placed.append(Placed(idx, spec, pos, sz, "p%d_%s" % (idx, spec.key), profile.name))
        pos += sz
    if logi:
        if profile.table != "mbr":
            raise ValueError("論理パーティションは MBR のみ")
        idx += 1
        ext_idx = idx
        ext_start = pos
        lp = ext_start
        for spec in logi:
            idx += 1
            lp += ALIGN                      # EBR 用に 1 MiB 空ける
            sz = size_of(spec, lp, False)
            placed.append(Placed(idx, spec, lp, sz, "p%d_%s" % (idx, spec.key), profile.name))
            lp += sz
        extended = (ext_idx, ext_start, lp - ext_start)
        pos = lp
        if idx > 128:
            raise ValueError("パーティション数が多すぎる")
    if profile.table == "mbr" and len(prim) + (1 if logi else 0) > 4:
        raise ValueError("MBR のプライマリは 4 つまで")
    if pos > limit:
        raise ValueError("プロファイル %s: 合計 %s がデバイス容量 %s (末尾 %s 残し) に収まらない" % (profile.name, human(pos - LEAD), human(dev_size), human(TAIL_RESERVE)))
    tail = dev_size - pos
    if tail < profile.min_tail:
        raise ValueError("プロファイル %s: 末尾の未割り当てが %s で、必要な %s に足りない" % (profile.name, human(tail), human(profile.min_tail)))
    return Layout(profile, dev_size, ss, placed, extended, tail, pos)


# ----------------------------------------------------------------------------------------------------------------------
# コマンドの表現 (生成部分は純関数にして単体テストできるようにする)
# ----------------------------------------------------------------------------------------------------------------------


@dataclasses.dataclass
class Cmd:
    argv: list
    stdin: object = None       # 標準入力に渡す文字列
    stdout_file: object = None
    kind: str = "run"          # run=失敗したら中止 / check=失敗は記録するだけ / special=@ で始まる内部操作
    note: str = ""
    env: object = None
    cwd: object = None

    def text(self):
        t = " ".join(shlex.quote(a) for a in self.argv)
        if self.stdin is not None:
            t += "   < (スクリプト %d 行)" % self.stdin.count("\n")
        if self.stdout_file:
            t += "   > " + shlex.quote(self.stdout_file)
        return t


def part_type_code(spec, table):
    """GPT: sgdisk のタイプ。MBR: sfdisk のタイプ。"""
    fs = spec.fs
    if table == "gpt":
        if fs in ("ntfs", "fat32", "exfat", "bitlocker", "udf", "iso"):
            return "0700"
        if fs == "luks":
            return "8309"
        return "8300"
    if fs == "fat32":
        return "c"
    if fs in ("ntfs", "exfat", "bitlocker"):
        return "7"
    return "83"


def sgdisk_cmds(layout, dev):
    p = layout.profile
    ss = layout.ss
    zap = Cmd(["sgdisk", "--zap-all", dev], note="既存のパーティション表 (GPT / MBR) を消す")
    args = ["sgdisk", "-a", "2048", "-U", derive_uuid(p.name, "disk")]
    for pp in layout.parts:
        args += ["-n", "%d:%d:%d" % (pp.index, pp.start // ss, pp.end // ss - 1),
                 "-t", "%d:%s" % (pp.index, part_type_code(pp.spec, "gpt")),
                 "-c", "%d:%s" % (pp.index, pp.name),
                 "-u", "%d:%s" % (pp.index, derive_uuid(p.name, pp.name, "part"))]
    args.append(dev)
    return [zap, Cmd(args, note="GPT を作る")]


def sfdisk_script(layout):
    """MBR (dos) の sfdisk 入力。論理パーティションは 4 番目 (拡張) の後ろに並べる。"""
    ss = layout.ss
    p = layout.profile
    lines = ["label: dos", "label-id: 0x%s" % derive_hex32(p.name, "mbr-id"), "unit: sectors", ""]
    prim = [pp for pp in layout.parts if pp.spec.kind == "primary"]
    logi = [pp for pp in layout.parts if pp.spec.kind == "logical"]
    for pp in prim:
        lines.append("start=%d, size=%d, type=%s" % (pp.start // ss, pp.size // ss, part_type_code(pp.spec, "mbr")))
    if layout.extended:
        _, es, esz = layout.extended
        lines.append("start=%d, size=%d, type=5" % (es // ss, esz // ss))
        for pp in logi:
            lines.append("start=%d, size=%d, type=%s" % (pp.start // ss, pp.size // ss, part_type_code(pp.spec, "mbr")))
    return "\n".join(lines) + "\n"


def sfdisk_cmds(layout, dev):
    return [Cmd(["sfdisk", "--wipe", "always", "--wipe-partitions", "always", dev], stdin=sfdisk_script(layout), note="MBR + 拡張パーティションを作る")]


def table_cmds(layout, dev):
    return sgdisk_cmds(layout, dev) if layout.profile.table == "gpt" else sfdisk_cmds(layout, dev)


def part_node(dev, index):
    """/dev/sdb + 1 -> /dev/sdb1、/dev/mmcblk0 + 1 -> /dev/mmcblk0p1"""
    return dev + ("p" if dev[-1].isdigit() else "") + str(index)


def backup_cmds(dev, dev_size, bdir, full):
    cmds = [
        Cmd(["sgdisk", "--backup=" + os.path.join(bdir, "sgdisk_backup.bin"), dev], kind="check", note="GPT のバックアップ (GPT でなければ失敗してよい)"),
        Cmd(["sfdisk", "-d", dev], stdout_file=os.path.join(bdir, "sfdisk_dump.txt"), kind="check", note="sfdisk のダンプ (パーティション表なしなら失敗してよい)"),
        Cmd(["dd", "if=" + dev, "of=" + os.path.join(bdir, "head_4MiB.bin"), "bs=1M", "count=4", "status=none"], note="先頭 4 MiB"),
        Cmd(["dd", "if=" + dev, "of=" + os.path.join(bdir, "tail_1MiB.bin"), "bs=1M", "skip=%d" % max(dev_size - MiB, 0), "iflag=skip_bytes", "count=1", "status=none"], note="末尾 1 MiB"),
    ]
    if full:
        cmds.append(Cmd(["dd", "if=" + dev, "of=" + os.path.join(bdir, "full.img"), "bs=4M", "status=none"], note="全体イメージ"))
    return cmds


# ----------------------------------------------------------------------------------------------------------------------
# テストデータ (全 FS 共通ジェネレータ。決定的)
# ----------------------------------------------------------------------------------------------------------------------

_BASE = None


def _base_block():
    global _BASE
    if _BASE is None:
        _BASE = random.Random(SEED + "/base").randbytes(MiB)
    return _BASE


def content_chunks(fid, size):
    """fid (ファイル識別名) と size だけで決まる内容を、チャンクで返す。"""
    if size <= 0:
        return
    if size <= 4 * MiB:
        yield random.Random(SEED + "/" + fid).randbytes(size)
        return
    base = _base_block()
    o0 = int(hashlib.sha256((SEED + "/" + fid).encode()).hexdigest()[:8], 16)
    idx = 0
    remaining = size
    while remaining > 0:
        n = min(MiB, remaining)
        o = (o0 + idx * 104729) % MiB
        blk = idx.to_bytes(8, "little") + (base[o:] + base[:o])[8:]
        yield blk[:n]
        remaining -= n
        idx += 1


def sparse_segments(size):
    """疎ファイルのデータ部分 (offset, fid, len)。残りは穴 (0)。"""
    return [(0, "sparse-head", 4096), (MiB + 17, "sparse-mid", 5000), (size - 4096, "sparse-tail", 4096)]


def sparse_hash(size):
    h = hashlib.sha256()
    pos = 0
    zero = bytes(MiB)
    for off, fid, ln in sparse_segments(size):
        gap = off - pos
        while gap > 0:
            n = min(gap, MiB)
            h.update(zero[:n])
            gap -= n
        data = b"".join(content_chunks(fid, ln))
        h.update(data)
        pos = off + ln
    gap = size - pos
    while gap > 0:
        n = min(gap, MiB)
        h.update(zero[:n])
        gap -= n
    return h.hexdigest()


@dataclasses.dataclass
class Caps:
    case: bool
    symlink: bool
    hardlink: bool
    sparse: bool
    name_chars: int
    nonbmp: bool
    gib: bool                 # 1 GiB のファイルを入れる
    big: bool                 # 4.5 GiB のファイルを入れる
    small_count: int
    sparse_size: int


def caps_for(spec, part_size, no_big=False, small_count=None, udf_hybrid=False):
    c = dict(FS_CAPS[spec.fs])
    if spec.fs == "udf" and udf_hybrid:      # genisoimage -udf は Joliet 経由: 64 文字・BMP のみ・大文字小文字の衝突を避ける
        c.update(case=False, name_chars=60, nonbmp=False)
    tiny = part_size < 600 * MiB
    gib = (not spec.big) and part_size >= int(1.4 * GiB) and not no_big
    big = spec.big and not no_big
    sc = small_count if small_count is not None else (300 if tiny else 3000)
    sparse_size = (8 * MiB if tiny else 64 * MiB) if c["sparse"] else 0
    return Caps(c["case"], c["symlink"], c["hardlink"], c["sparse"], c["name_chars"], c["nonbmp"], gib, big, sc, sparse_size)


class Dataset:
    """root 以下にテストデータを作り、マニフェストのエントリを集める。"""

    def __init__(self, root):
        self.root = root
        self.entries = []      # (kind, path, size, sha, extra)
        self.files = []

    def p(self, rel):
        return os.path.join(self.root, rel)

    def mkdir(self, rel):
        os.makedirs(self.p(rel), exist_ok=True)
        parts = rel.split("/")
        for i in range(1, len(parts) + 1):
            e = ("D", "/".join(parts[:i]), 0, "-", "")
            if e not in self.entries:
                self.entries.append(e)

    def file(self, rel, size, fid=None):
        d = os.path.dirname(rel)
        if d:
            self.mkdir(d)
        h = hashlib.sha256()
        with open(self.p(rel), "wb") as f:
            for chunk in content_chunks(fid or rel, size):
                f.write(chunk)
                h.update(chunk)
            if size > 64 * MiB:
                f.flush()
                os.fsync(f.fileno())
        self.entries.append(("F", rel, size, h.hexdigest(), ""))

    def sparse(self, rel, size):
        d = os.path.dirname(rel)
        if d:
            self.mkdir(d)
        with open(self.p(rel), "wb") as f:
            for off, fid, ln in sparse_segments(size):
                f.seek(off)
                f.write(b"".join(content_chunks(fid, ln)))
            f.truncate(size)
        self.entries.append(("F", rel, size, sparse_hash(size), "sparse"))

    def symlink(self, rel, target):
        d = os.path.dirname(rel)
        if d:
            self.mkdir(d)
        os.symlink(target, self.p(rel))
        self.entries.append(("L", rel, 0, "-", target))

    def hardlink(self, rel, other):
        os.link(self.p(other), self.p(rel))
        src = [e for e in self.entries if e[0] == "F" and e[1] == other][0]
        self.entries.append(("H", rel, src[2], src[3], other))

    def finalize_times(self):
        """全ファイル・ディレクトリの mtime を固定 (深い方から)。"""
        paths = []
        for dp, dns, fns in os.walk(self.root):
            for n in dns + fns:
                paths.append(os.path.join(dp, n))
        paths.sort(key=lambda s: -s.count("/"))
        for p in paths:
            try:
                os.utime(p, (FIXED_TIME, FIXED_TIME), follow_symlinks=False)
            except (OSError, NotImplementedError):
                pass


def japanese_names(caps):
    names = [
        "がぎぐげご.txt",                                            # 濁点 (NFC)
        unicodedata.normalize("NFD", "がぎぐげご") + ".txt",          # 濁点 (NFD: 清音 + 結合濁点)
        "ばびぶべぼ_ぱぴぷぺぽ_バビブベボ_パピプペポ.txt",
        "ﾊﾝｶｸｶﾅ ﾃｽﾄ ｶﾞｷﾞｸﾞｹﾞｺﾞ ﾊﾟﾋﾟﾌﾟﾍﾟﾎﾟ.txt",                      # 半角カナ (半角濁点・半濁点)
        "全角ＡＢＣ１２３　全角空白.txt",
        "空白 と (括弧) と [角括弧] と #%&'+,;=@.txt",
        ".隠しファイル",
        "拡張子なしの日本語ファイル名",
        "漢字ひらがなカタカナ_混在_テスト.日本語",
    ]
    if caps.nonbmp:
        names.append("𠮷野家_BMP外の文字.txt")                         # U+20BB7 (サロゲートペア)
    stem = "長い名前"
    long_name = (stem * 40)[:caps.name_chars] + ".txt"
    names.append(long_name)
    return names


def build_dataset(root, caps, meta):
    """全 FS 共通のテストデータを root に作り、README.txt と MANIFEST.sha256 も書く。Dataset を返す。"""
    ds = Dataset(root)
    # サイズの境界
    ds.file("00_sizes/empty.bin", 0)
    for sz in (1, 4095, 4096, 4097, MiB - 1, MiB, MiB + 1):
        ds.file("00_sizes/size_%d.bin" % sz, sz)
    # 日本語
    for i, n in enumerate(japanese_names(caps)):
        ds.file("01_japanese/" + n, 100 + i * 37, fid="jp/%d" % i)
    ds.file("01_japanese/日本語ディレクトリ/サブ ディレクトリ/深い所.txt", 1234, fid="jp/deep")
    ds.mkdir("01_japanese/空のディレクトリ")
    # 小さいファイル数千個
    per = 250
    for i in range(caps.small_count):
        ds.file("03_small/g%02d/f%05d.dat" % (i // per, i), (i * 37) % 2049, fid="small/%d" % i)
    # 深さ 20
    deep = "04_deep"
    for i in range(1, 21):
        deep += "/d%02d" % i
    ds.mkdir(deep)
    ds.file(deep + "/leaf.txt", 777, fid="deep/leaf")
    # 大文字小文字違いの同名
    if caps.case:
        for n in ("Case.txt", "case.txt", "CASE.txt"):
            ds.file("05_case/" + n, 321, fid="case/" + n)
    # リンク
    ds.file("06_links/target.txt", 555, fid="links/target")
    if caps.symlink:
        ds.symlink("06_links/sym_relative", "target.txt")
        ds.symlink("06_links/sym_dir", "../01_japanese")
        ds.symlink("06_links/sym_dangling", "no_such_file")
        ds.symlink("06_links/日本語リンク", "target.txt")
    if caps.hardlink:
        ds.file("06_links/hard_a.dat", 10000, fid="links/hard")
        ds.hardlink("06_links/hard_b.dat", "06_links/hard_a.dat")
    # 疎ファイル
    if caps.sparse and caps.sparse_size:
        ds.sparse("07_sparse/sparse_%dMiB.bin" % (caps.sparse_size // MiB), caps.sparse_size)
    # 大きいファイル (最後に作る: 失敗したときの時間を短く)
    if caps.gib:
        ds.file("02_big/gib_1GiB.bin", GiB, fid="big/1gib")
    if caps.big:
        ds.file("02_big/big_4p5GiB.bin", 4 * GiB + 512 * MiB, fid="big/4p5gib")
    # README
    readme = readme_text(meta, caps, ds)
    with open(ds.p("README.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write(readme)
    ds.entries.append(("F", "README.txt", len(readme.encode()), hashlib.sha256(readme.encode()).hexdigest(), ""))
    ds.finalize_times()
    write_manifest(ds.p("MANIFEST.sha256"), ds.entries, meta)
    return ds


def readme_text(meta, caps, ds):
    ex = lambda b: "あり" if b else "なし"
    return (
        "usbmem 実機検証用テストデータ\n"
        "================================\n"
        "プロファイル: %(profile)s   パーティション: %(part)s   FS: %(fs)s   ラベル: %(label)s\n"
        "生成: tools/make_real_usb_testset.py (固定シード %(seed)s。内容は毎回同じ)\n\n"
        "内容:\n"
        "  00_sizes/    0B / 1B / 4095 / 4096 / 4097 / 1MiB-1 / 1MiB / 1MiB+1 バイトのファイル\n"
        "  01_japanese/ 日本語名 (濁点 NFC/NFD・半角カナ・全角空白・長い名前・隠しファイル・深い日本語ディレクトリ)\n"
        "  02_big/      1GiB のファイル: %(gib)s / 4.5GiB のファイル: %(big)s\n"
        "  03_small/    小さいファイル %(small)d 個\n"
        "  04_deep/     深さ 20 階層\n"
        "  05_case/     大文字小文字だけが違う同名: %(case)s\n"
        "  06_links/    symlink: %(sym)s / hardlink: %(hl)s\n"
        "  07_sparse/   疎ファイル: %(sparse)s\n\n"
        "照合: MANIFEST.sha256 (種別 サイズ sha256 パス のタブ区切り) と同じものが PC 側の manifest-dir にもある。\n"
        "  python3 tools/make_real_usb_testset.py verify MANIFEST.sha256 <マウント先のルート>\n"
        "書き込み試験をした場合、追加したファイルは「余分」として報告される (--allow-extra で許容)。\n"
    ) % dict(profile=meta["profile"], part=meta["part"], fs=meta["fs"], label=meta["label"], seed=SEED, gib=ex(caps.gib), big=ex(caps.big),
             small=caps.small_count, case=ex(caps.case), sym=ex(caps.symlink), hl=ex(caps.hardlink), sparse=ex(caps.sparse and caps.sparse_size))


def manifest_text(entries, meta=None):
    lines = ["# usbmem testset manifest v1"]
    if meta:
        lines.append("# profile=%s partition=%s fs=%s label=%s" % (meta["profile"], meta["part"], meta["fs"], meta["label"]))
    lines.append("# 種別\tサイズ\tsha256\tパス\t(L: リンク先 / H: 同一 inode のパス / sparse: 疎ファイル)")
    for k, path, size, sha, extra in sorted(entries, key=lambda e: e[1].encode()):
        if k in ("D",):
            lines.append("D\t0\t-\t%s" % path)
        elif k == "L":
            lines.append("L\t0\t-\t%s\t%s" % (path, extra))
        elif k == "H":
            lines.append("H\t%d\t%s\t%s\t%s" % (size, sha, path, extra))
        else:
            lines.append("F\t%d\t%s\t%s%s" % (size, sha, path, ("\t" + extra) if extra else ""))
    return "\n".join(lines) + "\n"


def write_manifest(path, entries, meta=None):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(manifest_text(entries, meta))


def parse_manifest(text):
    out = []
    for ln in text.splitlines():
        if not ln or ln.startswith("#"):
            continue
        c = ln.split("\t")
        k = c[0]
        if k == "D":
            out.append(("D", c[3], 0, "-", ""))
        elif k == "L":
            out.append(("L", c[3], 0, "-", c[4]))
        elif k == "H":
            out.append(("H", c[3], int(c[1]), c[2], c[4]))
        elif k == "F":
            out.append(("F", c[3], int(c[1]), c[2], c[4] if len(c) > 4 else ""))
    return out


IGNORED_EXTRA = {"MANIFEST.sha256", "lost+found", "System Volume Information", "$RECYCLE.BIN", ".Trash-0", ".Trash-1000", "found.000", ".fseventsd", ".Spotlight-V100", "rr_moved"}


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(4 * MiB)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def verify_tree(entries, root, check_links=True, allow_extra=False):
    """マニフェストとツリーを照合。(問題のリスト, 件数辞書) を返す。"""
    problems = []
    cnt = dict(F=0, D=0, L=0, H=0, bytes=0)
    known = set()
    for k, path, size, sha, extra in entries:
        known.add(path)
        p = os.path.join(root, path)
        try:
            st = os.lstat(p)
        except OSError as e:
            if k == "L" and not check_links:
                continue
            if k == "H" and not check_links:
                continue
            problems.append("欠落: %s (%s)" % (path, e.strerror))
            continue
        if k == "D":
            cnt["D"] += 1
            if not stat.S_ISDIR(st.st_mode):
                problems.append("ディレクトリでない: " + path)
        elif k in ("F", "H"):
            if k == "H" and not check_links:
                continue
            if not stat.S_ISREG(st.st_mode):
                problems.append("通常ファイルでない: " + path)
                continue
            if st.st_size != size:
                problems.append("サイズ不一致: %s (期待 %d / 実際 %d)" % (path, size, st.st_size))
                continue
            if sha256_file(p) != sha:
                problems.append("sha256 不一致: " + path)
                continue
            cnt[k] += 1
            cnt["bytes"] += size
            if k == "H":
                if not os.path.samestat(st, os.lstat(os.path.join(root, extra))):
                    problems.append("ハードリンクになっていない: %s と %s" % (path, extra))
        elif k == "L":
            if not check_links:
                continue
            if not stat.S_ISLNK(st.st_mode):
                problems.append("シンボリックリンクでない: " + path)
            elif os.readlink(p) != extra:
                problems.append("リンク先不一致: %s (期待 %s / 実際 %s)" % (path, extra, os.readlink(p)))
            else:
                cnt["L"] += 1
    if not allow_extra:
        for dp, dns, fns in os.walk(root):
            rel_dir = os.path.relpath(dp, root)
            rel_dir = "" if rel_dir == "." else rel_dir
            for n in list(dns):
                if rel_dir == "" and n in IGNORED_EXTRA:
                    dns.remove(n)
            for n in dns + fns:
                if rel_dir == "" and n in IGNORED_EXTRA:
                    continue
                rel = (rel_dir + "/" + n) if rel_dir else n
                if rel not in known:
                    problems.append("余分: " + rel)
    return problems, cnt


# ----------------------------------------------------------------------------------------------------------------------
# デバイスの検証 (安全機構。lsblk の出力・マウント一覧を引数にして単体テストできる)
# ----------------------------------------------------------------------------------------------------------------------

LSBLK_COLS = "NAME,KNAME,PATH,TYPE,SIZE,TRAN,RM,HOTPLUG,RO,MODEL,VENDOR,SERIAL,MOUNTPOINTS,FSTYPE,LABEL,LOG-SEC,MAJ:MIN"
SYSTEM_MOUNTS = ("/", "/home", "/boot", "/boot/efi", "/efi", "/usr", "/var", "/etc", "/opt", "/root", "/srv", "/nix", "/var/log", "/tmp")
INTERNAL_TRANS = {"sata", "ata", "nvme", "sas", "scsi", "mmc", "virtio", "pcie", "fc", "iscsi", "spi", "ide"}


def truthy(v):
    return v is True or v in (1, "1", "true", "True")


def parse_lsblk_json(text):
    data = json.loads(text)
    return data.get("blockdevices", [])


def lsblk_tree(runner=None):
    """全ブロックデバイスのツリー (lsblk -J)。MOUNTPOINTS 列が無い古い lsblk では MOUNTPOINT で再試行する。"""
    for cols in (LSBLK_COLS, LSBLK_COLS.replace("MOUNTPOINTS", "MOUNTPOINT")):
        r = subprocess.run(["lsblk", "-J", "-b", "-p", "-o", cols], capture_output=True, text=True)
        if r.returncode == 0:
            return parse_lsblk_json(r.stdout)
    raise RuntimeError("lsblk に失敗: " + r.stderr.strip())


def flatten(nodes):
    for n in nodes:
        yield n
        yield from flatten(n.get("children") or [])


def node_mounts(node):
    mp = node.get("mountpoints", node.get("mountpoint"))
    if mp is None:
        return []
    if not isinstance(mp, list):
        mp = [mp]
    return [m for m in mp if m]


def _unescape_mount(s):
    return re.sub(r"\\([0-7]{3})", lambda m: chr(int(m.group(1), 8)), s)


def parse_mounts(text):
    """/proc/mounts 形式 -> [(source, mountpoint, fstype)]"""
    out = []
    for ln in text.splitlines():
        c = ln.split()
        if len(c) >= 3:
            out.append((_unescape_mount(c[0]), _unescape_mount(c[1]), c[2]))
    return out


def parse_swaps(text):
    return [ln.split()[0] for ln in text.splitlines()[1:] if ln.strip()]


def _paths_of(node):
    return {x.get("path") for x in flatten([node]) if x.get("path")} | {x.get("kname") for x in flatten([node]) if x.get("kname")}


def disk_of_source(tree, src):
    """マウント元 (/dev/...) を含むトップレベルのディスクノードを返す。見つからなければ None。"""
    if not src or not src.startswith("/dev/"):
        return None
    cand = {src}
    try:
        cand.add(os.path.realpath(src))
    except OSError:
        pass
    for top in tree:
        if cand & _paths_of(top):
            return top
    return None


MEMORY_FS = ("tmpfs", "ramfs", "devtmpfs", "proc", "sysfs", "devpts", "cgroup2", "securityfs", "configfs", "debugfs", "tracefs", "mqueue", "hugetlbfs", "pstore", "bpf", "efivarfs", "fusectl", "autofs", "binfmt_misc")


def parse_mountinfo(text):
    """/proc/self/mountinfo -> [{mp, fstype, source, opts}] (super options は FS 固有オプション)。"""
    out = []
    for ln in text.splitlines():
        if " - " not in ln:
            continue
        left, right = ln.split(" - ", 1)
        lc, rc = left.split(), right.split()
        if len(lc) < 5 or len(rc) < 2:
            continue
        out.append({"mp": _unescape_mount(lc[4]), "fstype": rc[0], "source": _unescape_mount(rc[1]),
                    "opts": rc[2] if len(rc) > 2 else ""})
    return out


def _read_sys(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read().strip()
    except OSError:
        return None


def _mount_entry_for(mi, path):
    """path を含む最も深いマウント (同じマウント先なら後のもの) を返す。"""
    path = os.path.normpath(path)
    best = None
    for e in mi:
        mp = e["mp"]
        if path == mp or mp == "/" or path.startswith(mp.rstrip("/") + "/"):
            if best is None or len(mp) >= len(best["mp"]):
                best = e
    return best


def resolve_path_disks(tree, mi, path, reader=_read_sys, depth=0):
    """path が実際に載っているディスク (トップレベルの path) の集合と、特定できなかった理由を返す。
    overlay (lowerdir/upperdir/workdir)、loop (backing_file)、dm (slaves) をたどる。tmpfs 等のメモリ上の FS はディスク無し。"""
    if depth > 6:
        return set(), ["マウントの連鎖が深すぎる (%s)" % path]
    e = _mount_entry_for(mi, path)
    if e is None:
        return set(), ["%s を含むマウントが見つからない" % path]
    fs, src = e["fstype"], e["source"]
    if fs in MEMORY_FS:
        return set(), []
    if fs == "overlay":
        opts = {}
        for kv in e["opts"].split(","):
            if "=" in kv:
                k, v = kv.split("=", 1)
                opts[k] = v
        dirs = [d for d in opts.get("lowerdir", "").split(":") if d] + [opts[k] for k in ("upperdir", "workdir") if opts.get(k)]
        if not dirs:
            return set(), ["overlay のディレクトリが分からない (%s)" % e["opts"]]
        disks, bad = set(), []
        for d in dirs:
            a, b = resolve_path_disks(tree, mi, d, reader, depth + 1)
            disks |= a
            bad += b
        return disks, bad
    if not src.startswith("/dev/"):
        return set(), ["マウント元 '%s' (%s) がどのディスクか特定できない" % (src, fs)]
    return _resolve_dev_disks(tree, mi, src, reader, depth)


def _resolve_dev_disks(tree, mi, src, reader, depth):
    top = disk_of_source(tree, src)
    name = os.path.basename(os.path.realpath(src))
    if name.startswith("loop"):
        back = reader("/sys/block/%s/loop/backing_file" % name)
        if back:
            return resolve_path_disks(tree, mi, back, reader, depth + 1)
        return set(), ["loop デバイス %s の元ファイルが分からない" % src]
    if top is not None:
        return {top.get("path") or top.get("name")}, []
    return set(), ["マウント元 '%s' がどのディスクか特定できない" % src]


def system_disks(tree, mounts, mountinfo=None, reader=_read_sys):
    """システム/ホームなどのマウント元ディスクの集合 (path) と、特定できなかった理由のリストを返す。
    マウント元が overlay や loop のとき (ライブ USB 等) は mountinfo をたどって実体のディスクを探す。"""
    disks, unresolved = set(), []
    last = {}
    for src, mp, _fs in mounts:
        last[mp] = (src, _fs)
    mi = None
    for mp in SYSTEM_MOUNTS:
        if mp not in last:
            continue
        src, fs = last[mp]
        top = disk_of_source(tree, src)
        if top is not None:
            disks.add(top.get("path") or top.get("name"))
            continue
        if mi is None:
            txt = mountinfo if mountinfo is not None else _read_sys("/proc/self/mountinfo")
            mi = parse_mountinfo(txt or "")
        found, bad = (set(), ["mountinfo を読めない"]) if not mi else resolve_path_disks(tree, mi, mp, reader)
        disks |= found
        if bad and mp == "/":
            unresolved.append("ルート (/) のマウント元 '%s' がどのディスクか特定できない (%s)" % (src, "; ".join(bad)))
    return disks, unresolved


def validate_device(dev, profile, tree, mounts, swaps=(), is_block=True, mountinfo=None, reader=_read_sys):
    """実デバイスを検証し、拒否理由のリストを返す (空なら OK)。実際には何も書かない。"""
    why = []
    if not is_block:
        why.append("%s はブロックデバイスではない" % dev)
        return why
    real = os.path.realpath(dev)
    node = None
    for n in flatten(tree):
        if n.get("path") in (dev, real):
            node = n
            break
    if node is None:
        why.append("lsblk に %s が見つからない" % dev)
        return why
    if node.get("type") != "disk":
        why.append("%s は種別 '%s' で、ディスク全体ではない (パーティションや loop は不可)" % (dev, node.get("type")))
    top = None
    for t in tree:
        if node in list(flatten([t])):
            top = t
    tran = (node.get("tran") or "").lower()
    if tran in INTERNAL_TRANS:
        why.append("内蔵/非 USB バス (TRAN=%s) のディスクは対象外" % tran)
    elif tran != "usb":
        why.append("TRAN が usb ではない (TRAN='%s')" % (node.get("tran") or ""))
    if not (truthy(node.get("rm")) or tran == "usb"):
        why.append("リムーバブルでも USB 経由でもない")
    if truthy(node.get("ro")):
        why.append("読み取り専用デバイス")
    exp = profile.expected
    try:
        size = int(node.get("size"))
    except (TypeError, ValueError):
        size = -1
    if size < 0 or abs(size - exp) > SIZE_TOLERANCE * exp:
        why.append("サイズ %s がプロファイル %s の期待値 %s の ±%d%% を外れている" % (human(size) if size >= 0 else "不明", profile.name, human(exp), int(SIZE_TOLERANCE * 100)))
    ls = node.get("log-sec", node.get("log_sec"))
    if ls is not None and int(ls) not in (512, 4096):
        why.append("論理セクタサイズ %s は未対応" % ls)
    # マウント済み
    paths = _paths_of(node)
    paths |= {os.path.realpath(p) for p in paths if p}
    for x in flatten([node]):
        for m in node_mounts(x):
            why.append("%s が %s にマウントされている" % (x.get("path"), m))
    for src, mp, _fs in mounts:
        if src in paths or (src.startswith("/dev/") and os.path.realpath(src) in paths):
            why.append("/proc/mounts: %s が %s にマウントされている" % (src, mp))
    for sw in swaps:
        if sw in paths or os.path.realpath(sw) in paths:
            why.append("スワップとして使われている: " + sw)
    # システム/ホームのあるディスク
    sdisks, unresolved = system_disks(tree, mounts, mountinfo, reader)
    mine = {node.get("path"), (top or node).get("path")}
    if sdisks & mine:
        why.append("システム/ホーム/ブートのあるディスクである (%s)" % ", ".join(sorted(sdisks & mine)))
    if unresolved:
        why.append("安全側で中止: " + "; ".join(unresolved))
    # 重複を除く
    seen, out = set(), []
    for w in why:
        if w not in seen:
            seen.add(w)
            out.append(w)
    return out


def device_summary(node):
    return [
        ("デバイス", node.get("path")),
        ("モデル", ((node.get("vendor") or "").strip() + " " + (node.get("model") or "").strip()).strip() or "(不明)"),
        ("シリアル", node.get("serial") or "(不明)"),
        ("サイズ", "%s (%s バイト)" % (human(int(node.get("size") or 0)), node.get("size"))),
        ("バス", "%s  RM=%s  HOTPLUG=%s  RO=%s" % (node.get("tran"), node.get("rm"), node.get("hotplug"), node.get("ro"))),
    ]


def confirm_typed(typed, dev):
    """デバイスパスの手入力が一致しているか (前後の空白だけ許す)。"""
    return typed is not None and typed.strip() == dev


def check_image_target(path):
    """イメージモードの出力先がブロック/キャラクタデバイスでないこと。"""
    if os.path.exists(path):
        m = os.stat(path).st_mode
        if stat.S_ISBLK(m) or stat.S_ISCHR(m):
            return ["%s はデバイスファイル。--image は通常ファイル専用" % path]
    real = os.path.realpath(path)
    if real.startswith("/dev/") and not real.startswith("/dev/shm/"):
        return ["%s は /dev 配下 (/dev/shm 以外) で、イメージファイルの置き場所として不適切" % path]
    return []


# ----------------------------------------------------------------------------------------------------------------------
# ファイルシステムごとの計画 (コマンド列)。'@' で始まる argv[0] は内部操作
# ----------------------------------------------------------------------------------------------------------------------


@dataclasses.dataclass
class Opts:
    skip_mount_fs: bool = False
    dirty_ntfs: bool = False
    no_big: bool = False
    small_count: object = None
    luks_pbkdf: str = "argon2id"
    f2fs_method: str = "sload"
    udf_method: str = "mkudffs"
    bitlocker: str = "synth"
    verify_content: bool = False
    iso_tool: str = "genisoimage"
    ss: int = 512
    exfat_method: str = "mount"      # mount=カーネルの exFAT を mount (root) / fuse=exfat-fuse (root 不要)
    luks_method: str = "open"        # open=cryptsetup open + mke2fs (root) / xts=内側の ext4 を平文で作って Python で XTS 暗号化して書く (root 不要)


def mapper_name(pp):
    return "usbmem_ts_%s_%d" % (pp.name, os.getpid())


def fs_plan(pp, dev, stage, out, opts):
    """パーティション pp を dev に作る計画。dev=パーティションのデバイスノード (イメージモードでは一時ファイル)。
    stage=-d 用の投入ツリー置き場、out=中間イメージファイルの置き場 (squashfs/erofs/iso/bitlocker 用)。"""
    fs = pp.fs
    prof = pp.profile
    lab = pp.label
    u = derive_uuid(prof, pp.name, "fs")
    plan = []
    add = plan.append
    S = lambda *a, **k: Cmd(list(a), kind="special", **k)

    if fs in ("ext2", "ext3", "ext4"):
        add(S("@stage"))
        mk = ["mke2fs", "-q", "-F", "-t", fs, "-b", "4096", "-m", "0", "-L", lab, "-U", u, "-E", "lazy_itable_init=0,lazy_journal_init=0,nodiscard,hash_seed=" + derive_uuid(prof, pp.name, "hash"), "-d", stage, dev]
        add(Cmd(mk))
        add(S("@rmstage"))
        add(Cmd(["e2fsck", "-fn", dev], kind="check"))
        if pp.spec.needs_recovery:
            add(Cmd(["debugfs", "-w", "-R", "feature needs_recovery", dev], note="needs_recovery を立てる (空のジャーナルだが、マウント時にリカバリが要る状態)"))
            add(Cmd(["dumpe2fs", "-h", dev], kind="check", note="needs_recovery が立っていることの確認 (出力に needs_recovery があれば OK)"))
    elif fs == "fat32":
        add(S("@stage"))
        add(Cmd(["mkfs.fat", "-F", "32", "-n", lab[:11], "-i", derive_hex32(prof, pp.name, "fatid"), dev]))
        add(Cmd(["mcopy", "-i", dev, "-s", "-m", "-Q", "-n", "<ステージ直下の全エントリ>", "::/"], cwd=stage, env={"MTOOLS_SKIP_CHECK": "1"}, note="mtools で投入 (名前は UTF-8 のまま長い名前で入る)"))
        add(S("@rmstage"))
        add(Cmd(["fsck.fat", "-n", dev], kind="check"))
    elif fs == "exfat":
        if opts.exfat_method == "fuse":
            # exfat-fuse 1.4.0 はボリューム GUID のエントリ (0xA0) を知らずにマウントを拒否する。GUID は投入と照合のあとに tune.exfat で付ける
            add(Cmd(["mkfs.exfat", "-L", lab, dev]))
            add(S("@mount", "exfat-fuse", dev))
            add(S("@populate"))
            add(S("@umount"))
        elif opts.skip_mount_fs:
            add(Cmd(["mkfs.exfat", "-L", lab, "-U", u, dev]))
            add(S("@note", "データ投入を省略 (--skip-mount-fs。exFAT は root の mount が要る。--exfat-method fuse なら root 不要)"))
        else:
            add(Cmd(["mkfs.exfat", "-L", lab, "-U", u, dev]))
            add(S("@mount", "exfat", dev))
            add(S("@populate"))
            add(S("@umount"))
        add(Cmd(["fsck.exfat", "-n", dev], kind="check"))
    elif fs == "ntfs":
        add(Cmd(["mkntfs", "-F", "-Q", "-q", "-L", lab, dev]))
        add(S("@mount", "ntfs", dev))
        add(S("@populate"))
        add(S("@umount"))
        add(Cmd(["ntfsfix", "-n", dev], kind="check"))
        if opts.verify_content:
            add(S("@verify", dev))      # ダーティにする前に照合する (ダーティだと ntfs-3g が読み取りマウントも拒否する)
        if pp.spec.dirty_opt and opts.dirty_ntfs:
            add(S("@ntfs-dirty", dev))
            add(Cmd(["ntfsinfo", "-m", dev], kind="check", note="独立実装 (libntfs-3g) がダーティと認識することの確認 (出力に 'scheduled for check' があれば OK)"))
    elif fs == "xfs":
        add(S("@stage"))
        # 新しい mkfs.xfs (7.x) の既定 (rmapbt=1 / parent=1) は、アプリの書き込みが拒否する構成 (docs/xfs-write.md)。
        # 実機の書き込み試験ができるよう、無効にして作る。古い mkfs.xfs に無いオプションは外す。
        add(Cmd(["mkfs.xfs", "-f", "-q", "-L", lab, "-m", "uuid=" + u + _xfs_mkfs_m_opts()] + _xfs_mkfs_n_opts() + ["-p", stage, dev]))
        add(S("@rmstage"))
        add(Cmd(["xfs_repair", "-n", dev], kind="check"))
    elif fs == "btrfs":
        add(S("@stage"))
        add(Cmd(["mkfs.btrfs", "-f", "-q", "-L", lab, "-U", u, "--rootdir", stage, dev]))
        add(S("@rmstage"))
        add(Cmd(["btrfs", "check", "--readonly", dev], kind="check"))
    elif fs == "f2fs":
        add(Cmd(["mkfs.f2fs", "-f", "-q", "-l", lab, "-U", u, dev]))
        if opts.f2fs_method == "sload":
            add(S("@stage"))
            add(Cmd(["sload.f2fs", "-f", stage, dev]))
            add(S("@rmstage"))
        elif opts.skip_mount_fs:
            add(S("@note", "データ投入を省略 (--skip-mount-fs。--f2fs-method mount は root の mount が要る)"))
        else:
            add(S("@mount", "f2fs", dev))
            add(S("@populate"))
            add(S("@umount"))
        add(Cmd(["fsck.f2fs", dev], kind="check"))
    elif fs == "luks":
        key = out + ".key"
        pb = ["--pbkdf", opts.luks_pbkdf]
        if opts.luks_pbkdf == "argon2id":
            pb += ["--pbkdf-memory", "65536", "--pbkdf-parallel", "1", "--iter-time", "300"]
        else:
            pb += ["--iter-time", "300"]
        if opts.luks_method == "xts":
            pb += ["--sector-size", str(LUKS_SECTOR)]
        add(S("@luks-key", key))
        add(Cmd(["cryptsetup", "luksFormat", "--type", "luks2", "--batch-mode", "--key-file", key, "--label", lab, "--uuid", u] + pb + [dev], note="LUKS2 ヘッダ。パスフレーズは %s" % LUKS_PASSPHRASE))
        if opts.luks_method == "xts":
            inner = out + ".inner"
            add(S("@stage"))
            add(S("@luks-inner", inner, key, note="内側の ext4 を平文のファイルに作る (cryptsetup open の代わり。root 不要)"))
            add(Cmd(["mke2fs", "-q", "-F", "-t", "ext4", "-b", "4096", "-m", "0", "-L", "TS_LUKS_IN", "-U", derive_uuid(prof, pp.name, "inner"), "-E", "lazy_itable_init=0,lazy_journal_init=0,nodiscard", "-d", stage, inner]))
            add(Cmd(["e2fsck", "-fn", inner], kind="check"))
            add(S("@luks-encrypt", inner, dev, note="ボリューム鍵で aes-xts-plain64 (512 バイトセクタ) 暗号化してペイロードへ書く"))
            add(S("@rmstage"))
        elif opts.skip_mount_fs:
            add(S("@note", "データ投入を省略 (--skip-mount-fs。LUKS を開くには root が要る。--luks-method xts なら root 不要)。ヘッダだけ作った"))
        else:
            add(S("@stage"))
            mp = "/dev/mapper/" + mapper_name(pp)
            add(Cmd(["cryptsetup", "open", "--type", "luks2", "--key-file", key, dev, mapper_name(pp)]))
            add(Cmd(["mke2fs", "-q", "-F", "-t", "ext4", "-b", "4096", "-m", "0", "-L", "TS_LUKS_IN", "-U", derive_uuid(prof, pp.name, "inner"), "-E", "lazy_itable_init=0,lazy_journal_init=0,nodiscard", "-d", stage, mp]))
            add(Cmd(["e2fsck", "-fn", mp], kind="check"))
            add(Cmd(["cryptsetup", "close", mapper_name(pp)]))
            add(S("@rmstage"))
        add(Cmd(["cryptsetup", "luksDump", dev], kind="check"))
    elif fs == "bitlocker":
        if opts.bitlocker == "skip":
            add(S("@note", "BitLocker は省略 (--bitlocker skip)。Windows で作る手順は docs/real-usb-testset.md"))
        else:
            plain = out + ".plain"
            add(S("@truncate", plain, str(pp.size - 4 * 64 * KiB)))
            add(Cmd(["mkntfs", "-F", "-Q", "-q", "-L", "TS_BL_IN", plain]))
            add(S("@mount", "ntfs", plain))
            add(S("@populate"))
            add(S("@umount"))
            add(Cmd([sys.executable, os.path.join(HERE, "bitlocker_synth.py"), plain, out, "--method", "8004", "--password", BITLOCKER_PASSWORD, "--meta", out + ".meta"], note="合成 BitLocker (XTS-AES-128、パスワード保護)。実 Windows 製ではない"))
            add(S("@deliver", out))
            add(S("@save-meta", out + ".meta"))
            add(S("@check-fve", dev))
    elif fs == "squashfs":
        add(S("@stage"))
        add(Cmd(["mksquashfs", stage, out, "-noappend", "-quiet", "-no-progress", "-all-root", "-no-xattrs", "-b", "131072", "-mkfs-time", str(FIXED_TIME), "-all-time", str(FIXED_TIME)]))
        add(S("@rmstage"))
        add(S("@deliver", out))
        add(Cmd(["unsquashfs", "-s", dev], kind="check"))
    elif fs == "erofs":
        add(S("@stage"))
        add(Cmd(["mkfs.erofs", "-d0", "-T", str(FIXED_TIME), "-U", u, "-L", lab, "--all-root", out, stage]))
        add(S("@rmstage"))
        add(S("@deliver", out))
        add(Cmd(["fsck.erofs", dev], kind="check"))
    elif fs == "iso":
        add(S("@stage"))
        common = ["-V", lab, "-J", "-R", "-joliet-long", "-input-charset", "utf-8", "-iso-level", "3", "-o", out, stage]
        if opts.iso_tool == "xorriso":
            add(Cmd(["xorriso", "-as", "mkisofs", "-quiet"] + common))
        else:
            add(Cmd(["genisoimage", "-quiet"] + common))
        add(S("@rmstage"))
        add(S("@deliver", out))
        add(Cmd(["isoinfo", "-d", "-i", dev], kind="check"))
    elif fs == "udf":
        if opts.udf_method == "genisoimage":
            add(S("@stage"))
            add(Cmd(["genisoimage", "-quiet", "-udf", "-V", lab, "-J", "-R", "-joliet-long", "-input-charset", "utf-8", "-o", out, stage], note="ISO9660 + UDF 1.02 ブリッジ (mount 不要。純粋な UDF ではない)"))
            add(S("@rmstage"))
            add(S("@deliver", out))
        else:
            add(Cmd(["mkudffs", "--utf8", "--blocksize=%d" % opts.ss, "--label=" + lab, "--lvid=" + lab, "--vid=" + lab, "--media-type=hd", dev]))
            if opts.skip_mount_fs:
                add(S("@note", "データ投入を省略 (--skip-mount-fs。UDF は root の mount が要る)"))
            else:
                add(S("@mount", "udf", dev))
                add(S("@populate"))
                add(S("@umount"))
        add(Cmd(["udfinfo", dev], kind="check"))
    else:
        raise ValueError("未対応の FS: " + fs)
    if opts.verify_content and fs != "ntfs":
        add(S("@verify", dev))
    if fs == "exfat" and opts.exfat_method == "fuse":
        add(Cmd(["tune.exfat", "-U", u, dev], note="ボリューム GUID を付ける (mkfs.exfat -U と同じ値)"))
        add(Cmd(["fsck.exfat", "-n", dev], kind="check"))
    return plan


def required_tools(profile, table, opts, mode, euid_root):
    """イメージ作成 (a) に必須のコマンドを (コマンド -> 理由) で返す。mode='device' なら実デバイスの検証・バックアップ・消去に要るものも加える
    (流し込みに使う nbdcopy / nbdkit / dd は flash_tools)。"""
    req = {}
    tbl = ["sgdisk"] if table == "gpt" else ["sfdisk"]
    for t in tbl:
        req[t] = "パーティション表の作成 (デバイスと同じ大きさの疎ファイルに作る)"
    if mode == "device":
        for t in ("sgdisk", "sfdisk", "lsblk", "wipefs", "blockdev", "dd"):
            req[t] = "実デバイスの検証・バックアップ・消去・キャッシュ破棄"
    for spec in profile.parts:
        fs = spec.fs
        mnt = [] if opts.skip_mount_fs else ["mount", "umount"]
        need = {
            "ext2": ["mke2fs", "e2fsck"], "ext3": ["mke2fs", "e2fsck"], "ext4": ["mke2fs", "e2fsck", "debugfs", "dumpe2fs"],
            "fat32": ["mkfs.fat", "mcopy", "fsck.fat"],
            "exfat": ["mkfs.exfat", "fsck.exfat"] + (["mount.exfat-fuse", "fusermount3", "cc", "tune.exfat"] if opts.exfat_method == "fuse" else mnt),
            "ntfs": ["mkntfs", "ntfs-3g", "ntfsfix"], "xfs": ["mkfs.xfs", "xfs_repair"], "btrfs": ["mkfs.btrfs", "btrfs"],
            "f2fs": ["mkfs.f2fs", "fsck.f2fs"] + (["sload.f2fs"] if opts.f2fs_method == "sload" else mnt),
            "luks": ["cryptsetup"] + (["mke2fs", "e2fsck"] if (opts.luks_method == "xts" or not opts.skip_mount_fs) else []),
            "bitlocker": [] if opts.bitlocker == "skip" else ["mkntfs", "ntfs-3g"],
            "squashfs": ["mksquashfs", "unsquashfs"], "erofs": ["mkfs.erofs", "fsck.erofs"],
            "iso": ["xorriso" if opts.iso_tool == "xorriso" else "genisoimage", "isoinfo"],
            "udf": (["genisoimage"] if opts.udf_method == "genisoimage" else ["mkudffs"] + mnt) + ["udfinfo"],
        }[fs]
        for t in need:
            req.setdefault(t, FS_DISPLAY[fs])
        if fs == "ntfs" and opts.dirty_ntfs and spec.dirty_opt:
            req.setdefault("ntfsinfo", "ダーティフラグの確認")
        if fs == "ntfs" and not euid_root:
            req.setdefault("fusermount3", "NTFS (ntfs-3g FUSE) のアンマウント")
        if fs == "exfat" and opts.exfat_method == "fuse" and not euid_root:
            req.setdefault("fusermount3", "exFAT (exfat-fuse) のアンマウント")
    return req


def flash_tools(writer):
    """流し込み (d) と読み戻し (e) に要るコマンド。writer は 'nbdcopy' か 'dd'。"""
    if writer == "nbdcopy":
        return {"nbdcopy": "流し込み (NBD 経由の順次コピー)", "nbdkit": "nbdcopy の書き込み先のパーティション範囲を切り出す (offset フィルタ)"}
    return {"dd": "流し込み (dd)"}


def find_missing(req):
    return sorted((t, why) for t, why in req.items() if shutil.which(t) is None)


# ----------------------------------------------------------------------------------------------------------------------
# NTFS ダーティフラグ ($Volume の VOLUME_INFORMATION の flags の bit0) を直接書く / 読む
# ----------------------------------------------------------------------------------------------------------------------


def _ntfs_locate_volume_record(f, base):
    f.seek(base)
    b = f.read(512)
    if b[3:11] != b"NTFS    ":
        raise ValueError("NTFS のブートセクタではない")
    bps = int.from_bytes(b[11:13], "little")
    spc = b[13]
    cs = bps * spc
    mft = int.from_bytes(b[0x30:0x38], "little")
    mirr = int.from_bytes(b[0x38:0x40], "little")
    rs = b[0x40] - 256 if b[0x40] > 127 else b[0x40]
    rec = (1 << -rs) if rs < 0 else rs * cs
    return base + mft * cs + 3 * rec, rec, base + mirr * cs + 3 * rec


def _ntfs_find_flags(r):
    """fixup 適用済みの MFT レコード r の VOLUME_INFORMATION flags のオフセット。"""
    a = int.from_bytes(r[0x14:0x16], "little")
    while a + 8 <= len(r):
        t = int.from_bytes(r[a:a + 4], "little")
        ln = int.from_bytes(r[a + 4:a + 8], "little")
        if t == 0xFFFFFFFF or ln == 0:
            break
        if t == 0x70:
            vo = int.from_bytes(r[a + 0x14:a + 0x16], "little")
            return a + vo + 10
        a += ln
    raise ValueError("$VOLUME_INFORMATION が見つからない")


def ntfs_flags(path, base=0, set_dirty=None):
    """$Volume の flags を返す。set_dirty が True/False なら bit0 をその値にして書く (戻り値は書く前の値)。"""
    with open(path, "r+b" if set_dirty is not None else "rb") as f:
        pos, rec, mpos = _ntfs_locate_volume_record(f, base)
        f.seek(pos)
        raw = bytearray(f.read(rec))
        if raw[:4] != b"FILE":
            raise ValueError("MFT レコードの署名が FILE でない")
        uo = int.from_bytes(raw[4:6], "little")
        uc = int.from_bytes(raw[6:8], "little")
        usn = bytes(raw[uo:uo + 2])
        r = bytearray(raw)
        for i in range(1, uc):
            if raw[i * 512 - 2:i * 512] != usn:
                raise ValueError("fixup の値が一致しない")
            r[i * 512 - 2:i * 512] = raw[uo + 2 * i:uo + 2 * i + 2]
        fo = _ntfs_find_flags(r)
        old = int.from_bytes(r[fo:fo + 2], "little")
        if set_dirty is not None:
            new = (old | 1) if set_dirty else (old & ~1)
            r[fo:fo + 2] = new.to_bytes(2, "little")
            out = bytearray(r)
            for i in range(1, uc):
                out[uo + 2 * i:uo + 2 * i + 2] = r[i * 512 - 2:i * 512]
                out[i * 512 - 2:i * 512] = usn
            f.seek(pos)
            f.write(bytes(out))
            f.seek(mpos)                     # $MFTMirr (先頭 4 レコードの写し) も同じに。食い違うと ntfs-3g が別の理由で拒否する
            f.write(bytes(out))
            f.flush()
            os.fsync(f.fileno())
        return old


# ----------------------------------------------------------------------------------------------------------------------
# 実行系
# ----------------------------------------------------------------------------------------------------------------------


class StepError(Exception):
    pass


class Runner:
    def __init__(self, log_path, dry=False, quiet=False):
        self.dry = dry
        self.quiet = quiet
        self.log_path = log_path
        self.logf = open(log_path, "a", encoding="utf-8") if (log_path and not dry) else None

    def log(self, msg):
        if self.logf:
            self.logf.write("%s %s\n" % (datetime.datetime.now().isoformat(timespec="seconds"), msg))
            self.logf.flush()

    def say(self, msg):
        print(msg, flush=True)
        self.log(msg)

    def run(self, cmd, check=True, timeout=None, live=False):
        """Cmd を実行。(returncode, 出力) を返す。check=True で失敗なら StepError。
        live=True は進捗表示つきのコマンド (nbdcopy --progress / dd status=progress) 用: 標準エラーを逐次そのまま画面に流す。"""
        env = dict(os.environ, LC_ALL="C.UTF-8")
        if cmd.env:
            env.update(cmd.env)
        self.log("$ " + cmd.text() + ((" (cwd=%s)" % cmd.cwd) if cmd.cwd else ""))
        if not self.quiet:
            print("    $ " + cmd.text(), flush=True)
        argv = cmd.argv
        if cmd.argv[:1] == ["mcopy"] and cmd.cwd:
            argv = cmd.argv[:-2] + sorted(os.listdir(cmd.cwd)) + cmd.argv[-1:]   # プレースホルダを実際のエントリに置換
        if live:
            out = self._run_live(argv, env, cmd.cwd)
            rc = out[0]
            out = out[1]
            self.log("  rc=%d %s" % (rc, out[-3000:].strip().replace("\n", "\n  ")))
            if rc != 0 and check:
                raise StepError("コマンド失敗 (rc=%d): %s\n%s" % (rc, cmd.text(), out[-1500:]))
            return rc, out
        stdout = open(cmd.stdout_file, "wb") if cmd.stdout_file else subprocess.PIPE
        try:
            r = subprocess.run(argv, input=cmd.stdin.encode() if cmd.stdin is not None else None, stdout=stdout, stderr=subprocess.PIPE,
                               env=env, cwd=cmd.cwd, timeout=timeout)
        except FileNotFoundError as e:
            raise StepError("コマンドが見つからない: %s" % e)
        finally:
            if cmd.stdout_file:
                stdout.close()
        out = (r.stdout.decode(errors="replace") if r.stdout else "") + r.stderr.decode(errors="replace")
        self.log("  rc=%d %s" % (r.returncode, out[-3000:].strip().replace("\n", "\n  ")))
        if r.returncode != 0 and check:
            raise StepError("コマンド失敗 (rc=%d): %s\n%s" % (r.returncode, cmd.text(), out[-1500:]))
        return r.returncode, out

    def _run_live(self, argv, env, cwd):
        try:
            p = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, cwd=cwd)
        except FileNotFoundError as e:
            raise StepError("コマンドが見つからない: %s" % e)
        tail = bytearray()
        fd = p.stdout.fileno()
        while True:
            b = os.read(fd, 4096)
            if not b:
                break
            if not self.quiet:
                sys.stderr.buffer.write(b)
                sys.stderr.buffer.flush()
            tail += b
            if len(tail) > 8192:
                del tail[:-4096]
        rc = p.wait()
        if not self.quiet:
            sys.stderr.write("\n")
        return rc, tail.decode(errors="replace")


# ---- 実行コンテキスト -------------------------------------------------------------------------------------------------


class Ctx:
    def __init__(self, args, runner, work, mode, ss, opts, manifest_dir):
        self.args, self.run, self.work, self.mode, self.ss, self.opts, self.manifest_dir = args, runner, work, mode, ss, opts, manifest_dir
        self.mnt = None
        self.ds = None
        self.dataset_entries = None


def run_special(ctx, pp, dev, cmd, state):
    a = cmd.argv
    op = a[0]
    R = ctx.run
    caps = state["caps"]
    meta = dict(profile=pp.profile, part=pp.name, fs=pp.fs, label=pp.label)
    if op == "@stage":
        stage = state["stage"]
        shutil.rmtree(stage, ignore_errors=True)
        os.makedirs(stage)
        R.say("    (データ生成: %s)" % stage)
        state["ds"] = build_dataset(stage, caps, meta)
    elif op == "@rmstage":
        shutil.rmtree(state["stage"], ignore_errors=True)
    elif op == "@note":
        R.say("    ※ " + a[1])
        state["notes"].append(a[1])
        state["populated"] = False
    elif op == "@mount":
        fstype, mdev = a[1], a[2]
        mnt = tempfile.mkdtemp(prefix=WORK_MNT_PREFIX, dir=ctx.work)
        if fstype == "ntfs":
            R.run(Cmd(["ntfs-3g", "-o", "rw,nodev,nosuid,windows_names", mdev, mnt]))
        elif fstype == "exfat-fuse":
            R.run(exfat_fuse_cmd(ctx, mdev, mnt, rw=True))
        elif fstype == "exfat":
            R.run(Cmd(["mount", "-t", "exfat", "-o", "rw,uid=0,gid=0,iocharset=utf8", mdev, mnt]))
        elif fstype == "f2fs":
            R.run(Cmd(["mount", "-t", "f2fs", mdev, mnt]))
        elif fstype == "udf":
            R.run(Cmd(["mount", "-t", "udf", "-o", "rw,utf8", mdev, mnt]))
        state["mnt"], state["mnt_fs"] = mnt, fstype            # マウントできてから記録する (失敗時にアンマウントを繰り返さない)
    elif op == "@populate":
        R.say("    (マウント先へデータ生成: %s)" % state["mnt"])
        state["ds"] = build_dataset(state["mnt"], caps, meta)
        R.run(Cmd(["sync", "-f", state["mnt"]]))          # このファイルシステムだけ書き出す (全デバイス対象の sync はしない)
    elif op == "@umount":
        umount(ctx, state)
    elif op == "@luks-key":
        with open(a[1], "w") as f:
            f.write(LUKS_PASSPHRASE)
        os.chmod(a[1], 0o600)
        state["tmpfiles"].append(a[1])
    elif op == "@luks-inner":
        luks_prepare_inner(ctx, pp, dev, a[1], a[2], state)
    elif op == "@luks-encrypt":
        luks_encrypt_inner(ctx, pp, dev, a[1], state)
    elif op == "@truncate":
        with open(a[1], "wb") as f:
            f.truncate(int(a[2]))
        state["tmpfiles"].append(a[1])
    elif op == "@deliver":
        deliver(ctx, a[1], dev, state)
    elif op == "@save-meta":
        if os.path.exists(a[1]):
            os.makedirs(ctx.manifest_dir, exist_ok=True)
            shutil.copyfile(a[1], os.path.join(ctx.manifest_dir, pp.name + ".bitlocker-meta.txt"))
        state["tmpfiles"].append(a[1])
    elif op == "@check-fve":
        with open(a[1], "rb") as f:
            sig = f.read(16)[3:11]
        if sig != b"-FVE-FS-":
            raise StepError("BitLocker の署名 (-FVE-FS-) がブートセクタにない")
        R.say("    BitLocker 署名 (-FVE-FS-) を確認")
    elif op == "@ntfs-dirty":
        base = 0
        old = ntfs_flags(a[1], base, set_dirty=True)
        new = ntfs_flags(a[1], base)
        R.say("    NTFS ダーティフラグを立てた: flags 0x%04x -> 0x%04x" % (old, new))
        state["notes"].append("NTFS ダーティフラグ付き (flags=0x%04x)" % new)
    elif op == "@verify":
        verify_partition_content(ctx, pp, dev, state)
    else:
        raise StepError("未知の内部操作: " + op)


def umount(ctx, state):
    mnt = state.get("mnt")
    if not mnt:
        return
    R = ctx.run
    if state.get("mnt_fs") in ("ntfs", "exfat-fuse") and os.geteuid() != 0:
        argv = ["fusermount3", "-u", mnt]
    else:
        argv = ["umount", mnt]
    for i in range(10):
        rc, _ = R.run(Cmd(argv), check=False)
        if rc == 0:
            break
        time.sleep(1)
    else:
        raise StepError("アンマウントできない: " + mnt)
    state["mnt"] = None
    try:
        os.rmdir(mnt)
    except OSError:
        pass


def deliver(ctx, src, dev, state):
    """中間イメージ src の中身をパーティションイメージ dev (ローカルの疎ファイル) の先頭へ書く。ゼロの部分は書かず疎のままにする。"""
    sz = os.path.getsize(src)
    dsz = device_size(dev)
    if sz > dsz:
        raise StepError("イメージ %s (%s) がパーティション (%s) に入らない" % (src, human(sz), human(dsz)))
    ctx.run.say("    (書き込み: %s -> %s)" % (human(sz), dev))
    ctx.run.log("deliver %s -> %s %d bytes" % (src, dev, sz))
    splice(src, dev, 0)
    fsync_path(dev)
    state["tmpfiles"].append(src)


def fsync_path(path):
    """ファイル 1 本だけを書き出す (全デバイス対象の sync はしない)。"""
    fd = os.open(path, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def device_size(path):
    st = os.stat(path)
    if stat.S_ISREG(st.st_mode):
        return st.st_size
    with open(path, "rb") as f:
        return f.seek(0, os.SEEK_END)


def verify_partition_content(ctx, pp, dev, state):
    """FS の中身を取り出して (または読み取りマウントして) マニフェストと照合する。取り出せない FS は fsck 相当のみ。"""
    R = ctx.run
    ds = state.get("ds")
    if ds is None:
        R.say("    (中身の照合: データ未投入のためなし)")
        return
    ex = tempfile.mkdtemp(prefix="usbmem_verify_", dir=ctx.work)
    mounted = None
    try:
        fs = pp.fs
        links = False
        if fs == "luks":
            if not state.get("luks_inner"):
                R.say("    (LUKS の中身は cryptsetup open が要るため照合は fsck 相当のみ)")
                return
            fs, dev = "ext4", state["luks_inner"]          # 暗号化前の内側の ext4 (平文) を照合する
        if fs in ("ext2", "ext3", "ext4"):
            R.run(Cmd(["debugfs", "-R", "rdump / " + ex, dev]))
        elif fs == "fat32":
            R.run(Cmd(["mcopy", "-i", dev, "-s", "-m", "-Q", "-n", "::/", ex], env={"MTOOLS_SKIP_CHECK": "1"}))
        elif fs == "btrfs":
            R.run(Cmd(["btrfs", "restore", "-S", dev, ex]))
        elif fs == "squashfs":
            shutil.rmtree(ex)
            R.run(Cmd(["unsquashfs", "-no-progress", "-d", ex, dev]))
            links = True
        elif fs == "erofs":
            R.run(Cmd(["fsck.erofs", "--extract=" + ex, "--force", dev]))
            links = True
        elif fs in ("iso", "udf") and (fs == "iso" or ctx.opts.udf_method == "genisoimage"):
            shutil.rmtree(ex)
            R.run(Cmd(["xorriso", "-osirrox", "on", "-indev", dev, "-extract", "/", ex]))
        elif fs == "ntfs":
            rc, _ = R.run(Cmd(["ntfs-3g", "-o", "ro,nodev,nosuid", dev, ex]), check=False)
            if rc != 0:
                R.say("    (NTFS の読み取りマウントができないため照合を省略)")
                return
            mounted = ex
            links = True
        elif fs == "exfat" and ctx.opts.exfat_method == "fuse":
            rc, _ = R.run(exfat_fuse_cmd(ctx, dev, ex, rw=False), check=False)
            if rc != 0:
                R.say("    (exFAT の読み取りマウントができないため照合を省略)")
                return
            mounted = ex
        else:
            R.say("    (%s は取り出せるツールがないため、中身の照合は fsck 相当のみ)" % FS_DISPLAY[fs])
            return
        problems, cnt = verify_tree(ds.entries + [e for e in []], ex, check_links=links, allow_extra=True)
        # MANIFEST.sha256 自身は ds.entries に入っていない。投入ツリーのものと比べる
        R.say("    中身の照合: ファイル %d (うちハードリンク %d)・ディレクトリ %d・リンク %d・%s 一致%s" % (
            cnt["F"], cnt["H"], cnt["D"], cnt["L"], human(cnt["bytes"]), "" if links else " (リンク類はこの取り出し方法では検証対象外)"))
        if problems:
            raise StepError("マニフェスト照合に失敗 (%d 件): %s" % (len(problems), "; ".join(problems[:5])))
    finally:
        if mounted:
            subprocess.run(["fusermount3", "-u", mounted], capture_output=True)
        shutil.rmtree(ex, ignore_errors=True)


def exfat_fuse_shim(ctx):
    """exfat-fuse 1.4.0 が fusermount3 に渡す user= / blkdev / blksize= を取り除く LD_PRELOAD の .so を作業ディレクトリに作る (tools/exfat_fuse_shim.c)。"""
    so = os.path.join(ctx.work, "exfat_fuse_shim.so")
    if not os.path.exists(so):
        cc = shutil.which("cc") or shutil.which("gcc")
        if not cc:
            raise StepError("exfat-fuse の補助 .so を作るのに cc / gcc が要る")
        r = subprocess.run([cc, "-shared", "-fPIC", "-O1", "-o", so, os.path.join(HERE, "exfat_fuse_shim.c"), "-ldl"], capture_output=True, text=True)
        if r.returncode != 0:
            raise StepError("exfat_fuse_shim.c のコンパイルに失敗: " + r.stderr[-500:])
    return so


def exfat_fuse_cmd(ctx, dev, mnt, rw):
    """exfat-fuse (root 不要) でイメージファイル dev を mnt にマウントするコマンド。"""
    opt = "%s,uid=%d,gid=%d" % ("rw" if rw else "ro", os.geteuid(), os.getegid())
    return Cmd(["mount.exfat-fuse", "-o", opt, dev, mnt], env={"LD_PRELOAD": exfat_fuse_shim(ctx)}, note="exfat-fuse (FUSE。root 不要)")


# ---- LUKS2 を root なしで作る: 内側の ext4 を平文のファイルに作り、ボリューム鍵で aes-xts-plain64 暗号化してペイロードへ書く ----


def luks_payload_offset(dev):
    r = subprocess.run(["cryptsetup", "luksDump", "--dump-json-metadata", dev], capture_output=True, text=True)
    if r.returncode != 0:
        raise StepError("luksDump に失敗: " + r.stderr[-300:])
    seg = json.loads(r.stdout)["segments"]["0"]
    if seg.get("encryption") != "aes-xts-plain64" or int(seg.get("sector_size", 512)) != LUKS_SECTOR:
        raise StepError("想定外の LUKS セグメント: %s" % seg)
    return int(seg["offset"])


def xts_tweak(sector_no):
    return int(sector_no).to_bytes(16, "little")


def xts_crypt_buffer(key, data, first_sector, decrypt=False, sector=LUKS_SECTOR):
    """dm-crypt の aes-xts-plain64 と同じ: セクタごとに、IV = セクタ番号 (リトルエンディアン 128 ビット、ペイロード先頭が 0)。key は 64 バイト (AES-256-XTS)。"""
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    out = bytearray(len(data))
    alg = algorithms.AES(key)
    for i in range(0, len(data), sector):
        c = Cipher(alg, modes.XTS(xts_tweak(first_sector + i // sector)))
        op = c.decryptor() if decrypt else c.encryptor()
        out[i:i + sector] = op.update(bytes(data[i:i + sector])) + op.finalize()
    return bytes(out)


def luks_prepare_inner(ctx, pp, dev, inner, keyfile, state):
    """ペイロードのサイズの平文ファイル inner を作り、ボリューム鍵を取り出す。"""
    if ctx.opts.ss != 512:
        raise StepError("--luks-method xts は論理セクタ 512 のデバイスだけ対応 (root で --luks-method open を使う)")
    off = luks_payload_offset(dev)
    vk = inner + ".vk"
    if os.path.exists(vk):
        os.unlink(vk)
    r = subprocess.run(["cryptsetup", "luksDump", "--batch-mode", "--dump-volume-key", "--volume-key-file", vk, "--key-file", keyfile, dev], capture_output=True, text=True)
    if r.returncode != 0:
        raise StepError("ボリューム鍵を取り出せない: " + r.stderr[-300:])
    state["tmpfiles"] += [vk, inner]
    with open(vk, "rb") as f:
        key = f.read()
    if len(key) != 64:
        raise StepError("ボリューム鍵が 64 バイトでない (%d)" % len(key))
    size = pp.size - off
    if size <= 0 or size % 4096:
        raise StepError("LUKS のペイロードのサイズが不正: %d" % size)
    with open(inner, "wb") as f:
        f.truncate(size)
    state.update(luks_off=off, luks_key=key, luks_inner=inner)
    ctx.run.say("    (LUKS ペイロード: オフセット %s・内側 %s)" % (human(off), human(size)))


def reference_xts_sector(key, sector_no, data, decrypt=False):
    """独立実装: AES-ECB だけを使い、IEEE P1619 の XTS (GF(2^128) でのトゥイーク倍化) を手で組んで 1 セクタを変換する。
    暗号ライブラリの XTS モードとは別の経路なので、向き・IV・鍵の分割の取り違えを検出できる (openssl enc は XTS 非対応)。"""
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    half = len(key) // 2
    k1, k2 = key[:half], key[half:]
    t = Cipher(algorithms.AES(k2), modes.ECB()).encryptor().update(xts_tweak(sector_no))
    tw = int.from_bytes(t, "little")
    c = Cipher(algorithms.AES(k1), modes.ECB())
    op = c.decryptor() if decrypt else c.encryptor()
    out = bytearray()
    for i in range(0, len(data), 16):
        mask = tw.to_bytes(16, "little")
        blk = bytes(x ^ y for x, y in zip(data[i:i + 16], mask))
        blk = op.update(blk)
        out += bytes(x ^ y for x, y in zip(blk, mask))
        tw <<= 1
        if tw >> 128:
            tw = (tw & ((1 << 128) - 1)) ^ 0x87
    return bytes(out)


def luks_encrypt_inner(ctx, pp, dev, inner, state):
    key, off = state["luks_key"], state["luks_off"]
    total = os.path.getsize(inner)
    t0 = time.time()
    with open(inner, "rb") as src, open(dev, "r+b") as dst:
        pos = 0
        while pos < total:
            blk = src.read(MiB)
            dst.seek(off + pos)
            dst.write(xts_crypt_buffer(key, blk, pos // LUKS_SECTOR))
            pos += len(blk)
        dst.flush()
        os.fsync(dst.fileno())
    ctx.run.say("    (XTS 暗号化: %s を %.0f 秒)" % (human(total), time.time() - t0))
    # 独立実装 (AES-ECB による手組みの XTS) で数セクタを照合する (暗号化の向き・IV・鍵の分割の取り違えを検出)
    sectors = sorted({0, 1, total // LUKS_SECTOR // 2, total // LUKS_SECTOR - 1} | set(range(8, 16)))
    checked = 0
    with open(inner, "rb") as src, open(dev, "rb") as dst:
        for sn in sectors:
            src.seek(sn * LUKS_SECTOR)
            plain = src.read(LUKS_SECTOR)
            dst.seek(off + sn * LUKS_SECTOR)
            cipher = dst.read(LUKS_SECTOR)
            ref = reference_xts_sector(key, sn, plain)
            checked += 1
            if ref != cipher:
                raise StepError("LUKS の暗号化が独立実装の結果と一致しない (セクタ %d)" % sn)
    ctx.run.say("    (独立実装の XTS との照合: %d セクタ一致)" % checked)


def build_partition(ctx, pp, dev):
    """1 パーティションを作る。(状態辞書) を返す。失敗は StepError。"""
    R = ctx.run
    opts = ctx.opts
    spec = pp.spec
    caps = caps_for(spec, pp.size, opts.no_big, opts.small_count, opts.udf_method == "genisoimage")
    stage = os.path.join(ctx.work, "stage_" + pp.name)
    out = os.path.join(ctx.work, "out_" + pp.name + ".img")
    state = dict(caps=caps, stage=stage, notes=[], tmpfiles=[], populated=True, ds=None, mnt=None, checks=[])
    plan = fs_plan(pp, dev, stage, out, opts)
    R.say("  [%s] %s (%s) を作る" % (pp.name, FS_DISPLAY[pp.fs], human(pp.size)))
    try:
        for cmd in plan:
            if cmd.kind == "special":
                run_special(ctx, pp, dev, cmd, state)
            elif cmd.kind == "check":
                rc, out_txt = R.run(cmd, check=False)
                if cmd.argv[0] == "dumpe2fs":
                    ok = "needs_recovery" in out_txt
                elif cmd.argv[0] == "ntfsinfo":
                    ok = "scheduled for check" in out_txt
                elif cmd.argv[0] == "cryptsetup":
                    ok = rc == 0
                else:
                    ok = rc == 0
                state["checks"].append((cmd.argv[0], ok))
                if not ok:
                    R.say("    !! 検査 %s が失敗 (rc=%d): %s" % (cmd.argv[0], rc, out_txt.strip()[-300:]))
            else:
                R.run(cmd)
    finally:
        if state.get("mnt"):
            try:
                umount(ctx, state)
            except StepError:
                pass
        for d in os.listdir(ctx.work) if os.path.isdir(ctx.work) else []:
            if d.startswith(WORK_MNT_PREFIX) or d.startswith("usbmem_verify_"):
                try:
                    os.rmdir(os.path.join(ctx.work, d))        # 空のマウントポイントだけ消える (中身があれば消さない)
                except OSError:
                    pass
        shutil.rmtree(stage, ignore_errors=True)
        for t in state["tmpfiles"] + [out, out + ".plain", out + ".meta", out + ".key"]:
            try:
                os.unlink(t)
            except OSError:
                pass
    if state["ds"] is not None:
        os.makedirs(ctx.manifest_dir, exist_ok=True)
        meta = dict(profile=pp.profile, part=pp.name, fs=pp.fs, label=pp.label)
        write_manifest(os.path.join(ctx.manifest_dir, pp.name + ".sha256"), state["ds"].entries, meta)
    return state


def splice(src, dst, offset):
    """疎ファイル src (パーティションの一時イメージ) を dst の offset へ、穴を飛ばしてコピー。"""
    sfd = os.open(src, os.O_RDONLY)
    with open(dst, "r+b") as d:
        try:
            pos = 0
            size = os.fstat(sfd).st_size
            while pos < size:
                try:
                    data = os.lseek(sfd, pos, os.SEEK_DATA)
                except OSError:
                    break           # これより後ろは穴
                hole = os.lseek(sfd, data, os.SEEK_HOLE)
                os.lseek(sfd, data, os.SEEK_SET)
                d.seek(offset + data)
                left = hole - data
                while left > 0:
                    b = os.read(sfd, min(left, 4 * MiB))
                    if not b:
                        break
                    if b.count(0) != len(b):
                        d.write(b)
                    else:
                        d.seek(len(b), os.SEEK_CUR)
                    left -= len(b)
                pos = hole
        finally:
            os.close(sfd)
        d.flush()


# ----------------------------------------------------------------------------------------------------------------------
# イメージ一式 (パーティション表のイメージ + パーティションごとの疎イメージ + メタデータ JSON) と、その流し込み範囲
# ----------------------------------------------------------------------------------------------------------------------


def script_sha256():
    try:
        with open(os.path.abspath(__file__), "rb") as f:
            return hashlib.sha256(f.read()).hexdigest()
    except OSError:
        return ""


def table_regions(layout):
    """パーティション表のイメージ (TABLE_IMG) のうち、デバイスへ書く領域 [(offset, length)]。
    先頭 (PMBR と GPT 本体) / MBR の拡張パーティション内の各論理パーティションの直前 1 MiB (EBR) / 末尾 (GPT のバックアップ)。
    それ以外 (パーティションの中身と未割り当て) には含まれない。"""
    regs = [(0, LEAD)]
    for pp in layout.parts:
        if pp.spec.kind == "logical":
            regs.append((pp.start - ALIGN, ALIGN))
    regs.append((layout.dev_size - TAIL_RESERVE, TAIL_RESERVE))
    return regs


@dataclasses.dataclass
class FlashItem:
    name: str
    src: str
    src_off: int
    dst_off: int
    length: int
    sha256: str = ""
    kind: str = "part"          # part=パーティションのイメージ全体 / table=パーティション表の領域

    @property
    def dst_end(self):
        return self.dst_off + self.length


def flash_items(layout, imgdir, meta=None):
    """流し込む範囲の一覧。パーティション (番号順) を先に、パーティション表の領域を最後 (論理の EBR → 末尾 → 先頭の順) にする。
    表が最後なので、途中で止まったデバイスは「有効なパーティション表が無い」状態になり、中途半端な表が残らない。"""
    hashes = {}
    thashes = {}
    if meta:
        hashes = {p["name"]: p.get("sha256", "") for p in meta.get("parts", [])}
        thashes = {r["offset"]: r.get("sha256", "") for r in (meta.get("table_image") or {}).get("regions", [])}
    items = []
    for pp in layout.parts:
        items.append(FlashItem(pp.name, os.path.join(imgdir, pp.name + ".img"), 0, pp.start, pp.size, hashes.get(pp.name, ""), "part"))
    regs = table_regions(layout)
    tbl = os.path.join(imgdir, TABLE_IMG)
    mid = [r for r in regs[1:-1]]
    for off, ln in mid + [regs[-1], regs[0]]:
        items.append(FlashItem("table@" + human(off).replace(" ", ""), tbl, off, off, ln, thashes.get(off, ""), "table"))
    return items


def unallocated_ranges(layout):
    """流し込みでも消去でも触らない範囲 (未割り当て)。"""
    cover = sorted((it.dst_off, it.dst_end) for it in flash_items(layout, "."))
    out, pos = [], 0
    for a, b in cover:
        if a > pos:
            out.append((pos, a - pos))
        pos = max(pos, b)
    if pos < layout.dev_size:
        out.append((pos, layout.dev_size - pos))
    return out


def hash_sparse_file(path, progress=None):
    """疎ファイルの全内容の SHA-256 (穴はゼロとして計算。穴の読み込みはしない)。"""
    size = os.path.getsize(path)
    h = hashlib.sha256()
    zero = bytes(IO_BLOCK)

    def feed_zero(n):
        while n > 0:
            k = min(n, len(zero))
            h.update(memoryview(zero)[:k])
            n -= k

    fd = os.open(path, os.O_RDONLY)
    try:
        pos = 0
        while pos < size:
            try:
                data = os.lseek(fd, pos, os.SEEK_DATA)
            except OSError:
                data = size
            feed_zero(min(data, size) - pos)
            if data >= size:
                break
            hole = min(os.lseek(fd, data, os.SEEK_HOLE), size)
            os.lseek(fd, data, os.SEEK_SET)
            left = hole - data
            while left > 0:
                b = os.read(fd, min(left, IO_BLOCK))
                if not b:
                    feed_zero(left)
                    break
                h.update(b)
                left -= len(b)
            pos = hole
            if progress:
                progress(pos, size)
    finally:
        os.close(fd)
    return h.hexdigest()


def read_range_hash(path, off, length, direct=False, progress=None):
    """path の [off, off+length) の SHA-256。direct=True なら O_DIRECT (ページキャッシュを通さず、デバイスから実際に読む)。
    O_DIRECT が使えない (tmpfs など) ときは通常の読み出しにする。"""
    flags = os.O_RDONLY
    fd = None
    if direct and hasattr(os, "O_DIRECT"):
        try:
            fd = os.open(path, flags | os.O_DIRECT)
        except OSError:
            fd = None
    used_direct = fd is not None
    if fd is None:
        fd = os.open(path, flags)
    buf = mmap.mmap(-1, IO_BLOCK)          # ページ境界に揃ったバッファ (O_DIRECT 用)
    h = hashlib.sha256()
    try:
        pos, left = off, length
        while left > 0:
            n = min(left, IO_BLOCK)
            try:
                got = os.preadv(fd, [memoryview(buf)[:n]], pos)
            except OSError:
                if not used_direct:
                    raise
                os.close(fd)                       # O_DIRECT が通らなければ通常の読み出しに切り替える
                fd = os.open(path, flags)
                used_direct = False
                got = os.preadv(fd, [memoryview(buf)[:n]], pos)
            if got <= 0:
                raise StepError("%s の %d バイト目から先を読めない (短い)" % (path, pos))
            h.update(memoryview(buf)[:got])
            pos += got
            left -= got
            if progress:
                progress(pos - off, length)
    finally:
        os.close(fd)
        buf.close()
    return h.hexdigest()


def check_table_regions(path, layout):
    """TABLE_IMG の非ゼロの部分が table_regions の中だけに収まっていること (外にあると流し込みで落ちてしまう)。問題のリストを返す。"""
    regs = table_regions(layout)
    problems = []
    size = os.path.getsize(path)
    fd = os.open(path, os.O_RDONLY)
    try:
        pos = 0
        while pos < size:
            try:
                data = os.lseek(fd, pos, os.SEEK_DATA)
            except OSError:
                break
            hole = min(os.lseek(fd, data, os.SEEK_HOLE), size)
            p = data
            while p < hole:
                n = min(64 * KiB, hole - p)
                b = os.pread(fd, n, p)
                if b.count(0) != len(b):
                    if not any(a <= p and p + len(b) <= a + ln for a, ln in regs):
                        problems.append("パーティション表の領域の外に非ゼロのデータがある (オフセット %d)" % p)
                        return problems
                p += n
            pos = hole
    finally:
        os.close(fd)
    return problems


def layout_summary(layout):
    return dict(
        parts=[dict(index=pp.index, name=pp.name, fs=pp.fs, kind=pp.spec.kind, start=pp.start, size=pp.size) for pp in layout.parts],
        extended=list(layout.extended) if layout.extended else None, unallocated_tail=layout.unallocated_tail, used_end=layout.used_end)


def make_meta(layout, opts, parts_meta, table_hashes, complete, extra_notes=()):
    prof = layout.profile
    od = dataclasses.asdict(opts)
    return {
        "format": IMAGESET_FORMAT,
        "script": "tools/make_real_usb_testset.py",
        "script_sha256": script_sha256(),
        "created": datetime.datetime.now().isoformat(timespec="seconds"),
        "profile": prof.name, "table": prof.table, "expected": prof.expected,
        "dev_size": layout.dev_size, "ss": layout.ss,
        "options": od,
        "layout": layout_summary(layout),
        "table_image": {"file": TABLE_IMG, "regions": [dict(offset=o, length=n, sha256=table_hashes.get(o, "")) for o, n in table_regions(layout)]},
        "parts": [parts_meta[k] for k in sorted(parts_meta, key=lambda k: parts_meta[k]["index"])],
        "complete": bool(complete),
        "notes": list(extra_notes),
    }


def write_meta(imgdir, meta):
    tmp = os.path.join(imgdir, META_NAME + ".tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(meta, f, ensure_ascii=False, indent=1)
        f.write("\n")
    os.replace(tmp, os.path.join(imgdir, META_NAME))


def load_meta(imgdir):
    path = os.path.join(imgdir, META_NAME)
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except OSError as e:
        raise StepError("%s を読めない (--keep-images で作ったディレクトリではない?): %s" % (path, e.strerror))
    except ValueError as e:
        raise StepError("%s が壊れている: %s" % (path, e))


def meta_problems(meta, layout, imgdir=None):
    """イメージ一式 (meta) を、このデバイスの配置 (layout) に流してよいかの検査。拒否理由のリスト。"""
    why = []
    if meta.get("format") != IMAGESET_FORMAT:
        why.append("イメージ一式の形式が違う (イメージ %s / このスクリプト %s)。作り直すこと" % (meta.get("format"), IMAGESET_FORMAT))
        return why
    if meta.get("profile") != layout.profile.name:
        why.append("プロファイルが違う (イメージ %s / 指定 %s)" % (meta.get("profile"), layout.profile.name))
    if meta.get("dev_size") != layout.dev_size:
        why.append("デバイス容量が違う: イメージは %s バイト (%s) 用、このデバイスは %s バイト (%s)。レイアウトは容量で決まるので、同じ容量のデバイスを使うか、このデバイスで作り直すこと"
                   % (meta.get("dev_size"), human(meta.get("dev_size") or 0), layout.dev_size, human(layout.dev_size)))
    if meta.get("ss") != layout.ss:
        why.append("論理セクタが違う (イメージ %s / デバイス %s)" % (meta.get("ss"), layout.ss))
    want = layout_summary(layout)["parts"]
    have = (meta.get("layout") or {}).get("parts")
    if not why and have != want:
        why.append("イメージのパーティション配置が、このスクリプトが計算する配置と違う (スクリプトの版が違う?)。作り直すこと")
    if not meta.get("complete"):
        built = {p["name"] for p in meta.get("parts", [])}
        miss = [pp.name for pp in layout.parts if pp.name not in built]
        why.append("イメージ一式が不完全 (未作成: %s)" % ", ".join(miss))
    if imgdir and not why:
        for pp in layout.parts:
            f = os.path.join(imgdir, pp.name + ".img")
            if not os.path.exists(f):
                why.append("%s が無い" % f)
            elif os.path.getsize(f) != pp.size:
                why.append("%s のサイズが違う (%d != %d)" % (f, os.path.getsize(f), pp.size))
        t = os.path.join(imgdir, TABLE_IMG)
        if not os.path.exists(t) or os.path.getsize(t) != layout.dev_size:
            why.append("%s が無い、またはサイズが違う" % t)
    return why


def check_image_hashes(runner, imgdir, meta):
    """イメージが作った時から変わっていないことを SHA-256 で確かめる (流し込む前。デバイスにはまだ触らない)。問題のリスト。"""
    bad = []
    for p in meta.get("parts", []):
        f = os.path.join(imgdir, p["file"])
        if not p.get("sha256"):
            continue
        t0 = time.time()
        sha = hash_sparse_file(f)
        runner.say("  イメージ確認 %-14s %s (%.0f 秒)" % (p["name"], "一致" if sha == p["sha256"] else "不一致", time.time() - t0))
        if sha != p["sha256"]:
            bad.append("%s が作成時から変わっている" % f)
    for r in (meta.get("table_image") or {}).get("regions", []):
        if r.get("sha256"):
            sha = read_range_hash(os.path.join(imgdir, TABLE_IMG), r["offset"], r["length"])
            if sha != r["sha256"]:
                bad.append("%s の領域 %d が作成時から変わっている" % (TABLE_IMG, r["offset"]))
    return bad


def create_table_image(runner, layout, path):
    """デバイスと同じ大きさの疎ファイルに、パーティション表だけを作る (sgdisk / sfdisk はこのローカルのファイルにだけ触る)。"""
    if os.path.exists(path):
        os.unlink(path)
    with open(path, "wb") as f:
        f.truncate(layout.dev_size)
    for c in table_cmds(layout, path):
        runner.run(c)
    fsync_path(path)
    bad = check_table_regions(path, layout)
    if bad:
        raise StepError("; ".join(bad))


def build_images(ctx, layout, imgdir, only=None):
    """(a) デバイスに触らずに、パーティション表と全パーティションのイメージを imgdir に作る。(結果リスト, メタデータ) を返す。失敗は StepError。
    only (パーティション名の集合) を指定すると、既存のイメージ一式 (同じプロファイル・容量) のそのパーティションだけ作り直す。"""
    R = ctx.run
    os.makedirs(imgdir, exist_ok=True)
    old = None
    table = os.path.join(imgdir, TABLE_IMG)
    if only and os.path.exists(os.path.join(imgdir, META_NAME)):
        old = load_meta(imgdir)
        probs = [w for w in meta_problems(old, layout) if "不完全" not in w]
        if probs:
            raise StepError("既存のイメージ一式に追記できない: " + "; ".join(probs))
    parts_meta = {p["name"]: p for p in old["parts"]} if old else {}
    t0 = time.time()
    if old is None or not os.path.exists(table):
        R.say("パーティション表のイメージ %s (%s, 疎ファイル) を作る" % (table, human(layout.dev_size)))
        create_table_image(R, layout, table)
    results = []
    for pp in layout.parts:
        if only and pp.name not in only:
            continue
        img = os.path.join(imgdir, pp.name + ".img")
        if os.path.exists(img):
            os.unlink(img)
        with open(img, "wb") as f:
            f.truncate(pp.size)
        tp = time.time()
        st = build_partition(ctx, pp, img)
        fsync_path(img)
        sha = hash_sparse_file(img)
        used = os.stat(img).st_blocks * 512
        R.say("    イメージ %s: 使用量 %s / %s・SHA-256 %s... (%.0f 秒)" % (os.path.basename(img), human(used), human(pp.size), sha[:16], time.time() - tp))
        parts_meta[pp.name] = dict(index=pp.index, name=pp.name, fs=pp.fs, start=pp.start, size=pp.size, file=pp.name + ".img", sha256=sha,
                                   populated=st["ds"] is not None, notes=list(st["notes"]), checks=[[n, bool(ok)] for n, ok in st["checks"]])
        mdir = os.path.join(imgdir, "manifests")
        os.makedirs(mdir, exist_ok=True)
        for fn in (pp.name + ".sha256", pp.name + ".bitlocker-meta.txt"):
            src = os.path.join(ctx.manifest_dir, fn)
            if os.path.exists(src):
                shutil.copyfile(src, os.path.join(mdir, fn))
        results.append((pp, st))
    thash = {}
    for off, ln in table_regions(layout):
        thash[off] = read_range_hash(table, off, ln)
    complete = all(pp.name in parts_meta for pp in layout.parts)
    meta = make_meta(layout, ctx.opts, parts_meta, thash, complete)
    write_meta(imgdir, meta)
    R.say("イメージ一式のメタデータ: %s (%.0f 秒)" % (os.path.join(imgdir, META_NAME), time.time() - t0))
    return results, meta


# ----------------------------------------------------------------------------------------------------------------------
# 流し込み (d) と読み戻し検証 (e)
# ----------------------------------------------------------------------------------------------------------------------


def nbd_range_spec(path, off, length, readonly=False, direct=False):
    """nbdcopy の端点に渡す [ nbdkit ... ] (offset フィルタで path の一部だけを NBD として見せる)。argv の別々の要素として返す
    ([ ] も別要素。1 つの文字列にすると nbdcopy はそれをファイル名とみなしてしまう)。"""
    a = ["[", "nbdkit", "--exit-with-parent"]
    if readonly:
        a.append("-r")
    a += ["--filter=offset", "file", path]
    if direct:
        a.append("cache=none")                  # O_DIRECT: ページキャッシュを使わない (メモリを食わず、書いた分が本当にデバイスへ行く)
    a += ["offset=%d" % off, "range=%d" % length, "]"]
    return a


def flash_cmd(item, target, writer, direct=False, dest_zero=False, progress=False):
    """item を target (デバイスまたはファイル) の dst_off へ書くコマンド。dest_zero=True なら、書き先がゼロと分かっているのでゼロの塊は書かない。
    False (既定) ならゼロも含めて全部書く (未使用領域に古いデータを残さないため)。"""
    if writer == "nbdcopy":
        argv = ["nbdcopy", "--flush", "-C", "1", "-T", "1", "-R", "4", "--request-size=%d" % IO_BLOCK]
        if progress:
            argv.append("--progress")
        if dest_zero:
            argv.append("--destination-is-zero")
        else:
            argv += ["--sparse=0", "--no-extents"]
        argv.append("--")
        if item.src_off == 0 and item.kind == "part":
            argv.append(item.src)
        else:
            argv += nbd_range_spec(item.src, item.src_off, item.length, readonly=True)
        argv += nbd_range_spec(target, item.dst_off, item.length, direct=direct)
        return Cmd(argv, note="%s -> %s +%d" % (item.name, target, item.dst_off))
    argv = ["dd", "if=" + item.src, "of=" + target, "bs=4M", "iflag=skip_bytes,count_bytes", "skip=%d" % item.src_off, "count=%d" % item.length,
            "seek=%d" % item.dst_off, "oflag=seek_bytes" + (",direct" if direct else ""), "conv=notrunc,fsync" + (",sparse" if dest_zero else ""),
            "status=progress" if progress else "status=none"]
    return Cmd(argv, note="%s -> %s +%d" % (item.name, target, item.dst_off))


def prezero_cmds(item, target):
    """--sparse-write 用: 書き込み前にその範囲をゼロにする (blkdiscard -z が使えなければ dd)。"""
    return [Cmd(["blkdiscard", "-z", "-o", str(item.dst_off), "-l", str(item.length), target], kind="check"),
            Cmd(["dd", "if=/dev/zero", "of=" + target, "bs=4M", "iflag=count_bytes", "count=%d" % item.length, "seek=%d" % item.dst_off,
                 "oflag=seek_bytes,direct", "conv=notrunc,fsync", "status=none"])]


def erase_cmds(dev, dev_size):
    """バックアップ後の消去: 既存の表の署名と、先頭 4 MiB・末尾 1 MiB をゼロにする (デバイス 1 本だけを fsync)。"""
    return [Cmd(["wipefs", "-a", "-f", dev]),
            Cmd(["dd", "if=/dev/zero", "of=" + dev, "bs=1M", "count=4", "conv=fsync", "status=none"]),
            Cmd(["dd", "if=/dev/zero", "of=" + dev, "bs=1M", "seek=%d" % max(dev_size - MiB, 0), "oflag=seek_bytes", "count=1", "conv=fsync", "status=none"])]


def choose_writer(want, probe=True):
    """probe=False (dry-run) では nbdkit を実行して確かめず、コマンドの有無だけで決める。"""
    if want in ("nbdcopy", "dd"):
        return want
    if shutil.which("nbdcopy") and shutil.which("nbdkit"):
        if not probe:
            return "nbdcopy"
        r = subprocess.run(["nbdkit", "--filter=offset", "file", "--dump-plugin"], capture_output=True)
        if r.returncode == 0:
            return "nbdcopy"
    return "dd"


def flash_all(runner, layout, items, target, is_device, writer, sparse_write=False, progress=False):
    """(d) items を target へ順に流し込む。デバイスには書くだけ (全デバイス対象の sync はしない。最後にそのデバイスだけ flush)。"""
    total = sum(it.length for it in items)
    done = 0
    t_all = time.time()
    if not is_device:
        if os.path.exists(target):
            os.unlink(target)
        with open(target, "wb") as f:
            f.truncate(layout.dev_size)
    dest_zero = (not is_device) or sparse_write         # 新しく作ったファイルはゼロ。実デバイスは --sparse-write のときだけ (事前にゼロ化する)
    for i, it in enumerate(items, 1):
        t0 = time.time()
        runner.say("  [%d/%d] %s: %s -> %s の %s から (全体の %.0f%%)" % (i, len(items), it.name, human(it.length), target, human(it.dst_off), 100.0 * done / max(total, 1)))
        if is_device and sparse_write and it.kind == "part":
            z = prezero_cmds(it, target)
            rc, _ = runner.run(z[0], check=False)
            if rc != 0:
                runner.run(z[1])
        runner.run(flash_cmd(it, target, writer, direct=is_device, dest_zero=dest_zero, progress=progress), live=progress)
        dt = max(time.time() - t0, 1e-6)
        done += it.length
        runner.say("      完了 %.1f 秒 (%.1f MiB/s)" % (dt, it.length / MiB / dt))
    if is_device:
        runner.run(Cmd(["blockdev", "--flushbufs", target]))     # このデバイスだけ書き出してキャッシュを捨てる
    else:
        fsync_path(target)
    runner.say("流し込み完了: %s を %.0f 秒 (平均 %.1f MiB/s)" % (human(total), time.time() - t_all, total / MiB / max(time.time() - t_all, 1e-6)))


def verify_flashed(runner, items, target, is_device):
    """(e) 流し込んだ範囲を読み戻して SHA-256 をイメージのものと比べる。(不一致のリスト) を返す。"""
    if is_device:
        runner.run(Cmd(["blockdev", "--flushbufs", target]), check=False)      # キャッシュを捨てて、デバイスから実際に読む
    bad = []
    total = sum(it.length for it in items)
    t_all = time.time()
    for i, it in enumerate(items, 1):
        t0 = time.time()
        want = it.sha256 or read_range_hash(it.src, it.src_off, it.length)
        got = read_range_hash(target, it.dst_off, it.length, direct=is_device)
        ok = got == want
        dt = max(time.time() - t0, 1e-6)
        runner.say("  読み戻し [%d/%d] %-14s %s  %s... (%.1f MiB/s)" % (i, len(items), it.name, "一致" if ok else "★不一致★", got[:16], it.length / MiB / dt))
        if not ok:
            bad.append("%s (オフセット %d・%d バイト) の読み戻しがイメージと一致しない" % (it.name, it.dst_off, it.length))
    runner.say("読み戻し検証: %s を %.0f 秒%s" % (human(total), time.time() - t_all, "" if is_device else " (ファイルなのでページキャッシュ経由)"))
    return bad


# ----------------------------------------------------------------------------------------------------------------------
# メイン処理
# ----------------------------------------------------------------------------------------------------------------------


def current_mounts():
    with open("/proc/mounts", encoding="utf-8", errors="replace") as f:
        m = parse_mounts(f.read())
    try:
        with open("/proc/swaps", encoding="utf-8", errors="replace") as f:
            s = parse_swaps(f.read())
    except OSError:
        s = []
    return m, s


def is_block_device(path):
    try:
        return stat.S_ISBLK(os.stat(path).st_mode)
    except OSError:
        return False


def device_node(tree, dev):
    real = os.path.realpath(dev)
    for n in flatten(tree):
        if n.get("path") in (dev, real):
            return n
    return None


def free_space_check(path, need):
    os.makedirs(path, exist_ok=True)
    return shutil.disk_usage(path).free >= need


def workdir_is_ram(mi, path, depth=0):
    """path がメモリ上 (tmpfs / ramfs、または upperdir が tmpfs の overlay = ライブ環境のルート) にあるか。"""
    if depth > 4:
        return False
    e = _mount_entry_for(mi, path)
    if e is None:
        return False
    if e["fstype"] in ("tmpfs", "ramfs"):
        return True
    if e["fstype"] == "overlay":
        for kv in e["opts"].split(","):
            if kv.startswith("upperdir=") and workdir_is_ram(mi, kv.split("=", 1)[1], depth + 1):
                return True
    return False


def mem_available():
    try:
        for ln in open("/proc/meminfo"):
            if ln.startswith("MemAvailable:"):
                return int(ln.split()[1]) * 1024
    except OSError:
        pass
    return None


def acquire_single_instance_lock():
    """同時に 2 つ以上動かさない (ハブの帯域と、全デバイス対象の sync で互いに詰まるため)。ロックを保持するファイルオブジェクトを返す。"""
    import fcntl
    f = open(os.path.join(tempfile.gettempdir(), "usbmem_testset.lock"), "w")
    try:
        fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        f.close()
        return None
    return f


def estimate_work_need(layout, opts):
    need = 0
    for pp in layout.parts:
        c = caps_for(pp.spec, pp.size, opts.no_big, opts.small_count)
        n = 300 * MiB + (GiB if c.gib else 0) + (4 * GiB + 512 * MiB if c.big else 0)
        if pp.fs in ("squashfs", "erofs", "iso", "bitlocker", "udf"):
            n += pp.size
        need = max(need, n)
    return need


def layout_table(layout, opts=None):
    p = layout.profile
    lines = []
    lines.append("  %-3s %-14s %-12s %12s %12s %12s  %s" % ("#", "名前", "FS", "開始", "サイズ", "終端", "データ"))
    for pp in layout.parts:
        caps = caps_for(pp.spec, pp.size)
        extra = []
        if caps.gib:
            extra.append("1GiB")
        if caps.big:
            extra.append("4.5GiB")
        if pp.spec.needs_recovery:
            extra.append("needs_recovery")
        if pp.spec.dirty_opt:
            extra.append("--dirty-ntfs可")
        if opts is not None and root_needed(pp.fs, opts):
            extra.append("投入にroot要")
        lines.append("  %-3d %-14s %-12s %12s %12s %12s  %s%s" % (
            pp.index, pp.name, FS_DISPLAY[pp.fs], human(pp.start), human(pp.size), human(pp.end),
            ("拡張内の論理 " if pp.spec.kind == "logical" else "") + (" ".join(extra)), ("  # " + pp.spec.desc) if pp.spec.desc else ""))
    if layout.extended:
        i, s, sz = layout.extended
        lines.append("  %-3d %-14s %-12s %12s %12s %12s  (拡張パーティション。論理は 1 MiB 空けて並べる)" % (i, "extended", "-", human(s), human(sz), human(s + sz)))
    return lines


ASSUMED_SPEEDS_MIBS = (15, 30, 60)       # 流し込み時間の目安に使う USB フラッシュの書き込み速度 (MiB/s)


def print_flash_plan(layout, writer, out=print, imgdir="<IMAGES>"):
    items = flash_items(layout, imgdir)
    total = sum(it.length for it in items)
    out("流し込み計画 (d)。デバイスへは (c) のバックアップ後に**一度だけ・順次**書く。ゼロの領域も含めて全部書く (--sparse-write を除く):")
    out("  書き込み方式: %s%s" % (writer, " (NBD 経由。nbdkit の offset フィルタで書き先の範囲を切り出し、O_DIRECT で書いて flush)" if writer == "nbdcopy" else " (dd bs=4M oflag=direct conv=fsync)"))
    out("  消去: 先頭 4 MiB と末尾 1 MiB をゼロにする (wipefs と dd。そのデバイスだけ fsync)")
    for i, it in enumerate(items, 1):
        out("  [%2d] %-14s デバイスの %12s から %12s  <- %s%s" % (i, it.name, human(it.dst_off), human(it.length), os.path.basename(it.src),
                                                              (" +%d" % it.src_off) if it.src_off else ""))
    for off, ln in unallocated_ranges(layout):
        out("  (触らない) 未割り当て %12s から %12s" % (human(off), human(ln)))
    est = ", ".join("%d MiB/s なら約 %.0f 分" % (v, total / MiB / v / 60) for v in ASSUMED_SPEEDS_MIBS)
    out("  書き込み合計 %s (%s)。読み戻し検証で同じ量をもう一度読む。" % (human(total), est))
    out("  最後に blockdev --flushbufs (そのデバイスだけ)。全デバイス対象の sync は使わない。")


def print_plan(layout, opts, dev_label, out=print, writer="nbdcopy"):
    p = layout.profile
    out("=" * 100)
    out("プロファイル %s: %s テーブル、期待容量 %s、%s" % (p.name, p.table.upper(), human(p.expected), p.note))
    out("対象: %s   容量: %s (%d バイト)   論理セクタ: %d" % (dev_label, human(layout.dev_size), layout.dev_size, layout.ss))
    out("")
    for ln in layout_table(layout, opts):
        out(ln)
    total = sum(pp.size for pp in layout.parts)
    out("")
    out("  先頭の空き %s / パーティション合計 %s / 末尾までの未割り当て %s (うち末尾予約 %s)" % (human(LEAD), human(total), human(layout.unallocated_tail), human(TAIL_RESERVE)))
    out("  整列: 全パーティションの開始・サイズは 1 MiB の倍数 -> %s" % ("OK" if all(pp.start % ALIGN == 0 and pp.size % ALIGN == 0 for pp in layout.parts) else "NG"))
    out("")
    out("方式: イメージ経由 (build-then-flash)。(a) デバイスに触らず、ローカルディスクの疎ファイルに全部作る → (b) 確認 → (c) バックアップ → (d) 流し込み → (e) 読み戻し検証。")
    out("")
    out("(a-1) パーティション表のイメージ (デバイスと同じ大きさの疎ファイル <IMAGES>/%s) に表を作るコマンド:" % TABLE_IMG)
    for c in table_cmds(layout, "<IMAGES>/" + TABLE_IMG):
        out("  $ " + c.text())
        if c.stdin:
            for ln in c.stdin.splitlines():
                out("      | " + ln)
    out("")
    out("(a-2) 各パーティションのイメージの作成計画 (<PART>=<IMAGES>/<名前>.img の疎ファイル、<STAGE>=投入ツリーの一時置き場):")
    for pp in layout.parts:
        out("  [%s] %s" % (pp.name, FS_DISPLAY[pp.fs]))
        for c in fs_plan(pp, "<PART>", "<STAGE>", "<WORK>/out.img", opts):
            if c.kind == "special":
                out("      * " + " ".join(c.argv))
            else:
                out("      $ %s%s" % (c.text(), "   (検査。失敗は記録のみ)" if c.kind == "check" else ""))
    out("")
    print_flash_plan(layout, writer, out)
    out("")


def build_parser():
    ap = argparse.ArgumentParser(
        description="実機 USB メモリ検証用のパーティション構成とテストデータを作る。既定は dry-run。実行は --execute --profile --device が必要。"
                    "イメージ経由: ローカルの疎イメージに全部作ってから、デバイスへ一度だけ順次書く。",
        epilog="サブコマンド: make_real_usb_testset.py verify MANIFEST ROOT [--no-links] [--allow-extra] (マニフェスト照合)")
    ap.add_argument("--profile", choices=sorted(PROFILES), help="A=7.8G(GPT) B=16G(MBR) C=31G(GPT)。dry-run で省略すると全部表示。--flash-only ならイメージのものを使う")
    ap.add_argument("--device", help="対象 USB メモリ (例 /dev/sdb)。ディスク全体。")
    ap.add_argument("--image", help="イメージモード: 実デバイスの代わりにこのファイルへ流し込む (デバイス検証は行わない。root 不要)")
    ap.add_argument("--overwrite-image", action="store_true", help="--image の既存ファイルを作り直す")
    ap.add_argument("--image-size", type=int, help="イメージのサイズ (バイト)。既定はプロファイルの期待容量")
    ap.add_argument("--device-size", type=int, help="dry-run だけ: デバイス容量を仮定する (バイト)")
    ap.add_argument("--execute", action="store_true", help="実際に作る・書き込む (これが無ければ dry-run)")
    ap.add_argument("--keep-images", metavar="DIR", help="作ったイメージ一式 (パーティションごとの疎イメージ + 表 + imageset.json) を DIR に残す。後で --flash-only DIR で流し込める。"
                                                         "指定しなければ --work-dir の下に作り、成功したら消す")
    ap.add_argument("--flash-only", metavar="DIR", help="イメージを作り直さず、--keep-images で作った DIR を流し込むだけ。デバイス容量・プロファイルが違うと拒否する")
    ap.add_argument("--build-only", action="store_true", help="イメージ一式を作るだけで終わる (--keep-images DIR が必要。デバイスには書かない。root 不要。"
                                                              "--device を付けるとそのデバイスの容量で配置する (検証だけ行い、書かない))")
    ap.add_argument("--overwrite-images", action="store_true", help="--keep-images DIR に既にあるイメージ一式を作り直す")
    ap.add_argument("--writer", choices=("auto", "nbdcopy", "dd"), default="auto", help="流し込みの方法。auto は nbdcopy + nbdkit が使えれば nbdcopy、無ければ dd")
    ap.add_argument("--sparse-write", action="store_true", help="ゼロの塊を書き飛ばす。**書き込み前に各パーティションの範囲を事前にゼロ化する** (blkdiscard -z、無ければ dd)。"
                                                                "ゼロ化しないと古いデータが FS の未使用領域に残って fsck 等の前提が崩れる。ゼロ化が速い機器以外は遅くなる")
    ap.add_argument("--progress", action="store_true", default=None, help="流し込みの進捗を表示する (端末なら既定で表示)")
    ap.add_argument("--no-progress", dest="progress", action="store_false", default=None)
    ap.add_argument("--skip-image-check", action="store_true", help="--flash-only で、流し込み前のイメージの SHA-256 再確認を省く (読み戻し検証は行う)")
    ap.add_argument("--ignore-image-checks", action="store_true", help="イメージ作成時の検査 (fsck 相当) が NG でも流し込む")
    ap.add_argument("--backup-dir", help="元のパーティション表・先頭 4MiB・末尾 1MiB の保存先 (既定 ./usbmem_testset_backup/<プロファイル>_<日時>)")
    ap.add_argument("--full-backup", action="store_true", help="デバイス全体のイメージも --backup-dir に保存する (空き容量が要る)")
    ap.add_argument("--manifest-dir", default="./usbmem_testset_manifests", help="マニフェストの保存先 (既定 ./usbmem_testset_manifests/<プロファイル>/<パーティション名>.sha256)")
    ap.add_argument("--allow-ram-workdir", action="store_true", help="作業ディレクトリ・イメージ置き場がメモリ上でも、空きメモリが足りなくても続行する")
    ap.add_argument("--work-dir", help="一時ファイル (投入ツリーなど) の置き場 (既定 ./usbmem_testset_work)。ローカルのディスク上に置くこと (tmpfs 不可)")
    ap.add_argument("--log", default="./usbmem_testset.log", help="ログファイル")
    ap.add_argument("--dirty-ntfs", action="store_true", help="プロファイル A の NTFS をダーティフラグ付きにする")
    ap.add_argument("--skip-mount-fs", action="store_true", help="root の mount / cryptsetup が要る方式の FS (exFAT mount、UDF mkudffs、LUKS open、F2FS mount) の中身投入を省く (FS 自体は作る)")
    ap.add_argument("--no-big-files", action="store_true", help="1 GiB と 4.5 GiB のファイルを作らない (所要時間の短縮用の試運転)")
    ap.add_argument("--small-count", type=int, help="小さいファイルの個数 (既定 3000、256〜512M の FS は 300)")
    ap.add_argument("--luks-pbkdf", choices=("argon2id", "pbkdf2"), default="argon2id", help="LUKS2 の KDF (既定 argon2id・メモリ 64 MiB)")
    ap.add_argument("--luks-method", choices=("auto", "open", "xts"), default="auto",
                    help="LUKS2 の中身の作り方。open=cryptsetup open + mke2fs (root)、xts=内側を平文で作って Python で XTS 暗号化 (root 不要)。auto は root なら open、そうでなければ xts")
    ap.add_argument("--exfat-method", choices=("auto", "mount", "fuse"), default="auto",
                    help="exFAT の投入方法。mount=カーネルの exFAT (root)、fuse=exfat-fuse (root 不要。cc が要る)。auto は root なら mount、そうでなければ fuse")
    ap.add_argument("--f2fs-method", choices=("sload", "mount"), default="sload", help="F2FS への投入方法 (sload.f2fs は root 不要)")
    ap.add_argument("--udf-method", choices=("mkudffs", "genisoimage"), default="mkudffs", help="UDF の作り方。genisoimage は mount 不要 (root 不要) だが ISO9660+UDF ブリッジ")
    ap.add_argument("--bitlocker", choices=("synth", "skip"), default="synth", help="C の BitLocker 風パーティション。skip なら空のまま")
    ap.add_argument("--iso-tool", choices=("genisoimage", "xorriso"), default=None, help="ISO を作るツール (既定は genisoimage、無ければ xorriso)")
    ap.add_argument("--verify-content", dest="verify_content", action="store_true", default=True, help="イメージを作った直後に中身を取り出してマニフェストと照合 (既定で有効)")
    ap.add_argument("--no-verify-content", dest="verify_content", action="store_false")
    ap.add_argument("--only", help="デバッグ用: このパーティション名 (カンマ区切り。例 p2_ext4) のイメージだけ作る。--keep-images の既存のイメージ一式があれば、そこへ作り直して差し替える "
                                   "(root が要るパーティションだけ sudo で作り直す、という使い方ができる)")
    return ap


def plan_for(profile, dev_size, ss):
    return compute_layout(profile, dev_size, ss)


def resolve_methods(args, euid):
    """auto の方式を決める: root なら従来どおり (kernel mount / cryptsetup open)、root でなければ root 不要の方式 (使えれば)。"""
    exfat = args.exfat_method
    if exfat == "auto":
        exfat = "mount"
        if euid != 0 and shutil.which("mount.exfat-fuse") and (shutil.which("cc") or shutil.which("gcc")):
            exfat = "fuse"
    luks = args.luks_method
    if luks == "auto":
        luks = "open"
        if euid != 0:
            try:
                import importlib.util
                if importlib.util.find_spec("cryptography") is not None:
                    luks = "xts"
            except (ImportError, ValueError):
                pass
    return exfat, luks


def make_opts(args, ss):
    iso_tool = args.iso_tool or ("genisoimage" if shutil.which("genisoimage") else "xorriso")
    exfat, luks = resolve_methods(args, os.geteuid())
    return Opts(skip_mount_fs=args.skip_mount_fs, dirty_ntfs=args.dirty_ntfs, no_big=args.no_big_files, small_count=args.small_count,
                luks_pbkdf=args.luks_pbkdf, f2fs_method=args.f2fs_method, udf_method=args.udf_method, bitlocker=args.bitlocker,
                verify_content=bool(args.verify_content), iso_tool=iso_tool, ss=ss, exfat_method=exfat, luks_method=luks)


def estimate_image_usage(pp, opts):
    """パーティション 1 つのイメージ (疎ファイル) の実使用量の目安。"""
    c = caps_for(pp.spec, pp.size, opts.no_big, opts.small_count)
    if pp.fs in ("luks", "bitlocker"):
        return pp.size                     # 暗号文はゼロも含めて全面が非ゼロ
    data = 24 * MiB + c.small_count * 4096 + (GiB if c.gib else 0) + (4 * GiB + 512 * MiB if c.big else 0) + (c.sparse_size if c.sparse else 0)
    over = {"ext2": pp.size // 40, "ext3": pp.size // 40 + 130 * MiB, "ext4": pp.size // 40 + 130 * MiB, "xfs": 200 * MiB, "btrfs": 700 * MiB,
            "f2fs": pp.size // 20, "ntfs": 80 * MiB, "exfat": 40 * MiB, "fat32": 40 * MiB}.get(pp.fs, 16 * MiB)
    return min(data + over, pp.size)


def estimate_images_need(layout, opts):
    return int(sum(estimate_image_usage(pp, opts) for pp in layout.parts) * 1.1) + 64 * MiB


def main_run(argv):
    ap = build_parser()
    args = ap.parse_args(argv)
    say = print
    flash_only = os.path.abspath(args.flash_only) if args.flash_only else None
    if args.image and args.device and not args.build_only:
        ap.error("--image と --device は同時に指定できない")
    if args.build_only:
        if not args.keep_images:
            ap.error("--build-only には --keep-images DIR が必要")
        if flash_only or args.image:
            ap.error("--build-only は --flash-only / --image と同時に使えない")
    if flash_only and args.keep_images:
        ap.error("--flash-only と --keep-images は同時に指定できない (--flash-only のディレクトリはそのまま残る)")
    if (args.execute or args.image or args.device) and not args.profile and not flash_only:
        ap.error("--execute / --device / --image には --profile {A,B,C} が必要")
    if args.execute and not (args.device or args.image or args.build_only):
        ap.error("--execute には --device /dev/sdX (実デバイス)、--image FILE、--build-only のどれかが必要")
    meta = None
    if flash_only:
        try:
            meta = load_meta(flash_only)
        except StepError as e:
            say("中止: %s" % e)
            return 2
        if meta.get("profile") not in PROFILES:
            say("中止: %s のプロファイル '%s' が不明" % (flash_only, meta.get("profile")))
            return 2
        if args.profile and args.profile != meta["profile"]:
            say("中止: --profile %s がイメージのプロファイル %s と違う" % (args.profile, meta["profile"]))
            return 2
        profiles = [PROFILES[meta["profile"]]]
    else:
        profiles = [PROFILES[args.profile]] if args.profile else [PROFILES[k] for k in sorted(PROFILES)]
    ss = 512
    opts = make_opts(args, ss)
    writer = choose_writer(args.writer, probe=bool(args.execute))

    # ---- dry-run ----
    if not args.execute:
        say("【dry-run】何も書き込みません。実行するには --execute --profile X --device /dev/sdX (または --image FILE / --build-only --keep-images DIR) を付けます。")
        for prof in profiles:
            dev_size = args.device_size or args.image_size or (meta or {}).get("dev_size") or prof.expected
            label = "(仮定のデバイス)"
            tree = None
            if args.device:
                try:
                    tree = lsblk_tree()
                    node = device_node(tree, args.device)
                    if node:
                        dev_size = int(node["size"])
                        ss = int(node.get("log-sec") or 512)
                        label = args.device
                        mounts, swaps = current_mounts()
                        why = validate_device(args.device, prof, tree, mounts, swaps, is_block=is_block_device(args.device))
                        for ln in device_summary(node):
                            say("  %s: %s" % ln)
                        say("  デバイス検証: %s" % ("OK" if not why else "NG (実行は拒否される)"))
                        for w in why:
                            say("    - " + w)
                except Exception as e:
                    say("  (デバイス情報を読めなかった: %s)" % e)
            try:
                layout = compute_layout(prof, dev_size, ss)
            except ValueError as e:
                say("プロファイル %s: 配置できない: %s" % (prof.name, e))
                return 2
            if flash_only:
                say("イメージ一式 %s: プロファイル %s・容量 %s・作成 %s・スクリプト版 %s" % (flash_only, meta.get("profile"), human(meta.get("dev_size") or 0), meta.get("created"), (meta.get("script_sha256") or "")[:12]))
                if meta.get("script_sha256") and meta["script_sha256"] != script_sha256():
                    say("  ※ このスクリプトは、イメージを作った版と違う (配置が同じなら流し込みには影響しない)")
                probs = meta_problems(meta, layout, flash_only)
                say("  イメージ一式の検査: %s" % ("OK" if not probs else "NG (実行は拒否される)"))
                for w in probs:
                    say("    - " + w)
                for ln in layout_table(layout):
                    say(ln)
                print_flash_plan(layout, writer, say, flash_only)
                continue
            print_plan(layout, make_opts(args, ss), label, writer=writer)
            miss = find_missing(required_tools(prof, prof.table, opts, "device" if args.device else "image", os.geteuid() == 0))
            miss += find_missing(flash_tools(writer)) if (args.device or args.image) else []
            if miss:
                say("  ※ この環境で足りないコマンド: " + ", ".join("%s (%s)" % m for m in miss))
            rn = root_needed_parts(prof, opts)
            if rn and not opts.skip_mount_fs:
                say("  ※ 次のパーティションの投入は root が要る方式: %s (root 不要にするには --exfat-method fuse / --luks-method xts / --udf-method genisoimage、省くなら --skip-mount-fs)" % ", ".join(rn))
            say("  作業領域の目安: イメージ %s (--keep-images または --work-dir/images_*) + 投入ツリーなど %s (--work-dir)。ローカルディスク上に置くこと (tmpfs 不可)"
                % (human(estimate_images_need(layout, opts)), human(estimate_work_need(layout, opts))))
        return 0

    prof = profiles[0]
    work = os.path.abspath(args.work_dir or "./usbmem_testset_work")
    os.makedirs(work, exist_ok=True)
    runner = Runner(args.log)
    runner.log("=== 開始: %s" % " ".join(shlex.quote(a) for a in sys.argv))

    # ---- 事前チェック ----
    mode = "image" if args.image else ("build" if args.build_only else "device")
    if sys.version_info < (3, 9):
        say("Python 3.9 以上が必要")
        return 2
    if mode == "device" and os.geteuid() != 0:
        say("実デバイスへの流し込みには root 権限が必要です (sudo で実行してください)。このスクリプトは自分では sudo を呼びません。"
            "root なしでできるのは、イメージ作成だけ (--build-only --keep-images DIR)。")
        return 2
    rn = root_needed_parts(prof, opts)
    if not flash_only and os.geteuid() != 0 and rn and not opts.skip_mount_fs:
        say("root でないため、mount / cryptsetup が要る方式のパーティション (%s) の中身は投入できません。次のどれかにしてください: "
            "root で実行 / --exfat-method fuse / --luks-method xts / --udf-method genisoimage / --f2fs-method sload / --skip-mount-fs。" % ", ".join(rn))
        return 2
    miss = []
    if not flash_only:
        req = required_tools(prof, prof.table, opts, "device" if mode == "device" else "image", os.geteuid() == 0)
        miss = find_missing(req)
        if any(s.fs == "bitlocker" for s in prof.parts) and opts.bitlocker == "synth":
            if not os.path.exists(os.path.join(HERE, "bitlocker_synth.py")):
                miss.append(("tools/bitlocker_synth.py", "BitLocker 合成"))
            else:
                r = subprocess.run([sys.executable, "-c", "import numpy, cryptography"], capture_output=True)
                if r.returncode != 0:
                    miss.append(("python3 の numpy / cryptography", "bitlocker_synth.py が必要 (--bitlocker skip で省略可)"))
        if opts.luks_method == "xts" and any(s.fs == "luks" for s in prof.parts):
            r = subprocess.run([sys.executable, "-c", "import cryptography"], capture_output=True)
            if r.returncode != 0:
                miss.append(("python3 の cryptography", "--luks-method xts が必要"))
    elif mode == "device":
        miss = find_missing({t: "デバイスの検証・バックアップ・消去" for t in ("sgdisk", "sfdisk", "lsblk", "wipefs", "blockdev", "dd")})
    if mode in ("device", "image"):
        miss += find_missing(flash_tools(writer))
    if miss:
        say("次のコマンド/モジュールが見つからないため中止します:")
        for t, why in miss:
            say("  - %s  (%s)" % (t, why))
        return 2

    node = None
    target = None
    if mode == "image":
        bad = check_image_target(args.image)
        if bad:
            say("中止: " + "; ".join(bad))
            return 2
        if os.path.exists(args.image) and not args.overwrite_image:
            say("中止: %s が既にある。作り直すなら --overwrite-image" % args.image)
            return 2
        dev_size = args.image_size or (meta or {}).get("dev_size") or prof.expected
        target = os.path.abspath(args.image)
    elif args.device:
        dev = args.device
        if not os.path.exists(dev) and not is_block_device(dev):
            say("中止: %s が存在しない" % dev)
            return 2
        is_blk = is_block_device(dev)
        tree = lsblk_tree()
        mounts, swaps = current_mounts()
        why = validate_device(dev, prof, tree, mounts, swaps, is_blk)
        if why:
            say("デバイスの検証で次の理由により中止します (何も書き込んでいません):")
            for w in why:
                say("  - " + w)
            return 2
        node = device_node(tree, dev)
        dev_size = int(node["size"])
        ss = int(node.get("log-sec") or 512)
        opts.ss = ss
        target = dev
    else:
        dev_size = args.image_size or (meta or {}).get("dev_size") or prof.expected
    try:
        layout = compute_layout(prof, dev_size, ss)
    except ValueError as e:
        say("中止: " + str(e))
        return 2
    if flash_only:
        probs = meta_problems(meta, layout, flash_only)
        if probs:
            say("イメージ一式 %s をこの対象に流せません (何も書き込んでいません):" % flash_only)
            for w in probs:
                say("  - " + w)
            return 2

    # ---- 置き場所の確認 (ローカルのディスク上か・空きがあるか) ----
    if flash_only:
        imgdir = flash_only
    elif args.keep_images:
        imgdir = os.path.abspath(args.keep_images)
        if os.path.exists(os.path.join(imgdir, META_NAME)) and not (args.overwrite_images or args.only):
            say("中止: %s に既にイメージ一式がある。流し込むだけなら --flash-only、作り直すなら --overwrite-images" % imgdir)
            return 2
    else:
        imgdir = os.path.join(work, "images_" + prof.name)
    try:
        mi_work = parse_mountinfo(_read_sys("/proc/self/mountinfo") or "")
    except Exception:
        mi_work = []
    if not flash_only:
        need_img, need_work = estimate_images_need(layout, opts), estimate_work_need(layout, opts)
        os.makedirs(imgdir, exist_ok=True)
        same_fs = os.stat(imgdir).st_dev == os.stat(work).st_dev
        checks = [("イメージ置き場", imgdir, need_img + (need_work if same_fs else 0))] + ([] if same_fs else [("作業ディレクトリ", work, need_work)])
        for label, path, need in checks:
            if not free_space_check(path, need):
                say("中止: %s %s の空きが足りない (目安 %s)。--work-dir / --keep-images で空きの多いローカルのディスクを指定" % (label, path, human(need)))
                return 2
        for label, path, need in (("イメージ置き場", imgdir, need_img), ("作業ディレクトリ", work, need_work)):
            if workdir_is_ram(mi_work, path):
                avail = mem_available()
                msg = "%s %s はメモリ上 (tmpfs / ライブ環境の overlay) にある。目安 %s がメモリを使う" % (label, path, human(need))
                if avail is not None and need > avail // 2 and not args.allow_ram_workdir:
                    say("中止: " + msg + " (空きメモリ %s)。ディスク上の場所を指定するか、--no-big-files を付ける。承知の上なら --allow-ram-workdir" % human(avail))
                    return 2
                say("警告: " + msg + "。スワップが増えると極端に遅くなる")
        say("空き容量の目安: イメージ %s・投入ツリーなど %s" % (human(need_img), human(need_work)))
    if mode == "device":
        lock = acquire_single_instance_lock()
        if lock is None:
            say("中止: 別の make_real_usb_testset.py が実行中。デバイスへの書き込みは 1 本ずつ順に行う (ハブの帯域を取り合うため)。イメージを作るだけの --build-only は同時に動かしてよい")
            return 2

    progress = args.progress if args.progress is not None else sys.stderr.isatty()
    manifest_dir = os.path.join(args.manifest_dir, prof.name) if not flash_only else os.path.join(flash_only, "manifests")
    if flash_only:
        say("イメージ一式 %s を流し込みます (再生成しない)。プロファイル %s・容量 %s" % (flash_only, prof.name, human(layout.dev_size)))
        print_flash_plan(layout, writer, say, flash_only)
    else:
        print_plan(layout, opts, target or "(デバイスなし: イメージ一式だけ作る)", writer=writer)

    # ---- (a) デバイスに触らず、イメージを作る (または既存のイメージを確認する) ----
    t_start = time.time()
    build_results = None
    try:
        if not flash_only:
            ctx = Ctx(args, runner, work, mode, ss, opts, manifest_dir)
            only = set(args.only.split(",")) if args.only else None
            build_results, meta = build_images(ctx, layout, imgdir, only)
        elif not args.skip_image_check:
            runner.say("イメージ一式の確認 (SHA-256。デバイスにはまだ触らない)")
            bad = check_image_hashes(runner, imgdir, meta)
            if bad:
                raise StepError("; ".join(bad))
    except (StepError, OSError, ValueError, KeyboardInterrupt) as e:
        say("")
        say("!!! 失敗: %s" % e)
        say("デバイスには一切書き込んでいません。イメージの置き場所: %s" % imgdir)
        runner.log("失敗 (イメージ作成): %s" % e)
        if not (args.keep_images or flash_only):
            shutil.rmtree(imgdir, ignore_errors=True)
        return 1
    if build_results is not None:
        ng = [(pp.name, n) for pp, st in build_results for n, ok in st["checks"] if not ok]
    else:
        ng = [(p["name"], n) for p in meta.get("parts", []) for n, ok in p.get("checks", []) if not ok]
    if mode == "build":
        return finish(runner, layout, build_results, manifest_dir, None, mode, imgdir, time.time() - t_start)
    if ng and not args.ignore_image_checks:
        say("")
        say("!!! イメージの検査 (fsck 相当) が NG: %s" % ", ".join("%s:%s" % x for x in ng))
        say("デバイスには書き込んでいません。イメージ (%s) を調べるか、承知の上なら --ignore-image-checks。" % imgdir)
        return 1
    if not meta.get("complete"):
        say("中止: イメージ一式が不完全")
        return 1
    items = flash_items(layout, imgdir, meta)

    # ---- device / image ----
    done = []
    bdir = None
    try:
        if mode == "device":
            say("")
            say("!!! 次のデバイスの内容を**全部消して**パーティションを作り直します (イメージはもうできています) !!!")
            for k, v in device_summary(node):
                say("  %s: %s" % (k, v))
            say("  現在のパーティション:")
            r = subprocess.run(["lsblk", "-o", "NAME,SIZE,FSTYPE,LABEL,MOUNTPOINTS", dev], capture_output=True, text=True)
            for ln in (r.stdout or r.stderr).splitlines():
                say("    " + ln)
            try:
                typed = input("実行するなら、対象デバイスのパス (%s) をそのまま入力してください: " % dev)
            except EOFError:
                typed = None
            if not confirm_typed(typed, dev):
                say("入力が一致しないため中止しました (何も書き込んでいません)。")
                return 2
            # 入力待ちの間に状態が変わっていないか再検証
            tree = lsblk_tree()
            mounts, swaps = current_mounts()
            why = validate_device(dev, prof, tree, mounts, swaps, True)
            if why:
                say("再検証で拒否: " + "; ".join(why))
                return 2
            bdir = args.backup_dir or os.path.join("./usbmem_testset_backup", "%s_%s" % (prof.name, datetime.datetime.now().strftime("%Y%m%d_%H%M%S")))
            bdir = os.path.abspath(bdir)
            os.makedirs(bdir, exist_ok=True)
            if args.full_backup and not free_space_check(bdir, dev_size + 64 * MiB):
                say("中止: --full-backup の保存先 %s に %s の空きがない" % (bdir, human(dev_size)))
                return 2
            runner.say("バックアップ先: " + bdir)
            with open(os.path.join(bdir, "lsblk.json"), "w") as f:
                json.dump(node, f, ensure_ascii=False, indent=1)
            for c in backup_cmds(dev, dev_size, bdir, args.full_backup):
                runner.run(c, check=(c.kind == "run"))
            done.append("バックアップ")
            runner.say("既存のパーティション表と先頭/末尾を消す")
            for c in erase_cmds(dev, dev_size):
                runner.run(c)
            done.append("既存表の消去")
            runner.say("(d) 流し込み (%s)" % writer)
            flash_all(runner, layout, items, dev, True, writer, args.sparse_write, progress)
            done.append("流し込み")
            runner.say("(e) 読み戻し検証")
            bad = verify_flashed(runner, items, dev, True)
        else:
            if os.path.exists(target) and args.overwrite_image:
                os.unlink(target)
            runner.say("(d) イメージファイル %s (%s, 疎ファイル) へ流し込み (%s)" % (target, human(layout.dev_size), writer))
            flash_all(runner, layout, items, target, False, writer, False, progress)
            done.append("流し込み")
            runner.say("(e) 読み戻し検証")
            bad = verify_flashed(runner, items, target, False)
        done.append("読み戻し検証")
        if bad:
            raise StepError("読み戻し検証に失敗: " + "; ".join(bad))
        rc = finish(runner, layout, build_results, manifest_dir, target, mode, imgdir, time.time() - t_start)
    except (StepError, OSError, ValueError, KeyboardInterrupt) as e:
        say("")
        say("!!! 失敗: %s" % e)
        say("完了した手順: %s" % (", ".join(done) or "なし"))
        if mode == "device":
            say("デバイスは中途半端な状態です (パーティション表が無い/FS が一部だけ、など)。イメージは %s に残してあります (--flash-only で流し直せます)。" % imgdir)
            say("元の状態へ戻すには %s のバックアップ (sgdisk --load-backup / sfdisk < sfdisk_dump.txt / dd) を使うか、同じコマンドをもう一度実行して最初からやり直してください。" % bdir)
        else:
            say("イメージは %s に残してあります。" % imgdir)
        runner.log("失敗: %s / 完了: %s" % (e, done))
        return 1
    if not (args.keep_images or flash_only):
        shutil.rmtree(imgdir, ignore_errors=True)
    return rc


def finish(runner, layout, results, manifest_dir, target, mode, imgdir, elapsed):
    """結果の表示。mode: build (イメージだけ) / device / image。パーティション表の表示は表のイメージから行う (デバイスのカーネルはまだ新しい表を読んでいない)。"""
    say = runner.say
    say("")
    say("=" * 100)
    say("結果")
    bad = 0
    for pp, st in (results or []):
        fails = [n for n, ok in st["checks"] if not ok]
        bad += len(fails)
        say("  %-14s %-12s %s%s%s" % (pp.name, FS_DISPLAY[pp.fs], "データ投入あり" if st["ds"] is not None else "データ投入なし",
                                     ("  検査NG: " + ",".join(fails)) if fails else "  検査OK", ("  ※" + " / ".join(st["notes"])) if st["notes"] else ""))
    say("マニフェスト: " + os.path.abspath(manifest_dir))
    tbl = target if mode == "image" else os.path.join(imgdir or "", TABLE_IMG)
    if tbl and os.path.exists(tbl):
        _, listing = runner.run(Cmd(["sgdisk", "-p", tbl] if layout.profile.table == "gpt" else ["sfdisk", "-l", tbl]), check=False)
        say(listing.rstrip())
    if mode == "build":
        say("イメージ一式: %s (流し込みは --flash-only %s --device /dev/sdX --execute)" % (os.path.abspath(imgdir), os.path.abspath(imgdir)))
    elif mode == "device":
        say("デバイスへの流し込みと読み戻し検証 (SHA-256 一致) が済みました。カーネルは新しいパーティション表をまだ読んでいません: 取り外して挿し直してください。")
    say("所要時間: %.0f 秒" % elapsed)
    say("完了。" + ("検査 NG が %d 件あります。ログを確認してください。" % bad if bad else "次は USB を Android 実機へ (docs/real-usb-testset.md)。" if mode != "build" else ""))
    return 1 if bad else 0


def verify_main(argv):
    ap = argparse.ArgumentParser(prog="make_real_usb_testset.py verify", description="マニフェストとマウントしたツリーを照合する")
    ap.add_argument("manifest")
    ap.add_argument("root")
    ap.add_argument("--no-links", action="store_true", help="symlink / hardlink を検証しない (FS が対応していない取り出し方のとき)")
    ap.add_argument("--allow-extra", action="store_true", help="マニフェストに無いファイル (書き込み試験で追加したもの) を許す")
    a = ap.parse_args(argv)
    with open(a.manifest, encoding="utf-8") as f:
        entries = parse_manifest(f.read())
    problems, cnt = verify_tree(entries, a.root, check_links=not a.no_links, allow_extra=a.allow_extra)
    print("照合: ファイル %d (ハードリンク %d)・ディレクトリ %d・シンボリックリンク %d・%s" % (cnt["F"], cnt["H"], cnt["D"], cnt["L"], human(cnt["bytes"])))
    for p in problems[:50]:
        print("  NG: " + p)
    if len(problems) > 50:
        print("  ... ほか %d 件" % (len(problems) - 50))
    print("結果: " + ("一致" if not problems else "不一致 %d 件" % len(problems)))
    return 0 if not problems else 1


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv and argv[0] == "verify":
        return verify_main(argv[1:])
    return main_run(argv)


if __name__ == "__main__":
    sys.exit(main())
