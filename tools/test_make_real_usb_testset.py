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

    def run_main(self, argv, input_text=None, euid=0, tree=None, mounts=None, is_blk=True, events=None, build=None):
        """イメージ作成 (build_images)・流し込み (flash_all)・読み戻し (verify_flashed) は差し替え、順序を events に記録する。"""
        calls = []
        events = events if events is not None else []

        def fake_run(argv_, *a, **k):
            calls.append(list(argv_))
            events.append(("RUN", list(argv_)))
            return subprocess.CompletedProcess(argv_, 0, stdout=b"" if not k.get("text") else "", stderr=b"" if not k.get("text") else "")

        def fake_build(ctx, layout, imgdir, only=None):
            events.append(("BUILD", imgdir))
            if build is not None:
                return build(ctx, layout, imgdir)
            parts = {pp.name: dict(index=pp.index, name=pp.name, fs=pp.fs, start=pp.start, size=pp.size, file=pp.name + ".img", sha256="ab" * 32,
                                   populated=True, notes=[], checks=[["fsck", True]]) for pp in layout.parts}
            return [], M.make_meta(layout, ctx.opts, parts, {}, True)

        def fake_flash(runner, layout, items, target, is_device, writer, sparse_write=False, progress=False):
            events.append(("FLASH", target, is_device, writer, sparse_write, [it.name for it in items]))

        def fake_verify(runner, items, target, is_device):
            events.append(("VERIFY", target))
            return []

        def fake_input(prompt=""):
            events.append(("CONFIRM", prompt))
            return input_text if input_text is not None else ""

        out = io.StringIO()
        with tempfile.TemporaryDirectory() as td:
            full = argv + ["--log", os.path.join(td, "log"), "--work-dir", os.path.join(td, "w"), "--backup-dir", os.path.join(td, "b"),
                           "--manifest-dir", os.path.join(td, "m"), "--bitlocker", "skip"]
            with mock.patch.object(M.subprocess, "run", side_effect=fake_run), \
                    mock.patch.object(M, "lsblk_tree", return_value=tree or make_tree()), \
                    mock.patch.object(M, "current_mounts", return_value=(mounts if mounts is not None else MOUNTS, [])), \
                    mock.patch.object(M, "find_missing", return_value=[]), \
                    mock.patch.object(M, "build_images", side_effect=fake_build), \
                    mock.patch.object(M, "flash_all", side_effect=fake_flash), \
                    mock.patch.object(M, "verify_flashed", side_effect=fake_verify), \
                    mock.patch.object(M.os, "geteuid", return_value=euid), \
                    mock.patch.object(M, "is_block_device", return_value=is_blk), \
                    mock.patch.object(M.os.path, "exists", side_effect=lambda p, _e=os.path.exists: True if p.startswith("/dev/sd") else _e(p)), \
                    mock.patch("builtins.input", side_effect=fake_input), \
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
        ev = []
        rc, out, calls = self.run_main(["--execute", "--profile", "A", "--device", "/dev/sdb"], input_text="/dev/sdb", events=ev)
        flat = [" ".join(c) for c in calls]
        idx = lambda pat: next((i for i, f in enumerate(flat) if pat in f), None)
        self.assertIsNotNone(idx("sgdisk --backup="), flat)
        self.assertIsNotNone(idx("sfdisk -d /dev/sdb"))
        self.assertIsNotNone(idx("wipefs -a -f /dev/sdb"))
        self.assertLess(idx("sgdisk --backup="), idx("wipefs -a -f /dev/sdb"))
        self.assertLess(idx("head_4MiB.bin"), idx("wipefs -a -f /dev/sdb"))
        self.assertLess(idx("tail_1MiB.bin"), idx("wipefs -a -f /dev/sdb"))
        # 触るのは /dev/sdb (とそのパーティション) だけ。全デバイス対象の sync は使わない
        for f in flat:
            for tok in f.split():
                if tok.startswith("/dev/") and tok not in ("/dev/zero", "/dev/null"):
                    self.assertTrue(tok.startswith("/dev/sdb"), f)
            self.assertNotEqual(f.split()[0], "sync", f)
        self.assertEqual(rc, 0, out)

    def test_execution_order_build_confirm_backup_flash_verify(self):
        """(a) イメージ作成 -> (b) 確認 -> (c) バックアップ -> 消去 -> (d) 流し込み -> (e) 読み戻し。デバイスに触るのは確認より後だけ。"""
        ev = []
        rc, out, calls = self.run_main(["--execute", "--profile", "A", "--device", "/dev/sdb"], input_text="/dev/sdb", events=ev)
        self.assertEqual(rc, 0, out)
        kinds = [e[0] for e in ev]
        pos = lambda k: kinds.index(k)
        self.assertLess(pos("BUILD"), pos("CONFIRM"))
        self.assertLess(pos("CONFIRM"), pos("FLASH"))
        self.assertLess(pos("FLASH"), pos("VERIFY"))
        flat = [" ".join(e[1]) if e[0] == "RUN" else e[0] for e in ev]
        first_backup = next(i for i, f in enumerate(flat) if "sgdisk --backup=" in f)
        wipe = next(i for i, f in enumerate(flat) if f.startswith("wipefs -a -f /dev/sdb"))
        self.assertGreater(first_backup, pos("CONFIRM"))
        self.assertLess(first_backup, wipe)
        self.assertLess(wipe, pos("FLASH"))
        for e in ev[:pos("BUILD")]:                      # BUILD より前に /dev/sdb を触るコマンドは無い
            if e[0] == "RUN":
                self.assertNotIn("/dev/sdb", " ".join(e[1]), e)
        fl = ev[pos("FLASH")]
        self.assertEqual((fl[1], fl[2], fl[4]), ("/dev/sdb", True, False))       # 既定ではゼロも含めて全部書く (--sparse-write なし)
        self.assertEqual(fl[5][-1], "table@0B")                                    # 表は最後

    def test_build_failure_never_touches_device(self):
        ev = []

        def boom(ctx, layout, imgdir):
            raise M.StepError("mke2fs が失敗")

        rc, out, calls = self.run_main(["--execute", "--profile", "A", "--device", "/dev/sdb"], input_text="/dev/sdb", events=ev, build=boom)
        self.assertEqual(rc, 1)
        self.assertIn("デバイスには一切書き込んでいません", out)
        self.assertEqual(self.destructive(calls), [])
        self.assertNotIn("CONFIRM", [e[0] for e in ev])

    def test_image_check_ng_stops_before_confirm(self):
        """イメージの検査 (fsck 相当) が NG なら、デバイスの確認にも進まない。--ignore-image-checks で進める。"""
        def ng(ctx, layout, imgdir):
            parts = {pp.name: dict(index=pp.index, name=pp.name, fs=pp.fs, start=pp.start, size=pp.size, file=pp.name + ".img", sha256="ab" * 32,
                                   populated=True, notes=[], checks=[["fsck", False]]) for pp in layout.parts}
            st = dict(ds=None, notes=[], checks=[("fsck", False)])
            return [(layout.parts[0], st)], M.make_meta(layout, ctx.opts, parts, {}, True)

        ev = []
        rc, out, calls = self.run_main(["--execute", "--profile", "A", "--device", "/dev/sdb"], input_text="/dev/sdb", events=ev, build=ng)
        self.assertEqual(rc, 1, out)
        self.assertIn("検査", out)
        self.assertNotIn("CONFIRM", [e[0] for e in ev])
        self.assertEqual(self.destructive(calls), [])
        ev = []
        rc, out, calls = self.run_main(["--execute", "--profile", "A", "--device", "/dev/sdb", "--ignore-image-checks"], input_text="/dev/sdb", events=ev, build=ng)
        self.assertIn("FLASH", [e[0] for e in ev])

    def test_sparse_write_flag_reaches_flasher(self):
        ev = []
        rc, out, calls = self.run_main(["--execute", "--profile", "A", "--device", "/dev/sdb", "--sparse-write"], input_text="/dev/sdb", events=ev)
        self.assertEqual(rc, 0, out)
        fl = next(e for e in ev if e[0] == "FLASH")
        self.assertTrue(fl[4])

    def _meta_dir(self, td, **over):
        lay = M.compute_layout(M.PROFILES["A"], M.PROFILES["A"].expected)
        parts = {pp.name: dict(index=pp.index, name=pp.name, fs=pp.fs, start=pp.start, size=pp.size, file=pp.name + ".img", sha256="", populated=True, notes=[], checks=[])
                 for pp in lay.parts}
        meta = M.make_meta(lay, M.Opts(), parts, {}, True)
        meta.update(over)
        M.write_meta(td, meta)
        for pp in lay.parts:
            with open(os.path.join(td, pp.name + ".img"), "wb") as f:
                f.truncate(pp.size)
        with open(os.path.join(td, M.TABLE_IMG), "wb") as f:
            f.truncate(lay.dev_size)

    def test_flash_only_rejects_other_capacity_without_touching(self):
        with tempfile.TemporaryDirectory() as td:
            self._meta_dir(td, dev_size=M.PROFILES["A"].expected + 4096)         # 別の容量用のイメージ一式
            ev = []
            rc, out, calls = self.run_main(["--execute", "--flash-only", td, "--device", "/dev/sdb"], input_text="/dev/sdb", events=ev)
            self.assertEqual(rc, 2, out)
            self.assertIn("流せません", out)
            self.assertNotIn("CONFIRM", [e[0] for e in ev])
            self.assertEqual(self.destructive(calls), [])

    def test_flash_only_rejects_incomplete_set(self):
        with tempfile.TemporaryDirectory() as td:
            self._meta_dir(td, complete=False)
            ev = []
            rc, out, calls = self.run_main(["--execute", "--flash-only", td, "--device", "/dev/sdb"], input_text="/dev/sdb", events=ev)
            self.assertEqual(rc, 2, out)
            self.assertEqual(self.destructive(calls), [])

    def test_flash_only_rejects_other_profile(self):
        with tempfile.TemporaryDirectory() as td:
            self._meta_dir(td)
            rc, out, calls = self.run_main(["--execute", "--flash-only", td, "--profile", "C", "--device", "/dev/sdb"], input_text="/dev/sdb")
            self.assertEqual(rc, 2, out)
            self.assertIn("プロファイル", out)

    def test_flash_only_skips_build(self):
        with tempfile.TemporaryDirectory() as td:
            self._meta_dir(td)
            ev = []
            rc, out, calls = self.run_main(["--execute", "--flash-only", td, "--device", "/dev/sdb", "--skip-image-check"], input_text="/dev/sdb", events=ev)
            self.assertEqual(rc, 0, out)
            kinds = [e[0] for e in ev]
            self.assertNotIn("BUILD", kinds)
            self.assertLess(kinds.index("CONFIRM"), kinds.index("FLASH"))


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


