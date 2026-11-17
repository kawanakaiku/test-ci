#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""「Windows で正しく取り外されなかった」NTFS ボリュームのテスト用イメージを作る (docs/dirty-volumes.md)。
mkntfs + ntfs-3g で 16 MiB のクリーンなボリュームを作り (a.txt を 1 つ)、その写しの $Volume のフラグと $LogFile の restart page を直接書き換える。
出力 (<outdir>/):
  ntfs_state_clean.img      クリーン (ログは空 = 0xFF)
  ntfs_state_dirty.img      ダーティフラグだけ (ログは静止)。ここは従来どおり書き込める (クリーンなアンマウントでフラグが下りる)
  ntfs_state_dirty_log.img  ダーティフラグ + **未再生のログ** (Windows 10 流の LFS 1.1 の restart page、restart area に「クリーン」の印なし)。上流ドライバの検査はこのログを通す
  ntfs_state_log.img        未再生のログだけ (ダーティフラグなし)
  ntfs_state_hiber.img      ルートの hiberfil.sys が "hibr" で始まる (休止状態 / 高速スタートアップ)。ダーティフラグなし
mkntfs / ntfs-3g / fusermount3 が無ければ終了コード 77 (SKIP)。使い方: gen_ntfs_state_fixture.py <outdir>
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ["PATH"] = os.environ.get("PATH", "") + ":/sbin:/usr/sbin"
SIZE = 16 * 1024 * 1024


def sh(cmd, **kw):
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, **kw)
    if p.returncode != 0:
        sys.stderr.write("command failed: %s\n%s\n" % (" ".join(cmd), p.stdout.decode("utf-8", "replace")))
        raise SystemExit(1)
    return p.stdout


def with_mount(img, fn):
    """img を ntfs-3g で書き込みマウントして fn(mnt) を呼び、クリーンにアンマウントする (ログは ntfs-3g が空にする)"""
    mnt = tempfile.mkdtemp(prefix="mnt.", dir=os.path.dirname(img))
    try:
        sh(["ntfs-3g", "-o", "rw", img, mnt])
        try:
            fn(mnt)
        finally:
            sh(["fusermount3", "-u", mnt])
    finally:
        shutil.rmtree(mnt, ignore_errors=True)


def restart_page(lsn, clean, file_size):
    """Windows 10 流 (LFS 1.1) の restart page 1 枚 (4096 バイト、MST 保護つき)。上流ドライバの検査 (ntfs_check_restart_page_header / area / client array) を通る形。"""
    pg = bytearray(4096)
    pg[0:4] = b"RSTR"
    # ヘッダ: usa_ofs, usa_count (1 + 4096/512), chkdsk_lsn, system_page_size, log_page_size, restart_area_offset, minor, major
    struct.pack_into("<HHQIIHHH", pg, 4, 0x1E, 9, 0, 4096, 4096, 0x30, 1, 1)
    # restart area (0x30 バイト) + クライアント 1 つ (0xA0 バイト)
    ra = 0x30
    fs_bits = file_size.bit_length()
    struct.pack_into("<QHHHH", pg, ra, lsn, 1, 0xFFFF, 0, 2 if clean else 0)   # current_lsn, log_clients=1, free list 空, in use = クライアント 0, flags
    struct.pack_into("<IHHQ", pg, ra + 0x10, 67 - fs_bits, 0x30 + 0xA0, 0x30, file_size)   # seq_number_bits, restart_area_length, client_array_offset, file_size
    struct.pack_into("<IHHII", pg, ra + 0x20, 0, 0x30, 0x40, 1, 0)   # last_lsn_data_length, log_record_header_length, log_page_data_offset, open_count, reserved
    ca = ra + 0x30
    struct.pack_into("<QQHHH", pg, ca, lsn, lsn, 0xFFFF, 0xFFFF, 0)   # oldest_lsn, client_restart_lsn, prev, next, seq_number
    name = "NTFS".encode("utf-16-le")
    struct.pack_into("<I", pg, ca + 0x1C, len(name))
    pg[ca + 0x20:ca + 0x20 + len(name)] = name
    # MST 保護: 各 512 バイトの末尾 2 バイトを USN (1) にし、元の値 (0) を USA に残す
    pg[0x1E:0x20] = struct.pack("<H", 1)
    for i in range(8):
        pg[0x20 + 2 * i:0x22 + 2 * i] = pg[512 * (i + 1) - 2:512 * (i + 1)]
        pg[512 * (i + 1) - 2:512 * (i + 1)] = struct.pack("<H", 1)
    return bytes(pg)


def main():
    out = sys.argv[1]
    for c in ("mkntfs", "ntfs-3g", "fusermount3"):
        if not shutil.which(c):
            sys.stderr.write("skip: %s not available\n" % c)
            return 77
    from dt_ntfs import Nt   # 検査用の MFT 編集器 (レコードの fixup を解いて編集する)
    os.makedirs(out, exist_ok=True)
    clean = os.path.join(out, "ntfs_state_clean.img")
    with open(clean, "wb") as f:
        f.truncate(SIZE)
    sh(["mkntfs", "-F", "-Q", "-q", "-s", "512", "-c", "4096", "-L", "STATEVOL", clean])

    def base_files(mnt):
        with open(os.path.join(mnt, "a.txt"), "wb") as f:
            f.write(b"hello\n")
    with_mount(clean, base_files)

    def mutate(name, fn):
        path = os.path.join(out, "ntfs_state_%s.img" % name)
        shutil.copyfile(clean, path)
        x = Nt(open(path, "rb").read())
        fn(x)
        with open(path, "wb") as f:
            f.write(x.d)

    def set_dirty(x):
        r = x.rec(3)
        a = x.find_attr(r, 0x70)
        vo = struct.unpack_from("<H", r, a[0] + 0x14)[0]
        fl = struct.unpack_from("<H", r, a[0] + vo + 10)[0]
        struct.pack_into("<H", r, a[0] + vo + 10, fl | 1)
        x.put_rec(3, r)

    def unclean_log(x):
        lr = x.rec(2)
        runs = x.runs_of(lr, 0x80)
        size = struct.unpack_from("<Q", lr, x.find_attr(lr, 0x80)[0] + 0x30)[0]
        pos = runs[0][1] * x.cs
        x.d[pos:pos + 4096] = restart_page(0x1000, False, size)
        x.d[pos + 4096:pos + 8192] = restart_page(0x0F00, False, size)

    mutate("dirty", set_dirty)
    mutate("dirty_log", lambda x: (set_dirty(x), unclean_log(x)))
    mutate("log", unclean_log)

    hiber = os.path.join(out, "ntfs_state_hiber.img")
    shutil.copyfile(clean, hiber)

    def add_hiberfil(mnt):
        with open(os.path.join(mnt, "hiberfil.sys"), "wb") as f:
            f.write(b"hibr" + bytes(8192 - 4))
    with_mount(hiber, add_hiberfil)
    print("ok: " + " ".join(sorted(f for f in os.listdir(out) if f.startswith("ntfs_state_"))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
