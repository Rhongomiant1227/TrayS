[CmdletBinding()]
param()

# Read-only diagnostics for TrayS compatibility triage.  This script never
# starts TrayS, loads its optional monitor DLL, installs a driver, or changes
# the registry.  It only queries Windows, Explorer's taskbar HWND tree,
# performance counters, and files already present on disk.

$ErrorActionPreference = 'Continue'
$repoRoot = Split-Path -Parent $PSScriptRoot

function Write-Section([string]$title) {
    Write-Output ""
    Write-Output ("=== {0} ===" -f $title)
}

function Get-PeMachine([string]$path) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        return 'missing'
    }
    try {
        $bytes = [IO.File]::ReadAllBytes($path)
        if ($bytes.Length -lt 0x40) { return 'invalid' }
        $peOffset = [BitConverter]::ToInt32($bytes, 0x3c)
        if ($peOffset -lt 0 -or $peOffset + 6 -gt $bytes.Length) { return 'invalid' }
        $machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
        switch ($machine) {
            0x014c { 'x86' }
            0x8664 { 'x64' }
            0xaa64 { 'arm64' }
            default { ('0x{0:x4}' -f $machine) }
        }
    }
    catch {
        'unreadable'
    }
}

if (-not ('TraySCompatibilityNative' -as [type])) {
    Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class TraySCompatibilityNative
{
    public delegate bool EnumChildProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindow(string className, string windowName);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr childAfter, string className, string windowName);
    [DllImport("user32.dll")]
    public static extern bool EnumChildWindows(IntPtr parent, EnumChildProc callback, IntPtr lParam);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassName(IntPtr hWnd, StringBuilder className, int maxChars);
    [DllImport("user32.dll")]
    public static extern IntPtr GetParent(IntPtr hWnd);
}
'@ -ErrorAction Stop
}

