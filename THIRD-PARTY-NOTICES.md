# Third-party notices

The standard TrayS release includes the following user-mode libraries:

| Component | Version | License | Source |
| --- | --- | --- | --- |
| LibreHardwareMonitorLib | 0.9.6 | MPL-2.0 | [v0.9.6 source](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/tree/v0.9.6) |
| DiskInfoToolkit | 1.1.2 | MPL-2.0 | [project source](https://github.com/Blacktempel/DiskInfoToolkit), [NuGet package](https://www.nuget.org/packages/DiskInfoToolkit/1.1.2) |
| RAMSPDToolkit-NDD | 1.4.2 | MPL-2.0 | [project source](https://github.com/Blacktempel/RAMSPDToolkit), [NuGet package](https://www.nuget.org/packages/RAMSPDToolkit-NDD/1.4.2) |
| BlackSharp.Core | 1.0.7 | MPL-2.0 | [project source](https://github.com/Blacktempel/BlackSharp), [NuGet package](https://www.nuget.org/packages/BlackSharp.Core/1.0.7) |
| PawnIO module code embedded in LibreHardwareMonitorLib | bundled with 0.9.6 | LGPL-2.1 | [PawnIO project](https://github.com/namazso/PawnIO.Modules) |
| HidSharp | 2.6.4 | MIT | [project page](https://software.seekye.com/hidsharp), [NuGet package](https://www.nuget.org/packages/HidSharp/2.6.4) |
| Microsoft BCL and System support assemblies | versions listed in the NuGet packages | MIT | [.NET maintenance packages](https://github.com/dotnet/maintenance-packages) |

The applicable license texts are included next to the third-party binaries in
the repository's `OpenHardwareMonitorApi/ThirdParty/LibreHardwareMonitor-0.9.6`
directory and are copied into each standard release archive. The package does
not contain a PawnIO driver binary. TrayS only probes and opens the PawnIO
device if it is already installed and available; it never installs or starts
the driver.

LibreHardwareMonitor and its MPL-2.0 dependencies are distributed unmodified.
Their source forms are available from the upstream links above. TrayS's
separate wrapper and application files remain covered by the repository's
existing license notices.
