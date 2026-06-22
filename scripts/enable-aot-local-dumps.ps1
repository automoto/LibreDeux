param(
    [string]$DumpDir = "artifacts\dumps",
    [int]$DumpCount = 10,
    [ValidateSet("Custom", "Mini", "Full")]
    [string]$DumpType = "Full",
    [switch]$Disable
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
if ([System.IO.Path]::IsPathRooted($DumpDir)) {
    $dumpPath = $DumpDir
} else {
    $dumpPath = Join-Path $repoRoot $DumpDir
}

$key = "HKCU:\Software\Microsoft\Windows\Windows Error Reporting\LocalDumps\aot.exe"

if ($Disable) {
    if (Test-Path -LiteralPath $key) {
        Remove-Item -LiteralPath $key -Recurse -Force
    }
    Write-Output "local_dumps=disabled"
    Write-Output "registry_key=$key"
    exit 0
}

New-Item -ItemType Directory -Force -Path $dumpPath | Out-Null
New-Item -Path $key -Force | Out-Null

$dumpTypeValue = switch ($DumpType) {
    "Custom" { 0 }
    "Mini" { 1 }
    "Full" { 2 }
}

New-ItemProperty -Path $key -Name "DumpFolder" -PropertyType ExpandString -Value $dumpPath -Force | Out-Null
New-ItemProperty -Path $key -Name "DumpCount" -PropertyType DWord -Value $DumpCount -Force | Out-Null
New-ItemProperty -Path $key -Name "DumpType" -PropertyType DWord -Value $dumpTypeValue -Force | Out-Null

Write-Output "local_dumps=enabled"
Write-Output "registry_key=$key"
Write-Output "dump_folder=$dumpPath"
Write-Output "dump_count=$DumpCount"
Write-Output "dump_type=$DumpType"
