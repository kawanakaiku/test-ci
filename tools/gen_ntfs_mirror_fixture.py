#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""$MFTMirr まわりのテスト用 NTFS イメージを作る (docs/ntfs-mftmirr.md)。
mkntfs + ntfs-3g でクリーンなボリュームを作り (a.txt と d/f00.txt..f29.txt)、その写しの $MFT / $MFTMirr を直接書き換える。

(A) 64 KiB クラスタの偽陽性: 上流は 64 KiB クラスタで $MFTMirr の 1 クラスタ分 (1 KiB レコードなら 64 レコード) を $MFT と比べるが、
    Windows が $MFTMirr と同期するのは先頭の少数だけ (ntfsfix のコメント: "Windows 10 does not mirror to full $MFTMirr when using big clusters")。
    mkntfs が作るミラーは先頭 1 クラスタ分すべてが $MFT と同じなので、Windows が書いた後の状態を「$MFT 側だけ更新 (LSN を進める)」で再現する。
  ntfs_mirror_plain.img    64 KiB クラスタ。ミラーは $MFT と一致 (対照)
  ntfs_mirror_win.img      64 KiB クラスタ。使用中の MFT レコード 16-63 ($Extend の子 24-26 など) の LSN を $MFT 側だけ進め、ミラーには使用中でない古いレコードの写しも置く (Windows 製の状態の再現)
  ntfs_mirror_win_mid.img  win に加え、システムレコード 5 (ルート)・10 ($UpCase)・11 ($Extend) も $MFT 側だけ更新 (ntfs-3g のマウントなら拒否する範囲。先頭 4 レコードの外)
  ntfs_mirror_bad0.img     先頭 4 レコードの内側 ($MFT のレコード 0) が $MFTMirr と食い違う: 本物の不一致なので読み取り専用に落ちる (負の対照)
  ntfs_mirror_bad3.img     同じく $Volume (レコード 3)
  ntfs_mirror_plain4k.img  4 KiB クラスタの対照 (従来から問題なし)
(B) torn write で壊れた MFT システムレコード (4 KiB クラスタ、$MFTMirr は無傷): レコードの先頭セクタだけが新しい内容で、残りが古い (fixup の USN が食い違う)
  ntfs_mirror_torn0.img    レコード 0 ($MFT)    ntfs_mirror_torn1.img  レコード 1 ($MFTMirr)
  ntfs_mirror_torn2.img    レコード 2 ($LogFile)  ntfs_mirror_torn3.img  レコード 3 ($Volume)
  ntfs_mirror_torn03.img   レコード 0 と 3
  ntfs_mirror_torn3m.img   レコード 3 と、$MFTMirr 側のレコード 3 も壊れている (救えない: マウントは失敗のまま。負の対照)
