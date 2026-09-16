[CmdletBinding()]
param(
    [ValidateSet('x64', 'Win32')]
    [string]$Platform = 'x64',

    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',

    [string]$PackageName,

    [string]$ConfigSourceDirectory,

    [switch]$Force,

    # LHM 0.9.4 still contains a legacy WinRing0 backend. Keep it out of the
    # default package; include it only when an operator explicitly requests an
    # isolated compatibility package for testing.
    [switch]$IncludeLhm
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

if ([string]::IsNullOrWhiteSpace($PackageName)) {
    $platformLabel = if ($Platform -eq 'x64') { 'x64' } else { 'x86' }
    $PackageName = "_${platformLabel}_ALL_1.4.0"
}

$solutionPath = Join-Path $repoRoot 'TrayS.sln'
$buildOutput = Join-Path $repoRoot ("Bin\{0}\{1}" -f $Platform, $Configuration)
$packageRoot = Join-Path $repoRoot ("dist\{0}" -f $PackageName)
$archivePath = Join-Path $repoRoot ("dist\{0}.zip" -f $PackageName)
$machine = if ($Platform -eq 'x64') { 'x64' } else { 'x86' }
# The solution exposes the 32-bit configuration as x86, while the individual
# Visual C++ projects retain their historical Win32 platform name.
$solutionPlatform = if ($Platform -eq 'Win32') { 'x86' } else { $Platform }

if (-not (Test-Path -LiteralPath $solutionPath)) {
    throw "Solution file is missing: $solutionPath"
}

# Run the repository's read-only compatibility checks before asking MSBuild to
# produce a package. This does not start TrayS or load any driver.
$validationScript = Join-Path $PSScriptRoot 'validate-compatibility.ps1'
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $validationScript
if ($LASTEXITCODE -ne 0) {
    throw 'Compatibility validation failed; package was not created.'
}

$msbuild = Get-Command msbuild.exe -ErrorAction SilentlyContinue
if ($null -eq $msbuild) {
    $candidatePaths = @(
        (Join-Path $repoRoot '.buildtools\MSBuild\Current\Bin\MSBuild.exe'),
        'C:\Program Files\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe',
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe',
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe',
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe'
    ) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if ($candidatePaths) {
        $msbuildPath = $candidatePaths
    }
} else {
    $msbuildPath = $msbuild.Source
}

if ([string]::IsNullOrWhiteSpace($msbuildPath)) {
    throw @'
MSBuild was not found. Install Visual Studio 2022 Build Tools with:
  Desktop development with C++
  MSVC v143 build tools
  Windows 10/11 SDK
  C++/CLI support for v143 build tools
Then run this script again from the repository root.
'@
}

# C++/CLI projects link against MSCOREE.lib. Some lightweight Build Tools
# installations do not include the .NET Framework SDK import library, even
# though the Windows runtime provides mscoree.dll. Generate a local import
# library from the system DLL export list so the build remains self-contained.
$mscoreeDef = Join-Path $repoRoot 'tools\mscoree.def'
$mscoreeLib = Join-Path $repoRoot ("tools\mscoree-{0}.lib" -f $machine)
$projectMscoreeLib = Join-Path $repoRoot 'OpenHardwareMonitorApi\mscoree.lib'
if (-not (Test-Path -LiteralPath $mscoreeLib -PathType Leaf)) {
    $libCandidates = Get-ChildItem -LiteralPath (Join-Path $repoRoot '.buildtools\VC\Tools\MSVC') -Recurse -Filter lib.exe -File -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match ("\\bin\\Host(?:x64|x86)\\{0}\\lib\.exe$" -f $machine) } |
        Select-Object -First 1 -ExpandProperty FullName
    if ([string]::IsNullOrWhiteSpace($libCandidates) -or -not (Test-Path -LiteralPath $mscoreeDef -PathType Leaf)) {
        throw 'MSCOREE import library is missing and lib.exe/mscoree.def could not be found.'
    }
    & $libCandidates /nologo ("/machine:{0}" -f $machine) /def:$mscoreeDef /out:$mscoreeLib
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $mscoreeLib -PathType Leaf)) {
        throw 'Failed to generate the local MSCOREE import library.'
    }
}
Copy-Item -LiteralPath $mscoreeLib -Destination $projectMscoreeLib -Force

Write-Host ("Building {0}|{1} with {2}" -f $Configuration, $Platform, $msbuildPath)
$buildArguments = @(
    $solutionPath,
    '/m',
    '/t:Build',
    "/p:Configuration=$Configuration",
    "/p:Platform=$solutionPlatform",
    "/p:MSCOREE_LIB=$mscoreeLib",
    '/p:BuildProjectReferences=true',
    '/nologo'
)

# The repository can build without a machine-wide .NET Framework Developer
# Pack by using the pinned NuGet reference assemblies kept under tools/.
$referenceRoot = Join-Path $repoRoot 'tools\reference-assemblies\build\.NETFramework\v4.7.2'
if (Test-Path -LiteralPath (Join-Path $referenceRoot 'mscorlib.dll')) {
    $buildArguments += "/p:FrameworkPathOverride=$referenceRoot"
    $buildArguments += "/p:TargetFrameworkRootPath=$(Split-Path -Parent $referenceRoot)\"
}

