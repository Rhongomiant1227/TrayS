#pragma once

#include <windows.h>

// Keep this identity synchronized with TrayS.rc and the versioned GitHub
// assets. The updater refuses a release that is not a newer semantic version
// for the current process architecture.
#define TRAYS_VERSION_MAJOR 1
#define TRAYS_VERSION_MINOR 5
#define TRAYS_VERSION_PATCH 0
#define TRAYS_VERSION_STRING L"1.5.0"
#define TRAYS_VERSION_TAG L"v1.5.0"
#define TRAYS_UPDATE_REPOSITORY L"Rhongomiant1227/TrayS"
#define TRAYS_UPDATE_API_HOST L"api.github.com"
#define TRAYS_UPDATE_API_PATH L"/repos/Rhongomiant1227/TrayS/releases/latest"
#define TRAYS_UPDATE_START_TIMER 42101
#define TRAYS_UPDATE_PERIOD_TIMER 42102

// The update worker posts an owned TRAYS_UPDATE_MESSAGE to the main window.
// The UI must call FreeTraySUpdateMessage after handling it. No browser is
// opened; all metadata and package bytes are fetched with WinHTTP.
#define WM_APP_TRAYS_UPDATE (WM_APP + 0x5A)

enum TRAYS_UPDATE_EVENT
{
	TRAYS_UPDATE_EVENT_NONE = 0,
	TRAYS_UPDATE_EVENT_NO_UPDATE = 1,
	TRAYS_UPDATE_EVENT_READY = 2,
	TRAYS_UPDATE_EVENT_ERROR = 3
};

typedef struct _TRAYS_UPDATE_INFO
{
	WCHAR version[32];
	WCHAR assetName[128];
	WCHAR downloadUrl[1024];
	WCHAR sha256[65];
	WCHAR downloadPath[32768];
	BOOL automatic;
} TRAYS_UPDATE_INFO;

typedef struct _TRAYS_UPDATE_MESSAGE
{
	DWORD event;
	WCHAR error[256];
	TRAYS_UPDATE_INFO info;
} TRAYS_UPDATE_MESSAGE;

BOOL ReadTraySAutoUpdateSetting();
BOOL WriteTraySAutoUpdateSetting(BOOL enabled);
BOOL StartTraySUpdateCheck(HWND notifyWindow, BOOL automatic);
BOOL LaunchTraySUpdateApplier(const TRAYS_UPDATE_INFO* info, DWORD parentProcessId, LPCWSTR targetPath);
BOOL TryRunTraySUpdateCommandLine();
void FreeTraySUpdateMessage(TRAYS_UPDATE_MESSAGE* message, BOOL deleteDownloadedFile);
