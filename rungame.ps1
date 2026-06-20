<#
.SYNOPSIS
    Manual launcher for Libre Army of Two.

.EXAMPLE
    .\rungame.ps1 -StopExisting

.EXAMPLE
    .\rungame.ps1 -Keyboard -StopExisting

.EXAMPLE
    .\rungame.ps1 -Build -StopExisting

.EXAMPLE
    .\rungame.ps1 -Vulkan -StopExisting

.EXAMPLE
    .\rungame.ps1 -XInput -LocalCoop -CoopTrace -StopExisting

.EXAMPLE
    .\rungame.ps1 -XInput -LocalCoop -CoopTrace -GuideButton -StopExisting
#>

[CmdletBinding()]
param(
    [string]$ExePath = "build\aot-nmake-release\aot.exe",
    [string]$GameRoot = "game",
    [switch]$StopExisting,
    [switch]$Build,
    [switch]$Regenerate,
    [switch]$Keyboard,
    [switch]$XInput,
    [switch]$LocalCoop,
    [switch]$CoopTrace,
    [switch]$GuideButton,
    [switch]$D3D12,
    [switch]$Vulkan,
    [switch]$VerboseProbe,
    [switch]$Wait,
    [switch]$DryRun,
    [string[]]$ExtraArgs = @()
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path $PSScriptRoot

function Resolve-RepoPath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return $Path
    }
    return Join-Path $repoRoot $Path
}

function Normalize-ExtraArgs([string[]]$InputArgs) {
    $normalized = @()
    foreach ($arg in $InputArgs) {
        foreach ($commaPart in $arg.Split(",", [System.StringSplitOptions]::RemoveEmptyEntries)) {
            $normalized += $commaPart.Split([char[]]@(" ", "`t", "`r", "`n"), [System.StringSplitOptions]::RemoveEmptyEntries)
        }
    }
    return $normalized
}

