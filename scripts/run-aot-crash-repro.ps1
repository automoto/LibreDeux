param(
    [int]$Seconds = 30,
    [string]$ExePath = "build\aot-nmake-release\aot.exe",
    [string]$ExtraArgs = "--log_verbose --log_noisy",
    [switch]$NopAudio,
    [switch]$ConfigureLocalDumps
)

$ErrorActionPreference = "Stop"

if ($ConfigureLocalDumps) {
    & (Join-Path $PSScriptRoot "enable-aot-local-dumps.ps1")
}

$args = $ExtraArgs
if ($NopAudio -and $args -notmatch "(^|\s)--aot_nop_audio(\s|$)") {
    $args = "--aot_nop_audio $args"
}

& (Join-Path $PSScriptRoot "run-aot-input-capture.ps1") `
    -Seconds $Seconds `
    -ExePath $ExePath `
    -StopExisting `
    -NoInput `
    -ExtraArgs $args
