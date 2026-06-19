param(
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot ".."))
)

$ErrorActionPreference = "Stop"
$rootPath = Resolve-Path $Root
$errors = New-Object System.Collections.Generic.List[string]

function Invoke-GitOrNull([string[]]$Arguments) {
    $previousErrorActionPreference = $ErrorActionPreference
    $script:LASTEXITCODE = 0
    try {
        $ErrorActionPreference = "Continue"
        $output = & git @Arguments 2>$null
        $exitCode = $LASTEXITCODE
        return [pscustomobject]@{
            ExitCode = $exitCode
            Output = @($output)
        }
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
}

function Get-RelativePathCompat([string]$BasePath, [string]$ChildPath) {
    $baseFull = [System.IO.Path]::GetFullPath($BasePath)
    if (-not $baseFull.EndsWith([System.IO.Path]::DirectorySeparatorChar)) {
        $baseFull += [System.IO.Path]::DirectorySeparatorChar
    }

    $childFull = [System.IO.Path]::GetFullPath($ChildPath)
    $baseUri = [System.Uri]$baseFull
    $childUri = [System.Uri]$childFull
    return [System.Uri]::UnescapeDataString($baseUri.MakeRelativeUri($childUri).ToString()) -replace "/", "\"
}

$trackedRelativePaths = @()
$mode = "git"

$gitRoot = Invoke-GitOrNull @("-C", $rootPath, "rev-parse", "--show-toplevel")
if ($gitRoot.ExitCode -eq 0) {
    $gitFiles = Invoke-GitOrNull @("-C", $rootPath, "ls-files")
    if ($gitFiles.ExitCode -ne 0) {
        throw "Unable to list tracked files with git."
    }
    $trackedRelativePaths = $gitFiles.Output
} else {
    $mode = "filesystem"
    $excludedRoots = @(
        ".git",
        "artifacts",
        "assets",
        "build",
        "config\generated",
        "game",
        "generated",
        "install",
        "iso",
        "logs",
        "out",
        "private",
        "rexglue-install",
        "tools"
    )

    $trackedRelativePaths = Get-ChildItem -LiteralPath $rootPath -Recurse -File -Force |
        ForEach-Object {
            Get-RelativePathCompat $rootPath $_.FullName
        } |
        Where-Object {
            $relative = $_ -replace "/", "\"
            if ($relative -eq "generated\.gitkeep") {
                return $true
            }
            foreach ($excludedRoot in $excludedRoots) {
                if ($relative -eq $excludedRoot -or $relative.StartsWith("$excludedRoot\")) {
                    return $false
                }
            }
            return $true
        }
}

$forbiddenExtensions = @(".iso", ".xex", ".xexp", ".bin", ".pe", ".png", ".jpg", ".jpeg")
foreach ($relative in $trackedRelativePaths) {
    $normalized = $relative -replace "/", "\"
    $extension = [System.IO.Path]::GetExtension($relative)
    if ($extension -in $forbiddenExtensions) {
        $errors.Add("Forbidden tracked file extension: $relative")
    }
    if ($normalized -match "^(game|generated\\default|build|out|tools|private|artifacts)\\") {
        $errors.Add("Forbidden tracked path: $relative")
    }
}

if ($errors.Count -gt 0) {
    $errors | ForEach-Object { Write-Error $_ }
    throw "Public tree check failed with $($errors.Count) issue(s)."
}

Write-Output "public_tree_ok=$rootPath mode=$mode"