class FlashRangeTests(unittest.TestCase):
    """流し込み範囲の計算 (パーティションの範囲・パーティション表の領域・GPT のバックアップ位置・未割り当て)。"""

    def lay(self, p):
        return M.compute_layout(M.PROFILES[p], M.PROFILES[p].expected)

    def test_gpt_table_regions_head_and_tail(self):
        lay = self.lay("A")
        self.assertEqual(M.table_regions(lay), [(0, M.LEAD), (lay.dev_size - M.TAIL_RESERVE, M.TAIL_RESERVE)])
        # GPT のバックアップ (エントリ表 32 セクタ + ヘッダ 1 セクタ) は末尾 1 MiB に収まる
        self.assertGreaterEqual(M.TAIL_RESERVE, 33 * 512)

    def test_mbr_table_regions_include_ebr_gaps(self):
        lay = self.lay("B")
        regs = M.table_regions(lay)
        logical = [pp for pp in lay.parts if pp.spec.kind == "logical"]
        self.assertEqual(len(logical), 3)
        for pp in logical:
            self.assertIn((pp.start - M.ALIGN, M.ALIGN), regs)          # 各論理パーティションの直前 1 MiB (EBR)
        self.assertEqual(regs[0], (0, M.LEAD))

    def test_flash_items_partitions_then_table_last(self):
        for p in "ABC":
            lay = self.lay(p)
            items = M.flash_items(lay, "/img")
            parts = [it for it in items if it.kind == "part"]
            self.assertEqual([it.name for it in parts], [pp.name for pp in lay.parts])
            for it, pp in zip(parts, lay.parts):
                self.assertEqual((it.dst_off, it.length, it.src_off), (pp.start, pp.size, 0))
            self.assertEqual(items[-1].name, "table@0B")                  # 先頭 (表本体) が最後
            self.assertTrue(all(it.kind == "table" for it in items[len(parts):]))
            self.assertTrue(all(it.src == "/img/" + M.TABLE_IMG and it.src_off == it.dst_off for it in items if it.kind == "table"))

    def test_flash_items_never_overlap_and_stay_in_device(self):
        for p in "ABC":
            lay = self.lay(p)
            items = sorted(M.flash_items(lay, "/img"), key=lambda it: it.dst_off)
            for a, b in zip(items, items[1:]):
                self.assertLessEqual(a.dst_end, b.dst_off, (p, a.name, b.name))
            self.assertLessEqual(items[-1].dst_end, lay.dev_size)

    def test_unallocated_ranges_are_the_complement(self):
        for p in "ABC":
            lay = self.lay(p)
            items = M.flash_items(lay, "/img")
            un = M.unallocated_ranges(lay)
            self.assertEqual(sum(it.length for it in items) + sum(n for _, n in un), lay.dev_size, p)
            for off, n in un:
                for it in items:
                    self.assertTrue(off + n <= it.dst_off or it.dst_end <= off, (p, off, n, it.name))
        self.assertGreater(sum(n for _, n in M.unallocated_ranges(self.lay("C"))), 10 * GiB)

    def test_erase_covers_first_4mib_and_last_1mib_only(self):
        flat = [" ".join(c.argv) for c in M.erase_cmds("/dev/sdb", 8 * GiB)]
        self.assertTrue(any(f.startswith("wipefs -a -f /dev/sdb") for f in flat))
        self.assertTrue(any("count=4" in f and "of=/dev/sdb" in f for f in flat))
        self.assertTrue(any("seek=%d" % (8 * GiB - MiB) in f and "oflag=seek_bytes" in f for f in flat))
        self.assertFalse(any(f.split()[0] == "sync" for f in flat))

    def test_nbd_range_spec_tokens_are_separate_argv(self):
        a = M.nbd_range_spec("/dev/sdb", 1048576, 4096, direct=True)
        self.assertEqual((a[0], a[-1]), ("[", "]"))
        for tok in ("--filter=offset", "cache=none", "offset=1048576", "range=4096"):
            self.assertIn(tok, a)
        self.assertNotIn("-s", a)
        self.assertTrue(all(" " not in x for x in a))

    def test_flash_cmd_writes_zeros_by_default(self):
        it = M.FlashItem("p1", "/img/p1.img", 0, 1048576, 4096)
        c = M.flash_cmd(it, "/dev/sdb", "nbdcopy", direct=True).argv
        self.assertIn("--sparse=0", c)
        self.assertIn("--no-extents", c)
        self.assertNotIn("--destination-is-zero", c)
        self.assertIn("cache=none", c)                                  # O_DIRECT
        self.assertIn("--flush", c)
        c = M.flash_cmd(it, "/dev/sdb", "nbdcopy", direct=True, dest_zero=True).argv
        self.assertIn("--destination-is-zero", c)
        self.assertNotIn("--sparse=0", c)
        d = M.flash_cmd(it, "/dev/sdb", "dd", direct=True).argv
        self.assertIn("oflag=seek_bytes,direct", d)
        self.assertIn("conv=notrunc,fsync", d)                          # 疎書き込み (conv=sparse) なし
        self.assertIn("bs=4M", d)
        d = M.flash_cmd(it, "/dev/sdb", "dd", direct=True, dest_zero=True).argv
        self.assertIn("conv=notrunc,fsync,sparse", d)

    def test_table_item_reads_from_table_image_range(self):
        it = M.FlashItem("table@0B", "/img/disk_table.img", 0, 0, M.LEAD, kind="table")
        c = M.flash_cmd(it, "/dev/sdb", "nbdcopy").argv
        self.assertEqual(c[c.index("--") + 1], "[")                    # 送り元も範囲指定 (表のイメージはデバイスと同じ大きさ)
        self.assertIn("range=%d" % M.LEAD, c)

    def test_prezero_for_sparse_write(self):
        it = M.FlashItem("p1", "/img/p1.img", 0, 1048576, 4096)
        z = M.prezero_cmds(it, "/dev/sdb")
        self.assertEqual(z[0].argv[:2], ["blkdiscard", "-z"])
        self.assertIn("-o", z[0].argv)
        self.assertIn("seek=1048576", z[1].argv)

    def test_flash_all_sparse_write_prezeroes_then_dest_zero(self):
        runner = mock.Mock()
        runner.run.return_value = (0, "")
        lay = self.lay("A")
        items = M.flash_items(lay, "/img")
        M.flash_all(runner, lay, items, "/dev/sdb", True, "nbdcopy", sparse_write=True)
        cmds = [c.args[0].argv for c in runner.run.call_args_list]
        self.assertTrue(any(c[0] == "blkdiscard" for c in cmds))
        self.assertTrue(all("--destination-is-zero" in c for c in cmds if c[0] == "nbdcopy"))
        self.assertEqual(cmds[-1], ["blockdev", "--flushbufs", "/dev/sdb"])
        self.assertFalse(any(c[0] == "sync" for c in cmds))
        runner.reset_mock()
        runner.run.return_value = (0, "")
        M.flash_all(runner, lay, items, "/dev/sdb", True, "nbdcopy")             # 既定 (--sparse-write なし) はゼロも書く
        cmds = [c.args[0].argv for c in runner.run.call_args_list]
        self.assertFalse(any(c[0] == "blkdiscard" for c in cmds))
        self.assertTrue(all("--sparse=0" in c for c in cmds if c[0] == "nbdcopy"))

    def test_flash_to_file_and_verify_roundtrip(self):
        """dd 方式で通常ファイルへ流し込み、読み戻しが一致する。壊すと不一致を検出する。範囲外は触らない。"""
        with tempfile.TemporaryDirectory() as td:
            dev_size = 16 * MiB
            tbl = os.path.join(td, "t.img")
            with open(tbl, "wb") as f:
                f.truncate(dev_size)
                f.write(b"TABLE" * 1000)
            a = os.path.join(td, "a.img")
            with open(a, "wb") as f:
                f.write(os.urandom(2 * MiB))
                f.truncate(4 * MiB)                       # 後半は穴
            items = [M.FlashItem("a", a, 0, 2 * MiB, 4 * MiB, M.hash_sparse_file(a)),
                     M.FlashItem("table@0B", tbl, 0, 0, MiB, M.read_range_hash(tbl, 0, MiB), "table")]
            dev = os.path.join(td, "dev.img")
            with open(dev, "wb") as f:
                f.write(os.urandom(dev_size))             # 古いデータ
            old_tail = M.read_range_hash(dev, 8 * MiB, 8 * MiB)
            runner = M.Runner(os.devnull)
            real_run = runner.run
            runner.run = lambda cmd, check=True, timeout=None, live=False: (0, "") if cmd.argv[0] == "blockdev" else real_run(cmd, check=check, timeout=timeout, live=live)
            with contextlib.redirect_stdout(io.StringIO()):
                M.flash_all(runner, mock.Mock(dev_size=dev_size), items, dev, True, "dd")
                self.assertEqual(M.verify_flashed(runner, items, dev, True), [])
            self.assertEqual(M.read_range_hash(dev, 4 * MiB, 2 * MiB), M.hashlib.sha256(bytes(2 * MiB)).hexdigest())     # 穴もゼロとして書かれている
            self.assertEqual(M.read_range_hash(dev, 8 * MiB, 8 * MiB), old_tail)                                          # 範囲外は無傷
            with open(dev, "r+b") as f:
                f.seek(3 * MiB)
                f.write(b"X")
            with contextlib.redirect_stdout(io.StringIO()):
                bad = M.verify_flashed(runner, items, dev, True)
            self.assertEqual(len(bad), 1)


