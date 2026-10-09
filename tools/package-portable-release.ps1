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
$packageName = "TrayS_1.6.0_{0}" -f $label
$packageRoot = Join-Path $repoRoot ("dist\{0}" -f $packageName)
$archivePath = "$packageRoot.zip"

if (-not (Test-Path -LiteralPath $sourceExe -PathType Leaf)) {
    throw "Compatibility executable is missing: $sourceExe"
}
if ((Test-Path -LiteralPath $packageRoot) -or (Test-Path -LiteralPath $archivePath)) {
    if (-not $Force) { throw "Package already exists; pass -Force: $packageName" }
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
    'Version: 1.6.0',
    ("Architecture: {0}" -f $label),
    'Build: native Win32/LLVM-MinGW static runtime',
    'LibreHardwareMonitor wrapper and legacy WinRing0 files are intentionally omitted from this safe package.',
    'See COMPATIBILITY.md and MEMORY_AUDIT.md for scope and limitations.'
) | Set-Content -LiteralPath (Join-Path $packageRoot 'PACKAGE.txt') -Encoding UTF8

$hash = (Get-FileHash -LiteralPath (Join-Path $packageRoot 'TrayS.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
@("$hash  TrayS.exe") | Set-Content -LiteralPath (Join-Path $packageRoot 'SHA256SUMS.txt') -Encoding ASCII
Compress-Archive -LiteralPath $packageRoot -DestinationPath $archivePath -CompressionLevel Optimal
Write-Output "Package archive: $archivePath"
