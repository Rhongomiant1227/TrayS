[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$failures = New-Object 'System.Collections.Generic.List[string]'

function Assert-Condition([bool]$condition, [string]$message) {
    if (-not $condition) {
        [void]$failures.Add($message)
    }
}

foreach ($project in @('OpenHardwareMonitorApi/OpenHardwareMonitorApi.vcxproj', 'TrayS/TrayS.vcxproj')) {
    $path = Join-Path $repoRoot $project
    try {
        [xml](Get-Content -LiteralPath $path -Raw) | Out-Null
    } catch {
        [void]$failures.Add("Invalid MSBuild XML: $project")
    }
}

# Source files contain Chinese UI text. Do not let MSVC infer the decoding from
# the machine's active code page, and keep the C++ headers in the same UTF-8
# encoding so builds behave consistently across locales.
$strictUtf8 = New-Object System.Text.UTF8Encoding($false, $true)
foreach ($project in @('OpenHardwareMonitorApi/OpenHardwareMonitorApi.vcxproj', 'TrayS/TrayS.vcxproj')) {
    $projectPath = Join-Path $repoRoot $project
    try {
        [xml]$projectXml = Get-Content -LiteralPath $projectPath -Raw
        $compileGroups = @($projectXml.Project.ItemDefinitionGroup)
        Assert-Condition ($compileGroups.Count -eq 4) "Expected four build configurations in $project"
        foreach ($group in $compileGroups) {
            Assert-Condition ($group.ClCompile.AdditionalOptions -match '(?i)(^|\s)/utf-8(?:\s|$)') "MSVC UTF-8 source mode is missing in $project"
            if ($project -eq 'TrayS/TrayS.vcxproj') {
                Assert-Condition ($group.Link.AdditionalDependencies -match '(?i)(^|;)comctl32\.lib(;|$)') "Common controls library is missing in $project $($group.Condition)"
            }
        }

        $projectDirectory = Split-Path -Parent $projectPath
        foreach ($itemGroup in $projectXml.Project.ItemGroup) {
            foreach ($source in @($itemGroup.ClCompile) + @($itemGroup.ClInclude)) {
                if (-not $source.Include -or [IO.Path]::GetExtension($source.Include) -notin @('.cpp', '.h')) {
                    continue
                }
                $sourcePath = Join-Path $projectDirectory $source.Include
                if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
                    continue
                }
                try {
                    $sourceText = $strictUtf8.GetString([IO.File]::ReadAllBytes($sourcePath))
                    Assert-Condition (-not $sourceText.Contains([char]0xFFFD)) "Replacement character found in source: $sourcePath"
                } catch [System.Text.DecoderFallbackException] {
                    [void]$failures.Add("C++ source is not valid UTF-8: $sourcePath")
                }
            }
        }
    } catch {
        [void]$failures.Add("Unable to validate source encoding settings: $project")
    }
}

$resourceBytes = [IO.File]::ReadAllBytes((Join-Path $repoRoot 'TrayS/TrayS.rc'))
Assert-Condition ($resourceBytes.Length -ge 2 -and $resourceBytes[0] -eq 0xff -and $resourceBytes[1] -eq 0xfe) 'TrayS.rc must remain UTF-16LE so localized resource text is decoded consistently'

$ohmaProject = [xml](Get-Content -LiteralPath (Join-Path $repoRoot 'OpenHardwareMonitorApi/OpenHardwareMonitorApi.vcxproj') -Raw)
$targetFramework = $ohmaProject.Project.PropertyGroup | Where-Object { $_.Label -eq 'Globals' } | Select-Object -ExpandProperty TargetFrameworkVersion
Assert-Condition ($targetFramework -eq 'v4.7.2') 'TargetFrameworkVersion must be v4.7.2'