class ImageSetMetaTests(unittest.TestCase):
    def setUp(self):
        self.lay = M.compute_layout(M.PROFILES["A"], M.PROFILES["A"].expected)

    def meta(self, complete=True):
        parts = {pp.name: dict(index=pp.index, name=pp.name, fs=pp.fs, start=pp.start, size=pp.size, file=pp.name + ".img", sha256="", populated=True, notes=[], checks=[])
                 for pp in self.lay.parts}
        return M.make_meta(self.lay, M.Opts(), parts, {}, complete)

    def test_meta_has_profile_size_and_script_version(self):
        m = self.meta()
        self.assertEqual((m["profile"], m["dev_size"], m["ss"]), ("A", self.lay.dev_size, 512))
        self.assertEqual(len(m["script_sha256"]), 64)
        self.assertEqual(M.meta_problems(m, self.lay), [])

    def test_rejects_other_capacity(self):
        other = M.compute_layout(M.PROFILES["A"], M.PROFILES["A"].expected - 100 * MiB)
        why = M.meta_problems(self.meta(), other)
        self.assertTrue(any("容量が違う" in w for w in why), why)

    def test_rejects_other_profile_and_sector_size(self):
        why = M.meta_problems(self.meta(), M.compute_layout(M.PROFILES["B"], M.PROFILES["B"].expected))
        self.assertTrue(any("プロファイル" in w for w in why), why)
        m = self.meta()
        m["ss"] = 4096
        self.assertTrue(any("セクタ" in w for w in M.meta_problems(m, self.lay)))

    def test_rejects_incomplete_and_wrong_format(self):
        self.assertTrue(any("不完全" in w for w in M.meta_problems(self.meta(complete=False), self.lay)))
        m = self.meta()
        m["format"] = 999
        self.assertTrue(any("形式" in w for w in M.meta_problems(m, self.lay)))

    def test_rejects_changed_layout(self):
        m = self.meta()
        m["layout"]["parts"][0]["size"] += 1048576
        self.assertTrue(any("配置が" in w for w in M.meta_problems(m, self.lay)))

    def test_checks_files_in_dir(self):
        with tempfile.TemporaryDirectory() as td:
            m = self.meta()
            self.assertTrue(any("が無い" in w for w in M.meta_problems(m, self.lay, td)))
            for pp in self.lay.parts:
                with open(os.path.join(td, pp.name + ".img"), "wb") as f:
                    f.truncate(pp.size - 4096)
            with open(os.path.join(td, M.TABLE_IMG), "wb") as f:
                f.truncate(self.lay.dev_size)
            self.assertTrue(any("サイズが違う" in w for w in M.meta_problems(m, self.lay, td)))

    def test_write_and_load_roundtrip_and_errors(self):
        with tempfile.TemporaryDirectory() as td:
            m = self.meta()
            M.write_meta(td, m)
            self.assertEqual(M.load_meta(td)["dev_size"], m["dev_size"])
            self.assertEqual([f for f in os.listdir(td) if f.endswith(".tmp")], [])
        with tempfile.TemporaryDirectory() as td:
            with self.assertRaises(M.StepError):
                M.load_meta(td)
            with open(os.path.join(td, M.META_NAME), "w") as f:
                f.write("{broken")
            with self.assertRaises(M.StepError):
                M.load_meta(td)

    def test_image_hash_check_detects_modification(self):
        with tempfile.TemporaryDirectory() as td:
            img = os.path.join(td, "p.img")
            with open(img, "wb") as f:
                f.write(b"abc" * 1000)
                f.truncate(MiB)
            meta = {"parts": [dict(name="p", file="p.img", sha256=M.hash_sparse_file(img))], "table_image": {"regions": []}}
            runner = mock.Mock()
            self.assertEqual(M.check_image_hashes(runner, td, meta), [])
            with open(img, "r+b") as f:
                f.seek(500000)
                f.write(b"!")
            self.assertEqual(len(M.check_image_hashes(runner, td, meta)), 1)


