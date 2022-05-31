# exit on fail
$ErrorActionPreference = "Stop"

Invoke-WebRequest -Uri "https://github.com/kawanakaiku/test-ci/releases/download/win10/Optimize-Offline.zip" -OutFile "Optimize-Offline.zip" -Verbose -PassThru
Expand-Archive ".\Optimize-Offline.zip" .

Foreach ($item in @("aa", "ab", "ac", "ad", "ae")) { Invoke-WebRequest -Uri '{0}{1}' -f "https://github.com/kawanakaiku/test-ci/releases/download/win10/Win10_21H2_Japanese_x64.wim_", $item -OutFile '{0}{1}' -f "Win10_21H2_Japanese_x64.wim_", $item -Verbose -PassThru }
Get-Content "Win10_21H2_Japanese_x64.wim_*" | Set-Content "Win10_21H2_Japanese_x64.wim"
Remove-Item "Win10_21H2_Japanese_x64.wim_*"

powershell.exe -NoProfile -ExecutionPolicy Unrestricted ".\Start-Optimize.ps1"
