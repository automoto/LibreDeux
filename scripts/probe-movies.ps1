param(
    [string]$MoviesDir = "game\AO2Game\Movies",
    [string]$FfmpegExe = "",
    [string]$FfprobeExe = "",
    [switch]$Decode
)

# Go/no-go probe for host-side Bink movie playback (see docs/movie-play.md).
# Locates ffmpeg/ffprobe, reports codec/resolution/duration for every .bik, and
# optionally test-decodes a small and a large movie to confirm libavcodec's bink
# decoder accepts these files.

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
if ([System.IO.Path]::IsPathRooted($MoviesDir)) {
    $moviesPath = $MoviesDir
} else {
    $moviesPath = Join-Path $repoRoot $MoviesDir
}

function Find-Tool {
    param([string]$Explicit, [string]$Name)
    if ($Explicit) {
        if (Test-Path -LiteralPath $Explicit) { return (Resolve-Path -LiteralPath $Explicit).Path }
        throw "$Name not found at: $Explicit"
    }
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $candidates = @(
        "$env:LOCALAPPDATA\Tartube\msys64\mingw64\bin\$Name.exe",
        "C:\ffmpeg\bin\$Name.exe",
        "$env:USERPROFILE\scoop\shims\$Name.exe",
        "C:\ProgramData\chocolatey\bin\$Name.exe"
    )
    foreach ($c in $candidates) { if (Test-Path -LiteralPath $c) { return $c } }
    return $null
}

$ffmpeg = Find-Tool -Explicit $FfmpegExe -Name "ffmpeg"
$ffprobe = Find-Tool -Explicit $FfprobeExe -Name "ffprobe"

if (-not $ffmpeg) { throw "ffmpeg not found. Pass -FfmpegExe <path>." }
Write-Output "ffmpeg=$ffmpeg"
if ($ffprobe) { Write-Output "ffprobe=$ffprobe" }

# Confirm the bink decoders are compiled into this ffmpeg.
$decoders = & $ffmpeg -hide_banner -decoders 2>$null | Select-String -Pattern "\bbink" | ForEach-Object { $_.Line.Trim() }
Write-Output "--- bink decoders available ---"
if ($decoders) { $decoders | ForEach-Object { Write-Output $_ } } else { Write-Output "WARNING: no bink decoder reported by this ffmpeg" }

if (-not (Test-Path -LiteralPath $moviesPath)) { throw "Movies dir not found: $moviesPath" }

# ffmpeg writes stream info to stderr. In Windows PowerShell, letting a native
# exe's stderr reach the pipeline wraps each line as a NativeCommandError; run it
# via Start-Process with stderr redirected to a temp file and read it back.
$errFile = Join-Path $env:TEMP "bik_probe_ffmpeg.err"
function Invoke-Ffmpeg {
    param([string[]]$FfArgs)
    Start-Process -FilePath $ffmpeg -ArgumentList $FfArgs -NoNewWindow -Wait `
        -RedirectStandardError $errFile -RedirectStandardOutput "$errFile.out" | Out-Null
}
function Get-FfmpegStreamInfo {
    param([string]$Path)
    Invoke-Ffmpeg -FfArgs @("-hide_banner", "-i", $Path)
    if (Test-Path -LiteralPath $errFile) {
        Get-Content -LiteralPath $errFile |
            Select-String -Pattern "Duration|Video:|Audio:" | ForEach-Object { $_.Line.Trim() }
    }
}

$movies = Get-ChildItem -LiteralPath $moviesPath -Filter "*.bik" | Sort-Object Length
Write-Output "--- $($movies.Count) movies in $moviesPath ---"

foreach ($m in $movies) {
    $info = Get-FfmpegStreamInfo -Path $m.FullName
    $mb = "{0:N1}" -f ($m.Length / 1MB)
    Write-Output ""
    Write-Output "$($m.Name)  (${mb} MB)"
    $info | ForEach-Object { Write-Output "    $_" }
}

if ($Decode) {
    $tmp = Join-Path $env:TEMP "bik_probe"
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    $small = $movies | Select-Object -First 1
    $large = $movies | Select-Object -Last 1
    foreach ($t in @($small, $large)) {
        if (-not $t) { continue }
        Write-Output ""
        Write-Output "--- decode 3 frames: $($t.Name) ---"
        Get-ChildItem "$tmp\probe_*.png" -ErrorAction SilentlyContinue | Remove-Item -Force
        Invoke-Ffmpeg -FfArgs @("-hide_banner", "-loglevel", "error", "-i", $t.FullName, "-frames:v", "3", "$tmp\probe_%02d.png")
        $pngs = Get-ChildItem "$tmp\probe_*.png" -ErrorAction SilentlyContinue
        if ($pngs) {
            Write-Output "    OK: decoded $($pngs.Count) frames to $tmp"
        } else {
            Write-Output "    FAIL: no frames decoded"
        }
    }
}

Write-Output ""
Write-Output "probe=done"
