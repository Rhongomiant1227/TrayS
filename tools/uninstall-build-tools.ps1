[CmdletBinding()]
param(
    [string]$InstallPath,
    [switch]$KeepInstallFolder
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($InstallPath)) {
    $InstallPath = Join-Path (Split-Path -Parent $PSScriptRoot) '.buildtools'
}

if (-not (Test-Path -LiteralPath $InstallPath)) {
    Write-Output "Build Tools install path does not exist: $InstallPath"
    exit 0
}

$installerCandidates = @(
    'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vs_installer.exe',
    'C:\Program Files\Microsoft Visual Studio\Installer\vs_installer.exe'
)
$installerPath = $installerCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if ([string]::IsNullOrWhiteSpace($installerPath)) {
    throw 'Visual Studio Installer was not found; do not delete the install folder manually.'
}

Write-Host "Uninstalling Visual Studio Build Tools from $InstallPath"
$installerProcess = Start-Process -FilePath $installerPath `
    -ArgumentList @('uninstall', '--installPath', $InstallPath, '--quiet', '--norestart') `
    -PassThru -Wait
$installerExitCode = $installerProcess.ExitCode
if ($installerExitCode -ne 0) {
    throw "Visual Studio Installer failed with exit code $installerExitCode; install folder was kept."
}

if (-not $KeepInstallFolder -and (Test-Path -LiteralPath $InstallPath)) {
    Remove-Item -LiteralPath $InstallPath -Recurse -Force
}

Write-Output 'Visual Studio Build Tools uninstall completed.'
