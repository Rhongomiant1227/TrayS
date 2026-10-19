#pragma once

#include <windows.h>
#include <string>

#if !defined(_M_X64) && !defined(__x86_64__)
#error TrayS 1.7.9 and later support x64 builds only.
#endif

// This is a separate, user-approved prerequisite installation. Application
// updates never run the installer, and no driver is embedded in TrayS's ZIP.
#define TRAYS_PAWNIO_VERSION L"2.2.0"
#define WM_APP_TRAYS_PAWNIO_SETUP (WM_APP + 0x5B)

enum PawnIoSetupPhase
{
	PAWNIO_SETUP_IDLE = 0,
	PAWNIO_SETUP_DOWNLOADING,
	PAWNIO_SETUP_VERIFYING,
	PAWNIO_SETUP_AUTHORIZING,
	PAWNIO_SETUP_INSTALLING,
	PAWNIO_SETUP_FINISHED
};

BOOL IsPawnIoMissingError(DWORD error);
BOOL IsPawnIoSetupArchitectureSupported(WORD nativeArchitecture);
BOOL IsPawnIoSetupSupported();
BOOL ShouldOfferPawnIoSetup(BOOL monitoringEnabled, DWORD deviceError,
	BOOL setupInProgress, BOOL alreadyOffered);

// Download uses only the pinned official HTTPS release. Both download and
// elevated execution verify its SHA-256 and Windows Authenticode signature.
DWORD DownloadVerifiedPawnIoInstaller(std::wstring* directory, std::wstring* path,
	volatile LONG* phase, HANDLE cancelEvent);
DWORD VerifyPawnIoInstaller(LPCWSTR path);
DWORD InstallVerifiedPawnIoDriver(LPCWSTR path);
void RemovePawnIoDownload(const std::wstring& directory, const std::wstring& path);
