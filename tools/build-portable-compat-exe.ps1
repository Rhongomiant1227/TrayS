[CmdletBinding()]
param(
    [string]$OutputDirectory,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repoRoot 'dist\TrayS-compat-win11-x64'
} elseif (-not [IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $repoRoot $OutputDirectory
}

function Resolve-WithinRepository([string]$path, [string]$label) {
    $fullPath = [IO.Path]::GetFullPath($path)
    $rootPath = ([IO.Path]::GetFullPath($repoRoot)).TrimEnd('\') + '\'
    if (-not $fullPath.StartsWith($rootPath, [StringComparison]::OrdinalIgnoreCase)) {
        throw "$label must stay inside the repository: $fullPath"
    }
    return $fullPath
}

$OutputDirectory = Resolve-WithinRepository $OutputDirectory 'OutputDirectory'
$buildDirectory = Resolve-WithinRepository (Join-Path $repoRoot '.build-native\portable-compat-x64') 'build directory'
$archivePath = Resolve-WithinRepository (Join-Path (Split-Path -Parent $OutputDirectory) ((Split-Path -Leaf $OutputDirectory) + '.zip')) 'archive path'
$sourceDirectory = Join-Path $repoRoot 'TrayS'

$toolchain = Get-ChildItem -LiteralPath (Join-Path $repoRoot '.buildtools\llvm-mingw') -Directory -ErrorAction SilentlyContinue |
    Where-Object {
        (Test-Path -LiteralPath (Join-Path $_.FullName 'bin\clang++.exe')) -and
        (Test-Path -LiteralPath (Join-Path $_.FullName 'bin\windres.exe'))
    } |
    Sort-Object Name -Descending |
    Select-Object -First 1
if ($null -eq $toolchain) {
    throw 'The portable LLVM-MinGW toolchain was not found under .buildtools\llvm-mingw. Run the project build-tool setup first.'
}

$toolDirectory = Join-Path $toolchain.FullName 'bin'
$clang = Join-Path $toolDirectory 'clang++.exe'
$windres = Join-Path $toolDirectory 'windres.exe'
$readobj = Join-Path $toolDirectory 'llvm-readobj.exe'
foreach ($tool in @($clang, $windres, $readobj)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Required portable build tool is missing: $tool"
    }
}

if ((Test-Path -LiteralPath $OutputDirectory) -or (Test-Path -LiteralPath $buildDirectory) -or (Test-Path -LiteralPath $archivePath)) {
    if (-not $Force) {
        throw "Build output already exists. Pass -Force to replace it: $OutputDirectory"
    }
    if (Test-Path -LiteralPath $OutputDirectory) {
        Remove-Item -LiteralPath $OutputDirectory -Recurse -Force
    }
    if (Test-Path -LiteralPath $buildDirectory) {
        Remove-Item -LiteralPath $buildDirectory -Recurse -Force
    }
    if (Test-Path -LiteralPath $archivePath) {
        Remove-Item -LiteralPath $archivePath -Force
    }
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
New-Item -ItemType Directory -Path $buildDirectory -Force | Out-Null

$compileFlags = @(
    '-target', 'x86_64-w64-windows-gnu',
    '-std=c++17',
    '-D_DEBUG',
    '-D_UNICODE',
    '-DUNICODE',
    '-D_WIN64',
    '-DTRAYS_PORTABLE_COMPAT',
    '-O2',
    '-g0',
    '-I', $sourceDirectory
)

foreach ($sourceName in @('TrayS.cpp', 'Function.cpp', 'Update.cpp')) {
    $sourcePath = Join-Path $sourceDirectory $sourceName
    $objectPath = Join-Path $buildDirectory (($sourceName -replace '\.cpp$', '') + '.o')
    if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
        throw "Source file is missing: $sourcePath"
    }
    Write-Host ("Compiling {0}" -f $sourceName)
    & $clang @compileFlags -c $sourcePath -o $objectPath
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $objectPath -PathType Leaf)) {
        throw "LLVM-MinGW failed to compile $sourceName (exit code $LASTEXITCODE)."
    }
}

