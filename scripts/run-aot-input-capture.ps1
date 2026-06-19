param(
    [int]$Seconds = 90,
    [string]$ExePath = "build\aot-nmake-release\aot.exe",
    [string]$GameRoot = "game",
    [string]$OutDir = "artifacts\captures",
    [string[]]$ExtraArgs = @("--mnk_mode"),
    [switch]$StopExisting,
    [switch]$NoInput
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")

function Resolve-RepoPath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return $Path
    }
    return Join-Path $repoRoot $Path
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

function Normalize-ExtraArgs([string[]]$InputArgs) {
    $normalized = @()
    foreach ($arg in $InputArgs) {
        foreach ($commaPart in $arg.Split(",", [System.StringSplitOptions]::RemoveEmptyEntries)) {
            $normalized += $commaPart.Split([char[]]@(" ", "`t", "`r", "`n"), [System.StringSplitOptions]::RemoveEmptyEntries)
        }
    }
    return $normalized
}

$interopSource = @"
using System;
using System.Runtime.InteropServices;

public static class AotProbeInputInterop {
  [StructLayout(LayoutKind.Sequential)]
  public struct RECT {
    public int Left;
    public int Top;
    public int Right;
    public int Bottom;
  }

  [StructLayout(LayoutKind.Sequential)]
  public struct INPUT {
    public uint type;
    public INPUTUNION U;
  }

  [StructLayout(LayoutKind.Explicit)]
  public struct INPUTUNION {
    [FieldOffset(0)]
    public KEYBDINPUT ki;
  }

  [StructLayout(LayoutKind.Sequential)]
  public struct KEYBDINPUT {
    public ushort wVk;
    public ushort wScan;
    public uint dwFlags;
    public uint time;
    public UIntPtr dwExtraInfo;
  }

  [DllImport("user32.dll")]
  public static extern bool SetForegroundWindow(IntPtr hWnd);

  [DllImport("user32.dll")]
  public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

  [DllImport("user32.dll")]
  public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);

  [DllImport("user32.dll", SetLastError=true)]
  public static extern uint SendInput(uint nInputs, INPUT[] pInputs, int cbSize);
}
"@

Add-Type -TypeDefinition $interopSource
Add-Type -AssemblyName System.Drawing

function Focus-ProbeWindow([System.Diagnostics.Process]$Process) {
    $Process.Refresh()
    if ($Process.MainWindowHandle -eq [IntPtr]::Zero) {
        return $false
    }
    [AotProbeInputInterop]::ShowWindow($Process.MainWindowHandle, 9) | Out-Null
    [AotProbeInputInterop]::SetForegroundWindow($Process.MainWindowHandle) | Out-Null
    Start-Sleep -Milliseconds 60
    return $true
}

function Send-Key([System.Diagnostics.Process]$Process, [UInt16]$Vk, [int]$HoldMs = 140) {
    if (-not (Focus-ProbeWindow $Process)) {
        return
    }

    $down = New-Object AotProbeInputInterop+INPUT
    $down.type = 1
    $down.U.ki.wVk = $Vk
    $down.U.ki.wScan = 0
    $down.U.ki.dwFlags = 0
    $down.U.ki.time = 0
    $down.U.ki.dwExtraInfo = [UIntPtr]::Zero

    $up = New-Object AotProbeInputInterop+INPUT
    $up.type = 1
    $up.U.ki.wVk = $Vk
    $up.U.ki.wScan = 0
    $up.U.ki.dwFlags = 2
    $up.U.ki.time = 0
    $up.U.ki.dwExtraInfo = [UIntPtr]::Zero

    [AotProbeInputInterop]::SendInput(1, @($down), [Runtime.InteropServices.Marshal]::SizeOf([type][AotProbeInputInterop+INPUT])) | Out-Null
    Start-Sleep -Milliseconds $HoldMs
    [AotProbeInputInterop]::SendInput(1, @($up), [Runtime.InteropServices.Marshal]::SizeOf([type][AotProbeInputInterop+INPUT])) | Out-Null
    Start-Sleep -Milliseconds 90
}

function Save-WindowScreenshot([System.Diagnostics.Process]$Process, [string]$Path) {
    $Process.Refresh()
    if ($Process.HasExited -or $Process.MainWindowHandle -eq $null -or $Process.MainWindowHandle -eq [IntPtr]::Zero) {
        return $false
    }
    $rect = New-Object AotProbeInputInterop+RECT
    if (-not [AotProbeInputInterop]::GetWindowRect($Process.MainWindowHandle, [ref]$rect)) {
        return $false
    }
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    if ($width -le 0 -or $height -le 0) {
        return $false
    }
    $bitmap = New-Object System.Drawing.Bitmap($width, $height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size)
        $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
    return $true
}

