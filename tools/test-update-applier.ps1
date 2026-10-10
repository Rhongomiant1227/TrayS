[CmdletBinding()]
param(
    [string]$CandidatePath
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($CandidatePath)) {
    $candidatePlatform = if ([IntPtr]::Size -eq 8) { 'x64' } else { 'Win32' }
    $CandidatePath = Join-Path $repoRoot ("Bin\{0}\Release\TrayS.exe" -f $candidatePlatform)
}
if (-not (Test-Path -LiteralPath $CandidatePath -PathType Leaf)) {
    throw "Build TrayS first or pass -CandidatePath: $CandidatePath"
}
$candidateVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($CandidatePath).FileVersion
if (-not $candidateVersion.StartsWith('1.7.0.')) {
    throw "The test candidate must have version 1.7.0; found $candidateVersion"
}
$candidateBytes = [IO.File]::ReadAllBytes($CandidatePath)
if ($candidateBytes.Length -lt 64 -or [BitConverter]::ToUInt16($candidateBytes, 0) -ne 0x5a4d) {
    throw 'The test candidate has an invalid DOS header.'
}
$candidatePeOffset = [BitConverter]::ToInt32($candidateBytes, 0x3c)
if ($candidatePeOffset -lt 64 -or $candidatePeOffset -gt $candidateBytes.Length - 6 -or
    [BitConverter]::ToUInt32($candidateBytes, $candidatePeOffset) -ne 0x4550) {
    throw 'The test candidate has an invalid PE header.'
}
$candidateMachine = [BitConverter]::ToUInt16($candidateBytes, $candidatePeOffset + 4)
$expectedMachine = if ([IntPtr]::Size -eq 8) { 0x8664 } else { 0x014c }
if ($candidateMachine -ne $expectedMachine) {
    throw ('Candidate architecture does not match this PowerShell process (0x{0:x4}).' -f $expectedMachine)
}

$updateSource = Get-Content -LiteralPath (Join-Path $repoRoot 'TrayS\Update.cpp') -Raw -Encoding UTF8
$scriptBuilder = New-Object Text.StringBuilder
foreach ($literal in [regex]::Matches($updateSource, '(?m)^\s*ps \+= L("(?:\\.|[^"\\])*");')) {
    [void]$scriptBuilder.Append((ConvertFrom-Json -InputObject $literal.Groups[1].Value))
}
$template = $scriptBuilder.ToString()
if ([string]::IsNullOrWhiteSpace($template)) {
    throw 'Could not extract the embedded updater script from Update.cpp.'
}
$aclSddlMatch = [regex]::Match($updateSource, 'ConvertStringSecurityDescriptorToSecurityDescriptorW\(\s*L"([^"]+)"')
if (-not $aclSddlMatch.Success) {
    throw 'Could not find the updater helper access-control descriptor.'
}
Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class TraySUpdateAclRegression {
    [DllImport("advapi32.dll", CharSet=CharSet.Unicode, ExactSpelling=true, SetLastError=true)]
    private static extern bool ConvertStringSecurityDescriptorToSecurityDescriptorW(string sddl, uint revision, out IntPtr descriptor, out uint size);
    [DllImport("advapi32.dll", CharSet=CharSet.Unicode, ExactSpelling=true, SetLastError=true)]
    private static extern bool SetFileSecurityW(string path, uint information, IntPtr descriptor);
    [DllImport("kernel32.dll", ExactSpelling=true)]
    private static extern IntPtr LocalFree(IntPtr memory);
    public static void Apply(string path, string sddl) {
        IntPtr descriptor;
        uint size;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, 1, out descriptor, out size))
            throw new Win32Exception(Marshal.GetLastWin32Error());
        try {
            if (!SetFileSecurityW(path, 0x80000004, descriptor))
                throw new Win32Exception(Marshal.GetLastWin32Error());
        } finally {
            LocalFree(descriptor);
        }
    }
}
'@

function ConvertTo-PowerShellLiteral([string]$Value) {
    return "'" + $Value.Replace("'", "''") + "'"
}