function Has-ArgPrefix([string[]]$InputArgs, [string]$Prefix) {
    foreach ($arg in $InputArgs) {
        if ($arg.StartsWith($Prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            return $true
        }
    }
    return $false
}

function Stop-ProcessByIdRobust([int]$Id) {
    $target = Get-Process -Id $Id -ErrorAction SilentlyContinue
    if (-not $target) {
        return $true
    }

    try {
        $target.Kill()
        if ($target.WaitForExit(5000)) {
            return $true
        }
    } catch {
        Stop-Process -Id $Id -Force -ErrorAction SilentlyContinue
        Wait-Process -Id $Id -Timeout 5 -ErrorAction SilentlyContinue
    }

    if (Get-Process -Id $Id -ErrorAction SilentlyContinue) {
        $taskkill = Join-Path $env:SystemRoot "System32\taskkill.exe"
        if (Test-Path -LiteralPath $taskkill) {
            & $taskkill /PID $Id /T /F | Out-Null
            Start-Sleep -Milliseconds 500
        }
    }

    return -not (Get-Process -Id $Id -ErrorAction SilentlyContinue)
}

function Get-AotProcess {
    Get-Process | Where-Object { $_.ProcessName -like "aot" }
}

if ($Build -or $Regenerate) {
    $buildScript = Join-Path $repoRoot "scripts\build-aot.ps1"
    if (-not (Test-Path -LiteralPath $buildScript)) {
        throw "Build script not found: $buildScript"
    }

    $buildArgs = @()
    if ($Regenerate) {
        $buildArgs += "-Regenerate"
    }
    if ($DryRun) {
        Write-Output "dry_run_would_build=true"
        Write-Output "build_script=$buildScript"
        Write-Output "build_args=$($buildArgs -join ' ')"
    } else {
        & $buildScript @buildArgs
    }
}

$exe = Resolve-RepoPath $ExePath
$gameRootPath = Resolve-RepoPath $GameRoot
$logsDir = Join-Path (Split-Path -Parent $exe) "logs"
$normalizedExtraArgs = Normalize-ExtraArgs $ExtraArgs

if ($D3D12 -and $Vulkan) {
    throw "Choose only one graphics backend switch: -D3D12 or -Vulkan."
}

$launchArgs = @("--game_data_root=$gameRootPath")
if ($Keyboard -and -not (Has-ArgPrefix -InputArgs $normalizedExtraArgs -Prefix "--mnk_mode")) {
    $launchArgs += "--mnk_mode"
}
if ($XInput -and -not (Has-ArgPrefix -InputArgs $normalizedExtraArgs -Prefix "--input_backend")) {
    $launchArgs += "--input_backend=xinput"
}
if ($LocalCoop -and -not (Has-ArgPrefix -InputArgs $normalizedExtraArgs -Prefix "--aot_coop_local")) {
    $launchArgs += "--aot_coop_local"
}
if ($CoopTrace -and -not (Has-ArgPrefix -InputArgs $normalizedExtraArgs -Prefix "--aot_trace_xam")) {
    $launchArgs += "--aot_trace_xam"
}
if ($GuideButton -and -not (Has-ArgPrefix -InputArgs $normalizedExtraArgs -Prefix "--guide_button")) {
    $launchArgs += "--guide_button"
}
if ($D3D12 -and -not (Has-ArgPrefix -InputArgs $normalizedExtraArgs -Prefix "--aot_graphics_backend")) {
    $launchArgs += "--aot_graphics_backend=d3d12"
}
if ($Vulkan -and -not (Has-ArgPrefix -InputArgs $normalizedExtraArgs -Prefix "--aot_graphics_backend")) {
    $launchArgs += "--aot_graphics_backend=vulkan"
}
if ($VerboseProbe) {
    $launchArgs += @(
        "--log_noisy",
        "--log_verbose",
        "--log_high_frequency_kernel_calls"
    )
}
$launchArgs += $normalizedExtraArgs

if (-not (Test-Path -LiteralPath $exe)) {
    throw "ReXGlue executable not found: $exe. Run .\rungame.ps1 -Build first."
}
if (-not (Test-Path -LiteralPath $gameRootPath)) {
    throw "Game data root not found: $gameRootPath"
}

$existing = Get-AotProcess
if ($existing) {
    if ($DryRun) {
        Write-Output "existing_aot_processes:"
        $existing | Select-Object ProcessName,Id,CPU,StartTime,Path
        if ($StopExisting) {
            Write-Output "dry_run_would_stop_existing=true"
        } else {
            Write-Output "dry_run_would_refuse_existing=true"
        }
    } elseif ($StopExisting) {
        foreach ($process in $existing) {
            Stop-ProcessByIdRobust $process.Id | Out-Null
        }
    } else {
        Write-Output "existing_aot_processes:"
        $existing | Select-Object ProcessName,Id,CPU,StartTime,Path
        throw "Refusing to launch while aot processes are already running. Re-run with -StopExisting if they should be terminated."
    }
}

Write-Output "exe=$exe"
Write-Output "game_data_root=$gameRootPath"
Write-Output "args=$($launchArgs -join ' ')"

if ($DryRun) {
    Write-Output "dry_run=true"
    return
}

$startedAt = Get-Date
$proc = Start-Process `
    -FilePath $exe `
    -ArgumentList $launchArgs `
    -WorkingDirectory $repoRoot `
    -PassThru

Write-Output "started=true"
Write-Output "pid=$($proc.Id)"
Write-Output "logs_dir=$logsDir"

if ($Wait) {
    $proc.WaitForExit()
    $proc.Refresh()
    Write-Output "exit_code=$($proc.ExitCode)"

    $latestLog = Get-ChildItem $logsDir -File -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -ge $startedAt.AddSeconds(-2) } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1

    if ($latestLog) {
        Write-Output "latest_log=$($latestLog.FullName)"
    }
}
