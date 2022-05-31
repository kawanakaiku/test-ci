wget "https://github.com/kawanakaiku/test-ci/releases/download/win10/Optimize-Offline.zip"
Expand-Archive ".\Optimize-Offline.zip" .

Foreach ($item in @("aa", "ab", "ac", "ad", "ae")) { wget ("https://github.com/kawanakaiku/test-ci/releases/download/win10/Win10_21H2_Japanese_x64.wim_" + $item) }
Get-Content "Win10_21H2_Japanese_x64.wim_*" | Set-Content "Win10_21H2_Japanese_x64.wim"
Remove-Item "Win10_21H2_Japanese_x64.wim_*"

powershell.exe -NoProfile -ExecutionPolicy Unrestricted ".\Start-Optimize.ps1"