function Invoke-UpdaterScenario([string]$Scenario, [string]$ZipPath, [string]$TargetPath, [string]$Hash,
    [string]$MarkerPath, [int]$ParentProcessId, [string]$WorkingDirectory) {
    $scriptLines = $template -split '\r?\n'
    for ($index = 0; $index -lt $scriptLines.Count; $index++) {
        if ($scriptLines[$index].StartsWith('$scriptPath=')) {
            # These values are concatenated into Update.cpp at runtime, so add test inputs before the script body.
            $dynamicAssignments = @(
                ('$zip=' + (ConvertTo-PowerShellLiteral $ZipPath)),
                ('$target=' + (ConvertTo-PowerShellLiteral $TargetPath)),
                ('$expectedHash=' + (ConvertTo-PowerShellLiteral $Hash)),
                ('$expectedVersion=' + (ConvertTo-PowerShellLiteral '1.7.0') + "; `$expectedVersion=`$expectedVersion.TrimStart('v')"),
                ('$parentPid=' + [string]$ParentProcessId)
            )
            $scriptLines[$index] = ($dynamicAssignments -join "`r`n") + "`r`n" + $scriptLines[$index]
        }
        elseif ($scriptLines[$index].StartsWith('  Start-Process -FilePath $target')) {
            if ($Scenario -eq 'rollback') {
                $scriptLines[$index] = "  throw 'Injected regression failure after replacement'"
            }
            else {
                $scriptLines[$index] = '  Set-Content -LiteralPath ' + (ConvertTo-PowerShellLiteral $MarkerPath) + " -Value 'started'"
            }
        }
        elseif ($scriptLines[$index].StartsWith('  try { Add-Type -TypeDefinition')) {
            $scriptLines[$index] = "  Write-Output ('TrayS update failed: '+`$detail)"
        }
    }
    $scriptText = $scriptLines -join "`r`n"
    if ($ParentProcessId -le 0 -or -not $scriptText.Contains('$parentPid=' + [string]$ParentProcessId)) {
        throw "The updater simulation did not receive a valid parent PID ($ParentProcessId)."
    }

    $scriptPath = Join-Path $WorkingDirectory ("{0}.ps1" -f $Scenario)
    Set-Content -LiteralPath $scriptPath -Value $scriptText -Encoding Unicode
    $savedErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $scriptPath 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $savedErrorActionPreference
    }
    Remove-Item -LiteralPath $scriptPath -Force -ErrorAction SilentlyContinue
    return [pscustomobject]@{ ExitCode = $exitCode; Output = ($output -join "`n") }
}

$testRoot = Join-Path $env:TEMP ('TrayS-update-applier-' + [guid]::NewGuid().ToString('N'))
$packageDirectory = Join-Path $testRoot 'package'
$installDirectory = Join-Path $testRoot 'installed app with spaces'
New-Item -ItemType Directory -Path $packageDirectory, $installDirectory -Force | Out-Null
$packageExe = Join-Path $packageDirectory 'TrayS.exe'
$zipPath = Join-Path $testRoot 'package.zip'
$targetPath = Join-Path $installDirectory 'TrayS.exe'
$markerPath = Join-Path $testRoot 'restart-reached.txt'
$aclProbePath = Join-Path $testRoot 'helper-acl-probe.tmp'
$parent = $null

try {
    Set-Content -LiteralPath $aclProbePath -Value 'test'
    [TraySUpdateAclRegression]::Apply($aclProbePath, $aclSddlMatch.Groups[1].Value)
    Remove-Item -LiteralPath $aclProbePath -Force
    Write-Output 'PASS: elevated helper access-control descriptor applies to its input files.'

    Copy-Item -LiteralPath $CandidatePath -Destination $packageExe
    Copy-Item -LiteralPath (Join-Path $env:SystemRoot 'System32\cmd.exe') -Destination $targetPath
    Compress-Archive -LiteralPath $packageExe -DestinationPath $zipPath -Force
    $packageHash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $candidateHash = (Get-FileHash -LiteralPath $CandidatePath -Algorithm SHA256).Hash

    $parent = Start-Process -FilePath $targetPath -ArgumentList '/c ping 127.0.0.1 -n 4 >nul' -WindowStyle Hidden -PassThru
    $success = Invoke-UpdaterScenario 'success' $zipPath $targetPath $packageHash $markerPath $parent.Id $testRoot
    if ($success.ExitCode -ne 0) {
        throw "Successful update simulation failed ($($success.ExitCode)): $($success.Output)"
    }
    if ((Get-FileHash -LiteralPath $targetPath -Algorithm SHA256).Hash -ne $candidateHash) {
        throw 'Successful update simulation did not install the verified executable.'
    }
    if (-not (Test-Path -LiteralPath $markerPath)) {
        throw 'Successful update simulation did not reach the restart step.'
    }
    Write-Output 'PASS: verified update installs correctly from a path containing spaces.'

    Copy-Item -LiteralPath (Join-Path $env:SystemRoot 'System32\cmd.exe') -Destination $targetPath -Force
    $oldHash = (Get-FileHash -LiteralPath $targetPath -Algorithm SHA256).Hash
    Compress-Archive -LiteralPath $packageExe -DestinationPath $zipPath -Force
    $parent = Start-Process -FilePath $targetPath -ArgumentList '/c ping 127.0.0.1 -n 4 >nul' -WindowStyle Hidden -PassThru
    $failure = Invoke-UpdaterScenario 'rollback' $zipPath $targetPath $packageHash $markerPath $parent.Id $testRoot
    if ($failure.ExitCode -ne 1) {
        throw "Failed update simulation returned $($failure.ExitCode), expected 1: $($failure.Output)"
    }
    if ((Get-FileHash -LiteralPath $targetPath -Algorithm SHA256).Hash -ne $oldHash) {
        throw 'Failed update simulation did not restore the original executable.'
    }
    if ($failure.Output -notmatch 'Injected regression failure after replacement') {
        throw "Failure output did not include the cause: $($failure.Output)"
    }
    Write-Output 'PASS: post-replacement failure restores the old executable and reports the cause.'
}
finally {
    if ($parent -and -not $parent.HasExited) {
        Stop-Process -Id $parent.Id -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $testRoot) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}
