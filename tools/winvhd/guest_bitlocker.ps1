# guest-side: make an NTFS volume and turn BitLocker on (password protector). ASCII only.
param([int]$Disk, [string]$Letter = "T", [string]$Method = "XtsAes128")
$ErrorActionPreference = "Stop"
Initialize-Disk -Number $Disk -PartitionStyle MBR
New-Partition -DiskNumber $Disk -UseMaximumSize -DriveLetter $Letter | Out-Null
Format-Volume -DriveLetter $Letter -FileSystem NTFS -NewFileSystemLabel "BLVOL" -Confirm:$false | Out-Null
$r = "${Letter}:\"
[IO.File]::WriteAllText("${r}hello.txt", "hello from a bitlocker volume`r`n")
$pw = ConvertTo-SecureString "usbmem-Test-pw-1" -AsPlainText -Force
Enable-BitLocker -MountPoint "${Letter}:" -EncryptionMethod $Method -PasswordProtector -Password $pw -UsedSpaceOnly | Out-Null
for ($i = 0; $i -lt 120; $i++) {
  $v = Get-BitLockerVolume -MountPoint "${Letter}:"
  if ($v.VolumeStatus -eq "FullyEncrypted") { break }
  Start-Sleep -Seconds 2
}
Get-BitLockerVolume -MountPoint "${Letter}:" | Format-List MountPoint, VolumeStatus, EncryptionMethod, EncryptionPercentage, KeyProtector | Out-String