mkntfs / ntfs-3g / fusermount3 が無ければ終了コード 77 (SKIP)。使い方: gen_ntfs_mirror_fixture.py <outdir>
"""
import os
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ["PATH"] = os.environ.get("PATH", "") + ":/sbin:/usr/sbin"
from gen_ntfs_state_fixture import sh, with_mount   # noqa: E402  (mkntfs 後に ntfs-3g でマウントして中身を作る共通の補助)
from dt_ntfs import Nt   # noqa: E402  (レコードの fixup を解いて編集する検査用の編集器)

NFILES = 30


def make_base(path, size, cluster):
    with open(path, "wb") as f:
        f.truncate(size)
    sh(["mkntfs", "-F", "-Q", "-q", "-s", "512", "-c", str(cluster), "-L", "MIRRVOL", path])

    def fill(mnt):
        with open(os.path.join(mnt, "a.txt"), "wb") as f:
            f.write(b"hello\n")
        os.mkdir(os.path.join(mnt, "d"))
        for i in range(NFILES):
            with open(os.path.join(mnt, "d", "f%02d.txt" % i), "wb") as f:
                f.write(b"file %02d\n" % i)
    with_mount(path, fill)


def in_use(r):
    return bool(struct.unpack_from("<H", r, 0x16)[0] & 1) and r[0:4] == b"FILE"


def bump_lsn(x, n, mirror_too=False):
    """MFT レコード n の LSN (ヘッダのオフセット 8) を進める。通常は $MFT 側だけ (Windows が $MFTMirr に同期しなかった状態)。"""
    r = x.rec(n)
    lsn = struct.unpack_from("<Q", r, 8)[0]
    struct.pack_into("<Q", r, 8, lsn + 0x1000 + n)
    if mirror_too:
        x.put_rec(n, r)
    else:
        x.put_rec_nomirror(n, r)


def make_win(x, mid):
    used = [n for n in range(16, 64) if in_use(x.rec(n))]
    assert used, "no in-use MFT record in 16..63 (expected the $Extend children 24-26)"   # ntfs-3g は利用者のファイルを 64 より後ろに置く。24-26 は $Extend の子 (Windows 製でも最初に食い違うのはここ)
    for n in used:
        bump_lsn(x, n)
    # $MFT では使っていない 16..63 のレコードの位置に、ミラーだけ古い (削除済みの) レコードの写しを置く
    free = [n for n in range(16, 64) if not in_use(x.rec(n))]
    assert free, "no free MFT record slot below 64"
    stale = x.rec(used[0])
    stale[0x16] &= ~1   # 使用中フラグを下ろす
    mp = x.mirr_lcn * x.cs + free[-1] * x.rs
    x.d[mp:mp + x.rs] = x.fix(stale)
    if mid:
        for n in (5, 10, 11):
            bump_lsn(x, n)


def tear(x, n, mirror=False, sectors_new=1):
    """レコード n への書き込みが途中で電源断になった状態: 先頭 sectors_new セクタだけ新しい内容 (USN も新しい)、残りは古い。
    $MFT 側 (mirror=False) か $MFTMirr 側 (mirror=True)。"""
    pos = x.mirr_lcn * x.cs + n * x.rs if mirror else x.rec_pos(n)
    old_raw = bytes(x.d[pos:pos + x.rs])
    r = x.unfix(bytearray(old_raw))
    ufo = struct.unpack_from("<H", r, 4)[0]
    usn = struct.unpack_from("<H", r, ufo)[0]
    struct.pack_into("<H", r, ufo, (usn + 1) & 0xFFFF or 1)
    lsn = struct.unpack_from("<Q", r, 8)[0]
    struct.pack_into("<Q", r, 8, lsn + 0x2000 + n)
    new_raw = x.fix(r)
    k = 512 * sectors_new
    x.d[pos:pos + x.rs] = new_raw[:k] + old_raw[k:]


def main():
    out = sys.argv[1]
    for c in ("mkntfs", "ntfs-3g", "fusermount3"):
        if not shutil.which(c):
            sys.stderr.write("skip: %s not available\n" % c)
            return 77
    os.makedirs(out, exist_ok=True)
    plain64 = os.path.join(out, "ntfs_mirror_plain.img")
    make_base(plain64, 64 * 1024 * 1024, 65536)
    plain4k = os.path.join(out, "ntfs_mirror_plain4k.img")
    make_base(plain4k, 16 * 1024 * 1024, 4096)

    def mutate(base, name, fn):
        path = os.path.join(out, "ntfs_mirror_%s.img" % name)
        with open(base, "rb") as f:
            x = Nt(f.read())
        fn(x)
        with open(path, "wb") as f:
            f.write(x.d)

    mutate(plain64, "win", lambda x: make_win(x, False))
    mutate(plain64, "win_mid", lambda x: make_win(x, True))
    mutate(plain64, "bad0", lambda x: bump_lsn(x, 0))
    mutate(plain64, "bad3", lambda x: bump_lsn(x, 3))
    for n in (0, 1, 2, 3):
        mutate(plain4k, "torn%d" % n, lambda x, n=n: tear(x, n))
    mutate(plain4k, "torn03", lambda x: (tear(x, 0), tear(x, 3)))
    mutate(plain4k, "torn3m", lambda x: (tear(x, 3), tear(x, 3, mirror=True)))
    print("ok: " + " ".join(sorted(f for f in os.listdir(out) if f.startswith("ntfs_mirror_"))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
