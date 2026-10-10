[CmdletBinding()]
param(
    [ValidateSet('x64', 'Win32')]
    [string]$Platform = $(if ([IntPtr]::Size -eq 8) { 'x64' } else { 'Win32' }),

    [ValidateRange(1, 60)]
    [int]$SampleCount = 5,

    [switch]$RequireTemperature
)

# Uses only the already-installed PawnIO device. This script never installs a
# driver or changes service state. Run it in Windows PowerShell (.NET Framework).
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$expectedPointerSize = if ($Platform -eq 'x64') { 8 } else { 4 }
if ([IntPtr]::Size -ne $expectedPointerSize) {
    throw "Run this diagnostic with a $Platform Windows PowerShell process."
}
$script:cpuMonitorAssemblyRoot = Join-Path $repoRoot ("Bin\{0}\Release" -f $Platform)
$assemblyPath = Join-Path $script:cpuMonitorAssemblyRoot 'LibreHardwareMonitorLib.dll'
if (-not (Test-Path -LiteralPath $assemblyPath -PathType Leaf)) {
    throw "Build $Platform Release first: $assemblyPath"
}

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class TraySCpuDiagnosticNative {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, ExactSpelling=true, SetLastError=true)]
    public static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);
}
'@
$device = [TraySCpuDiagnosticNative]::CreateFileW('\\?\GLOBALROOT\Device\PawnIO', 3, 3, [IntPtr]::Zero, 3, 0x80, [IntPtr]::Zero)
$deviceError = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
try {
    if ($device.IsInvalid) {
        if ($deviceError -eq 5) {
            throw 'PawnIO access denied (Win32 5). The official driver permits only SYSTEM and elevated administrators. Run this diagnostic in an administrator Windows PowerShell; reinstalling the driver is unnecessary.'
        }
        throw "PawnIO device is unavailable: $([ComponentModel.Win32Exception]::new($deviceError).Message) (Win32 $deviceError). Check that the official signed PawnIO driver is installed and running."
    }
} finally {
    $device.Dispose()
}

$resolveHandler = [ResolveEventHandler] {
    param($sender, $eventArgs)
    $simpleName = ([Reflection.AssemblyName]::new($eventArgs.Name)).Name + '.dll'
    $candidate = Join-Path $script:cpuMonitorAssemblyRoot $simpleName
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        return [Reflection.Assembly]::LoadFrom($candidate)
    }
    return $null
}
[AppDomain]::CurrentDomain.add_AssemblyResolve($resolveHandler)
$assembly = [Reflection.Assembly]::LoadFrom($assemblyPath)
$computer = [Activator]::CreateInstance($assembly.GetType('LibreHardwareMonitor.Hardware.Computer', $true))
$validSamples = 0
try {
    $computer.IsCpuEnabled = $true
    $computer.Open()
    $cpus = @($computer.Hardware | Where-Object { $_.HardwareType.ToString() -eq 'Cpu' })
    if ($cpus.Count -eq 0) { throw 'LHM did not detect any CPU hardware.' }
    foreach ($cpu in $cpus) { Write-Output ("CPU: {0}; backend: {1}" -f $cpu.Name, $cpu.GetType().FullName) }
    for ($sample = 1; $sample -le $SampleCount; $sample++) {
        $hasTemperature = $false
        foreach ($cpu in $cpus) {
            $cpu.Update()
            foreach ($sensor in $cpu.Sensors) {
                if ($sensor.SensorType.ToString() -ne 'Temperature') { continue }
                $value = $sensor.Value
                Write-Output ("Sample {0}: {1} = {2} C ({3})" -f $sample, $sensor.Name, $value, $sensor.Identifier)
                if ($null -ne $value -and -not [single]::IsNaN($value) -and
                    -not [single]::IsInfinity($value) -and $value -gt 0 -and $value -le 255) {
                    $hasTemperature = $true
                }
            }
        }
        if ($hasTemperature) { $validSamples++ }
        if ($sample -lt $SampleCount) { Start-Sleep -Milliseconds 1000 }
    }
    if ($RequireTemperature -and $validSamples -ne $SampleCount) {
        throw "CPU hardware test failed: only $validSamples of $SampleCount samples had a valid temperature."
    }
    Write-Output ("Usable CPU temperature samples: {0}/{1}" -f $validSamples, $SampleCount)
} finally {
    $computer.Close()
    [AppDomain]::CurrentDomain.remove_AssemblyResolve($resolveHandler)
}
