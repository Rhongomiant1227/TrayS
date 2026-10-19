[CmdletBinding()]
param(
    [string]$InstallerPath,
    [switch]$Download
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($InstallerPath)) {
    $InstallerPath = Join-Path $repoRoot '.buildtools\pawnio\PawnIO_setup_2.2.0.exe'
}
if (-not (Test-Path -LiteralPath $InstallerPath -PathType Leaf)) {
    throw 'Pass -InstallerPath with the official PawnIO 2.2.0 installer for read-only validation.'
}
$toolchain = Get-ChildItem -LiteralPath (Join-Path $repoRoot '.buildtools\llvm-mingw') -Directory |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'bin\clang++.exe') } |
    Sort-Object Name -Descending | Select-Object -First 1
if (-not $toolchain) { throw 'Repository-local LLVM-MinGW is required.' }
$buildDirectory = Join-Path $repoRoot '.build-native\pawnio-setup'
New-Item -ItemType Directory -Path $buildDirectory -Force | Out-Null
$exePath = Join-Path $buildDirectory 'pawnio-setup-test.exe'
$arguments = @(
    '-target', 'x86_64-w64-windows-gnu', '-std=c++17', '-D_UNICODE', '-DUNICODE',
    '-O1', '-static', '-municode', '-I', (Join-Path $repoRoot 'TrayS'),
    (Join-Path $PSScriptRoot 'tests\pawnio-setup.cpp'), '-ladvapi32', '-lole32', '-luser32', '-o', $exePath
)
& (Join-Path $toolchain.FullName 'bin\clang++.exe') @arguments
if ($LASTEXITCODE -ne 0) { throw 'PawnIO setup test failed to compile.' }
$testArguments = @($InstallerPath)
if ($Download) { $testArguments += '--download' }
& $exePath @testArguments
if ($LASTEXITCODE -ne 0) { throw "PawnIO setup validation failed (exit $LASTEXITCODE)." }
