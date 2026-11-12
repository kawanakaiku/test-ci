# guest-side: NTFS volume with a deterministic file tree, then BitLocker in the requested variant. ASCII only.
# Output (stdout): "MANIFEST-BEGIN" ... lines "f size sha256 path" / "d 0 - path" ... "MANIFEST-END", then "META key=value" lines
# (recovery password, startup key file as base64, protector summary).
param([int]$Disk, [string]$Letter = "T", [string]$Variant = "xts256full", [int]$SizeMB = 256)
$ErrorActionPreference = "Stop"
Initialize-Disk -Number $Disk -PartitionStyle MBR
New-Partition -DiskNumber $Disk -UseMaximumSize -DriveLetter $Letter | Out-Null
Format-Volume -DriveLetter $Letter -FileSystem NTFS -NewFileSystemLabel "BLMATRIX" -Confirm:$false | Out-Null
$r = "${Letter}:\"

function New-Data([string]$Path, [int]$Size, [int]$Seed) {
  $rng = New-Object System.Random($Seed)
  $b = New-Object byte[] $Size
  $rng.NextBytes($b)
  [IO.File]::WriteAllBytes($Path, $b)
}
# unicode names built from code points (the script itself stays ASCII)
$jp = -join ([char]0x65E5, [char]0x672C, [char]0x8A9E)
New-Item -ItemType Directory -Path "${r}docs", "${r}bin", "${r}deep\a\b\c\d", "${r}many", "${r}$jp" | Out-Null
[IO.File]::WriteAllText("${r}hello.txt", "hello from a bitlocker volume`r`n")
[IO.File]::WriteAllText("${r}docs\readme.txt", ("line of text " * 400))
[IO.File]::WriteAllText("${r}$jp\${jp}.txt", "unicode name file")
[IO.File]::WriteAllText("${r}deep\a\b\c\d\leaf.txt", "deep leaf")
New-Item -ItemType File -Path "${r}empty.txt" | Out-Null
$seed = 1
foreach ($sz in 1, 511, 512, 513, 4095, 4096, 4097, 65536, 100000, 1048576, 5242880, 20971520) {
  New-Data "${r}bin\f_$sz.bin" $sz $seed
  $seed++
}
for ($i = 1; $i -le 150; $i++) { [IO.File]::WriteAllText("${r}many\file_$i.txt", "file number $i " * 20) }
Write-VolumeCache $Letter

# manifest
"MANIFEST-BEGIN"
Get-ChildItem -Path $r -Recurse -Force | Where-Object { $_.FullName -notmatch 'System Volume Information|RECYCLE' } | Sort-Object FullName | ForEach-Object {
  $rel = $_.FullName.Substring($r.Length).Replace('\', '/')
  if ($_.PSIsContainer) { "d 0 - $rel" }
  else { $h = (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash.ToLower(); "f $($_.Length) $h $rel" }
}
"MANIFEST-END"

function Wait-Encrypted {
  for ($i = 0; $i -lt 300; $i++) {
    $v = Get-BitLockerVolume -MountPoint "${Letter}:"
    if ($v.VolumeStatus -eq "FullyEncrypted") { return }
    Start-Sleep -Seconds 2
  }
}
$pw = ConvertTo-SecureString "usbmem-Test-pw-1" -AsPlainText -Force
switch ($Variant) {
  "xts256full" {   # XTS-AES-256, full encryption, password + recovery password
    Enable-BitLocker -MountPoint "${Letter}:" -EncryptionMethod XtsAes256 -UsedSpaceOnly -RecoveryPasswordProtector | Out-Null
    Add-BitLockerKeyProtector -MountPoint "${Letter}:" -PasswordProtector -Password $pw | Out-Null
    Wait-Encrypted
  }
  "cbc128" {       # AES-CBC-128 (Windows 8 style, no diffuser), full encryption
    Enable-BitLocker -MountPoint "${Letter}:" -EncryptionMethod Aes128 -UsedSpaceOnly -RecoveryPasswordProtector | Out-Null
    Add-BitLockerKeyProtector -MountPoint "${Letter}:" -PasswordProtector -Password $pw | Out-Null
    Wait-Encrypted
  }
  "cbc256" {
    Enable-BitLocker -MountPoint "${Letter}:" -EncryptionMethod Aes256 -UsedSpaceOnly -RecoveryPasswordProtector | Out-Null
    Add-BitLockerKeyProtector -MountPoint "${Letter}:" -PasswordProtector -Password $pw | Out-Null
    Wait-Encrypted
  }
  "startkey" {     # XTS-AES-128, used space only, password + startup key (.BEK)
    New-Item -ItemType Directory -Path "C:\Users\user\bek" -Force | Out-Null
    Remove-Item "C:\Users\user\bek\*" -Force -ErrorAction SilentlyContinue
    Enable-BitLocker -MountPoint "${Letter}:" -EncryptionMethod XtsAes128 -UsedSpaceOnly -StartupKeyProtector -StartupKeyPath "C:\Users\user\bek" | Out-Null
    Add-BitLockerKeyProtector -MountPoint "${Letter}:" -PasswordProtector -Password $pw | Out-Null
    Wait-Encrypted
    $bek = Get-ChildItem -Path "C:\Users\user\bek" -Force | Select-Object -First 1
    "META bek_name=$($bek.Name)"
    "META bek_b64=" + [Convert]::ToBase64String([IO.File]::ReadAllBytes($bek.FullName))
  }
  "suspended" {    # XTS-AES-128, used space only, password; then protection suspended (clear key stored in metadata)
    Enable-BitLocker -MountPoint "${Letter}:" -EncryptionMethod XtsAes128 -UsedSpaceOnly -PasswordProtector -Password $pw | Out-Null
    Wait-Encrypted
    manage-bde -protectors -disable "${Letter}:" | Out-Null   # Suspend-BitLocker is OS-drive only; this adds the clear key to data volumes too
  }
  "partial" {      # full encryption of a big volume, paused part-way
    Enable-BitLocker -MountPoint "${Letter}:" -EncryptionMethod XtsAes128 -PasswordProtector -Password $pw | Out-Null
    for ($i = 0; $i -lt 400; $i++) {
      $v = Get-BitLockerVolume -MountPoint "${Letter}:"
      if ($v.EncryptionPercentage -ge 30) { break }
      Start-Sleep -Milliseconds 200
    }
    manage-bde -pause "${Letter}:" | Out-Null
    Start-Sleep -Seconds 2
  }
}
$v = Get-BitLockerVolume -MountPoint "${Letter}:"
"META status=$($v.VolumeStatus) pct=$($v.EncryptionPercentage) method=$($v.EncryptionMethod)"
foreach ($kp in $v.KeyProtector) {
  "META protector=$($kp.KeyProtectorType) id=$($kp.KeyProtectorId)"
  if ($kp.KeyProtectorType -eq "RecoveryPassword") { "META recovery=$($kp.RecoveryPassword)" }
}
Write-VolumeCache $Letter
