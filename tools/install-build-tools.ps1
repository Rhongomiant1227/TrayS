[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$installPath = Join-Path $repoRoot '.buildtools'
$logPath = Join-Path $repoRoot 'build-tools-install.log'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]$identity
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'This script must be run from an elevated PowerShell process.'
}

$winget = Get-Command winget.exe -ErrorAction SilentlyContinue
if ($null -eq $winget) {
    throw 'winget.exe was not found.'
}

$override = @(
    '--quiet',
    '--wait',
    '--norestart',
    '--nocache',
    '--installPath', $installPath,
    '--add', 'Microsoft.VisualStudio.Workload.VCTools',
    '--includeRecommended',
    '--add', 'Microsoft.VisualStudio.Component.VC.v143.CLI.Support'
) -join ' '

Write-Output "Installing Microsoft Visual Studio 2022 Build Tools to $installPath"
& $winget.Source install `
    --id Microsoft.VisualStudio.2022.BuildTools `
    --exact `
    --source winget `
    --accept-source-agreements `
    --accept-package-agreements `
    --log $logPath `
    --override $override

if ($LASTEXITCODE -ne 0) {
    throw "Build Tools installation failed with exit code $LASTEXITCODE. See $logPath"
}

$msbuildPath = Join-Path $installPath 'MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path -LiteralPath $msbuildPath -PathType Leaf)) {
    throw "Installation completed but MSBuild was not found at $msbuildPath"
}

Write-Output "Build Tools installation completed: $msbuildPath"
