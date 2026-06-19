param(
    [string]$RexGlueSource = "tools\rexglue-sdk",
    [string]$BuildType = "Release",
    [string]$BuildDir = "",
    [string]$CompilerFlags = "-mssse3",
    [switch]$Configure
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")

function Resolve-RepoPath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return $Path
    }
    return [System.IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}

$sourcePath = Resolve-RepoPath $RexGlueSource
if (-not $BuildDir) {
    $BuildDir = "out\build\win-amd64-nmake"
}

$buildPath = Join-Path $sourcePath $BuildDir
$installPrefix = Join-Path $sourcePath "out\install\win-amd64-nmake"
$clang = "C:\Program Files\LLVM\bin\clang.exe"
$clangxx = "C:\Program Files\LLVM\bin\clang++.exe"

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vsInstall = $null
if (Test-Path $vswhere) {
    $vsInstall = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (-not $vsInstall) {
    $vsInstall = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
}
$vcvars = Join-Path $vsInstall "VC\Auxiliary\Build\vcvarsall.bat"

if (-not (Test-Path -LiteralPath (Join-Path $sourcePath "CMakeLists.txt"))) {
    throw "Official ReXGlue checkout not found: $sourcePath. Clone https://github.com/rexglue/rexglue-sdk.git or pass -RexGlueSource."
}
if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "vcvarsall.bat was not found at $vcvars"
}
if (-not (Test-Path -LiteralPath $clang)) {
    throw "clang.exe was not found at $clang"
}
if (-not (Test-Path -LiteralPath $clangxx)) {
    throw "clang++.exe was not found at $clangxx"
}

$repairScript = Join-Path $repoRoot "scripts\repair-rexglue-windows-symlinks.ps1"
if (Test-Path -LiteralPath $repairScript) {
    & $repairScript -RexGlueSource $sourcePath
    if ($LASTEXITCODE -ne 0) {
        throw "ReXGlue Windows symlink repair failed with exit code $LASTEXITCODE"
    }
}

$remote = & git -C $sourcePath remote get-url origin 2>$null
if ($LASTEXITCODE -eq 0 -and $remote -and $remote -notmatch "rexglue[/\\]rexglue-sdk(\.git)?$") {
    Write-Warning "ReXGlue origin is '$remote'. This project expects the official rexglue/rexglue-sdk repository unless a fork becomes necessary."
}

$previousErrorActionPreference = $ErrorActionPreference
$ErrorActionPreference = "Continue"
$envLines = cmd.exe /d /c "`"$vcvars`" x64 >nul && set" 2>&1
$ErrorActionPreference = $previousErrorActionPreference

$envLines | ForEach-Object {
    if ($_ -match "^(.*?)=(.*)$") {
        [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
    }
}

$cacheFile = Join-Path $buildPath "CMakeCache.txt"
if ($Configure -or -not (Test-Path -LiteralPath $cacheFile)) {
    cmake `
        -S $sourcePath `
        -B $buildPath `
        -G "NMake Makefiles" `
        "-DCMAKE_BUILD_TYPE=$BuildType" `
        "-DCMAKE_C_COMPILER=$clang" `
        "-DCMAKE_CXX_COMPILER=$clangxx" `
        "-DCMAKE_C_FLAGS=$CompilerFlags" `
        "-DCMAKE_CXX_FLAGS=$CompilerFlags" `
        "-DCMAKE_INSTALL_PREFIX=$installPrefix"
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configure failed with exit code $LASTEXITCODE"
    }
}

cmake --build $buildPath --target install
if ($LASTEXITCODE -ne 0) {
    throw "CMake build failed with exit code $LASTEXITCODE"
}

Write-Output "rexglue_install=$installPrefix"
