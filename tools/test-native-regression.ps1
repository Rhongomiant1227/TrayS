[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string]$Architecture = 'x64',
    [string]$SourceRoot
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($SourceRoot)) { $SourceRoot = $repoRoot }
$sourceDirectory = Join-Path $SourceRoot 'TrayS'
$toolchain = Get-ChildItem -LiteralPath (Join-Path $repoRoot '.buildtools\llvm-mingw') -Directory |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'bin\clang++.exe') } |
    Sort-Object Name -Descending | Select-Object -First 1
if (-not $toolchain) { throw 'Repository-local LLVM-MinGW is required.' }
$buildDirectory = Join-Path $repoRoot ('.build-native\review\native-tests-{0}' -f $Architecture)
New-Item -ItemType Directory -Path $buildDirectory -Force | Out-Null
$exePath = Join-Path $buildDirectory 'native-regression.exe'
$target = 'x86_64-w64-windows-gnu'
$arguments = @(
    '-target', $target, '-std=c++17', '-D_DEBUG', '-D_UNICODE', '-DUNICODE',
    '-DTRAYS_PORTABLE_COMPAT', '-O1', '-g0', '-static', '-I', $sourceDirectory,
    (Join-Path $PSScriptRoot 'tests\native-regression.cpp'),
    (Join-Path $sourceDirectory 'Function.cpp'), (Join-Path $sourceDirectory 'Update.cpp'), (Join-Path $sourceDirectory 'PawnIoSetup.cpp'),
    '-luser32', '-lgdi32', '-lcomctl32', '-lshell32', '-lole32', '-loleaut32', '-luuid',
    '-loleacc', '-ladvapi32', '-lpsapi', '-liphlpapi', '-lwinhttp', '-lpdh', '-ldwmapi', '-lversion',
    '-o', $exePath
)
& (Join-Path $toolchain.FullName 'bin\clang++.exe') @arguments
if ($LASTEXITCODE -ne 0) { throw 'Native regression harness failed to compile.' }
& $exePath
if ($LASTEXITCODE -ne 0) { throw 'Native regression failed.' }
