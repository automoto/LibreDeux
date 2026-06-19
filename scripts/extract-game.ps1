param(
    [string]$IsoPath = "iso\Army of Two (USA)(1)\Army of Two (USA).iso",
    [string]$OutputDir = "game",
    [string]$ToolsRoot = "..\3Unchallenged\tools\360tools",
    [switch]$Analyze
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")

function Resolve-RepoPath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return $Path
    }
    return Join-Path $repoRoot $Path
}

function Resolve-ToolPath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return $Path
    }
    return [System.IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}

function Get-PythonCommand {
    $py = Get-Command py -ErrorAction SilentlyContinue
    if ($py) {
        return @{ Command = $py.Source; PrefixArgs = @("-3") }
    }

    $python = Get-Command python -ErrorAction SilentlyContinue
    if ($python) {
        return @{ Command = $python.Source; PrefixArgs = @() }
    }

    throw "Python was not found. Install Python 3 or add it to PATH."
}

$iso = Resolve-RepoPath $IsoPath
$out = Resolve-RepoPath $OutputDir
$tools = Resolve-ToolPath $ToolsRoot
$extractIso = Join-Path $tools "tools\extract_iso.py"
$xexInfo = Join-Path $tools "tools\xex_info.py"
$parseImports = Join-Path $tools "tools\parse_xex_imports.py"

if (-not (Test-Path -LiteralPath $iso)) {
    throw "ISO not found: $iso"
}
if (-not (Test-Path -LiteralPath $extractIso)) {
    throw "360tools extract_iso.py not found: $extractIso"
}

$python = Get-PythonCommand
New-Item -ItemType Directory -Force -Path $out | Out-Null
$env:PYTHONIOENCODING = "utf-8"

& $python.Command @($python.PrefixArgs + @($extractIso, $iso, $out))
if ($LASTEXITCODE -ne 0) {
    throw "ISO extraction failed with exit code $LASTEXITCODE"
}

$defaultXex = Join-Path $out "default.xex"
if (-not (Test-Path -LiteralPath $defaultXex)) {
    throw "Extraction completed, but game\default.xex was not found. Check the extracted tree under $out."
}

Write-Output "game_root=$out"
Write-Output "default_xex=$defaultXex"

if ($Analyze) {
    $privateDir = Resolve-RepoPath "private"
    New-Item -ItemType Directory -Force -Path $privateDir | Out-Null

    if (Test-Path -LiteralPath $xexInfo) {
        & $python.Command @($python.PrefixArgs + @($xexInfo, $defaultXex)) |
            Tee-Object -FilePath (Join-Path $privateDir "aot_xex_info.txt")
        if ($LASTEXITCODE -ne 0) {
            throw "xex_info.py failed with exit code $LASTEXITCODE"
        }
    }

    if (Test-Path -LiteralPath $parseImports) {
        & $python.Command @($python.PrefixArgs + @($parseImports, $defaultXex)) |
            Tee-Object -FilePath (Join-Path $privateDir "aot_xex_imports.txt")
        if ($LASTEXITCODE -ne 0) {
            throw "parse_xex_imports.py failed with exit code $LASTEXITCODE"
        }
    }
}
