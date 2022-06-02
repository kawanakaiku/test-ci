# exit on fail
$ErrorActionPreference = "Stop"
# no progress bar for faster download
# https://stackoverflow.com/questions/28682642/powershell-why-is-using-invoke-webrequest-much-slower-than-a-browser-download
$ProgressPreference = "SilentlyContinue"

$esdfile = Join-Path (Get-Location) 'Win10_21H2_Japanese_x64.wim'
$vhdfile = 'C:\win10.vhd'
$7zfile = 'C:\win10.vhd.7z'

Write-Output -InputObject "creating vhd"
# create vhd file
'create vdisk file="{0}" maximum=32000 type=expandable' -f $vhdfile | Out-File -Encoding utf8 diskpart.txt
'select vdisk file="{0}"' -f $vhdfile | Out-File -Append -Encoding utf8 diskpart.txt
'attach vdisk' | Out-File -Append -Encoding utf8 diskpart.txt
'clean' | Out-File -Append -Encoding utf8 diskpart.txt
'convert gpt' | Out-File -Append -Encoding utf8 diskpart.txt
'create partition efi size=100' | Out-File -Append -Encoding utf8 diskpart.txt
'format quick fs=fat32' | Out-File -Append -Encoding utf8 diskpart.txt
'assign letter=s' | Out-File -Append -Encoding utf8 diskpart.txt
'create partition primary' | Out-File -Append -Encoding utf8 diskpart.txt
'format quick fs=ntfs' | Out-File -Append -Encoding utf8 diskpart.txt
'assign letter=w' | Out-File -Append -Encoding utf8 diskpart.txt
diskpart.exe /s diskpart.txt
Write-Output -InputObject "creating vhd completed"

Write-Output -InputObject "download esd starting"
python.exe -c "from urllib.request import urlopen;f=open('Win10_21H2_Japanese_x64.wim', 'wb');[[print(i), f.write(urlopen('https://github.com/kawanakaiku/test-ci/releases/download/win10/Win10_21H2_Japanese_x64.wim_'+i).read())] for i in ['aa', 'ab', 'ac', 'ad', 'ae']];f.close()"
Write-Output -InputObject "download esd completed"

dism.exe /Apply-Image /ImageFile:$esdfile /index:1 /ApplyDir:W:\
dism.exe /Image:W:\ /Set-LayeredDriver:6
bcdboot.exe W:\Windows /l ja-jp /s S: /f UEFI

Write-Output -InputObject "detaching vhd"
'select vdisk file="{0}"' -f $vhdfile | Out-File -Encoding utf8 diskpart.txt
'detach vdisk' | Out-File -Append -Encoding utf8 diskpart.txt
diskpart.exe /s diskpart.txt
Write-Output -InputObject "detaching vhd finished"

Write-Output -InputObject "archiving vhd"
7z.exe a -t7z -mx=9 -ms=on -bsp1 $7zfile $vhdfile
Write-Output -InputObject "archiving vhd finished"

cmd.exe /c "dir"
