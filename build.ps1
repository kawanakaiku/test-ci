# exit on fail
$ErrorActionPreference = "Stop"
# no progress bar for faster download
# https://stackoverflow.com/questions/28682642/powershell-why-is-using-invoke-webrequest-much-slower-than-a-browser-download
$ProgressPreference = "SilentlyContinue"

# client for download
$wc = New-Object net.webclient

$wc.DownloadFile("https://github.com/kawanakaiku/test-ci/releases/download/win10/Optimize-Offline.zip", "Optimize-Offline.zip")
Expand-Archive ".\Optimize-Offline.zip" .

# Foreach ($item in @("aa", "ab", "ac", "ad", "ae")) { Write-Output -InputObject $item ; $wc.DownloadFile("https://github.com/kawanakaiku/test-ci/releases/download/win10/Win10_21H2_Japanese_x64.wim_" + $item, "Win10_21H2_Japanese_x64.wim_" + $item) }
# Write-Output -InputObject "merging to a wim file"
# Get-Content "Win10_21H2_Japanese_x64.wim_*" | Set-Content "Win10_21H2_Japanese_x64.wim"
# Remove-Item "Win10_21H2_Japanese_x64.wim_*"
# Write-Output -InputObject "download completed"
python.exe -c "import io, shutil;from urllib.request import urlopen;b=io.BytesIO();[b.write(urlopen('https://github.com/kawanakaiku/test-ci/releases/download/win10/Win10_21H2_Japanese_x64.wim_'+i).read()) for i in ['aa', 'ab', 'ac', 'ad', 'ae']];b.seek(0);shutil.copyfileobj(b, open('Win10_21H2_Japanese_x64.wim', 'wb'))"

powershell.exe -NoProfile -ExecutionPolicy Unrestricted ".\Start-Optimize.ps1"
