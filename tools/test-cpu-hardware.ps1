[CmdletBinding()]
param()

# Read only already-built tests. Never installs a driver, changes its ACL, or
# requests elevation. Run explicitly in an administrator Windows PowerShell.
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this hardware validation in an administrator Windows PowerShell. The official PawnIO driver requires an elevated token.'
}
$powerShell64 = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
$wrapperTest = Join-Path $PSScriptRoot 'test-lhm-wrapper.ps1'
$nativeTest = Join-Path $repoRoot '.build-native\cpu-temperature-x64\cpu-temperature.exe'
if (-not (Test-Path -LiteralPath $nativeTest -PathType Leaf)) {
    throw 'Build the x64 native harness first with test-cpu-temperature.ps1 -BuildOnly.'
}
Write-Output "Checking the x64 C++/CLI wrapper against the CPU's sensors..."
& $powerShell64 -NoProfile -ExecutionPolicy Bypass -File $wrapperTest -Platform x64 -SampleCount 5 -RequireCpuTemperature
if ($LASTEXITCODE -ne 0) { throw "The x64 wrapper hardware test failed (exit $LASTEXITCODE)." }
Write-Output "Checking TrayS's x64 native temperature path..."
& $nativeTest
if ($LASTEXITCODE -ne 0) { throw "TrayS's x64 native hardware test failed (exit $LASTEXITCODE)." }
Write-Output 'PASS: x64 wrapper and TrayS native path each returned five usable CPU temperatures.'
