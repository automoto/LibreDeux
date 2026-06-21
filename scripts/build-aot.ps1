param(
    [string]$BuildType = "Release",
    [string]$BuildDir = "",
    [string]$RexGluePrefix = "tools\rexglue-sdk\out\install\win-amd64-nmake",
    [string]$CompilerFlags = "-mssse3",
    [switch]$Regenerate,
    [switch]$SkipGeneratedWorkarounds,
    [switch]$Configure
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")

function Resolve-RepoPath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return $Path
    }
    return Join-Path $repoRoot $Path
}

if (-not $BuildDir) {
    $BuildDir = "build\aot-nmake-$($BuildType.ToLowerInvariant())"
}

$buildPath = Resolve-RepoPath $BuildDir
$manifest = Resolve-RepoPath "config\aot_manifest.toml"
$gameXex = Resolve-RepoPath "game\default.xex"
$rexgluePrefixPath = Resolve-RepoPath $RexGluePrefix
$rexglueExe = Join-Path $rexgluePrefixPath "bin\rexglue.exe"
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

if (-not (Test-Path -LiteralPath $manifest)) { throw "Manifest not found: $manifest" }
if (-not (Test-Path -LiteralPath $gameXex)) { throw "Missing user-supplied game\default.xex. Extract your own legal copy into game\ first." }
if (-not (Test-Path -LiteralPath $rexglueExe)) { throw "ReXGlue executable not found: $rexglueExe. Build the official ReXGlue SDK or pass -RexGluePrefix." }
if (-not (Test-Path -LiteralPath $vcvars)) { throw "vcvarsall.bat was not found at $vcvars" }
if (-not (Test-Path -LiteralPath $clang)) { throw "clang.exe was not found at $clang" }
if (-not (Test-Path -LiteralPath $clangxx)) { throw "clang++.exe was not found at $clangxx" }

if ($Regenerate) {
    & $rexglueExe codegen $manifest
    if ($LASTEXITCODE -ne 0) {
        throw "ReXGlue codegen failed with exit code $LASTEXITCODE"
    }

    if (-not $SkipGeneratedWorkarounds) {
        $generatedWorkarounds = Join-Path $PSScriptRoot "apply-aot-generated-workarounds.ps1"
        if (Test-Path -LiteralPath $generatedWorkarounds) {
            & $generatedWorkarounds -Root $repoRoot
            if ($LASTEXITCODE -ne 0) {
                throw "Generated workaround patch failed with exit code $LASTEXITCODE"
            }
        }
    }
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
        -S $repoRoot `
        -B $buildPath `
        -G "NMake Makefiles" `
        "-DCMAKE_BUILD_TYPE=$BuildType" `
        "-DCMAKE_C_COMPILER=$clang" `
        "-DCMAKE_CXX_COMPILER=$clangxx" `
        "-DCMAKE_C_FLAGS=$CompilerFlags" `
        "-DCMAKE_CXX_FLAGS=$CompilerFlags" `
        "-DCMAKE_PREFIX_PATH=$rexgluePrefixPath"
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configure failed with exit code $LASTEXITCODE"
    }
}

cmake --build $buildPath --target aot
if ($LASTEXITCODE -ne 0) {
    throw "CMake build failed with exit code $LASTEXITCODE"
}
