param(
    [string]$BuildDir = "build\aot-nmake-relwithdebinfo",
    [string]$RexGluePrefix = "tools\rexglue-sdk\out\install\win-amd64-nmake",
    [string]$CompilerFlags = "-mssse3 -gcodeview",
    [switch]$Regenerate,
    [switch]$Configure
)

$ErrorActionPreference = "Stop"

$buildArgs = @{
    BuildType = "RelWithDebInfo"
    BuildDir = $BuildDir
    RexGluePrefix = $RexGluePrefix
    CompilerFlags = $CompilerFlags
}
if ($Regenerate) {
    $buildArgs.Regenerate = $true
}
if ($Configure) {
    $buildArgs.Configure = $true
}

& (Join-Path $PSScriptRoot "build-aot.ps1") @buildArgs
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
