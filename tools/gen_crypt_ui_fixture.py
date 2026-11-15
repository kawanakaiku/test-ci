#!/usr/bin/env python3
"""暗号ボリュームの UI 試験 (android/app/src/androidTest/.../CryptoUiTest.kt) 用のイメージを作る。tools/ci_android_tests.sh の crypt-ui グループが呼ぶ。
使い方: tools/gen_crypt_ui_fixture.py <出力ディレクトリ>
作るもの (すべて使い捨てのパスワード。イメージは tools/gen_cryptodisk_fixtures.py の Builder で作る。root 不要):
  gpt.img    GPT のディスク。#1 = LUKS2 (argon2id) → ext4 (seed 5001) / #2 = BitLocker (合成, パスワード) → NTFS (seed 5002)
  whole.img  ディスク全体が LUKS2 (pbkdf2) で、その中に GPT (ext4 seed 5011 / NTFS seed 5012)。表の無い暗号ボリューム + 中の表の試験
  bl.img     ディスク全体が BitLocker (合成)。保護はパスワード + 回復キー + BEK ファイル。中は NTFS (seed 5021)
  bl.ref_recovery  回復キー (48 桁をハイフン区切りにしたもの)。bl.ref_bek  BEK ファイル
  pw.ref_creds     パスワード (KEY=VALUE。cui_gpt_luks / cui_gpt_bl / cui_whole / cui_bl)。試験が読む
中身のファイルツリーは gen_cryptodisk_fixtures.make_tree と同じ (seed.txt = "volume seed <seed>\\n" など)。
"""
import importlib.util
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("gcf", os.path.join(HERE, "gen_cryptodisk_fixtures.py"))
gcf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gcf)

PW = {
    "cui_gpt_luks": "cui-luks-Pass1",
    "cui_gpt_bl": "cui-bl-Pass1",
    "cui_whole": "cui-whole-Pass1",
    "cui_bl": "cui-bl2-Pass1",
}
SMALL = 6  # 中の NTFS (MiB)
EXT4_MIB = 24  # 中の ext4 (MiB)。書き込み可で開けるよう、書き込みの試験 (gen_ext4_fixture.py の e4_def4k) と同じ機能で作る (内部ジャーナル・4K ブロック)


def make_ext4_writable(path, size, tree, label):
    with open(path, "wb") as f:
        f.truncate(size)
    gcf.run(["mke2fs", "-q", "-t", "ext4", "-b", "4096", "-I", "256", "-O", "has_journal,extent,flex_bg,metadata_csum,64bit,huge_file,dir_nlink,extra_isize",
             "-F", "-L", label, "-E", "root_owner=0:0", "-d", tree, path])


gcf.make_ext4 = make_ext4_writable


def main():
    out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "/tmp/usbmem_crypt_ui")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    b = gcf.Builder(out)
    # 1. GPT の中に LUKS2 / BitLocker
    gpt = b.disk("gpt", [b.enc(b.fs("ext4", 5001, "CUI_LUKS", EXT4_MIB), "luks2-argon2id", PW["cui_gpt_luks"], 5001),
                         b.enc(b.fs("ntfs", 5002, "CUI_BL", SMALL), "bitlocker", PW["cui_gpt_bl"], 5002)])
    os.replace(gpt.path, os.path.join(out, "gpt.img"))
    # 2. 全体が LUKS2、その中に GPT
    inner = b.disk("gpt", [b.fs("ext4", 5011, "CUI_W1", EXT4_MIB), b.fs("ntfs", 5012, "CUI_W2", SMALL)])
    whole = b.enc(inner, "luks2-pbkdf2", PW["cui_whole"], 5010)
    os.replace(whole.path, os.path.join(out, "whole.img"))
    # 3. 全体が BitLocker (パスワード + 回復キー + BEK)
    n = b.fs("ntfs", 5021, "CUI_BL2", SMALL)
    img = os.path.join(out, "bl.img")
    meta = os.path.join(b.work, "bl.meta")
    r = subprocess.run([sys.executable, os.path.join(HERE, "bitlocker_synth.py"), n.path, img, "--method", "8004", "--protectors", "pw,rec,bek",
                        "--password", PW["cui_bl"], "--seed", "cui5021", "--meta", meta], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if r.returncode != 0:
        raise SystemExit("bitlocker_synth failed: " + (r.stdout + r.stderr).decode(errors="replace")[-500:])
    rec = [l.split("=", 1)[1] for l in open(meta).read().splitlines() if l.startswith("recovery=")][0]
    open(os.path.join(out, "bl.ref_recovery"), "w").write(rec + "\n")
    os.replace(img + ".bek", os.path.join(out, "bl.ref_bek"))
    with open(os.path.join(out, "pw.ref_creds"), "w") as f:
        for k, v in PW.items():
            f.write("%s=%s\n" % (k, v))
    shutil.rmtree(b.work, ignore_errors=True)
    shutil.rmtree(b.man, ignore_errors=True)
    os.unlink(os.path.join(out, "cases.txt")) if os.path.exists(os.path.join(out, "cases.txt")) else None
    for f in sorted(os.listdir(out)):
        print("%s %d" % (f, os.path.getsize(os.path.join(out, f))))


if __name__ == "__main__":
    main()