class RootFreePlanTests(unittest.TestCase):
    def plans(self, profile, **kw):
        lay = M.compute_layout(M.PROFILES[profile], M.PROFILES[profile].expected)
        return {pp.name: M.fs_plan(pp, "/img/PART", "/stage", "/work/out.img", M.Opts(**kw)) for pp in lay.parts}

    def names(self, plan):
        return [c.argv[0] for c in plan]

    def test_exfat_fuse_is_root_free(self):
        p = self.plans("B", exfat_method="fuse")["p3_exfat"]
        self.assertEqual([c.argv[1] for c in p if c.argv[0] == "@mount"], ["exfat-fuse"])
        self.assertIn("tune.exfat", self.names(p))          # GUID は投入のあとに付ける (exfat-fuse は GUID エントリ付きを拒否する)
        mk = [c.argv for c in p if c.argv[0] == "mkfs.exfat"][0]
        self.assertNotIn("-U", mk)
        self.assertFalse(M.root_needed("exfat", M.Opts(exfat_method="fuse")))
        self.assertTrue(M.root_needed("exfat", M.Opts(exfat_method="mount")))

    def test_exfat_mount_keeps_guid_in_mkfs(self):
        p = self.plans("B", exfat_method="mount")["p3_exfat"]
        mk = [c.argv for c in p if c.argv[0] == "mkfs.exfat"][0]
        self.assertIn("-U", mk)
        self.assertNotIn("tune.exfat", self.names(p))

    def test_luks_xts_has_no_cryptsetup_open(self):
        p = self.plans("C", luks_method="xts")["p4_luks"]
        n = self.names(p)
        self.assertLess(n.index("@luks-inner"), n.index("@luks-encrypt"))
        self.assertFalse(any(c.argv[:2] == ["cryptsetup", "open"] for c in p))
        self.assertFalse(M.root_needed("luks", M.Opts(luks_method="xts")))
        self.assertTrue(M.root_needed("luks", M.Opts(luks_method="open")))

    def test_root_needed_parts(self):
        prof = M.PROFILES["C"]
        free = M.root_needed_parts(prof, M.Opts(exfat_method="fuse", luks_method="xts", udf_method="genisoimage", f2fs_method="sload"))
        self.assertEqual(free, [])
        rn = M.root_needed_parts(prof, M.Opts(luks_method="open", udf_method="mkudffs", f2fs_method="mount"))
        for name in ("udf", "f2fs", "luks"):
            self.assertIn(name, rn)

    def test_no_plan_runs_global_sync_or_partprobe(self):
        for prof in "ABC":
            for kw in (dict(), dict(exfat_method="fuse", luks_method="xts", udf_method="genisoimage")):
                for name, plan in self.plans(prof, **kw).items():
                    for c in plan:
                        self.assertNotEqual(c.argv[0], "sync", (name, c.argv))      # sync -f <ファイルシステム> だけ (@populate 内) を使う
                        self.assertNotIn("partprobe", c.argv[0])

    def test_table_cmds_run_on_image_only(self):
        for prof in "ABC":
            lay = M.compute_layout(M.PROFILES[prof], M.PROFILES[prof].expected)
            for c in M.table_cmds(lay, "/img/disk_table.img"):
                self.assertIn("/img/disk_table.img", c.argv)
                self.assertNotIn("partprobe", c.argv[0])

    def test_resolve_methods(self):
        ap = M.build_parser()
        args = ap.parse_args([])
        with mock.patch.object(M.shutil, "which", return_value="/usr/bin/x"):
            self.assertEqual(M.resolve_methods(args, 0), ("mount", "open"))                 # root は従来どおり
            self.assertEqual(M.resolve_methods(args, 1000)[0], "fuse")
        with mock.patch.object(M.shutil, "which", return_value=None):
            self.assertEqual(M.resolve_methods(args, 1000)[0], "mount")                     # exfat-fuse が無ければ mount のまま
        args = ap.parse_args(["--exfat-method", "mount", "--luks-method", "open"])
        self.assertEqual(M.resolve_methods(args, 1000), ("mount", "open"))

    def test_estimate_images_need(self):
        lay = M.compute_layout(M.PROFILES["C"], M.PROFILES["C"].expected)
        full = M.estimate_images_need(lay, M.Opts())
        small = M.estimate_images_need(lay, M.Opts(no_big=True))
        self.assertGreater(full, small)
        self.assertLess(full, lay.dev_size)
        # LUKS / BitLocker 風は全面が非ゼロになるので、そのサイズ分は必ず見込む
        self.assertGreaterEqual(small, sum(pp.size for pp in lay.parts if pp.fs in ("luks", "bitlocker")))


