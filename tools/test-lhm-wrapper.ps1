[CmdletBinding()]
param(
    [ValidateSet('x64', 'Win32')]
    [string]$Platform = $(if ([IntPtr]::Size -eq 8) { 'x64' } else { 'Win32' })
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$expectedPointerSize = if ($Platform -eq 'x64') { 8 } else { 4 }
if ([IntPtr]::Size -ne $expectedPointerSize) {
    throw "Run this test with a $Platform Windows PowerShell process."
}
$buildPlatform = if ($Platform -eq 'x64') { 'x64' } else { 'Win32' }
$buildOutput = Join-Path $repoRoot ("Bin\{0}\Release" -f $buildPlatform)
$wrapperPath = Join-Path $buildOutput 'OpenHardwareMonitorApi.dll'
if (-not (Test-Path -LiteralPath $wrapperPath -PathType Leaf)) {
    throw "Build the $Platform Release configuration first: $wrapperPath"
}

$nativeSource = @"
using System;
using System.Runtime.InteropServices;
public static class TraySLhmWrapperSmokeNative {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, ExactSpelling=true, SetLastError=true)]
    public static extern IntPtr LoadLibraryExW(string path, IntPtr file, uint flags);
    [DllImport("kernel32.dll", CharSet=CharSet.Ansi, ExactSpelling=true, SetLastError=true)]
    public static extern IntPtr GetProcAddress(IntPtr module, string name);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void GetTemperatureDelegate(ref float cpu, ref float gpu, IntPtr mainboard, ref float disk, int diskIndex, ref float package);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate IntPtr GetErrorDelegate();
}
"@
Add-Type -TypeDefinition $nativeSource
$script:assemblyRoot = $buildOutput
$resolveHandler = [ResolveEventHandler] {
    param($sender, $eventArgs)
    $simpleName = ([Reflection.AssemblyName]::new($eventArgs.Name)).Name + '.dll'
    $candidate = Join-Path $script:assemblyRoot $simpleName
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        return [Reflection.Assembly]::LoadFrom($candidate)
    }
    return $null
}
[AppDomain]::CurrentDomain.add_AssemblyResolve($resolveHandler)

$loadFlags = 0x00000100 -bor 0x00000200 -bor 0x00000800
$module = [TraySLhmWrapperSmokeNative]::LoadLibraryExW($wrapperPath, [IntPtr]::Zero, $loadFlags)
if ($module -eq [IntPtr]::Zero) {
    throw (New-Object ComponentModel.Win32Exception([Runtime.InteropServices.Marshal]::GetLastWin32Error()))
}
$entryPoint = [TraySLhmWrapperSmokeNative]::GetProcAddress($module, 'GetTemperature')
if ($entryPoint -eq [IntPtr]::Zero) {
    throw 'GetTemperature export was not found in OpenHardwareMonitorApi.dll.'
}
$reader = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer(
    $entryPoint, [type][TraySLhmWrapperSmokeNative+GetTemperatureDelegate])
[float]$cpu = -1.0
[float]$gpu = -1.0
[float]$disk = -1.0
[float]$package = -1.0
$reader.Invoke([ref]$cpu, [ref]$gpu, [IntPtr]::Zero, [ref]$disk, -1, [ref]$package)
$errorPointer = [TraySLhmWrapperSmokeNative]::GetProcAddress($module, 'TraySGetHardwareMonitorError')
$monitorError = ''
if ($errorPointer -ne [IntPtr]::Zero) {
    $getError = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer(
        $errorPointer, [type][TraySLhmWrapperSmokeNative+GetErrorDelegate])
    $monitorError = [Runtime.InteropServices.Marshal]::PtrToStringUni($getError.Invoke())
}
if ($cpu -lt -1.0 -or $cpu -gt 255.0 -or $package -lt -1.0 -or $package -gt 255.0) {
    throw "The wrapper returned an invalid CPU temperature ($cpu / $package)."
}

Write-Output ("PASS: {0} C++/CLI wrapper loaded LHM and returned CPU={1}, package={2}." -f $Platform, $cpu, $package)
if ($monitorError) { Write-Output ("Monitor diagnostic: {0}" -f $monitorError) }
Write-Output 'A zero reading is treated as unavailable by TrayS; ACPI remains the fallback when PawnIO is absent.'
