param(
    [string]$RexGlueSource = "tools\rexglue-sdk"
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
$mspackLinkDir = Join-Path $sourcePath "thirdparty\libmspack\cabextract\mspack"

if (-not (Test-Path -LiteralPath $mspackLinkDir)) {
    Write-Output "rexglue_symlink_repair_skipped=$mspackLinkDir"
    return
}

$repaired = 0
Get-ChildItem -LiteralPath $mspackLinkDir -File | ForEach-Object {
    if ($_.Length -gt 256) {
        return
    }

    $targetText = (Get-Content -LiteralPath $_.FullName -Raw).Trim()
    if ($targetText -notmatch "^\.\./\.\./") {
        return
    }

    $targetPath = [System.IO.Path]::GetFullPath((Join-Path $_.DirectoryName ($targetText -replace "/", "\")))
    if (-not (Test-Path -LiteralPath $targetPath)) {
        throw "Symlink target not found for $($_.FullName): $targetPath"
    }

    Copy-Item -LiteralPath $targetPath -Destination $_.FullName -Force
    $script:repaired += 1
}

Write-Output "rexglue_symlinks_materialized=$repaired"