class LuksXtsTests(unittest.TestCase):
    def setUp(self):
        try:
            import cryptography  # noqa: F401
        except ImportError:
            self.skipTest("cryptography が無い")

    def test_xts_matches_independent_implementation(self):
        key = bytes(range(64))
        data = bytes((i * 7) & 0xFF for i in range(512 * 4))
        for first in (0, 1, 12345, 2 ** 33 + 5):
            enc = M.xts_crypt_buffer(key, data, first)
            for i in range(4):
                sec = data[i * 512:(i + 1) * 512]
                self.assertEqual(enc[i * 512:(i + 1) * 512], M.reference_xts_sector(key, first + i, sec))
                self.assertEqual(M.reference_xts_sector(key, first + i, enc[i * 512:(i + 1) * 512], decrypt=True), sec)
            self.assertNotEqual(enc, data)

    def test_xts_tweak_is_plain64_little_endian(self):
        self.assertEqual(M.xts_tweak(1), (1).to_bytes(16, "little"))
        self.assertEqual(M.xts_tweak(0x0102030405060708), (0x0102030405060708).to_bytes(16, "little"))


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


class WorkdirRamTests(unittest.TestCase):
    def test_overlay_with_tmpfs_upper_is_ram(self):
        mi = M.parse_mountinfo(LIVE_MI_FOR_RAM)
        self.assertTrue(M.workdir_is_ram(mi, "/home/u/Downloads/usbmem_testset_work"))

    def test_real_disk_is_not_ram(self):
        mi = M.parse_mountinfo(LIVE_MI_FOR_RAM + "40 38 259:1 / /mnt/data rw - ext4 /dev/nvme0n1p3 rw\n")
        self.assertFalse(M.workdir_is_ram(mi, "/mnt/data/work"))

    def test_tmpfs_is_ram(self):
        mi = M.parse_mountinfo("1 0 0:1 / / rw - ext4 /dev/sda1 rw\n2 1 0:2 / /dev/shm rw - tmpfs tmpfs rw\n")
        self.assertTrue(M.workdir_is_ram(mi, "/dev/shm/x"))
        self.assertFalse(M.workdir_is_ram(mi, "/home/x"))


LIVE_MI_FOR_RAM = (
    "35 31 0:31 / /run/live/overlay rw - tmpfs tmpfs rw,size=1k\n"
    "38 2 0:32 / / rw - overlay overlay rw,lowerdir=/run/live/rootfs/filesystem.squashfs/,upperdir=/run/live/overlay/rw,workdir=/run/live/overlay/work\n")


if __name__ == "__main__":
    unittest.main()
