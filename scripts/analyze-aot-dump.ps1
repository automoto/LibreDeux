param(
    [string]$DumpDir = "artifacts\dumps",
    [string]$DumpPath = "",
    [string]$OutDir = "artifacts\crash-analysis",
    [string]$DebuggerPath = "",
    [string]$SymbolPath = ""
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")

function Resolve-RepoPath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return $Path
    }
    return Join-Path $repoRoot $Path
}

if (-not $DumpPath) {
    $dumpRoot = Resolve-RepoPath $DumpDir
    $latestDump = Get-ChildItem -LiteralPath $dumpRoot -Filter "*.dmp" -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if (-not $latestDump) {
        throw "No .dmp files found under $dumpRoot. Run scripts\enable-aot-local-dumps.ps1, reproduce the crash, then retry."
    }
    $DumpPath = $latestDump.FullName
} else {
    $DumpPath = Resolve-RepoPath $DumpPath
}

if (-not (Test-Path -LiteralPath $DumpPath)) {
    throw "Dump not found: $DumpPath"
}

$analysisRoot = Resolve-RepoPath $OutDir
New-Item -ItemType Directory -Force -Path $analysisRoot | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$analysisPath = Join-Path $analysisRoot "aot_dump_$stamp.txt"

if (-not $SymbolPath) {
    $symbolParts = @(
        (Resolve-RepoPath "build\aot-nmake-relwithdebinfo"),
        (Resolve-RepoPath "build\aot-nmake-release"),
        "srv*"
    )
    $SymbolPath = $symbolParts -join ";"
}

if (-not $DebuggerPath) {
    $debugger = & (Join-Path $PSScriptRoot "find-aot-debugger.ps1") 2>$null
    if ($LASTEXITCODE -eq 0 -and $debugger) {
        $DebuggerPath = $debugger | Select-Object -First 1
    }
}

$header = @(
    "dump=$DumpPath",
    "analysis=$analysisPath",
    "symbol_path=$SymbolPath",
    "debugger=$DebuggerPath",
    "created_at=$((Get-Date).ToString("o"))",
    ""
)
$header | Set-Content -Path $analysisPath -Encoding UTF8

if (-not $DebuggerPath) {
    Add-Content -Path $analysisPath -Encoding UTF8 -Value "No debugger found. Install Windows Debugging Tools so cdb.exe is available, then re-run this script."
    Write-Output "analysis=$analysisPath"
    Write-Output "status=no_debugger"
    exit 2
}

$debuggerName = [System.IO.Path]::GetFileName($DebuggerPath).ToLowerInvariant()
if ($debuggerName -eq "cdb.exe") {
    $commands = ".sympath $SymbolPath; .reload; !analyze -v; ~* k; lm; q"
    & $DebuggerPath -z $DumpPath -c $commands | Add-Content -Path $analysisPath -Encoding UTF8
    Write-Output "analysis=$analysisPath"
    Write-Output "status=analyzed"
    exit $LASTEXITCODE
}

if ($debuggerName -eq "windbg.exe") {
    Add-Content -Path $analysisPath -Encoding UTF8 -Value "windbg.exe was found, but non-interactive dump analysis requires cdb.exe. Install cdb.exe or pass -DebuggerPath to cdb.exe."
    Write-Output "analysis=$analysisPath"
    Write-Output "status=needs_cdb"
    exit 2
}

if ($debuggerName -eq "lldb.exe") {
    & $DebuggerPath --batch -c "target create --core `"$DumpPath`"" -c "bt all" | Add-Content -Path $analysisPath -Encoding UTF8
    Write-Output "analysis=$analysisPath"
    Write-Output "status=analyzed"
    exit $LASTEXITCODE
}

Add-Content -Path $analysisPath -Encoding UTF8 -Value "Unsupported debugger: $DebuggerPath"
Write-Output "analysis=$analysisPath"
Write-Output "status=unsupported_debugger"
exit 2
