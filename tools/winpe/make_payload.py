#!/usr/bin/env python3
"""Windows (WinPE) に NTFS を作らせるためのペイロード CD を生成する。
出力: <out>/payload.iso (autounattend.xml + usbmem_make.cmd + data/) と <out>/manifest.txt (ホスト側で計算した期待値)。
manifest 形式は gen_ntfs_fixture.sh と同じ: "<f|d> <サイズ> <sha256|-> <パス>"。
内容はすべてホスト側で決定的に生成するので、VM 内でハッシュを計算する必要がない (WinPE は最小構成のため)。
"""
import hashlib, os, shutil, subprocess, sys

out = sys.argv[1]
variant = sys.argv[2] if len(sys.argv) > 2 else "mbr_gpt"   # mbr_gpt | usb4kn | dirty
pay = os.path.join(out, "payload")
shutil.rmtree(pay, ignore_errors=True)
data = os.path.join(pay, "data")
os.makedirs(data)
manifest = []

def xorshift_bytes(n, seed):
    x = seed; buf = bytearray()
    while len(buf) < n:
        x ^= (x << 13) & (2**64 - 1); x ^= x >> 7; x ^= (x << 17) & (2**64 - 1)
        buf += x.to_bytes(8, "little")
    return bytes(buf[:n])

