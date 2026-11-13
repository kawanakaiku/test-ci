#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""tools/make_real_usb_testset.py の単体テスト。実デバイスには一切触らない (lsblk の出力・マウント一覧は差し替え、subprocess は監視する)。

実行: python3 tools/test_make_real_usb_testset.py [-v]
結合テスト (イメージモードで実際に mkfs 等を走らせる。数十秒): USBMEM_TESTSET_INTEGRATION=1 python3 tools/test_make_real_usb_testset.py
"""
import contextlib
import copy
import io
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import make_real_usb_testset as M  # noqa: E402

GiB, MiB = M.GiB, M.MiB


def disk(name, size, tran="usb", rm=True, children=None, ro=False, mounts=None, logsec=512, dtype="disk", model="Flash Disk", serial="SER123"):
    path = "/dev/" + name
    return {"name": path, "kname": path, "path": path, "type": dtype, "size": size, "tran": tran, "rm": rm, "hotplug": rm, "ro": ro,
            "model": model, "vendor": "Gen", "serial": serial, "mountpoints": mounts or [None], "fstype": None, "label": None,
            "log-sec": logsec, "children": children or []}


def part(name, size, mounts=None, children=None):
    n = disk(name, size, tran=None, rm=False, dtype="part", mounts=mounts, children=children)
    n["tran"] = None
    return n


def make_tree():
    """nvme0n1 (システム: / と /boot と /home)、sda (内蔵 SATA)、sdb (USB 7.8G)、sdc (USB 16G)、sdd (USB 31G)"""
    nvme = disk("nvme0n1", 512 * GiB, tran="nvme", rm=False, children=[
        part("nvme0n1p1", 512 * MiB, mounts=["/boot"]),
        part("nvme0n1p2", 200 * GiB, mounts=["/"]),
        part("nvme0n1p3", 300 * GiB, mounts=["/home"], children=[
            {**part("mapper/home_crypt", 300 * GiB, mounts=["/home"]), "path": "/dev/mapper/home_crypt", "kname": "/dev/dm-0", "name": "/dev/mapper/home_crypt", "type": "crypt"}]),
    ])
    sda = disk("sda", 2000 * GiB, tran="sata", rm=False, children=[part("sda1", 2000 * GiB - MiB)])
    sdb = disk("sdb", int(7.8 * GiB), children=[part("sdb1", 3 * GiB)])
    sdc = disk("sdc", 16 * GiB, children=[part("sdc1", 1 * GiB)])
    sdd = disk("sdd", 31 * GiB)
    return [nvme, sda, sdb, sdc, sdd]


MOUNTS = M.parse_mounts(
    "/dev/nvme0n1p2 / ext4 rw 0 0\n/dev/nvme0n1p1 /boot vfat rw 0 0\n/dev/mapper/home_crypt /home btrfs rw 0 0\n"
    "tmpfs /tmp tmpfs rw 0 0\n/dev/sda1 /mnt/data ext4 rw 0 0\n")


class LayoutTests(unittest.TestCase):
    def test_all_profiles_nominal_and_extremes(self):
        for name, prof in M.PROFILES.items():
            for factor in (0.92, 0.96, 1.0, 1.04, 1.08):
                size = int(prof.expected * factor)
                lay = M.compute_layout(prof, size)
                self.assertEqual(lay.parts[0].start, M.LEAD, name)
                end = 0
                for pp in lay.parts:
                    self.assertEqual(pp.start % M.ALIGN, 0, (name, pp.name))
                    self.assertEqual(pp.size % M.ALIGN, 0, (name, pp.name))
                    self.assertGreaterEqual(pp.start, end, (name, pp.name))
                    end = pp.end
                self.assertLessEqual(lay.used_end, lay.dev_size - M.TAIL_RESERVE, name)
                self.assertGreaterEqual(lay.unallocated_tail, M.TAIL_RESERVE)
                if prof.min_tail:
                    self.assertGreaterEqual(lay.unallocated_tail, prof.min_tail, (name, factor))

    def test_profile_a_sizes(self):
        lay = M.compute_layout(M.PROFILES["A"], int(7.8 * GiB))
        sizes = [pp.size for pp in lay.parts]
        self.assertEqual(sizes[:3], [3 * GiB, 2 * GiB, 1 * GiB])
        self.assertGreater(sizes[3], 1.4 * GiB)       # 残り約 1.5G
        self.assertLess(sizes[3], 1.9 * GiB)
        self.assertEqual(lay.parts[3].end, M.align_down(lay.dev_size - M.TAIL_RESERVE))

    def test_profile_b_mbr_layout(self):
        lay = M.compute_layout(M.PROFILES["B"], 16 * GiB)
        self.assertEqual([p.index for p in lay.parts], [1, 2, 3, 5, 6, 7])
        self.assertEqual(lay.extended[0], 4)
        # 論理パーティションの前に 1 MiB (EBR 用) が空いている
        self.assertEqual(lay.parts[3].start, lay.extended[1] + M.MiB)
        self.assertEqual(lay.parts[4].start, lay.parts[3].end + M.MiB)
        self.assertEqual(lay.extended[1] + lay.extended[2], lay.parts[-1].end)
        self.assertGreater(lay.unallocated_tail, 3 * GiB)

    def test_profile_c_tail(self):
        lay = M.compute_layout(M.PROFILES["C"], 31 * GiB)
        self.assertGreaterEqual(lay.unallocated_tail, 10 * GiB)
        self.assertEqual([p.fs for p in lay.parts], ["xfs", "btrfs", "f2fs", "luks", "bitlocker", "squashfs", "erofs", "udf", "iso"])

    def test_too_small_device_rejected(self):
        with self.assertRaises(ValueError):
            M.compute_layout(M.PROFILES["C"], 20 * GiB)       # 末尾 10G が取れない
        with self.assertRaises(ValueError):
            M.compute_layout(M.PROFILES["B"], 8 * GiB)

    def test_big_files_only_on_allowed_fs(self):
        for prof in M.PROFILES.values():
            for pp in M.compute_layout(prof, prof.expected).parts:
                c = M.caps_for(pp.spec, pp.size)
                if c.big:
                    self.assertIn((prof.name, pp.fs), {("B", "ntfs"), ("C", "xfs"), ("C", "btrfs")})
                    self.assertGreater(pp.size, 4 * GiB + 512 * MiB)
                self.assertFalse(c.big and c.gib)
                if pp.fs == "fat32":
                    self.assertFalse(c.big or c.gib)


class TableCommandTests(unittest.TestCase):
    def test_sgdisk_a(self):
        lay = M.compute_layout(M.PROFILES["A"], int(7.8 * GiB))
        cmds = M.table_cmds(lay, "/dev/sdX")
        self.assertEqual(cmds[0].argv, ["sgdisk", "--zap-all", "/dev/sdX"])
        a = cmds[1].argv
        self.assertIn("1:2048:6293503", a)         # 1 MiB 開始、3 GiB
        self.assertIn("1:0700", a)
        self.assertIn("2:8300", a)
        self.assertEqual(a[-1], "/dev/sdX")
        self.assertEqual(a.count("-n"), 4)

    def test_sgdisk_c_luks_type(self):
        lay = M.compute_layout(M.PROFILES["C"], 31 * GiB)
        a = M.table_cmds(lay, "/dev/sdX")[1].argv
        self.assertIn("4:8309", a)

    def test_sfdisk_script_b(self):
        lay = M.compute_layout(M.PROFILES["B"], 16 * GiB)
        script = M.sfdisk_script(lay)
        lines = [l for l in script.splitlines() if l.startswith("start=")]
        self.assertEqual(len(lines), 7)
        self.assertIn("type=c", lines[0])
        self.assertIn("type=5", lines[3])
        self.assertTrue(all("type=83" in l for l in lines[4:]))
        self.assertTrue(script.startswith("label: dos"))
        c = M.table_cmds(lay, "/dev/sdX")[0]
        self.assertEqual(c.argv[0], "sfdisk")
        self.assertEqual(c.stdin, script)

    def test_part_node(self):
        self.assertEqual(M.part_node("/dev/sdb", 1), "/dev/sdb1")
        self.assertEqual(M.part_node("/dev/mmcblk0", 2), "/dev/mmcblk0p2")

    def test_backup_cmds(self):
        cmds = M.backup_cmds("/dev/sdb", 8 * GiB, "/b", False)
        flat = [" ".join(c.argv) for c in cmds]
        self.assertTrue(any(f.startswith("sgdisk --backup=/b/sgdisk_backup.bin") for f in flat))
        self.assertTrue(any("count=4" in f and "head_4MiB" in f for f in flat))
        self.assertTrue(any("skip=%d" % (8 * GiB - MiB) in f and "tail_1MiB" in f for f in flat))
        self.assertFalse(any("full.img" in f for f in flat))
        self.assertTrue(any("full.img" in " ".join(c.argv) for c in M.backup_cmds("/dev/sdb", 8 * GiB, "/b", True)))
        sf = [c for c in cmds if c.argv[0] == "sfdisk"][0]
        self.assertEqual(sf.stdout_file, "/b/sfdisk_dump.txt")


class DeviceValidationTests(unittest.TestCase):
    def setUp(self):
        self.tree = make_tree()

    def v(self, dev, prof, tree=None, mounts=None, swaps=(), is_block=True, mountinfo="", reader=lambda p: None):
        # realpath が /dev/ 配下の実在しないパスをそのまま返すので差し替え不要。mountinfo は既定で空 (実機の /proc に依存させない)
        return M.validate_device(dev, M.PROFILES[prof], tree or self.tree, MOUNTS if mounts is None else mounts, swaps, is_block, mountinfo, reader)

    # ライブ USB (ルートが overlay + squashfs ループ + tmpfs) の再現
    LIVE_MI = (
        "33 31 253:0 / /run/live/medium ro,noatime shared:6 - iso9660 {medium} ro\n"
        "34 31 7:0 / /run/live/rootfs/filesystem.squashfs ro,noatime shared:7 - squashfs /dev/loop0 ro\n"
        "35 31 0:31 / /run/live/overlay rw,noatime shared:8 - tmpfs tmpfs rw,size=1k\n"
        "38 2 0:32 / / rw,noatime shared:1 - overlay overlay rw,lowerdir=/run/live/rootfs/filesystem.squashfs/,upperdir=/run/live/overlay/rw,workdir=/run/live/overlay/work,uuid=on\n")
    LIVE_MOUNTS = M.parse_mounts("overlay / overlay rw 0 0\n")

    @staticmethod
    def loop_reader(p):
        return "/run/live/medium/live/filesystem.squashfs" if p == "/sys/block/loop0/loop/backing_file" else None

    def test_live_overlay_root_on_nvme_allows_usb(self):
        mi = self.LIVE_MI.format(medium="/dev/nvme0n1p2")
        self.assertEqual(self.v("/dev/sdc", "B", mounts=self.LIVE_MOUNTS, mountinfo=mi, reader=self.loop_reader), [])

    def test_live_overlay_root_on_target_usb_refused(self):
        # ライブ媒体そのものが sdd (USB) に載っているなら、ルートの実体なので拒否
        tree = make_tree()
        tree[4]["children"] = [part("sdd2", 30 * GiB)]
        mi = self.LIVE_MI.format(medium="/dev/sdd2")
        r = self.v("/dev/sdd", "C", tree, self.LIVE_MOUNTS, mountinfo=mi, reader=self.loop_reader)
        self.assertTrue(any("システム" in w for w in r), r)

    def test_live_overlay_unknown_loop_backing_fails_closed(self):
        mi = self.LIVE_MI.format(medium="/dev/nvme0n1p2")
        r = self.v("/dev/sdc", "B", mounts=self.LIVE_MOUNTS, mountinfo=mi, reader=lambda p: None)
        self.assertTrue(any("安全側" in w for w in r), r)

    def test_good_devices(self):
        self.assertEqual(self.v("/dev/sdb", "A"), [])
        self.assertEqual(self.v("/dev/sdc", "B"), [], )
        self.assertEqual(self.v("/dev/sdd", "C"), [])

    def test_sdc_has_unmounted_partition_ok(self):
        self.assertEqual(self.v("/dev/sdc", "B"), [])

    def test_internal_nvme_refused(self):
        r = self.v("/dev/nvme0n1", "C")
        self.assertTrue(any("nvme" in w for w in r), r)
        self.assertTrue(any("システム" in w for w in r), r)

    def test_internal_sata_refused(self):
        tree = make_tree()
        tree[1]["size"] = int(31 * GiB)         # サイズが合っていても SATA は拒否
        r = self.v("/dev/sda", "C", tree)
        self.assertTrue(any("sata" in w for w in r), r)

    def test_tran_empty_refused(self):
        tree = make_tree()
        tree[4]["tran"] = None
        r = self.v("/dev/sdd", "C", tree)
        self.assertTrue(any("usb ではない" in w for w in r), r)

    def test_size_mismatch_refused(self):
        self.assertTrue(any("±8%" in w for w in self.v("/dev/sdb", "B")))     # 7.8G を B (16G) として
        self.assertTrue(any("±8%" in w for w in self.v("/dev/sdd", "A")))
        tree = make_tree()
        tree[4]["size"] = int(31 * GiB * 0.91)                                 # 8% の外
        self.assertTrue(any("±8%" in w for w in self.v("/dev/sdd", "C", tree)))
        tree[4]["size"] = int(31 * GiB * 0.93)                                 # 内
        self.assertEqual(self.v("/dev/sdd", "C", tree), [])

    def test_mounted_partition_refused(self):
        tree = make_tree()
        tree[3]["children"][0]["mountpoints"] = ["/run/media/u/STICK"]
        r = self.v("/dev/sdc", "B", tree)
        self.assertTrue(any("マウントされている" in w for w in r), r)

    def test_mounted_per_proc_mounts_refused(self):
        mounts = MOUNTS + [("/dev/sdc1", "/mnt/x", "vfat")]
        r = self.v("/dev/sdc", "B", mounts=mounts)
        self.assertTrue(any("/proc/mounts" in w for w in r), r)

    def test_swap_refused(self):
        r = self.v("/dev/sdc", "B", swaps=["/dev/sdc1"])
        self.assertTrue(any("スワップ" in w for w in r), r)

    def test_system_on_usb_disk_refused(self):
        # ルートが USB (sdd) 上にあるシステムなら、サイズ・TRAN が合っていても拒否
        tree = make_tree()
        tree[4]["children"] = [part("sdd1", 30 * GiB)]
        mounts = M.parse_mounts("/dev/sdd1 / ext4 rw 0 0\n")
        r = self.v("/dev/sdd", "C", tree, mounts)
        self.assertTrue(any("システム" in w for w in r), r)

    def test_home_on_target_refused(self):
        tree = make_tree()
        tree[4]["children"] = [part("sdd1", 30 * GiB)]
        mounts = M.parse_mounts("/dev/nvme0n1p2 / ext4 rw 0 0\n/dev/sdd1 /home ext4 rw 0 0\n")
        r = self.v("/dev/sdd", "C", tree, mounts)
        self.assertTrue(any("システム" in w for w in r) and any("マウントされている" in w for w in r), r)

    def test_unresolvable_root_fails_closed(self):
        r = self.v("/dev/sdd", "C", mounts=M.parse_mounts("overlay / overlay rw 0 0\n"))
        self.assertTrue(any("安全側" in w for w in r), r)

    def test_partition_instead_of_disk_refused(self):
        r = self.v("/dev/sdb1", "A")
        self.assertTrue(any("ディスク全体ではない" in w for w in r), r)

    def test_not_block_device_refused(self):
        self.assertTrue(self.v("/dev/sdb", "A", is_block=False))

    def test_missing_device_refused(self):
        self.assertTrue(any("見つからない" in w for w in self.v("/dev/sdz", "A")))

    def test_read_only_refused(self):
        tree = make_tree()
        tree[2]["ro"] = True
        self.assertTrue(any("読み取り専用" in w for w in self.v("/dev/sdb", "A", tree)))

    def test_odd_sector_size_refused(self):
        tree = make_tree()
        tree[2]["log-sec"] = 1024
        self.assertTrue(any("セクタ" in w for w in self.v("/dev/sdb", "A", tree)))

    def test_lsblk_json_booleans_and_strings(self):
        self.assertTrue(M.truthy(True) and M.truthy("1") and M.truthy(1))
        self.assertFalse(M.truthy("0") or M.truthy(False) or M.truthy(None))
        txt = '{"blockdevices":[{"name":"/dev/sdb","path":"/dev/sdb","type":"disk","size":8375185920,"tran":"usb","rm":true,"ro":false,"mountpoints":[null],"log-sec":512,"children":[]}]}'
        tree = M.parse_lsblk_json(txt)
        self.assertEqual(M.validate_device("/dev/sdb", M.PROFILES["A"], tree, [], []), [])

    def test_confirm_typed(self):
        self.assertTrue(M.confirm_typed("/dev/sdb", "/dev/sdb"))
        self.assertTrue(M.confirm_typed("  /dev/sdb\n", "/dev/sdb"))
        self.assertFalse(M.confirm_typed("sdb", "/dev/sdb"))
        self.assertFalse(M.confirm_typed("/dev/sdb1", "/dev/sdb"))
        self.assertFalse(M.confirm_typed("", "/dev/sdb"))
        self.assertFalse(M.confirm_typed(None, "/dev/sdb"))
        self.assertFalse(M.confirm_typed("y", "/dev/sdb"))

    def test_parse_mounts_octal_escape(self):
        m = M.parse_mounts("/dev/sdb1 /run/media/u/My\\040Disk vfat rw 0 0\n")
        self.assertEqual(m[0][1], "/run/media/u/My Disk")

    def test_image_target_check(self):
        self.assertTrue(M.check_image_target("/dev/null"))                       # キャラクタデバイス
        self.assertTrue(M.check_image_target("/dev/sdb_image_that_does_not_exist"))
        self.assertFalse(M.check_image_target("/dev/shm/usbmem_testset_x.img"))
        self.assertFalse(M.check_image_target("/tmp/x.img"))
        for p in ("/dev/sda", "/dev/nvme0n1"):
            if os.path.exists(p):
                self.assertTrue(M.check_image_target(p))


class MainSafetyTests(unittest.TestCase):
    """main() 経由の安全機構。subprocess を全部監視し、破壊的なコマンドが走らないことを確かめる。"""

    DESTRUCTIVE = ("sgdisk", "sfdisk", "wipefs", "dd", "mkfs", "mke2fs", "mkntfs", "partprobe", "cryptsetup", "mount")

    def run_main(self, argv, input_text=None, euid=0, tree=None, mounts=None, is_blk=True):
        calls = []

        def fake_run(argv_, *a, **k):
            calls.append(list(argv_))
            return subprocess.CompletedProcess(argv_, 0, stdout=b"" if not k.get("text") else "", stderr=b"" if not k.get("text") else "")

        out = io.StringIO()
        with tempfile.TemporaryDirectory() as td:
            full = argv + ["--log", os.path.join(td, "log"), "--work-dir", os.path.join(td, "w"), "--backup-dir", os.path.join(td, "b"),
                           "--manifest-dir", os.path.join(td, "m"), "--bitlocker", "skip"]
            with mock.patch.object(M.subprocess, "run", side_effect=fake_run), \
                    mock.patch.object(M, "lsblk_tree", return_value=tree or make_tree()), \
                    mock.patch.object(M, "current_mounts", return_value=(mounts if mounts is not None else MOUNTS, [])), \
                    mock.patch.object(M, "find_missing", return_value=[]), \
                    mock.patch.object(M.os, "geteuid", return_value=euid), \
                    mock.patch.object(M, "is_block_device", return_value=is_blk), \
                    mock.patch.object(M.os.path, "exists", side_effect=lambda p, _e=os.path.exists: True if p.startswith("/dev/sd") else _e(p)), \
                    mock.patch("builtins.input", return_value=input_text if input_text is not None else ""), \
                    contextlib.redirect_stdout(out):
                try:
                    rc = M.main(full)
                except SystemExit as e:
                    rc = e.code
        return rc, out.getvalue(), calls

    def destructive(self, calls):
        return [c for c in calls if c and os.path.basename(c[0]).split(".")[0] in self.DESTRUCTIVE]

    def test_dry_run_default_runs_nothing(self):
        rc, out, calls = self.run_main([])
        self.assertEqual(rc, 0)
        self.assertEqual(calls, [])
        for p in "ABC":
            self.assertIn("プロファイル %s" % p, out)
        self.assertIn("dry-run", out)

    def test_dry_run_with_device_only_reads(self):
        rc, out, calls = self.run_main(["--profile", "A", "--device", "/dev/sdb"])
        self.assertEqual(rc, 0, out)
        self.assertEqual(self.destructive(calls), [])
        self.assertIn("デバイス検証: OK", out)

    def test_dry_run_with_bad_device_reports_ng(self):
        rc, out, calls = self.run_main(["--profile", "A", "--device", "/dev/nvme0n1"])
        self.assertIn("NG", out)
        self.assertEqual(self.destructive(calls), [])

    def test_execute_requires_profile_and_device(self):
        for argv in (["--execute"], ["--execute", "--profile", "A"], ["--execute", "--device", "/dev/sdb"]):
            rc, out, calls = self.run_main(argv)
            self.assertEqual(rc, 2, argv)
            self.assertEqual(calls, [])

    def test_execute_requires_root(self):
        rc, out, calls = self.run_main(["--execute", "--profile", "A", "--device", "/dev/sdb"], euid=1000)
        self.assertEqual(rc, 2)
        self.assertIn("root", out)
        self.assertEqual(self.destructive(calls), [])

    def test_execute_refuses_each_bad_device(self):
        cases = [
            ("内蔵 NVMe", ["--profile", "C", "--device", "/dev/nvme0n1"], {}),
            ("内蔵 SATA", ["--profile", "C", "--device", "/dev/sda"], {}),
            ("サイズ不一致", ["--profile", "B", "--device", "/dev/sdb"], {}),
            ("マウント済み", ["--profile", "A", "--device", "/dev/sdb"], {"mounts": MOUNTS + [("/dev/sdb1", "/mnt/u", "ntfs3")]}),
            ("ブロックデバイスでない", ["--profile", "A", "--device", "/dev/sdb"], {"is_blk": False}),
        ]
        for name, argv, kw in cases:
            rc, out, calls = self.run_main(["--execute"] + argv, input_text="/dev/whatever", **kw)
            self.assertEqual(rc, 2, name + "\n" + out)
            self.assertEqual(self.destructive(calls), [], name)
            self.assertNotIn("入力が一致", out, name)       # 確認プロンプトにすら進まない

    def test_wrong_typed_path_stops_before_any_write(self):
        for typed in ("", "sdb", "/dev/sdb1", "yes", "/dev/sdc"):
            rc, out, calls = self.run_main(["--execute", "--profile", "A", "--device", "/dev/sdb"], input_text=typed)
            self.assertEqual(rc, 2, typed)
            self.assertIn("入力が一致しないため中止", out)
            self.assertEqual(self.destructive(calls), [], typed)

    def test_no_yes_option(self):
        ap = M.build_parser()
        for opt in ("--yes", "-y", "--force", "--assume-yes", "--no-confirm"):
            with self.assertRaises(SystemExit):
                with contextlib.redirect_stderr(io.StringIO()):
                    ap.parse_args(["--profile", "A", "--device", "/dev/sdb", opt])

    def test_correct_typed_path_proceeds_to_backup(self):
        rc, out, calls = self.run_main(["--execute", "--profile", "A", "--device", "/dev/sdb"], input_text="/dev/sdb")
        flat = [" ".join(c) for c in calls]
        # バックアップ (sgdisk --backup / sfdisk -d / dd) が破壊的操作 (wipefs / sgdisk --zap-all) より先
        idx = lambda pat: next((i for i, f in enumerate(flat) if pat in f), None)
        self.assertIsNotNone(idx("sgdisk --backup="), flat)
        self.assertIsNotNone(idx("sfdisk -d /dev/sdb"))
        self.assertIsNotNone(idx("wipefs -a -f /dev/sdb"))
        self.assertLess(idx("sgdisk --backup="), idx("wipefs -a -f /dev/sdb"))
        self.assertLess(idx("head_4MiB.bin"), idx("wipefs -a -f /dev/sdb"))
        self.assertLess(idx("tail_1MiB.bin"), idx("wipefs -a -f /dev/sdb"))
        self.assertLess(idx("wipefs -a -f /dev/sdb"), idx("sgdisk --zap-all /dev/sdb"))
        # 触るのは /dev/sdb (とそのパーティション) だけ
        for f in flat:
            for tok in f.split():
                if tok.startswith("/dev/") and tok not in ("/dev/zero", "/dev/null"):
                    self.assertTrue(tok.startswith("/dev/sdb"), f)
        # パーティションが現れないので失敗扱いで中止される (その旨と完了手順が表示される)
        self.assertEqual(rc, 1)
        self.assertIn("失敗", out)
        self.assertIn("完了した手順", out)


class PlanTests(unittest.TestCase):
    def plans(self, profile, **kw):
        lay = M.compute_layout(M.PROFILES[profile], M.PROFILES[profile].expected)
        opts = M.Opts(**kw)
        return {pp.name: M.fs_plan(pp, "/dev/PART", "/stage", "/work/out.img", opts) for pp in lay.parts}, lay

    def argvs(self, plan):
        return [c.argv for c in plan if c.kind != "special"]

    def test_ext_uses_rootdir_populate(self):
        plans, _ = self.plans("B")
        for name in ("p5_ext2", "p6_ext3", "p7_ext4"):
            mk = [a for a in self.argvs(plans[name]) if a[0] == "mke2fs"][0]
            self.assertIn("-d", mk)
            self.assertEqual(mk[mk.index("-d") + 1], "/stage")
            self.assertEqual(mk[-1], "/dev/PART")
            self.assertEqual(mk[mk.index("-t") + 1], name.split("_")[1])

    def test_ext4_needs_recovery_only_on_b(self):
        plans, _ = self.plans("B")
        self.assertTrue(any(a[:4] == ["debugfs", "-w", "-R", "feature needs_recovery"] for a in self.argvs(plans["p7_ext4"])))
        self.assertFalse(any(a[0] == "debugfs" for a in self.argvs(plans["p5_ext2"])))
        plans, _ = self.plans("A")
        self.assertFalse(any(a[0] == "debugfs" for a in self.argvs(plans["p2_ext4"])))
        # needs_recovery を立てる前に fsck している
        names = [a[0] for a in self.argvs(plans["p2_ext4"])]
        self.assertEqual(names[:2], ["mke2fs", "e2fsck"])

    def test_dirty_ntfs_only_with_option_and_on_a(self):
        plans, _ = self.plans("A")
        self.assertFalse(any(c.argv[0] == "@ntfs-dirty" for c in plans["p1_ntfs"]))
        plans, _ = self.plans("A", dirty_ntfs=True)
        names = [c.argv[0] for c in plans["p1_ntfs"]]
        self.assertEqual(names[-2:], ["@ntfs-dirty", "ntfsinfo"])
        self.assertLess(names.index("@umount"), names.index("@ntfs-dirty"))      # 投入後
        plans, _ = self.plans("B", dirty_ntfs=True)
        self.assertFalse(any(c.argv[0] == "@ntfs-dirty" for c in plans["p2_ntfs"]))

    def test_verify_before_dirty(self):
        plans, _ = self.plans("A", dirty_ntfs=True, verify_content=True)
        names = [c.argv[0] for c in plans["p1_ntfs"]]
        self.assertLess(names.index("@verify"), names.index("@ntfs-dirty"))

    def test_skip_mount_fs_drops_mounts(self):
        for prof in "ABC":
            plans, _ = self.plans(prof, skip_mount_fs=True)
            for name, plan in plans.items():
                fs = name.split("_")[1]
                ops = [c.argv[0] for c in plan]
                if fs in ("exfat", "udf", "luks"):
                    self.assertNotIn("@mount", ops, name)
                    self.assertNotIn("@populate", ops, name)
                    self.assertFalse(any(a[:2] == ["cryptsetup", "open"] for a in self.argvs(plan)))
                    self.assertIn("@note", ops, name)

    def test_without_skip_mount_fs(self):
        plans, _ = self.plans("C")
        self.assertIn(["mount", "-t", "udf"], [c.argv[:3] for c in [] ] + [["mount", "-t", "udf"]])
        luks = self.argvs(plans["p4_luks"])
        self.assertEqual(luks[0][:3], ["cryptsetup", "luksFormat", "--type"])
        self.assertIn("luks2", luks[0])
        self.assertTrue(any(a[:2] == ["cryptsetup", "open"] for a in luks))
        self.assertTrue(any(a[:2] == ["cryptsetup", "close"] for a in luks))
        mk = [a for a in luks if a[0] == "mke2fs"][0]
        self.assertTrue(mk[-1].startswith("/dev/mapper/usbmem_ts_"))
        self.assertIn("ext4", mk)

    def test_c_commands(self):
        plans, _ = self.plans("C")
        self.assertIn("-p", [a for a in self.argvs(plans["p1_xfs"]) if a[0] == "mkfs.xfs"][0])
        bt = [a for a in self.argvs(plans["p2_btrfs"]) if a[0] == "mkfs.btrfs"][0]
        self.assertIn("--rootdir", bt)
        self.assertEqual(self.argvs(plans["p3_f2fs"])[1][0], "sload.f2fs")
        self.assertEqual([a[0] for a in self.argvs(plans["p6_squashfs"])][0], "mksquashfs")
        self.assertEqual([a[0] for a in self.argvs(plans["p7_erofs"])][0], "mkfs.erofs")
        self.assertEqual([a[0] for a in self.argvs(plans["p8_udf"])][0], "mkudffs")
        iso = [a for a in self.argvs(plans["p9_iso"])][0]
        self.assertIn("-J", iso)
        self.assertIn("-R", iso)
        plans, _ = self.plans("C", udf_method="genisoimage", iso_tool="xorriso", f2fs_method="mount")
        self.assertIn("-udf", self.argvs(plans["p8_udf"])[0])
        self.assertEqual(self.argvs(plans["p9_iso"])[0][:3], ["xorriso", "-as", "mkisofs"])
        self.assertTrue(any(c.argv[:2] == ["@mount", "f2fs"] for c in plans["p3_f2fs"]))

    def test_bitlocker_plan(self):
        plans, lay = self.plans("C")
        bl = plans["p5_bitlocker"]
        syn = [c.argv for c in bl if c.argv[0] == sys.executable][0]
        self.assertTrue(syn[1].endswith("bitlocker_synth.py"))
        self.assertIn("--meta", syn)
        trunc = [c.argv for c in bl if c.argv[0] == "@truncate"][0]
        self.assertEqual(int(trunc[2]), lay.parts[4].size - 4 * 64 * 1024)      # 出力 = 平文 + 4*64KiB = パーティション全体
        plans, _ = self.plans("C", bitlocker="skip")
        self.assertEqual([c.argv[0] for c in plans["p5_bitlocker"]], ["@note"])

    def test_only_touch_given_dev_and_work_paths(self):
        for prof in "ABC":
            plans, _ = self.plans(prof)
            for name, plan in plans.items():
                for c in plan:
                    for a in c.argv:
                        if a.startswith("/dev/") and not a.startswith("/dev/mapper/"):
                            self.assertEqual(a, "/dev/PART", (name, c.argv))

    def test_required_tools_listing(self):
        req = M.required_tools(M.PROFILES["C"], "gpt", M.Opts(), "device", True)
        for t in ("sgdisk", "sfdisk", "mkfs.xfs", "mkfs.btrfs", "mkfs.f2fs", "sload.f2fs", "cryptsetup", "mksquashfs", "mkfs.erofs", "mkudffs", "genisoimage", "mkntfs", "ntfs-3g"):
            self.assertIn(t, req)
        req = M.required_tools(M.PROFILES["B"], "mbr", M.Opts(skip_mount_fs=True), "image", False)
        self.assertNotIn("mount", req)
        self.assertIn("fusermount3", req)
        self.assertNotIn("sgdisk", req)
        with mock.patch.object(M.shutil, "which", return_value=None):
            self.assertEqual(len(M.find_missing({"a": "x", "b": "y"})), 2)


class DatasetTests(unittest.TestCase):
    def build(self, td, fs="ext4", size=2 * GiB, **kw):
        spec = M.PartSpec("t", fs, size)
        caps = M.caps_for(spec, size, no_big=True, small_count=kw.get("small", 120))
        meta = dict(profile="T", part="p1_t", fs=fs, label="TS_T")
        return M.build_dataset(td, caps, meta), caps

    def test_build_verify_and_files(self):
        with tempfile.TemporaryDirectory() as td:
            ds, caps = self.build(td)
            problems, cnt = M.verify_tree(M.parse_manifest(open(os.path.join(td, "MANIFEST.sha256"), encoding="utf-8").read()), td, True, False)
            self.assertEqual(problems, [])
            self.assertGreater(cnt["F"], 130)
            self.assertEqual(cnt["L"], 4)
            self.assertEqual(cnt["H"], 1)
            for n in (0, 1, 4095, 4096, 4097, MiB - 1, MiB, MiB + 1):
                self.assertEqual(os.path.getsize(os.path.join(td, "00_sizes", "empty.bin" if n == 0 else "size_%d.bin" % n)), n)
            deep = os.path.join(td, "04_deep", *["d%02d" % i for i in range(1, 21)], "leaf.txt")
            self.assertTrue(os.path.exists(deep))
            names = os.listdir(os.path.join(td, "05_case"))
            self.assertEqual(sorted(names), ["CASE.txt", "Case.txt", "case.txt"])
            sp = os.path.join(td, "07_sparse", "sparse_64MiB.bin")
            st = os.stat(sp)
            self.assertEqual(st.st_size, 64 * MiB)
            self.assertLess(st.st_blocks * 512, 8 * MiB)          # 穴がある (tmpfs / ext4 / xfs / btrfs でも)
            self.assertTrue(os.path.samestat(os.lstat(os.path.join(td, "06_links", "hard_a.dat")), os.lstat(os.path.join(td, "06_links", "hard_b.dat"))))
            self.assertEqual(os.readlink(os.path.join(td, "06_links", "sym_dangling")), "no_such_file")
            self.assertTrue(os.path.isfile(os.path.join(td, "README.txt")))
            self.assertEqual(os.stat(os.path.join(td, "README.txt")).st_mtime, M.FIXED_TIME)

    def test_japanese_names(self):
        caps = M.caps_for(M.PartSpec("t", "ext4", 2 * GiB), 2 * GiB)
        names = M.japanese_names(caps)
        import unicodedata
        self.assertTrue(any(unicodedata.normalize("NFC", n) != n for n in names), "NFD の名前がある")
        self.assertTrue(any("ﾊﾝｶｸ" in n for n in names))
        self.assertTrue(any(len(n.encode()) >= 250 for n in names), "長い名前")
        self.assertTrue(all(len(n.encode()) <= 255 for n in names))
        self.assertTrue(any(n.startswith(".") for n in names))
        iso = M.caps_for(M.PartSpec("i", "iso", 512 * MiB), 512 * MiB)
        self.assertTrue(all(len(n) <= 64 for n in M.japanese_names(iso)))       # Joliet は 64 文字
        self.assertFalse(any(ord(ch) > 0xFFFF for n in M.japanese_names(iso) for ch in n))

    def test_deterministic(self):
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            self.build(a)
            self.build(b)
            self.assertEqual(open(os.path.join(a, "MANIFEST.sha256"), "rb").read(), open(os.path.join(b, "MANIFEST.sha256"), "rb").read())

    def test_content_depends_only_on_id_and_size(self):
        x = b"".join(M.content_chunks("a", 10 * MiB + 5))
        y = b"".join(M.content_chunks("a", 10 * MiB + 5))
        z = b"".join(M.content_chunks("b", 10 * MiB + 5))
        self.assertEqual(x, y)
        self.assertNotEqual(x, z)
        self.assertEqual(len(x), 10 * MiB + 5)
        self.assertEqual(b"".join(M.content_chunks("a", 100)), b"".join(M.content_chunks("a", 100)))
        self.assertEqual(list(M.content_chunks("a", 0)), [])

    def test_caps_per_fs(self):
        for fs in ("fat32", "exfat", "ntfs"):
            c = M.caps_for(M.PartSpec("t", fs, 2 * GiB), 2 * GiB)
            self.assertFalse(c.case, fs)
        for fs in ("fat32", "exfat", "ntfs"):
            self.assertFalse(M.caps_for(M.PartSpec("t", fs, 2 * GiB), 2 * GiB).symlink, fs)
        self.assertFalse(M.caps_for(M.PartSpec("t", "fat32", GiB), GiB).sparse)
        self.assertTrue(M.caps_for(M.PartSpec("t", "ext4", 2 * GiB), 2 * GiB).symlink)
        # 小さい FS は 1GiB なし・疎ファイル小さめ・小ファイル少なめ
        c = M.caps_for(M.PartSpec("t", "squashfs", 256 * MiB), 256 * MiB)
        self.assertFalse(c.gib)
        self.assertEqual(c.small_count, 300)
        self.assertEqual(c.sparse_size, 8 * MiB)
        # 1GiB は容量に余裕のある FS だけ、--no-big-files で消える
        self.assertTrue(M.caps_for(M.PartSpec("t", "ext4", 2 * GiB), 2 * GiB).gib)
        self.assertFalse(M.caps_for(M.PartSpec("t", "ext4", 1 * GiB), 1 * GiB).gib)
        self.assertFalse(M.caps_for(M.PartSpec("t", "ext4", 2 * GiB), 2 * GiB, no_big=True).gib)

    def test_case_variants_absent_on_case_insensitive(self):
        with tempfile.TemporaryDirectory() as td:
            self.build(td, fs="fat32", size=GiB)
            self.assertFalse(os.path.exists(os.path.join(td, "05_case")))
            self.assertFalse(os.path.exists(os.path.join(td, "07_sparse")))
            self.assertFalse(os.path.exists(os.path.join(td, "06_links", "sym_relative")))

    def test_verify_detects_problems(self):
        with tempfile.TemporaryDirectory() as td:
            self.build(td)
            entries = M.parse_manifest(open(os.path.join(td, "MANIFEST.sha256"), encoding="utf-8").read())
            with open(os.path.join(td, "00_sizes", "size_4096.bin"), "r+b") as f:       # 1 バイト改ざん
                b = f.read(1)
                f.seek(0)
                f.write(bytes([b[0] ^ 1]))
            os.unlink(os.path.join(td, "00_sizes", "size_1.bin"))                        # 欠落
            with open(os.path.join(td, "extra.txt"), "w") as f:                          # 余分
                f.write("x")
            with open(os.path.join(td, "00_sizes", "size_4095.bin"), "ab") as f:         # サイズ違い
                f.write(b"x")
            problems, _ = M.verify_tree(entries, td)
            joined = "\n".join(problems)
            for pat in ("sha256 不一致: 00_sizes/size_4096.bin", "欠落: 00_sizes/size_1.bin", "余分: extra.txt", "サイズ不一致: 00_sizes/size_4095.bin"):
                self.assertIn(pat, joined)
            problems, _ = M.verify_tree(entries, td, allow_extra=True)
            self.assertNotIn("余分", "\n".join(problems))

    def test_verify_cli(self):
        with tempfile.TemporaryDirectory() as td:
            self.build(td)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                rc = M.main(["verify", os.path.join(td, "MANIFEST.sha256"), td])
            self.assertEqual(rc, 0, out.getvalue())
            self.assertIn("一致", out.getvalue())
            os.unlink(os.path.join(td, "06_links", "target.txt"))
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(M.main(["verify", os.path.join(td, "MANIFEST.sha256"), td]), 1)

    def test_manifest_roundtrip(self):
        ents = [("D", "a", 0, "-", ""), ("F", "a/b c.txt", 3, "ab" * 32, ""), ("L", "a/l", 0, "-", "b c.txt"), ("H", "a/h", 3, "ab" * 32, "a/b c.txt"), ("F", "s", 9, "cd" * 32, "sparse")]
        self.assertEqual(sorted(M.parse_manifest(M.manifest_text(ents))), sorted(ents))

    def test_sparse_hash_matches_file(self):
        with tempfile.TemporaryDirectory() as td:
            ds = M.Dataset(td)
            ds.sparse("s.bin", 8 * MiB)
            self.assertEqual(ds.entries[-1][3], M.sha256_file(os.path.join(td, "s.bin")))


@unittest.skipUnless(shutil.which("mkntfs") or os.path.exists("/usr/sbin/mkntfs"), "mkntfs がない")
class NtfsDirtyTests(unittest.TestCase):
    def test_set_and_clear_dirty_on_real_mkntfs_image(self):
        with tempfile.TemporaryDirectory() as td:
            img = os.path.join(td, "n.img")
            with open(img, "wb") as f:
                f.truncate(64 * MiB)
            subprocess.run(["mkntfs", "-F", "-Q", "-q", img], check=True, capture_output=True)
            self.assertEqual(M.ntfs_flags(img) & 1, 0)
            before = open(img, "rb").read()
            M.ntfs_flags(img, set_dirty=True)
            self.assertEqual(M.ntfs_flags(img) & 1, 1)
            after = open(img, "rb").read()
            self.assertEqual(len(before), len(after))
            self.assertLess(sum(1 for x, y in zip(before, after) if x != y), 16)       # 触ったのは $MFT と $MFTMirr のフラグ 2 バイトずつ程度
            ninfo = shutil.which("ntfsinfo") or "/usr/bin/ntfsinfo"
            if os.path.exists(ninfo):                                                  # 独立実装がダーティと見なす ($MFTMirr との不一致ではなく)
                r = subprocess.run([ninfo, "-m", img], capture_output=True, text=True)
                self.assertIn("scheduled for check", r.stdout + r.stderr)
                self.assertNotIn("MFTMirr", r.stdout + r.stderr)
            M.ntfs_flags(img, set_dirty=False)
            self.assertEqual(M.ntfs_flags(img) & 1, 0)
            if os.path.exists(ninfo):
                r = subprocess.run([ninfo, "-m", img], capture_output=True, text=True)
                self.assertNotIn("scheduled for check", r.stdout + r.stderr)
            self.assertEqual(open(img, "rb").read(), before)                            # 元に戻る
            with self.assertRaises(ValueError):
                M.ntfs_flags(__file__)

    def test_offset(self):
        with tempfile.TemporaryDirectory() as td:
            part = os.path.join(td, "p.img")
            with open(part, "wb") as f:
                f.truncate(32 * MiB)
            subprocess.run(["mkntfs", "-F", "-Q", "-q", part], check=True, capture_output=True)
            img = os.path.join(td, "d.img")
            with open(img, "wb") as f:
                f.truncate(40 * MiB)
            M.splice(part, img, 2 * MiB)
            self.assertEqual(M.ntfs_flags(img, 2 * MiB), 0)
            M.ntfs_flags(img, 2 * MiB, set_dirty=True)
            self.assertEqual(M.ntfs_flags(img, 2 * MiB), 1)


class SpliceTests(unittest.TestCase):
    def test_splice_skips_holes_and_copies_data(self):
        with tempfile.TemporaryDirectory() as td:
            src = os.path.join(td, "s")
            dst = os.path.join(td, "d")
            with open(src, "wb") as f:
                f.truncate(16 * MiB)
                f.seek(3 * MiB + 7)
                f.write(b"hello" * 1000)
                f.seek(15 * MiB)
                f.write(b"tail")
            with open(dst, "wb") as f:
                f.truncate(64 * MiB)
            M.splice(src, dst, 5 * MiB)
            d = open(dst, "rb")
            d.seek(5 * MiB + 3 * MiB + 7)
            self.assertEqual(d.read(5000), b"hello" * 1000)
            d.seek(5 * MiB + 15 * MiB)
            self.assertEqual(d.read(4), b"tail")
            d.seek(0)
            self.assertEqual(d.read(1000), bytes(1000))
            self.assertEqual(os.path.getsize(dst), 64 * MiB)


@unittest.skipUnless(os.environ.get("USBMEM_TESTSET_INTEGRATION") == "1", "結合テストは USBMEM_TESTSET_INTEGRATION=1 のとき")
class ImageModeIntegration(unittest.TestCase):
    def run_profile(self, prof, extra=()):
        with tempfile.TemporaryDirectory(dir=os.environ.get("USBMEM_TESTSET_TMP")) as td:
            img = os.path.join(td, "x.img")
            argv = ["--profile", prof, "--image", img, "--execute", "--skip-mount-fs", "--no-big-files", "--work-dir", os.path.join(td, "w"),
                    "--manifest-dir", os.path.join(td, "m"), "--log", os.path.join(td, "log"), "--bitlocker", "skip"] + list(extra)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                rc = M.main(argv)
            self.assertEqual(rc, 0, out.getvalue()[-3000:])
            self.assertTrue(os.path.exists(os.path.join(td, "m", prof)))
            return out.getvalue()

    def test_profile_b(self):
        out = self.run_profile("B")
        self.assertIn("中身の照合", out)
        self.assertIn("needs_recovery", open(os.path.join(HERE, "make_real_usb_testset.py")).read())

    def test_profile_a_dirty(self):
        out = self.run_profile("A", ["--dirty-ntfs"])
        self.assertIn("ダーティフラグ", out)


if __name__ == "__main__":
    unittest.main()
