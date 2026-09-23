<#
.SYNOPSIS
Configures, builds, and runs STAR's CTest suite (default: x64 Release, parallel).
.EXAMPLE
.\test.ps1
.\test.ps1 -Config Debug -Jobs 4 -Filter 'backend_ownership|gdi_input'
#>
[CmdletBinding()]
param(
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release",

    [ValidateSet("x64", "x86")]
    [string]$Arch = "x64",

    [int]$Jobs = 0,

    [string]$Filter = ""
)

$ErrorActionPreference = "Stop"

$root = $PSScriptRoot
$buildDir = Join-Path $root "build\$Arch"

if ($Jobs -le 0) {
    $Jobs = [int]$env:NUMBER_OF_PROCESSORS
    if ($Jobs -le 0) { $Jobs = 8 }
}

if ($Arch -eq "x86") { $cmakeArch = "Win32" } else { $cmakeArch = "x64" }

function Invoke-Checked([scriptblock]$Block) {
    & $Block
    if ($LASTEXITCODE -ne 0) {
        throw "command failed with exit code $LASTEXITCODE"
    }
}

Write-Host "==> Configuring $Arch ($Config, tests on)"
Invoke-Checked { cmake -B $buildDir -A $cmakeArch -DSTAR_BUILD_TESTS=ON -Wno-deprecated }

Write-Host "==> Building $Arch ($Config)"
Invoke-Checked { cmake --build $buildDir --config $Config }

Write-Host "==> Testing $Arch ($Config, $Jobs jobs)"
$ctestArgs = @("--test-dir", $buildDir, "-C", $Config, "--output-on-failure", "-j", "$Jobs")
if ($Filter -ne "") {
    $ctestArgs += @("-R", $Filter)
}
Invoke-Checked { ctest @ctestArgs }

Write-Host ""
Write-Host "Tests complete."
