# exit on fail
$ErrorActionPreference = "Stop"
# no progress bar for faster download
# https://stackoverflow.com/questions/28682642/powershell-why-is-using-invoke-webrequest-much-slower-than-a-browser-download
$ProgressPreference = "SilentlyContinue"

Write-Output -InputObject "download starting"
# python.exe -c "from urllib.request import urlopen;f=open('Win10_21H2_Japanese_x64.esd', 'wb');[[print(i), f.write(urlopen('https://github.com/kawanakaiku/test-ci/releases/download/win10_custom/Win10_21H2_Japanese_x64.esd_'+i).read())] for i in ['aa', 'ab']];f.close()"
Write-Output -InputObject "download completed"

Write-Output -InputObject "enable hyper-v"
Enable-WindowsOptionalFeature -Online -FeatureName Microsoft-Hyper-V -All

# create vhd file
New-VHD -Path .\win10.vhd -SizeBytes 32GB -Dynamic

cmd.exe /c "dir"
