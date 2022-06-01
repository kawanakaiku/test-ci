# exit on fail
$ErrorActionPreference = "Stop"
# no progress bar for faster download
# https://stackoverflow.com/questions/28682642/powershell-why-is-using-invoke-webrequest-much-slower-than-a-browser-download
$ProgressPreference = "SilentlyContinue"

Write-Output -InputObject "download starting"
# python.exe -c "from urllib.request import urlopen;f=open('Win10_21H2_Japanese_x64.esd', 'wb');[[print(i), f.write(urlopen('https://github.com/kawanakaiku/test-ci/releases/download/win10_custom/Win10_21H2_Japanese_x64.esd_'+i).read())] for i in ['aa', 'ab']];f.close()"
Write-Output -InputObject "download completed"

Write-Output -InputObject "creating vhd"
# create vhd file
$vhdfile = Join-Path (Get-Location) 'win10.vhd'
'create vdisk file="{0}" maximum=32000 type=expandable' -f $vhdfile > diskpart.txt
'select vdisk file="{0}"' -f $vhdfile >> diskpart.txt
'attach vdisk' >> diskpart.txt
'clean' >> diskpart.txt
'convert gpt' >> diskpart.txt
'create partition efi size=100' >> diskpart.txt
'format quick fs=fat32' >> diskpart.txt
'assign letter=s' >> diskpart.txt
'create partition primary' >> diskpart.txt
'format quick fs=ntfs' >> diskpart.txt
'assign letter=w' >> diskpart.txt
diskpart.exe /s diskpart.txt
Write-Output -InputObject "creating vhd completed"

cmd.exe /c "dir"