# Use the newest Windows 10 SDK already installed on the machine. The project
# source retains its historical SDK value, but a newer SDK is source- and
# binary-compatible for this user-mode build.
$sdkIncludeRoot = 'C:\Program Files (x86)\Windows Kits\10\Include'
$sdkVersion = Get-ChildItem -LiteralPath $sdkIncludeRoot -Directory -ErrorAction SilentlyContinue |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'um') } |
    Sort-Object Name -Descending |
    Select-Object -First 1 -ExpandProperty Name
if (-not [string]::IsNullOrWhiteSpace($sdkVersion)) {
    $buildArguments += "/p:WindowsTargetPlatformVersion=$sdkVersion"
}

& $msbuildPath @buildArguments
if ($LASTEXITCODE -ne 0) {
    throw "MSBuild failed with exit code $LASTEXITCODE; package was not created."
}

$requiredFiles = @(
    (Join-Path $buildOutput 'TrayS.exe'),
    (Join-Path $buildOutput 'OpenHardwareMonitorApi.dll'),
    (Join-Path $repoRoot 'OpenHardwareMonitorApi/LibreHardwareMonitorLib.dll'),
    (Join-Path $repoRoot 'OpenHardwareMonitorApi/HidSharp.dll')
)
foreach ($requiredFile in $requiredFiles) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "Expected build/package input is missing: $requiredFile"
    }
}

if ((Test-Path -LiteralPath $packageRoot) -or (Test-Path -LiteralPath $archivePath)) {
    if (-not $Force) {
        throw "Package already exists. Choose another -PackageName or pass -Force explicitly: $packageRoot"
    }
    if (Test-Path -LiteralPath $packageRoot) {
        Remove-Item -LiteralPath $packageRoot -Recurse -Force
    }
    if (Test-Path -LiteralPath $archivePath) {
        Remove-Item -LiteralPath $archivePath -Force
    }
}

New-Item -ItemType Directory -Path $packageRoot -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $buildOutput 'TrayS.exe') -Destination $packageRoot
if ($IncludeLhm) {
    Copy-Item -LiteralPath (Join-Path $buildOutput 'OpenHardwareMonitorApi.dll') -Destination $packageRoot
    Copy-Item -LiteralPath (Join-Path $repoRoot 'OpenHardwareMonitorApi/LibreHardwareMonitorLib.dll') -Destination $packageRoot
    Copy-Item -LiteralPath (Join-Path $repoRoot 'OpenHardwareMonitorApi/HidSharp.dll') -Destination $packageRoot
}
Copy-Item -LiteralPath (Join-Path $repoRoot 'COMPATIBILITY.md') -Destination $packageRoot

if (-not [string]::IsNullOrWhiteSpace($ConfigSourceDirectory)) {
    if (-not (Test-Path -LiteralPath $ConfigSourceDirectory -PathType Container)) {
        throw "Config source directory does not exist: $ConfigSourceDirectory"
    }
    foreach ($configName in @('TrayS.dat', 'TrayS.xml')) {
        $configPath = Join-Path $ConfigSourceDirectory $configName
        if (Test-Path -LiteralPath $configPath -PathType Leaf) {
            Copy-Item -LiteralPath $configPath -Destination $packageRoot
        }
    }
}

# The maintained default package must not carry the old standalone driver or
# import library. Fail closed if a future build step accidentally introduces
# one of those artifacts.
$forbiddenNames = @('WinRing0x32.sys', 'WinRing0x64.sys', 'WinRing0x64.dll', 'OpenHardwareMonitorApi.lib', 'OlsApiInit.h', 'OlsApiInitDef.h', 'OlsDef.h')
foreach ($forbiddenName in $forbiddenNames) {
    if (Test-Path -LiteralPath (Join-Path $packageRoot $forbiddenName)) {
        throw "Forbidden legacy artifact was copied into the package: $forbiddenName"
    }
}

$manifestLines = @(
    "TrayS maintained package",
    ("Platform: {0}" -f $Platform),
    ("Configuration: {0}" -f $Configuration),
    ("Built: {0}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss K')),
    $(if ($IncludeLhm) { 'LHM compatibility payload included by explicit request; WinRing0 path remains opt-in.' } else { 'Safe package: LHM/WinRing0 payload omitted; vendor GPU APIs remain in TrayS.exe.' }),
    'This package was produced from the current working tree.'
)
$manifestLines | Set-Content -LiteralPath (Join-Path $packageRoot 'PACKAGE.txt') -Encoding UTF8

$hashLines = Get-ChildItem -LiteralPath $packageRoot -File |
    Sort-Object Name |
    ForEach-Object {
        $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        "{0}  {1}" -f $hash, $_.Name
    }
$hashLines | Set-Content -LiteralPath (Join-Path $packageRoot 'SHA256SUMS.txt') -Encoding ASCII

$archiveParent = Split-Path -Parent $archivePath
New-Item -ItemType Directory -Path $archiveParent -Force | Out-Null
Compress-Archive -LiteralPath $packageRoot -DestinationPath $archivePath -CompressionLevel Optimal

Write-Output ("Package directory: {0}" -f $packageRoot)
Write-Output ("Package archive:   {0}" -f $archivePath)
