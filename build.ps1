# exit on fail
$ErrorActionPreference = "Stop"
# no progress bar for faster download
# https://stackoverflow.com/questions/28682642/powershell-why-is-using-invoke-webrequest-much-slower-than-a-browser-download
$ProgressPreference = "SilentlyContinue"

Invoke-WebRequest -Uri "https://github.com/kawanakaiku/test-ci/releases/download/win10/Optimize-Offline.zip" -OutFile "Optimize-Offline.zip" -Verbose -PassThru
Expand-Archive ".\Optimize-Offline.zip" .

Foreach ($item in @("aa", "ab", "ac", "ad", "ae")) { Invoke-WebRequest -Uri ("https://github.com/kawanakaiku/test-ci/releases/download/win10/Win10_21H2_Japanese_x64.wim_" + $item) -OutFile ("Win10_21H2_Japanese_x64.wim_" + $item) -Verbose -PassThru }
Get-Content "Win10_21H2_Japanese_x64.wim_*" | Set-Content "Win10_21H2_Japanese_x64.wim"
Remove-Item "Win10_21H2_Japanese_x64.wim_*"

powershell.exe -NoProfile -ExecutionPolicy Unrestricted ".\Start-Optimize.ps1"
