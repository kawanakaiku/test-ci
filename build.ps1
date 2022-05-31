# exit on fail
$ErrorActionPreference = "Stop"
# no progress bar for faster download
# https://stackoverflow.com/questions/28682642/powershell-why-is-using-invoke-webrequest-much-slower-than-a-browser-download
$ProgressPreference = "SilentlyContinue"

# client for download
$wc = New-Object net.webclient

$wc.DownloadFile("https://github.com/kawanakaiku/test-ci/releases/download/win10/Optimize-Offline.zip", "Optimize-Offline.zip")
Expand-Archive ".\Optimize-Offline.zip" .

Foreach ($item in @("aa", "ab", "ac", "ad", "ae")) { C:\msys64\bin\wget.exe "https://github.com/kawanakaiku/test-ci/releases/download/win10/Win10_21H2_Japanese_x64.wim_" + $item }
Get-Content "Win10_21H2_Japanese_x64.wim_*" | Set-Content "Win10_21H2_Japanese_x64.wim"
Remove-Item "Win10_21H2_Japanese_x64.wim_*"

powershell.exe -NoProfile -ExecutionPolicy Unrestricted ".\Start-Optimize.ps1"
