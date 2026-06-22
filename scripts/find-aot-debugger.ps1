param()

$ErrorActionPreference = "Stop"

$candidates = @(
    "cdb.exe",
    "windbg.exe",
    "lldb.exe",
    "procdump.exe",
    "C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe",
    "C:\Program Files\Windows Kits\10\Debuggers\x64\cdb.exe",
    "C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\windbg.exe",
    "C:\Program Files\Windows Kits\10\Debuggers\x64\windbg.exe"
)

foreach ($candidate in $candidates) {
    if ([System.IO.Path]::IsPathRooted($candidate)) {
        if (Test-Path -LiteralPath $candidate) {
            Write-Output $candidate
            exit 0
        }
        continue
    }

    $command = Get-Command $candidate -ErrorAction SilentlyContinue
    if ($command) {
        Write-Output $command.Source
        exit 0
    }
}

exit 1