$solutionText = Get-Content -LiteralPath (Join-Path $repoRoot 'TrayS.sln') -Raw
Assert-Condition ($solutionText -notmatch '\|ARM(?:64)?\s*=') 'Unsupported ARM/ARM64 solution configuration found'
Assert-Condition ($solutionText -notmatch 'Any CPU') 'Unsupported Any CPU solution configuration found'
Assert-Condition ($solutionText -match 'ProjectSection\(ProjectDependencies\)') 'TrayS project dependency is missing'

$lhmRoot = Join-Path $repoRoot 'OpenHardwareMonitorApi/ThirdParty/LibreHardwareMonitor-0.9.6'
$assemblyPath = Join-Path $lhmRoot 'x64/LibreHardwareMonitorLib.dll'
Assert-Condition (Test-Path -LiteralPath $assemblyPath) 'x64 LibreHardwareMonitorLib.dll 0.9.6 is missing'
foreach ($architecture in @(
    [pscustomobject]@{ Name = 'x64'; Machine = 0x8664 },
    [pscustomobject]@{ Name = 'x86'; Machine = 0x014c }
)) {
    $architecturePath = Join-Path $lhmRoot (Join-Path $architecture.Name 'LibreHardwareMonitorLib.dll')
    Assert-Condition (Test-Path -LiteralPath $architecturePath) "$($architecture.Name) LibreHardwareMonitorLib.dll is missing"
    if (Test-Path -LiteralPath $architecturePath) {
        $architectureBytes = [IO.File]::ReadAllBytes($architecturePath)
        $architecturePeOffset = [BitConverter]::ToInt32($architectureBytes, 0x3c)
        $architectureMachine = [BitConverter]::ToUInt16($architectureBytes, $architecturePeOffset + 4)
        Assert-Condition ($architectureMachine -eq $architecture.Machine) ("Unexpected {0} LHM PE machine: 0x{1:x4}" -f $architecture.Name, $architectureMachine)
        $architectureVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($architecturePath).FileVersion
        Assert-Condition ($architectureVersion -eq '0.9.6.0') "$($architecture.Name) LibreHardwareMonitorLib is not version 0.9.6: $architectureVersion"
    }
}
if (Test-Path -LiteralPath $assemblyPath) {
    $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($assemblyPath).FileVersion
    Assert-Condition ($version -eq '0.9.6.0') "LibreHardwareMonitorLib must be version 0.9.6: $version"
    $bytes = [IO.File]::ReadAllBytes($assemblyPath)
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3c)
    $machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
    Assert-Condition ($machine -eq 0x8664) ('Unexpected x64 LHM PE machine: 0x{0:x4}' -f $machine)

    $assembly = [Reflection.Assembly]::LoadFrom($assemblyPath)
    $required = @{
        'LibreHardwareMonitor.Hardware.Computer' = @('Hardware', 'IsCpuEnabled', 'IsGpuEnabled', 'IsStorageEnabled', 'IsMotherboardEnabled', 'Open', 'Close', 'Accept')
        'LibreHardwareMonitor.Hardware.IComputer' = @('Hardware', 'IsCpuEnabled', 'IsGpuEnabled', 'IsStorageEnabled', 'IsMotherboardEnabled')
        'LibreHardwareMonitor.Hardware.IHardware' = @('HardwareType', 'Sensors', 'SubHardware', 'Name', 'Update')
        'LibreHardwareMonitor.Hardware.ISensor' = @('Value', 'Name', 'SensorType')
        'LibreHardwareMonitor.PawnIo.PawnIo' = @('IsInstalled', 'Version')
    }
    foreach ($typeName in $required.Keys) {
        $type = $assembly.GetType($typeName, $false)
        Assert-Condition ($null -ne $type) "Missing type: $typeName"
        if ($null -ne $type) {
            $memberNames = @($type.GetMembers([Reflection.BindingFlags]'Public,Instance,Static') | ForEach-Object Name)
            foreach ($member in $required[$typeName]) {
                Assert-Condition ($memberNames -contains $member) "Missing member: $typeName.$member"
            }
        }
    }
    $hardwareType = $assembly.GetType('LibreHardwareMonitor.Hardware.HardwareType', $false)
    Assert-Condition ($null -ne $hardwareType -and [Enum]::GetNames($hardwareType) -contains 'GpuIntel') 'LibreHardwareMonitorLib has no Intel GPU type'
}

