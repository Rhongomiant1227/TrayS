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
$powerShell32 = Join-Path $env:WINDIR 'SysWOW64\WindowsPowerShell\v1.0\powershell.exe'
$wrapperTest = Join-Path $PSScriptRoot 'test-lhm-wrapper.ps1'
foreach ($architecture in @('x64', 'x86')) {
    $nativeTest = Join-Path $repoRoot ('.build-native\cpu-temperature-{0}\cpu-temperature.exe' -f $architecture)
    if (-not (Test-Path -LiteralPath $nativeTest -PathType Leaf)) {
        throw "Build the $architecture native harness first with test-cpu-temperature.ps1 -BuildOnly."
    }
}
foreach ($platform in @('x64', 'Win32')) {
    Write-Output ("Checking the {0} C++/CLI wrapper against the CPU's sensors..." -f $platform)
    $powerShell = if ($platform -eq 'x64') { $powerShell64 } else { $powerShell32 }
    & $powerShell -NoProfile -ExecutionPolicy Bypass -File $wrapperTest -Platform $platform -SampleCount 5 -RequireCpuTemperature
    if ($LASTEXITCODE -ne 0) { throw "The $platform wrapper hardware test failed (exit $LASTEXITCODE)." }
}
foreach ($architecture in @('x64', 'x86')) {
    Write-Output ("Checking TrayS's {0} native temperature path..." -f $architecture)
    $nativeTest = Join-Path $repoRoot ('.build-native\cpu-temperature-{0}\cpu-temperature.exe' -f $architecture)
    & $nativeTest
    if ($LASTEXITCODE -ne 0) { throw "TrayS's $architecture native hardware test failed (exit $LASTEXITCODE)." }
}
Write-Output 'PASS: x64/x86 wrapper and TrayS native paths each returned five usable CPU temperatures.'