$vk = @{
    Space = [UInt16]0x20
    Escape = [UInt16]0x1B
    Enter = [UInt16]0x0D
}

$exe = Resolve-RepoPath $ExePath
$gameRootPath = Resolve-RepoPath $GameRoot
$logsDir = Join-Path (Split-Path -Parent $exe) "logs"
$runRoot = Resolve-RepoPath $OutDir
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null

if (-not (Test-Path -LiteralPath $exe)) {
    throw "ReXGlue executable not found: $exe"
}
if (-not (Test-Path -LiteralPath $gameRootPath)) {
    throw "Game data root not found: $gameRootPath"
}

$existing = Get-Process | Where-Object { $_.ProcessName -like "aot" }
if ($existing) {
    if ($StopExisting) {
        foreach ($process in $existing) {
            Stop-ProcessByIdRobust $process.Id | Out-Null
        }
    } else {
        $existing | Select-Object ProcessName,Id,CPU,StartTime,Path
        throw "Refusing to launch while aot processes are already running. Re-run with -StopExisting if they should be terminated."
    }
}

$normalizedExtraArgs = Normalize-ExtraArgs $ExtraArgs
$argumentList = @("--game_data_root=$gameRootPath") + $normalizedExtraArgs
$startedAt = Get-Date
$runId = $startedAt.ToString("yyyyMMdd-HHmmss")
$runDir = Join-Path $runRoot $runId
New-Item -ItemType Directory -Force -Path $runDir | Out-Null

$proc = Start-Process -FilePath $exe -ArgumentList $argumentList -WorkingDirectory $repoRoot -PassThru
$watch = [Diagnostics.Stopwatch]::StartNew()

$windowReady = $false
while ($watch.Elapsed.TotalSeconds -lt 20 -and -not $proc.HasExited) {
    $proc.Refresh()
    if ($proc.MainWindowHandle -ne [IntPtr]::Zero) {
        $windowReady = $true
        Focus-ProbeWindow $proc | Out-Null
        break
    }
    Start-Sleep -Milliseconds 250
}

$nextScreenshotAt = 12.0
while ($watch.Elapsed.TotalSeconds -lt $Seconds -and -not $proc.HasExited) {
    if ($watch.Elapsed.TotalSeconds -ge $nextScreenshotAt) {
        $screenshotPath = Join-Path $runDir ("probe_{0:000}s.png" -f [int]$watch.Elapsed.TotalSeconds)
        Save-WindowScreenshot $proc $screenshotPath | Out-Null
        $nextScreenshotAt += 20.0
    }

    if ($NoInput) {
        Start-Sleep -Milliseconds 500
    } else {
        Send-Key $proc $vk.Space 120
        Start-Sleep -Milliseconds 500
        Send-Key $proc $vk.Escape 120
        Start-Sleep -Milliseconds 500
        Send-Key $proc $vk.Enter 120
        Start-Sleep -Milliseconds 500
    }
}

$finalScreenshot = Join-Path $runDir "probe_final.png"
Save-WindowScreenshot $proc $finalScreenshot | Out-Null

$proc.Refresh()
if (-not $proc.HasExited) {
    Stop-ProcessByIdRobust $proc.Id | Out-Null
    $status = "timed_out_stopped"
} else {
    $status = "exit_code=$($proc.ExitCode)"
}

Start-Sleep -Milliseconds 500
$latestLog = Get-ChildItem $logsDir -File -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTime -ge $startedAt.AddSeconds(-2) } |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1

Write-Output "status=$status"
Write-Output "seconds=$Seconds"
Write-Output "window_ready=$windowReady"
Write-Output "run_dir=$runDir"
Write-Output "exe=$exe"
Write-Output "game_data_root=$gameRootPath"
Write-Output "extra_args=$($normalizedExtraArgs -join ' ')"

if ($latestLog) {
    Write-Output "latest_log=$($latestLog.FullName)"
} else {
    Write-Output "latest_log="
}

Get-ChildItem $runDir -Filter *.png -File | Sort-Object Name | ForEach-Object {
    Write-Output "screenshot=$($_.FullName)"
}