# TrayS.rc is stored as UTF-16LE for Visual Studio. llvm-rc/windres does not
# accept that BOM, so convert only the temporary build copy and leave the
# checked-in resource unchanged.
$resourcePath = Join-Path $sourceDirectory 'TrayS.rc'
$temporaryResourcePath = Join-Path $buildDirectory 'TrayS.rc'
$resourceText = [IO.File]::ReadAllText($resourcePath, [Text.Encoding]::Unicode)
$utf8Bom = New-Object System.Text.UTF8Encoding($true)
[IO.File]::WriteAllText($temporaryResourcePath, $resourceText, $utf8Bom)
$resourceObjectPath = Join-Path $buildDirectory 'TrayS_res.o'
Push-Location $sourceDirectory
try {
    Write-Host 'Compiling TrayS.rc'
    & $windres '--codepage=65001' '--target=x86_64-w64-mingw32' '-I' $sourceDirectory $temporaryResourcePath '-o' $resourceObjectPath
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $resourceObjectPath -PathType Leaf)) {
        throw "LLVM-MinGW failed to compile TrayS.rc (exit code $LASTEXITCODE)."
    }
} finally {
    Pop-Location
}

$exePath = Join-Path $OutputDirectory 'TrayS-compat-win11-x64.exe'
$linkLibraries = @(
    '-luser32', '-lgdi32', '-lcomctl32', '-lshell32', '-lole32', '-loleaut32', '-luuid',
    '-loleacc', '-ladvapi32', '-lpsapi', '-liphlpapi', '-lwinhttp', '-lpdh',
    '-ldwmapi'
)
Write-Host 'Linking a self-contained x64 GUI executable'
# _DEBUG selects the regular wWinMain path in the legacy source. The binary is
# still optimized, and -static removes the compiler's libc++/libunwind DLL
# dependency so the copied EXE runs directly from the output directory.
& $clang '-target' 'x86_64-w64-windows-gnu' '-municode' '-mwindows' '-static' '-D_DEBUG' '-DTRAYS_PORTABLE_COMPAT' '-O2' '-o' $exePath `
    (Join-Path $buildDirectory 'TrayS.o') `
    (Join-Path $buildDirectory 'Function.o') `
    (Join-Path $buildDirectory 'Update.o') `
    $resourceObjectPath @linkLibraries
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $exePath -PathType Leaf)) {
    throw "LLVM-MinGW failed to link TrayS (exit code $LASTEXITCODE)."
}

$headerText = (& $readobj '--file-headers' $exePath | Out-String)
if ($headerText -notmatch 'IMAGE_FILE_MACHINE_AMD64' -or $headerText -notmatch 'IMAGE_SUBSYSTEM_WINDOWS_GUI') {
    throw 'The generated file is not a Windows x64 GUI executable.'
}
$imports = (& $readobj '--coff-imports' $exePath | Out-String)
if ($imports -match '(?im)Name:\s+(libc\+\+|libunwind|libwinpthread-1)\.dll') {
    throw 'The generated executable still imports an LLVM-MinGW runtime DLL.'
}

Copy-Item -LiteralPath (Join-Path $repoRoot 'README.md') -Destination (Join-Path $OutputDirectory 'README.md')
Copy-Item -LiteralPath (Join-Path $repoRoot 'COMPATIBILITY.md') -Destination (Join-Path $OutputDirectory 'COMPATIBILITY.md')
Copy-Item -LiteralPath (Join-Path $repoRoot 'MEMORY_AUDIT.md') -Destination (Join-Path $OutputDirectory 'MEMORY_AUDIT.md')
$buildInfo = @(
    'TrayS compatibility validation executable',
    'Target: Windows 10/11 x64',
    'Entry path: Win32 GUI (wWinMain)',
    'Runtime: LLVM-MinGW statically linked C++ runtime',
    ('Toolchain: {0}' -f $toolchain.Name),
    ('Built: {0}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss K')),
    'This is a compatibility validation build. The standard MSVC/C++/CLI release package remains available through package-release.ps1 when Visual Studio Build Tools are installed.',
    'The default build does not include LibreHardwareMonitor, WinRing0, PawnIO, Ols, or vendor driver files.'
)
$buildInfo | Set-Content -LiteralPath (Join-Path $OutputDirectory 'BUILD.txt') -Encoding UTF8

Compress-Archive -LiteralPath $OutputDirectory -DestinationPath $archivePath -CompressionLevel Optimal

Write-Output ("Compatibility executable: {0}" -f $exePath)
Write-Output ("Build intermediates:      {0}" -f $buildDirectory)
Write-Output ("Compatibility archive:   {0}" -f $archivePath)