Write-Section 'OS and process architecture'
$os = Get-CimInstance Win32_OperatingSystem
if ($os) {
    Write-Output ("Product: {0}; Version: {1}; Build: {2}; Architecture: {3}" -f `
        $os.Caption, $os.Version, $os.BuildNumber, $os.OSArchitecture)
}
Write-Output ("PowerShell process: {0}-bit; OS is 64-bit: {1}" -f ([IntPtr]::Size * 8), [Environment]::Is64BitOperatingSystem)
try {
    $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1 Name,Manufacturer,Architecture,AddressWidth
    if ($cpu) { Write-Output ("CPU: {0}; Vendor: {1}; AddressWidth: {2}" -f $cpu.Name,$cpu.Manufacturer,$cpu.AddressWidth) }
    $gpus = Get-CimInstance Win32_VideoController | Select-Object Name,AdapterCompatibility,DriverVersion
    foreach ($gpu in $gpus) { Write-Output ("GPU: {0}; Vendor: {1}; Driver: {2}" -f $gpu.Name,$gpu.AdapterCompatibility,$gpu.DriverVersion) }
}
catch { Write-Output ("Hardware query failed: {0}" -f $_.Exception.Message) }

Write-Section 'Explorer taskbar window discovery'
$tray = [TraySCompatibilityNative]::FindWindow('Shell_TrayWnd', $null)
Write-Output ("Shell_TrayWnd: 0x{0:x}" -f $tray.ToInt64())
$classes = @(
    'ReBarWindow32',
    'Start',
    'TrayNotifyWnd',
    'MSTaskSwWClass',
    'MSTaskListWClass',
    'ToolbarWindow32',
    'Windows.UI.Composition.DesktopWindowContentBridge'
)
foreach ($className in $classes) {
    $direct = [TraySCompatibilityNative]::FindWindowEx($tray, [IntPtr]::Zero, $className, $null)
    Write-Output ("FindWindowEx direct {0}: 0x{1:x}" -f $className,$direct.ToInt64())
}
$found = @{}
$callback = [TraySCompatibilityNative+EnumChildProc] {
    param($hWnd, $lParam)
    $name = New-Object Text.StringBuilder 128
    $length = [TraySCompatibilityNative]::GetClassName($hWnd, $name, $name.Capacity)
    if ($length -gt 0 -and $found.ContainsKey($name.ToString()) -eq $false) {
        $found[$name.ToString()] = $hWnd
    }
    return $true
}
if ($tray -ne [IntPtr]::Zero) {
    [TraySCompatibilityNative]::EnumChildWindows($tray, $callback, [IntPtr]::Zero) | Out-Null
}
foreach ($className in $classes) {
    $descendant = if ($found.ContainsKey($className)) { $found[$className] } else { [IntPtr]::Zero }
    Write-Output ("EnumChildWindows {0}: 0x{1:x}" -f $className,$descendant.ToInt64())
}

Write-Section 'ACPI thermal-zone performance counters'
$thermalCounters = @(
    '\Thermal Zone Information(*)\High Precision Temperature',
    '\Thermal Zone Information(*)\Temperature',
    '\Thermal Zone Information(_Total)\High Precision Temperature',
    '\Thermal Zone Information(_Total)\Temperature'
)
foreach ($counterPath in $thermalCounters) {
    try {
        $samples = (Get-Counter -Counter $counterPath -ErrorAction Stop).CounterSamples
        if ($samples) {
            foreach ($sample in $samples) {
                Write-Output ("{0} => value={1} status={2}" -f $sample.Path,$sample.CookedValue,$sample.Status)
            }
        }
        else { Write-Output ("{0} => no samples" -f $counterPath) }
    }
    catch { Write-Output ("{0} => unavailable: {1}" -f $counterPath,$_.Exception.Message) }
}

Write-Section 'Optional vendor DLLs'
$vendorDlls = @(
    "$env:WINDIR\System32\nvapi64.dll",
    "$env:WINDIR\System32\nvapi.dll",
    "$env:WINDIR\System32\atiadlxx.dll",
    "$env:WINDIR\System32\atiadlxy.dll",
    "$env:WINDIR\SysWOW64\nvapi.dll",
    "$env:WINDIR\SysWOW64\atiadlxy.dll"
)
foreach ($dll in $vendorDlls) {
    if (Test-Path -LiteralPath $dll -PathType Leaf) {
        $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($dll).FileVersion
        Write-Output ("{0}: present; PE={1}; version={2}" -f $dll,(Get-PeMachine $dll),$version)
    }
}

Write-Section 'Repository monitor payload and build tools'
$payloads = @(
    (Join-Path $repoRoot 'OpenHardwareMonitorApi\OpenHardwareMonitorApi.dll'),
    (Join-Path $repoRoot 'OpenHardwareMonitorApi\LibreHardwareMonitorLib.dll'),
    (Join-Path $repoRoot 'OpenHardwareMonitorApi\HidSharp.dll')
)
foreach ($payload in $payloads) {
    if (Test-Path -LiteralPath $payload -PathType Leaf) {
        Write-Output ("{0}: present; PE={1}; size={2}" -f $payload,(Get-PeMachine $payload),(Get-Item -LiteralPath $payload).Length)
    }
    else { Write-Output ("{0}: missing" -f $payload) }
}
$msbuild = Get-Command msbuild.exe -ErrorAction SilentlyContinue
if ($msbuild) { Write-Output ("MSBuild: {0}" -f $msbuild.Source) }
else {
    $localMsbuild = Join-Path $repoRoot '.buildtools\MSBuild\Current\Bin\MSBuild.exe'
    if (Test-Path -LiteralPath $localMsbuild -PathType Leaf) { Write-Output ("MSBuild: {0}" -f $localMsbuild) }
    else { Write-Output 'MSBuild: not found (run install-build-tools.ps1 from elevated PowerShell)' }
}

Write-Section 'Interpretation'
Write-Output 'CPU: High Precision Temperature is expected in tenths of Kelvin; Temperature may be Kelvin or tenths of Kelvin depending on the Windows provider.'
Write-Output 'GPU: TrayS only has AMD ADL/NVIDIA NVAPI vendor paths in the default package; Intel/virtual display adapters may have no temperature source.'
Write-Output 'Shell: a class found by EnumChildWindows but missing from FindWindowEx is the taskbar-layout compatibility condition fixed in TrayS.cpp.'