def put(path, content):
    full = os.path.join(data, path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    open(full, "wb").write(content)
    manifest.append(("f", len(content), hashlib.sha256(content).hexdigest(), path))

def mkdir(path):
    os.makedirs(os.path.join(data, path), exist_ok=True)
    manifest.append(("d", 0, "-", path))

text = b"".join(b"compressible text line %d\r\n" % i for i in range(25000))
put("a.txt", b"hello\r\n")
put("empty.txt", b"")
put("日本語.txt", "こんにちは、世界\r\n".encode("utf-8"))
mkdir("日本語ディレクトリ")
put("日本語ディレクトリ/ファイル.txt", "中身\r\n".encode("utf-8"))
# ほぼ上限 (NTFS は 255 UTF-16 単位) の長い名前。ISO (Joliet) は 103 文字までなので、WinPE のスクリプト内で作る。
LONG = "long_name_" + "x" * 240 + ".txt"
manifest.append(("f", 11, hashlib.sha256(b"long name\r\n").hexdigest(), LONG))
put("space and (paren) & name.txt", b"odd chars\r\n")
put("big.bin", xorshift_bytes(5 * 1024 * 1024 + 777, 88172645463325252))
mkdir("many")
for i in range(1, 1001):
    put(f"many/file_{i}.txt", b"file number %d\r\n" % i)
# LZNT1 圧縮 (compact /c) の対象。テキスト / 非圧縮性 / 混在
mkdir("comp")
put("comp/text.txt", text)
put("comp/random.bin", xorshift_bytes(300000, 7))
mix = bytearray()
for k in range(40):
    mix += (b"abc" * 5000) if k % 3 == 0 else (xorshift_bytes(9000, 100 + k) if k % 3 == 1 else bytes(20000))
put("comp/mixed.bin", bytes(mix))
# WOF (CompactOS) 圧縮: xpress4k / 8k / 16k / lzx
mkdir("wof")
for name in ("xpress4k", "xpress8k", "xpress16k", "lzx"):
    put(f"wof/{name}.txt", text)
# 属性 / ADS / リンクは内容が同じ通常ファイルとして検証する (属性は読み出しに影響しない)
mkdir("attr")
for name in ("hidden.txt", "system.txt", "readonly.txt"):
    put(f"attr/{name}", b"attribute test\r\n")
put("ads.txt", b"main stream\r\n")        # スクリプトが ads.txt:s1 を追加する (主ストリームだけが読める)
put("target_of_link.txt", b"link target\r\n")
# スクリプトが作る疎ファイル (fsutil): 10MiB 全域が穴 = ゼロ
manifest.append(("f", 10 * 1024 * 1024, hashlib.sha256(bytes(10 * 1024 * 1024)).hexdigest(), "sparse.bin"))

# ---- WinPE 側スクリプト。注意: `echo ... 0> f` の "0>" はハンドルのリダイレクトになるので、必ず ( ... ) > f の形で書く。
HEAD = r"""@echo off
set SRC=%1
set LOG=X:\make.log
echo === usbmem winpe make === > %LOG%
(echo list disk& echo list volume) > X:\dp0.txt
diskpart /s X:\dp0.txt >> %LOG% 2>&1
"""
DISKS = {
 # 2 本の IDE ディスク: MBR + 4KiB クラスタ / GPT + 64KiB クラスタ
 "mbr_gpt": r"""
(echo select disk 0& echo clean& echo create partition primary& echo format fs=ntfs quick label=WINMBR& echo assign letter=W) > X:\dp1.txt
diskpart /s X:\dp1.txt >> %LOG% 2>&1
(echo select disk 1& echo clean& echo convert gpt& echo create partition primary& echo format fs=ntfs quick unit=64K label=WINGPT64K& echo assign letter=V) > X:\dp2.txt
diskpart /s X:\dp2.txt >> %LOG% 2>&1
""",
 # USB 接続の 4Kn (論理/物理セクタ 4096) ディスク: GPT + 既定クラスタ
 "usb4kn": r"""
(echo select disk 0& echo clean& echo convert gpt& echo create partition primary& echo format fs=ntfs quick label=WIN4KN& echo assign letter=W) > X:\dp1.txt
diskpart /s X:\dp1.txt >> %LOG% 2>&1
""",
 # 抜去されたボリュームを模す: ダーティフラグを立てたまま (fsutil dirty set) 正常にアンマウントする
 "dirty": r"""
(echo select disk 0& echo clean& echo create partition primary& echo format fs=ntfs quick label=WINDIRTY& echo assign letter=W) > X:\dp1.txt
diskpart /s X:\dp1.txt >> %LOG% 2>&1
""",
}
LETTERS = {"mbr_gpt": "W V", "usb4kn": "W", "dirty": "W"}
POPULATE = r"""
for %%L in (@LETTERS@) do (
  echo --- populate %%L: >> %LOG%
  xcopy /E /H /Y /I /Q %SRC%\data\ %%L:\ >> %LOG% 2>&1
  fsutil file createnew %%L:\sparse.bin 10485760 >> %LOG% 2>&1
  fsutil sparse setflag %%L:\sparse.bin >> %LOG% 2>&1
  fsutil sparse setrange %%L:\sparse.bin 0 10485760 >> %LOG% 2>&1
  echo stream one> %%L:\ads.txt:s1
  echo long name> %%L:\@LONG@
  attrib +h %%L:\attr\hidden.txt
  attrib +s %%L:\attr\system.txt
  attrib +r %%L:\attr\readonly.txt
  mklink %%L:\link_to_file.txt %%L:\target_of_link.txt >> %LOG% 2>&1
  mklink /D %%L:\link_to_dir %%L:\comp >> %LOG% 2>&1
  mklink /J %%L:\junction_to_comp %%L:\comp >> %LOG% 2>&1
  for /L %%N in (1,1,30) do (echo tmp> %%L:\tmp_%%N.txt)
  for /L %%N in (1,1,30) do (del %%L:\tmp_%%N.txt)
  fsutil fsinfo ntfsinfo %%L: >> %LOG% 2>&1
)
rem 圧縮 (4KiB 以下のクラスタのボリュームのみ可。64KiB クラスタでは失敗するがログに残るだけ)
compact /c /s:W:\comp /i >> %LOG% 2>&1
for %%A in (xpress4k xpress8k xpress16k lzx) do compact /c /exe:%%A W:\wof\%%A.txt >> %LOG% 2>&1
compact /s:W:\comp >> %LOG% 2>&1
compact W:\wof\* >> %LOG% 2>&1
"""
TAIL = r"""
for %%L in (@LETTERS@) do copy %LOG% %%L:\winpe_make.log > nul
@DIRTY@
for %%L in (@LETTERS@) do fsutil volume dismount %%L: > nul 2>&1
ping -n 5 127.0.0.1 > nul
wpeutil shutdown
"""
cmd = HEAD + DISKS[variant] + POPULATE + TAIL
cmd = cmd.replace("@LETTERS@", LETTERS[variant]).replace("@DIRTY@", "for %%L in (W) do fsutil dirty set %%L:" if variant == "dirty" else "rem (clean)")
open(os.path.join(pay, "usbmem_make.cmd"), "w", newline="\r\n").write(cmd.replace("@LONG@", LONG))
shutil.copy(os.path.join(os.path.dirname(os.path.abspath(__file__)), "autounattend.xml"), pay)
with open(os.path.join(out, "manifest.txt"), "w") as f:
    for k, sz, sh, p in manifest:
        f.write(f"{k} {sz} {sh} {p}\n")
subprocess.check_call(["genisoimage", "-quiet", "-J", "-joliet-long", "-R", "-input-charset", "utf-8", "-V", "USBMEMPAYLOAD",
                       "-o", os.path.join(out, "payload.iso"), pay])
print(len(manifest), "entries;", os.path.getsize(os.path.join(out, "payload.iso")) // 1024, "KiB payload.iso")