$hidSharpPath = Join-Path $lhmRoot 'HidSharp.dll'
Assert-Condition (Test-Path -LiteralPath $hidSharpPath) 'HidSharp.dll is missing (required by LibreHardwareMonitorLib 0.9.6)'
if (Test-Path -LiteralPath $hidSharpPath) {
    $hidVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($hidSharpPath).FileVersion
    Assert-Condition ([version]$hidVersion -ge [version]'2.6.4.0') "HidSharp is too old: $hidVersion"
    $hidBytes = [IO.File]::ReadAllBytes($hidSharpPath)
    $hidPeOffset = [BitConverter]::ToInt32($hidBytes, 0x3c)
    $hidMachine = [BitConverter]::ToUInt16($hidBytes, $hidPeOffset + 4)
    Assert-Condition ($hidMachine -eq 0x014c) ('Unexpected HidSharp PE machine: 0x{0:x4}' -f $hidMachine)
}

$apiHeader = Get-Content -LiteralPath (Join-Path $repoRoot 'OpenHardwareMonitorApi/OpenHardwareMonitorApi.h') -Raw
$monitorImplementation = Get-Content -LiteralPath (Join-Path $repoRoot 'OpenHardwareMonitorApi/OpenHardwareMonitorImp.cpp') -Raw
$visitorSource = Get-Content -LiteralPath (Join-Path $repoRoot 'OpenHardwareMonitorApi/UpdateVisitor.cpp') -Raw
$trayHeader = Get-Content -LiteralPath (Join-Path $repoRoot 'TrayS/TrayS.h') -Raw
$traySource = Get-Content -LiteralPath (Join-Path $repoRoot 'TrayS/TrayS.cpp') -Raw
$updateHeaderPath = Join-Path $repoRoot 'TrayS/Update.h'
$updateSourcePath = Join-Path $repoRoot 'TrayS/Update.cpp'
$updateHeader = if (Test-Path -LiteralPath $updateHeaderPath) { Get-Content -LiteralPath $updateHeaderPath -Raw } else { '' }
$updateSource = if (Test-Path -LiteralPath $updateSourcePath) { Get-Content -LiteralPath $updateSourcePath -Raw -Encoding UTF8 } else { '' }
$resourceSource = Get-Content -LiteralPath (Join-Path $repoRoot 'TrayS/TrayS.rc') -Raw
$functionSource = Get-Content -LiteralPath (Join-Path $repoRoot 'TrayS/Function.cpp') -Raw
$trayProject = Get-Content -LiteralPath (Join-Path $repoRoot 'TrayS/TrayS.vcxproj') -Raw
$trayFilters = Get-Content -LiteralPath (Join-Path $repoRoot 'TrayS/TrayS.vcxproj.filters') -Raw
$portableBuildPath = Join-Path $repoRoot 'tools/build-portable-compat-exe.ps1'
$portableBuild = if (Test-Path -LiteralPath $portableBuildPath) { Get-Content -LiteralPath $portableBuildPath -Raw } else { '' }
$portablePackagePath = Join-Path $repoRoot 'tools/package-portable-release.ps1'
$portablePackage = if (Test-Path -LiteralPath $portablePackagePath) { Get-Content -LiteralPath $portablePackagePath -Raw } else { '' }
$readmePath = Join-Path $repoRoot 'README.md'
$readmeSource = if (Test-Path -LiteralPath $readmePath) { Get-Content -LiteralPath $readmePath -Raw } else { '' }
Assert-Condition ($apiHeader -match '\*fCpu\s*=\s*-1\.0f') 'GetTemperature does not initialize CPU output'
Assert-Condition ($apiHeader -match 'cpu\s*=\s*m_pMonitor->CpuTemperature\(\)') 'C ABI does not use the selected CPU/package temperature'
Assert-Condition ($apiHeader -match 'static_cast<size_t>\(iHDD\)\s*<\s*temperatures\.size\(\)') 'HDD index is not bounds checked'
Assert-Condition ($apiHeader -match 'try\s*\{[\s\S]*m_pMonitor->GetHardwareInfo\(\)') 'GetTemperature does not contain the managed update boundary'
Assert-Condition ($apiHeader -match 'catch\s*\(\.\.\.\)') 'GetTemperature does not contain a C ABI catch-all'
Assert-Condition ($monitorImplementation -match 'static bool TryGetLoadValue') 'Load sensor values are not validated independently'
Assert-Condition ($monitorImplementation -match 'case HardwareType::GpuIntel:' -and $monitorImplementation -match 'return m_gpu_intel_temperature;' -and $monitorImplementation -match 'return m_gpu_intel_usage;') 'Intel GPU sensor fallback is missing'
Assert-Condition ($monitorImplementation -match 'value < 0\.0f \|\| value > 100\.0f') 'Load sensor range is not limited to 0..100'
Assert-Condition ($monitorImplementation -match 'std::isfinite\(sum\).*std::isfinite\(average\)' -or $monitorImplementation -match 'std::isfinite\(preferredTemperature\)') 'Temperature aggregation is not checked for finite output'
Assert-Condition ($monitorImplementation -match 'catch \(System::Exception\^ e\)') 'Individual managed hardware nodes are not exception-isolated'
Assert-Condition ($visitorSource -match 'auto subHardwareList = hardware->SubHardware') 'SubHardware is not captured within a protected boundary'
Assert-Condition ($visitorSource -match 'subHardware->Accept\(this\)') 'SubHardware visitor traversal is missing'
Assert-Condition ($visitorSource -match 'computer->Traverse\(this\)') 'Computer traversal is missing'
Assert-Condition ($traySource -match 'static void ClosePDH') 'PDH cleanup helper is missing'
Assert-Condition ($traySource -match 'SelectObject\(g_argbTextSurface.maskDC, g_argbTextSurface.maskOldBitmap\)') 'ARGB text bitmap is not deselected before cleanup'
Assert-Condition ($traySource -match 'Thermal Zone Information\(\*\).*High Precision Temperature') 'ACPI high-precision thermal-zone query is missing'
Assert-Condition ($traySource -match 'static BOOL EnsureThermalQueryUnlocked') 'ACPI thermal query initialization helper is missing'
Assert-Condition ($traySource -match 'static void CloseThermalQueryUnlocked') 'ACPI thermal query cleanup helper is missing'
Assert-Condition ($traySource -match 'static int GetAcpiCpuTemperature') 'ACPI CPU temperature reader is missing'
Assert-Condition ($traySource -match 'thermalProbeAttempted') 'ACPI unsupported-state probe cache is missing'
Assert-Condition ($traySource -match 'HasAcpiTemperaturePath') 'ACPI availability is not read through a lock-protected helper'
Assert-Condition ($traySource -match 'offset \+ 1\s*<\s*pathChars') 'ACPI wildcard path walk is not bounded'
Assert-Condition ($traySource -match 'return GetAcpiCpuTemperature\(\);') 'GetCpuTemp is missing its ACPI fallback'
Assert-Condition ($traySource -match 'TrayData->iTemperature1\s*=\s*GetCpuTemp\(1\)') 'Monitoring loop does not sample CPU temperature without LHM'
Assert-Condition ($traySource -match 'bRing0\s*') 'Managed temperature availability flag is missing'
Assert-Condition ($traySource -match 'IsPawnIoDeviceAvailable') 'PawnIO availability probe is missing'
Assert-Condition ($traySource -match 'CreateFileW\(L"[^\r\n]*PawnIO",\s*FILE_READ_DATA\s*\|\s*FILE_WRITE_DATA') 'PawnIO probe must match the data-access rights requested by LHM'
Assert-Condition ($traySource -match 'bLhmDisabled\s*=\s*!IsPawnIoDeviceAvailable') 'LHM is not gated on an already-accessible PawnIO device'
Assert-Condition ($traySource -match 'g_pawnIoProbeError == ERROR_ACCESS_DENIED' -and $traySource -match 'UpdateCpuTemperatureStatus') 'CPU temperature permission failures have no settings status'
Assert-Condition ($resourceSource -match 'IDC_LABEL_CPU_TEMPERATURE_STATUS' -and $resourceSource -match 'IDC_BUTTON_CPU_TEMPERATURE_HELP') 'CPU temperature settings status/help controls are missing'
Assert-Condition ($monitorImplementation -match 'computer->IsCpuEnabled\s*=\s*true[\s\S]*computer->Open\(\)') 'LHM CPU hardware group is not enabled before Computer::Open'
Assert-Condition ($monitorImplementation -match 'Core \(Tctl/Tdie\)' -and $monitorImplementation -match 'Core \(Tdie\)') 'AMD package temperature sensor preference is missing'
Assert-Condition ($traySource -match 'InitializeSupportedWindowsVersion') 'Windows 10+ startup gate is missing'
Assert-Condition ($traySource -match 'dwMajorVersion\s*>=\s*10') 'Windows 10+ version comparison is missing'
Assert-Condition ($traySource -match 'FindDescendantByClass') 'Shell descendant window discovery helper is missing'
Assert-Condition ($traySource -match 'EnumChildWindows\(root') 'Shell discovery does not enumerate descendant windows'
Assert-Condition ($resourceSource -match 'TrayS 1\.7') 'Maintained UI version label is missing'
Assert-Condition ($resourceSource -match 'FILEVERSION 1,7,2,0' -and $resourceSource -match 'PRODUCTVERSION 1,7,2,0') 'Resource version is not 1.7.2'
Assert-Condition ($resourceSource -match 'IDC_SYSLINK_COMPAT') 'Compatibility documentation link is missing'
Assert-Condition ($resourceSource -match 'Windows 10/11') 'Windows 10/11 UI support note is missing'
Assert-Condition ($resourceSource -notmatch '52[Pp]o[Jj]ie|52破解|Win8只能|Win7只能|Ver 1\.3\.9') 'Obsolete UI compatibility text remains'
Assert-Condition ($traySource -notmatch '52pojie|52PoJie|cgbsmy/TrayS') 'Obsolete project/forum links remain'
Assert-Condition ($traySource -notmatch '\bInitOpenLibSys\b|\bm_hOpenLibSys\b|\bRdmsr\b|\bReadPciConfigDwordEx\b') 'Legacy WinRing0 temperature path is still referenced by TrayS'
Assert-Condition ($trayHeader -match 'typedef void\*\s+NvPhysicalGpuHandle') 'NVAPI handles are not opaque pointer-sized values'
Assert-Condition ($traySource -match 'ADL_Adapter_NumberOfAdapters_Get' -and $traySource -match 'ADL_Adapter_Active_Get') 'AMD adapter enumeration/active filtering is missing'
Assert-Condition ($traySource -match 'for\s*\(int GpuIndex\s*=\s*0;\s*GpuIndex\s*<\s*NVAPI_MAX_PHYSICAL_GPUS') 'NVIDIA physical GPU enumeration is not bounded across all adapters'
foreach ($legacyPath in @('TrayS/OlsApiInit.h', 'TrayS/OlsApiInitDef.h', 'TrayS/OlsDef.h', 'TrayS/WinRing0x32.sys', 'TrayS/WinRing0x64.sys', 'TrayS/OpenHardwareMonitorApi.lib')) {
    Assert-Condition (-not (Test-Path -LiteralPath (Join-Path $repoRoot $legacyPath))) "Legacy driver artifact remains: $legacyPath"
}
Assert-Condition ($traySource -notmatch 'while\s*\(nleft\s*!=') 'Taskbar movement still uses a per-pixel loop'
Assert-Condition ($trayHeader -match 'NvPhysicalGpuHandle hPhysicalGpu\[NVAPI_MAX_PHYSICAL_GPUS\]') 'NVAPI physical GPU handle buffer is not sized for the API maximum'
Assert-Condition ($trayProject -match '<UACExecutionLevel>asInvoker</UACExecutionLevel>') 'Default UAC execution level is not asInvoker'
Assert-Condition (Test-Path -LiteralPath $portableBuildPath -PathType Leaf) 'Portable compatibility build script is missing'
Assert-Condition ($portableBuild -match 'llvm-mingw') 'Portable compatibility build does not use the repository toolchain'
Assert-Condition ($portableBuild -match "'x86_64-w64-windows-gnu'") 'Portable compatibility build target is not x64 MinGW'
Assert-Condition ($portableBuild -match "'-static'") 'Portable compatibility build does not statically link its C++ runtime'
Assert-Condition ($portableBuild -match 'TrayS-compat-win11-x64\.exe' -or $portableBuild -match 'TrayS-compat-win11-\{0\}\.exe') 'Portable compatibility output name is missing'
Assert-Condition ($portablePackage -match 'TrayS_1\.7\.2_compat_' -and $portablePackage -match 'FileMinorPart -ne 7' -and $portablePackage -match 'FileBuildPart -ne 2') 'Portable compatibility package version does not match the application version'
Assert-Condition ($functionSource -match 'LoadLibraryExW\(L"winhttp\.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32\)') 'WinHTTP is not loaded from System32'
Assert-Condition ($functionSource -match 'kPriceHttpTimeoutMs\s*=\s*5000' -and $functionSource -match 'winHttpSetTimeouts\(') 'WinHTTP timeout is not bounded to 5000 ms'
Assert-Condition ($functionSource -notmatch '(?m)^\s*(?!//).*\bLoadLibrary\s*\(') 'Function.cpp contains an unqualified LoadLibrary call'
Assert-Condition ($functionSource -match '#if defined\(TRAYS_ENABLE_LEGACY_SERVICE\)') 'Legacy service code is not compile-time gated'
Assert-Condition ($updateHeader -match 'TRAYS_VERSION_STRING L"1\.7\.2"') 'Updater version identity is missing'
Assert-Condition ($updateHeader -match 'TRAYS_VERSION_PATCH 2') 'Updater patch component does not match the application version'
Assert-Condition ($updateHeader -match 'TRAYS_UPDATE_API_PATH') 'Updater GitHub API path is missing'
Assert-Condition ($updateSource -match 'LoadLibraryExW\(L"winhttp\.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32\)') 'Updater WinHTTP is not loaded from System32'
Assert-Condition ($updateSource -match 'WINHTTP_FLAG_SECURE') 'Updater request is not HTTPS-only'
Assert-Condition ($updateSource -match 'TrayS_%s_%s\.zip') 'Updater does not require the versioned architecture-specific TrayS asset name'
Assert-Condition ($updateSource -match 'HashFileSha256') 'Updater SHA-256 verification is missing'
Assert-Condition ($updateSource -match 'Get-FileHash') 'Updater applier does not re-check SHA-256'
Assert-Condition ($updateSource -match 'Expand-Archive') 'Updater extraction step is missing'
Assert-Condition ($updateSource -match '\.WaitForExit\(120000\)') 'Updater does not wait for the old process before replacing its executable'
$extractDirectoryIndex = $updateSource.IndexOf('New-Item -ItemType Directory -Path $extract', [StringComparison]::Ordinal)
$expandArchiveIndex = $updateSource.IndexOf('Expand-Archive -LiteralPath $zip', [StringComparison]::Ordinal)
Assert-Condition ($extractDirectoryIndex -ge 0 -and $expandArchiveIndex -gt $extractDirectoryIndex) 'Updater must create the extraction directory before expanding the package'
Assert-Condition ($updateSource -match '\$replacementAttempted=\$true; foreach \(\$name in \$payloadNames\)' -and $updateSource -match 'Copy-Item -LiteralPath \$backup -Destination \$installed -Force') 'Updater replacement does not stage and roll back the complete runtime payload'
Assert-Condition ($updateSource -match 'Installed update file failed verification' -and $updateSource -match 'rollback failed for') 'Updater does not verify every installed file and report rollback failures'
Assert-Condition ($updateSource -match 'TraySUpdateNative.*MessageBox' -and $updateSource -match '\$failureState' -and $updateSource -match 'rollback failed for') 'Updater failure path does not explain the recovery state to the user'
Assert-Condition ($updateSource -match 'GrantUpdateHelperReadAccess' -and $updateSource -match 'PROTECTED_DACL_SECURITY_INFORMATION') 'Elevated updater cannot safely read temporary files created by another user account'
Assert-Condition ($updateSource -notmatch 'TryRunTraySUpdateCommandLine|--trays-apply-update' -and $traySource -notmatch 'TryRunTraySUpdateCommandLine') 'Updater must not relaunch the executable that it is about to replace'
$updaterScriptBuilder = New-Object Text.StringBuilder
foreach ($scriptLiteral in [regex]::Matches($updateSource, '(?m)^\s*ps \+= L("(?:\\.|[^"\\])*");')) {
    [void]$updaterScriptBuilder.Append((ConvertFrom-Json -InputObject $scriptLiteral.Groups[1].Value))
}
$embeddedUpdaterScript = $updaterScriptBuilder.ToString()
$scriptTokens = $null
$scriptParseErrors = $null
[void][System.Management.Automation.Language.Parser]::ParseInput($embeddedUpdaterScript, [ref]$scriptTokens, [ref]$scriptParseErrors)
Assert-Condition ($embeddedUpdaterScript.Length -gt 0 -and $scriptParseErrors.Count -eq 0) 'Embedded PowerShell updater script has syntax errors'
Assert-Condition ($updateSource -match 'TRAYS_UPDATE_EVENT_READY') 'Updater ready event is missing'
Assert-Condition ($traySource -match 'StartTraySUpdateCheck\(hMain, FALSE\)') 'Manual update check button is not wired'
Assert-Condition ($traySource -match 'TRAYS_UPDATE_START_TIMER') 'Automatic update timer is not wired'
Assert-Condition ($resourceSource -match 'IDC_BUTTON_CHECK_UPDATE' -and $resourceSource -match 'IDC_CHECK_AUTO_UPDATE') 'Update settings controls are missing'
Assert-Condition ($trayProject -match '<ClCompile Include="Update\.cpp"' -and $trayProject -match '<ClInclude Include="Update\.h"') 'Updater files are not in the project'
Assert-Condition ($trayFilters -match 'Update\.cpp' -and $trayFilters -match 'Update\.h') 'Updater files are not in project filters'
Assert-Condition ($traySource -notmatch 'ShellExecuteW.*download|start.*https://') 'Updater must not launch a browser or external downloader'
Assert-Condition ($readmeSource -match 'cgbsmy/TrayS') 'README does not identify the upstream project'
Assert-Condition ($readmeSource -match 'TrayS_<版本>_<架构>\.zip') 'README does not document the named release package format'

if ($failures.Count -gt 0) {
    $failures | ForEach-Object { Write-Error $_ }
    exit 1
}

Write-Output 'TrayS compatibility static checks passed.'
