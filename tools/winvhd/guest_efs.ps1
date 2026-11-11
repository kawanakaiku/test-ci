# guest-side: make an NTFS volume holding EFS-encrypted files. ASCII only (PowerShell 5 reads BOM-less files as ANSI).
param([int]$Disk, [string]$Letter = "S")
$ErrorActionPreference = "Stop"
Initialize-Disk -Number $Disk -PartitionStyle MBR
New-Partition -DiskNumber $Disk -UseMaximumSize -DriveLetter $Letter | Out-Null
Format-Volume -DriveLetter $Letter -FileSystem NTFS -NewFileSystemLabel "EFSVOL" -Confirm:$false | Out-Null
$r = "${Letter}:\"
[IO.File]::WriteAllText("${r}plain.txt", "plain file`r`n")
New-Item -ItemType Directory "${r}secret" | Out-Null
[IO.File]::WriteAllText("${r}secret\a.txt", ("secret line {0}`r`n" -f (1..2000)))
$bytes = New-Object byte[] 300000; (New-Object Random 5).NextBytes($bytes)
[IO.File]::WriteAllBytes("${r}secret\random.bin", $bytes)
[IO.File]::WriteAllText("${r}secret_single.txt", "single encrypted file`r`n")
cipher /e /s:"${r}secret" | Out-Null
cipher /e "${r}secret_single.txt" | Out-Null
Get-ChildItem -Recurse -Force $r | Select-Object FullName, Length, Attributes | Format-Table -Auto | Out-String -Width 200
cipher /c "${r}secret\a.txt"
