# exit on fail
# $ErrorActionPreference = "Stop"
# no progress bar for faster download
# https://stackoverflow.com/questions/28682642/powershell-why-is-using-invoke-webrequest-much-slower-than-a-browser-download
# $ProgressPreference = "SilentlyContinue"

# client for download
$wc = New-Object net.webclient

$wc.DownloadFile("https://github.com/kawanakaiku/test-ci/releases/download/win10/Optimize-Offline.zip", "Optimize-Offline.zip")
Expand-Archive ".\Optimize-Offline.zip" .

Write-Output -InputObject "download starting"
# Foreach ($item in @("aa", "ab", "ac", "ad", "ae")) { Write-Output -InputObject $item ; $wc.DownloadFile("https://github.com/kawanakaiku/test-ci/releases/download/win10/Win10_21H2_Japanese_x64.wim_" + $item, "Win10_21H2_Japanese_x64.wim_" + $item) }
# Write-Output -InputObject "merging to a wim file"
# Get-Content "Win10_21H2_Japanese_x64.wim_*" | Set-Content "Win10_21H2_Japanese_x64.wim"
# Remove-Item "Win10_21H2_Japanese_x64.wim_*"
python.exe -c "from urllib.request import urlopen;f=open('Win10_21H2_Japanese_x64.wim', 'wb');[[print(i), f.write(urlopen('https://github.com/kawanakaiku/test-ci/releases/download/win10/Win10_21H2_Japanese_x64.wim_'+i).read())] for i in ['aa', 'ab', 'ac', 'ad', 'ae']];f.close()"
Write-Output -InputObject "download completed"

powershell.exe -NoProfile -ExecutionPolicy Unrestricted ".\Start-Optimize.ps1"

# store logs
Compress-Archive -Path .\OfflineTemp_*\*.log, .\OfflineTemp_*\*\*.log -DestinationPath logs.zip -Force

cmd.exe /c "dir"
