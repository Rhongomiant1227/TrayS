[CmdletBinding()]
param(
    [ValidateSet('x64', 'x86')]
    [string]$Architecture = 'x64',

    [switch]$BuildOnly
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$sourceDirectory = Join-Path $repoRoot 'TrayS'
$toolchain = Get-ChildItem -LiteralPath (Join-Path $repoRoot '.buildtools\llvm-mingw') -Directory |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'bin\clang++.exe') } |
    Sort-Object Name -Descending | Select-Object -First 1
if (-not $toolchain) { throw 'Repository-local LLVM-MinGW is required.' }
$buildDirectory = Join-Path $repoRoot ('.build-native\cpu-temperature-{0}' -f $Architecture)
New-Item -ItemType Directory -Path $buildDirectory -Force | Out-Null
$exePath = Join-Path $buildDirectory 'cpu-temperature.exe'
$target = if ($Architecture -eq 'x64') { 'x86_64-w64-windows-gnu' } else { 'i686-w64-windows-gnu' }
$arguments = @(
    '-target', $target, '-std=c++17', '-D_DEBUG', '-D_UNICODE', '-DUNICODE',
    '-DTRAYS_PORTABLE_COMPAT', '-O1', '-g0', '-static', '-I', $sourceDirectory,
    (Join-Path $PSScriptRoot 'tests\cpu-temperature.cpp'),
    (Join-Path $sourceDirectory 'Function.cpp'), (Join-Path $sourceDirectory 'Update.cpp'),
    '-luser32', '-lgdi32', '-lcomctl32', '-lshell32', '-lole32', '-loleaut32', '-luuid',
    '-loleacc', '-ladvapi32', '-lpsapi', '-liphlpapi', '-lwinhttp', '-lpdh', '-ldwmapi',
    '-o', $exePath
)
& (Join-Path $toolchain.FullName 'bin\clang++.exe') @arguments
if ($LASTEXITCODE -ne 0) { throw 'CPU temperature integration harness failed to compile.' }

$buildPlatform = if ($Architecture -eq 'x64') { 'x64' } else { 'Win32' }
$runtimeDirectory = Join-Path $repoRoot ("Bin\{0}\Release" -f $buildPlatform)
if (-not (Test-Path -LiteralPath (Join-Path $runtimeDirectory 'OpenHardwareMonitorApi.dll'))) {
    throw "Build the $buildPlatform Release monitor wrapper before running this test."
}
Get-ChildItem -LiteralPath $runtimeDirectory -Filter '*.dll' -File | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $buildDirectory -Force
}
if ($BuildOnly) {
    Write-Output "Built CPU temperature integration test: $exePath"
    return
}
& $exePath
if ($LASTEXITCODE -ne 0) { throw "CPU temperature integration test failed (exit $LASTEXITCODE)." }
