# 実 Windows 11 (VHD) を QEMU で起動して暗号化ボリューム等を作る

元 VHD は変更せず、qcow2 の backing overlay を /dev/shm に作って使う (`qemu-img create -f qcow2 -b <vhd> -F vpc ov.qcow2`)。
UEFI (`-bios OVMF.fd`)、TPM 2.0 は swtpm、**ネットワークなし (`-nic none`)**。キーボードは QMP 経由で、ゲストは**日本語配列** (`qmp_ctl.py` の `type` が対応)。

```
swtpm socket --tpm2 --tpmstate dir=tpm --ctrl type=unixio,path=swtpm.sock --daemon
qemu-system-x86_64 -enable-kvm -cpu host -smp 4 -m 4096 -machine q35 -bios OVMF.fd -display none -vga std \
  -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -device usb-tablet,bus=xhci.0 \
  -drive file=ov.qcow2,format=qcow2,if=none,id=d0 -device nvme,drive=d0,serial=w11 \
  -drive file=ex.raw,format=raw,if=none,id=exd -device usb-storage,bus=xhci.0,drive=exd \
  -chardev socket,id=chrtpm,path=swtpm.sock -tpmdev emulator,id=tpm0,chardev=chrtpm -device tpm-tis,tpmdev=tpm0 \
  -qmp unix:qmp.sock,server=on,wait=off -nic none
```
- `ex.raw` は「受け渡しディスク」: MBR + NTFS (mkntfs で作成、ホストで ntfs-3g により run.cmd を置く)。ゲストでは D: に見え、結果ファイルもここへ書かせてホストで読む。
- Win+R → `cmd`。`powershell -c Start-Process cmd -Verb RunAs` で UAC (「はい」をクリックするだけ) → 管理者 cmd。
- 環境: Windows 11 Pro 10.0.26200 (manage-bde / certutil / powershell / fc / cipher あり)、ユーザー `user`。
