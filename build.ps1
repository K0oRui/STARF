<#
.SYNOPSIS
Builds STAR for x86 and x64 and packages the DLLs into dist/.
.EXAMPLE
.\build.ps1
.\build.ps1 -Config Debug
#>
[CmdletBinding()]
param(
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"

$root = $PSScriptRoot
$dist = Join-Path $root "dist"
$buildDir = Join-Path $root "build"

function Invoke-Checked([scriptblock]$Block) {
    & $Block
    if ($LASTEXITCODE -ne 0) {
        throw "command failed with exit code $LASTEXITCODE"
    }
}

Write-Host "==> Configuring x86"
Invoke-Checked { cmake -B (Join-Path $buildDir "x86") -A Win32 --fresh -Wno-deprecated }

Write-Host "==> Building x86 ($Config)"
Invoke-Checked { cmake --build (Join-Path $buildDir "x86") --config $Config }

Write-Host "==> Configuring x64"
Invoke-Checked { cmake -B (Join-Path $buildDir "x64") -A x64 --fresh -Wno-deprecated }

Write-Host "==> Building x64 ($Config)"
Invoke-Checked { cmake --build (Join-Path $buildDir "x64") --config $Config }

Write-Host "==> Installing to $dist"
Invoke-Checked { cmake --install (Join-Path $buildDir "x86") --config $Config --prefix $dist }
Invoke-Checked { cmake --install (Join-Path $buildDir "x64") --config $Config --prefix $dist }

Write-Host ""
Write-Host "Build complete:"
Write-Host "  $dist\steam_api.dll   (x86)"
Write-Host "  $dist\steam_api64.dll (x64)"