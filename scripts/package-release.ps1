# Builds a self-contained Windows x64 release archive.
#
#   powershell -ExecutionPolicy Bypass -File scripts/package-release.ps1
#
# The archive contains the executable, the shader assets and the documentation.

param(
    [string]$Configuration = "Release",
    [string]$Version = "0.7.0"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $root "build"
$stage = Join-Path $buildDir "package\ExplosionSim-$Version-win64"
$archive = Join-Path $buildDir "ExplosionSim-$Version-win64.zip"

if (-not (Test-Path (Join-Path $buildDir "CMakeCache.txt"))) {
    throw "No build directory found. Configure the project first (see README.md)."
}

cmake --build $buildDir --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) {
    throw "Build failed."
}

if (Test-Path $stage) {
    Remove-Item -Recurse -Force $stage
}
New-Item -ItemType Directory -Force $stage | Out-Null

Copy-Item (Join-Path $buildDir "$Configuration\ExplosionSim.exe") $stage
Copy-Item (Join-Path $buildDir "$Configuration\ExplosionSimUpdater.exe") $stage
Copy-Item -Recurse (Join-Path $buildDir "$Configuration\assets") $stage
Copy-Item (Join-Path $root "README.md"), (Join-Path $root "LICENSE"),
          (Join-Path $root "CHANGELOG.md") $stage
Copy-Item (Join-Path $root "third_party\miniz\LICENSE") (Join-Path $stage "LICENSE-miniz.txt")

if (Test-Path $archive) {
    Remove-Item -Force $archive
}
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $archive -CompressionLevel Optimal

$sizeMb = [math]::Round((Get-Item $archive).Length / 1MB, 1)
Write-Host "Created $archive ($sizeMb MB)"
