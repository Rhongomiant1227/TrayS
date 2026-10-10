[CmdletBinding()]
param(
    [ValidateSet('x64', 'Win32')]
    [string]$Platform = $(if ([IntPtr]::Size -eq 8) { 'x64' } else { 'Win32' }),

    [switch]$RequireCpuTemperature,

    [ValidateRange(1, 60)]
    [int]$SampleCount = 1
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
$errorPointer = [TraySLhmWrapperSmokeNative]::GetProcAddress($module, 'TraySGetHardwareMonitorError')
$getError = $null
if ($errorPointer -ne [IntPtr]::Zero) {
    $getError = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer(
        $errorPointer, [type][TraySLhmWrapperSmokeNative+GetErrorDelegate])
}
$validCpuSamples = 0
for ($sample = 1; $sample -le $SampleCount; $sample++) {
    [float]$cpu = -1.0
    [float]$gpu = -1.0
    [float]$disk = -1.0
    [float]$package = -1.0
    $reader.Invoke([ref]$cpu, [ref]$gpu, [IntPtr]::Zero, [ref]$disk, -1, [ref]$package)
    if ([single]::IsNaN($cpu) -or [single]::IsInfinity($cpu) -or
        [single]::IsNaN($package) -or [single]::IsInfinity($package) -or
        $cpu -lt -1.0 -or $cpu -gt 255.0 -or $package -lt -1.0 -or $package -gt 255.0) {
        throw "The wrapper returned an invalid CPU temperature ($cpu / $package)."
    }
    if ($cpu -gt 0.0) { $validCpuSamples++ }
    Write-Output ("Sample {0}: CPU={1} C, package={2} C ({3})." -f $sample, $cpu, $package, $Platform)
    if ($getError) {
        $monitorError = [Runtime.InteropServices.Marshal]::PtrToStringUni($getError.Invoke())
        if ($monitorError) { Write-Output ("Monitor diagnostic: {0}" -f $monitorError) }
    }
    if ($sample -lt $SampleCount) { Start-Sleep -Milliseconds 1000 }
}

if ($RequireCpuTemperature -and $validCpuSamples -ne $SampleCount) {
    throw "CPU sensor test failed: only $validCpuSamples of $SampleCount samples contained a usable CPU temperature. Check the installed PawnIO device and monitor diagnostics."
}
if ($validCpuSamples -eq $SampleCount) {
    Write-Output ("PASS: {0} wrapper returned {1} usable CPU temperature samples." -f $Platform, $validCpuSamples)
} else {
    Write-Output ("PASS: {0} wrapper loaded; CPU temperature was unavailable. This is a loading test only, not a hardware-temperature validation." -f $Platform)
}
