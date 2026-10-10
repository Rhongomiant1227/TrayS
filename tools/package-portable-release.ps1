[CmdletBinding()]
param(
    [ValidateSet('x64', 'x86')]
    [string]$Architecture = 'x64',
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$label = $Architecture
$sourceRoot = Join-Path $repoRoot ("dist\TrayS-compat-win11-{0}" -f $label)
$sourceExe = Join-Path $sourceRoot ("TrayS-compat-win11-{0}.exe" -f $label)
$packageName = "TrayS_1.7.2_compat_{0}" -f $label
$packageRoot = Join-Path $repoRoot ("dist\{0}" -f $packageName)
$archivePath = "$packageRoot.zip"

# Check all computed outputs before any replacement, including -Force.
$distRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'dist')).TrimEnd('\') + '\'
foreach ($path in @($sourceRoot, $packageRoot, $archivePath)) {
    if (-not [IO.Path]::GetFullPath($path).StartsWith($distRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Package output must stay inside dist: $path"
    }
}
if ((Test-Path -LiteralPath $packageRoot) -or (Test-Path -LiteralPath $archivePath)) {
    if (-not $Force) { throw "Package already exists; pass -Force: $packageName" }
}

# Rebuild from the current sources instead of relabelling an arbitrary EXE
# that happens to be left in dist from an earlier release or architecture.
& (Join-Path $PSScriptRoot 'build-portable-compat-exe.ps1') -Architecture $Architecture -Force
if (-not (Test-Path -LiteralPath $sourceExe -PathType Leaf)) {
    throw "Compatibility executable is missing after the build: $sourceExe"
}
$bytes = [IO.File]::ReadAllBytes($sourceExe)
if ($bytes.Length -lt 64 -or [BitConverter]::ToUInt16($bytes, 0) -ne 0x5a4d) {
    throw "Invalid executable header: $sourceExe"
}
$peOffset = [BitConverter]::ToInt32($bytes, 0x3c)
if ($peOffset -lt 64 -or $peOffset -gt $bytes.Length - 6 -or
    [BitConverter]::ToUInt32($bytes, $peOffset) -ne 0x00004550) {
    throw "Invalid PE header: $sourceExe"
}
$expectedMachine = if ($Architecture -eq 'x64') { 0x8664 } else { 0x014c }
if ([BitConverter]::ToUInt16($bytes, $peOffset + 4) -ne $expectedMachine) {
    throw "Executable architecture does not match $Architecture"
}
$version = [Diagnostics.FileVersionInfo]::GetVersionInfo($sourceExe)
if ($version.FileMajorPart -ne 1 -or $version.FileMinorPart -ne 7 -or
    $version.FileBuildPart -ne 2 -or $version.FilePrivatePart -ne 0) {
    throw "Executable version does not match the 1.7.2 compatibility package: $($version.FileVersion)"
}

if ((Test-Path -LiteralPath $packageRoot) -or (Test-Path -LiteralPath $archivePath)) {
    if (Test-Path -LiteralPath $packageRoot) { Remove-Item -LiteralPath $packageRoot -Recurse -Force }
    if (Test-Path -LiteralPath $archivePath) { Remove-Item -LiteralPath $archivePath -Force }
}

New-Item -ItemType Directory -Path $packageRoot -Force | Out-Null
Copy-Item -LiteralPath $sourceExe -Destination (Join-Path $packageRoot 'TrayS.exe')
foreach ($name in @('README.md', 'COMPATIBILITY.md', 'MEMORY_AUDIT.md')) {
    Copy-Item -LiteralPath (Join-Path $repoRoot $name) -Destination $packageRoot
}
@(
    'TrayS maintained compatibility package',
    'Version: 1.7.2',
    ("Architecture: {0}" -f $label),
    'Build: native Win32/LLVM-MinGW static runtime',
    'This native compatibility build omits the C++/CLI LibreHardwareMonitor wrapper and kernel driver files.',
    'See COMPATIBILITY.md and MEMORY_AUDIT.md for scope and limitations.'
) | Set-Content -LiteralPath (Join-Path $packageRoot 'PACKAGE.txt') -Encoding UTF8

$hash = (Get-FileHash -LiteralPath (Join-Path $packageRoot 'TrayS.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
@("$hash  TrayS.exe") | Set-Content -LiteralPath (Join-Path $packageRoot 'SHA256SUMS.txt') -Encoding ASCII
Compress-Archive -LiteralPath $packageRoot -DestinationPath $archivePath -CompressionLevel Optimal
Write-Output "Package archive: $archivePath"
