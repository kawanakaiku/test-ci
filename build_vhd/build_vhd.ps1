Write-Output -InputObject "download starting"
# python.exe -c "from urllib.request import urlopen;f=open('Win10_21H2_Japanese_x64.esd', 'wb');[[print(i), f.write(urlopen('https://github.com/kawanakaiku/test-ci/releases/download/win10_custom/Win10_21H2_Japanese_x64.esd_'+i).read())] for i in ['aa', 'ab']];f.close()"
Write-Output -InputObject "download completed"

# create vhd file
New-VHD -Path .\win10.vhd -SizeBytes 32GB -Dynamic

cmd.exe /c "dir"
