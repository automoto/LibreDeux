param(
    [string]$GameRoot = "game",
    [string]$ToolsRoot = "..\3Unchallenged\tools\360tools",
    [string]$OutDir = "private"
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

$gameRootPath = Resolve-RepoPath $GameRoot
$defaultXex = Join-Path $gameRootPath "default.xex"
$tools = Resolve-ToolPath $ToolsRoot
$outPath = Resolve-RepoPath $OutDir
$xexInfo = Join-Path $tools "tools\xex_info.py"
$parseImports = Join-Path $tools "tools\parse_xex_imports.py"

if (-not (Test-Path -LiteralPath $defaultXex)) {
    throw "default.xex not found: $defaultXex"
}
if (-not (Test-Path -LiteralPath $xexInfo)) {
    throw "xex_info.py not found: $xexInfo"
}
if (-not (Test-Path -LiteralPath $parseImports)) {
    throw "parse_xex_imports.py not found: $parseImports"
}

$python = Get-PythonCommand
New-Item -ItemType Directory -Force -Path $outPath | Out-Null
$env:PYTHONIOENCODING = "utf-8"

& $python.Command @($python.PrefixArgs + @($xexInfo, $defaultXex)) |
    Tee-Object -FilePath (Join-Path $outPath "aot_xex_info.txt")
if ($LASTEXITCODE -ne 0) {
    throw "xex_info.py failed with exit code $LASTEXITCODE"
}

& $python.Command @($python.PrefixArgs + @($parseImports, $defaultXex)) |
    Tee-Object -FilePath (Join-Path $outPath "aot_xex_imports.txt")
if ($LASTEXITCODE -ne 0) {
    throw "parse_xex_imports.py failed with exit code $LASTEXITCODE"
}
