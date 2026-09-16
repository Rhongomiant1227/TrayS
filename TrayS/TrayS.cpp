// TrayS.cpp : 定义应用程序的入口点。
//
#ifdef _WIN64
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df' language='*'\"")
#else
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='x86' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#include "framework.h"
#include "TrayS.h"
#include <intrin.h>
#include <limits.h>
#include <psapi.h>
#include <cmath>
static void ClearNetworkSnapshotUnlocked();
static void ClearProcessSnapshotUnlocked();

// Network providers are normally trusted Windows components, but adapter
// enumeration is an externally supplied, variable-sized data boundary. Keep
// a corrupt size/count from turning into an unbounded heap allocation or loop.
static const SIZE_T kMaxAdapterAddressBytes = 4u * 1024u * 1024u;
static const SIZE_T kMaxIfTableBytes = 4u * 1024u * 1024u;
static const DWORD kMaxIfTableEntries = 4096;

static BOOL IsReasonableNetworkBufferSize(ULONG bytes, SIZE_T maximum)
{
	return bytes != 0 && (SIZE_T)bytes <= maximum;
}

static ULONG64 SaturatingAddUlong64(ULONG64 left, ULONG64 right)
{
	const ULONG64 maxValue = ~(ULONG64)0;
	return left > maxValue - right ? maxValue : left + right;
}

static ULONG64 SaturatingMultiplyUlong64(ULONG64 value, ULONG64 multiplier)
{
	const ULONG64 maxValue = ~(ULONG64)0;
	if (multiplier == 0 || value == 0)
		return 0;
	return value > maxValue / multiplier ? maxValue : value * multiplier;
}

static int TrafficDisplayInt(ULONG64 value)
{
	// GetTrafficStr ultimately targets the legacy wsprintf %d format.  Keep
	// every argument inside int's range instead of allowing a large 64-bit
	// counter to be truncated by the varargs ABI.
	return value > (ULONG64)INT_MAX ? INT_MAX : (int)value;
}

static LONG ScaleRectFill(LONG left, LONG right, UINT64 numerator, UINT64 denominator)
{
	if (right <= left || denominator == 0 || numerator == 0)
		return left;
	if (numerator > denominator)
		numerator = denominator;
	long double width = (long double)(right - left);
	long double scaled = width * (long double)numerator / (long double)denominator;
	long double maximum = (long double)LONG_MAX - (long double)left;
	if (scaled >= maximum)
		return LONG_MAX;
	return left + (LONG)scaled;
}

static BOOL IsValidIfTableBuffer(PMIB_IFTABLE table, ULONG bytes)
{
	if (!table || bytes < FIELD_OFFSET(MIB_IFTABLE, table))
		return FALSE;
	ULONG count = table->dwNumEntries;
	SIZE_T required = FIELD_OFFSET(MIB_IFTABLE, table);
	if (count > (SIZE_T)-1 / sizeof(MIB_IFROW))
		return FALSE;
	required += (SIZE_T)count * sizeof(MIB_IFROW);
	return required <= bytes;
}

static BOOL ComputeDibImageSize(int width, int height, DWORD* imageSize)
{
	if (!imageSize || width <= 0 || height <= 0)
		return FALSE;
	SIZE_T w = (SIZE_T)(unsigned int)width;
	SIZE_T h = (SIZE_T)(unsigned int)height;
	SIZE_T maxSize = (SIZE_T)-1;
	if (w > maxSize / h)
		return FALSE;
	SIZE_T pixels = w * h;
	if (pixels > maxSize / 4u)
		return FALSE;
	SIZE_T bytes = pixels * 4u;
	if (bytes == 0 || bytes > MAXDWORD)
		return FALSE;
	*imageSize = (DWORD)bytes;
	return TRUE;
}

static void SanitizeProcessDisplayDataUnlocked()
{
	// The worker and the paint handler share these pointer-sorted arrays.  Keep
	// a transient allocation or an interrupted refresh from turning a null
	// entry, unterminated name, or non-finite percentage into a GDI/formatting
	// fault in the UI thread.
	for (int i = 0; i < 6; ++i)
	{
		BOOL validCpuPointer = FALSE;
		BOOL validMemoryPointer = FALSE;
		for (int j = 0; j < 6; ++j)
		{
			if (ppcu[i] == &pcu[j])
				validCpuPointer = TRUE;
			if (ppmu[i] == &pmu[j])
				validMemoryPointer = TRUE;
		}
		if (!validCpuPointer)
			ppcu[i] = &pcu[i];
		if (!validMemoryPointer)
			ppmu[i] = &pmu[i];
		ppcu[i]->szExe[ARRAYSIZE(ppcu[i]->szExe) - 1] = L'\0';
		ppmu[i]->szExe[ARRAYSIZE(ppmu[i]->szExe) - 1] = L'\0';
		if (!std::isfinite(ppcu[i]->fCpuUsage) || ppcu[i]->fCpuUsage < 0.0f || ppcu[i]->fCpuUsage > 100.0f)
			ppcu[i]->fCpuUsage = 0.0f;
	}
}
COLORREF oPixelColor;
HDC hDesktopDC=NULL;

// Keep all DLL loads inside the directories we explicitly trust. The
// original project passed bare names to LoadLibrary, which allowed the
// current working directory (and other search locations) to win over the
// Windows system copy. TrayS can run elevated, so a DLL search hijack would
// otherwise become a privilege-escalation path.
static void ConfigureSafeDllSearch()
{
	typedef BOOL(WINAPI* pfnSetDefaultDllDirectories)(DWORD);
	HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
	if (!kernel32)
		return;
	pfnSetDefaultDllDirectories setDefaultDllDirectories =
		(pfnSetDefaultDllDirectories)GetProcAddress(kernel32, "SetDefaultDllDirectories");
	if (setDefaultDllDirectories)
		setDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
}

static HMODULE LoadSystemLibrarySafe(LPCWSTR moduleName)
{
	if (!moduleName || !moduleName[0])
		return NULL;
	return LoadLibraryExW(moduleName, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
}

static BOOL GetApplicationFilePath(LPCWSTR fileName, LPWSTR path, DWORD pathChars)
{
	if (!fileName || !fileName[0] || !path || pathChars < 2)
		return FALSE;
	DWORD length = GetModuleFileNameW(NULL, path, pathChars);
	if (length == 0 || length >= pathChars)
		return FALSE;
	LPWSTR slash = path + length;
	while (slash > path && slash[-1] != L'\\' && slash[-1] != L'/')
		--slash;
	if (slash == path)
		return FALSE;
	DWORD fileLength = lstrlenW(fileName);
	DWORD prefixLength = (DWORD)(slash - path);
	if (fileLength >= pathChars - prefixLength)
		return FALSE;
	CopyMemory(slash, fileName, (fileLength + 1) * sizeof(WCHAR));
	return TRUE;
}

static HMODULE LoadApplicationLibrarySafe(LPCWSTR moduleName)
{
	WCHAR path[32768] = {};
	if (!GetApplicationFilePath(moduleName, path, ARRAYSIZE(path)))
		return NULL;
	// Resolve dependencies such as HidSharp.dll beside the explicitly selected
	// application DLL without re-enabling the current-working-directory search.
	return LoadLibraryExW(path, NULL,
		LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
		LOAD_LIBRARY_SEARCH_SYSTEM32);
}

static BOOL BuildUserAutoRunCommand(WCHAR* command, DWORD commandChars)
{
	if (!command || commandChars < 4)
		return FALSE;
	DWORD length = GetModuleFileNameW(NULL, command + 1, commandChars - 4);
	if (length == 0 || length >= commandChars - 4)
		return FALSE;
	command[0] = L'"';
	command[length + 1] = L'"';
	command[length + 2] = L' ';
	command[length + 3] = L't';
	command[length + 4] = L'\0';
	return TRUE;
}

// Keep auto-start in HKCU only.  The legacy AutoRun helper also creates a
// SYSTEM service and a scheduled task for administrators; that combination is
// unnecessary for a tray UI and is a common source of security-product alerts.
static BOOL SetUserAutoRunSafe(BOOL enable, LPCWSTR valueName)
{
	if (!valueName || !valueName[0])
		return FALSE;
	HKEY key = NULL;
	LPCWSTR runPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
	LONG status = RegCreateKeyExW(HKEY_CURRENT_USER, runPath, 0, NULL, 0,
		KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &key, NULL);
	if (status != ERROR_SUCCESS)
		return FALSE;
	BOOL result = FALSE;
	if (!enable)
	{
		status = RegDeleteValueW(key, valueName);
		result = status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
	}
	else
	{
		WCHAR command[32768] = {};
		if (BuildUserAutoRunCommand(command, ARRAYSIZE(command)))
		{
			DWORD bytes = (lstrlenW(command) + 1) * sizeof(WCHAR);
			status = RegSetValueExW(key, valueName, 0, REG_SZ,
				reinterpret_cast<const BYTE*>(command), bytes);
			result = status == ERROR_SUCCESS;
		}
	}
	RegCloseKey(key);
	return result;
}

static BOOL IsUserAutoRunEnabledSafe(LPCWSTR valueName)
{
	if (!valueName || !valueName[0])
		return FALSE;
	HKEY key = NULL;
	if (RegOpenKeyExW(HKEY_CURRENT_USER,
		L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
		KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
		return FALSE;
	WCHAR configured[32768] = {};
	DWORD type = 0;
	DWORD bytes = sizeof(configured);
	LONG status = RegQueryValueExW(key, valueName, NULL, &type,
		reinterpret_cast<LPBYTE>(configured), &bytes);
	RegCloseKey(key);
	if (status != ERROR_SUCCESS || type != REG_SZ || bytes < sizeof(WCHAR))
		return FALSE;
	configured[ARRAYSIZE(configured) - 1] = L'\0';
	WCHAR expected[32768] = {};
	if (!BuildUserAutoRunCommand(expected, ARRAYSIZE(expected)))
		return FALSE;
	return lstrcmpiW(configured, expected) == 0;
}

static void TrimOwnWorkingSetSafe()
{
	// The old helper requested PROCESS_ALL_ACCESS even when trimming this
	// process, which can fail under UAC and unnecessarily resembles a process
	// injector to security software.  The pseudo-handle needs no extra token
	// privilege and never requires a CloseHandle call.
	SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

static DWORD GetSystemUsesLightThemeSafe()
{
	HKEY key = NULL;
	DWORD theme = 0;
	DWORD type = 0;
	DWORD size = sizeof(theme);
	if (RegOpenKeyExW(HKEY_CURRENT_USER,
		L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
		0, KEY_READ, &key) == ERROR_SUCCESS)
	{
		if (RegQueryValueExW(key, L"SystemUsesLightTheme", NULL, &type,
			reinterpret_cast<LPBYTE>(&theme), &size) != ERROR_SUCCESS ||
			type != REG_DWORD || size != sizeof(theme))
			theme = 0;
		RegCloseKey(key);
	}
	return theme;
}

// The legacy helper in Function.cpp assumes every monitor and taskbar lookup
// succeeds. Explorer can be restarting while TrayS is painting, so use the
// monitor's own work area here and fail closed when the window is gone.
static BOOL GetScreenRectSafe(HWND hWnd, LPRECT lpRect, BOOL bTray)
{
	if (!lpRect || !IsWindow(hWnd))
		return FALSE;
	HMONITOR monitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
	if (!monitor)
		return FALSE;
	MONITORINFO info{};
	info.cbSize = sizeof(info);
	if (!GetMonitorInfo(monitor, &info))
		return FALSE;
	*lpRect = bTray ? info.rcWork : info.rcMonitor;
	return TRUE;
}

int DPI(int pixel)
{
	return pixel * iDPI / 96;
}

COLORREF GetWindowPixel(HWND hWnd)
{
	if (!IsWindow(hWnd) || hDesktopDC == NULL)
		return RGB(0, 0, 1);
	RECT rc{};
	if (!GetWindowRect(hWnd, &rc))
		return RGB(0, 0, 1);
	int width = rc.right - rc.left;
	int height = rc.bottom - rc.top;
	if (width <= 0 || height <= 0)
		return RGB(0, 0, 1);
	COLORREF c = GetPixel(hDesktopDC, rc.left + 1, rc.top + height / 2);
	return c == CLR_INVALID ? RGB(0, 0, 1) : c;
}
BOOL CALLBACK FindSettingWindowFunc(HWND hWnd, LPARAM lpAram)///////////查找TrayS主窗口防止重复开启
{
	WCHAR szText[16];
	GetWindowText(hWnd, szText, 16);
	if (lstrcmp(szText, L"_TrayS_") == 0)
	{
		SendMessage(hWnd, WM_TRAYS, 0, 0);
		ExitProcess(0);
		return FALSE;
	}
	return TRUE;
}
BOOL CALLBACK IsZoomedFunc(HWND hWnd, LPARAM lpAram)////是否有最大化窗口
{
	if (::IsWindowVisible(hWnd) && IsZoomed(hWnd))
	{
		if (MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST) == (HMONITOR)lpAram)
		{
			BOOL Attribute = FALSE;
			if (pDwmGetWindowAttribute)
				pDwmGetWindowAttribute(hWnd, 14, &Attribute, sizeof(BOOL));
			if (Attribute == FALSE)
			{
				iWindowMode = 1;
				return FALSE;
			}
		}
	}
	return TRUE;
}
/*
BOOL CreateProcessByExplorer(LPCWSTR process, LPCWSTR szDir, LPCWSTR cmd)
{
	BOOL ret = FALSE;

	HANDLE hProcess = 0, hToken = 0, hDuplicatedToken = 0;
	LPVOID lpEnv = NULL;
	do
	{
		HWND hTrayWnd = ::FindWindow(szShellTray, NULL);
		DWORD explorerPid;
		GetWindowThreadProcessId(hTrayWnd, &explorerPid);// 获取explorer进程号，自行实现

		if (explorerPid == NULL)
			break;
		hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, TRUE, explorerPid);
		if (INVALID_HANDLE_VALUE == hProcess)
			break;

		if (!OpenProcessToken(hProcess, TOKEN_ALL_ACCESS, &hToken))
			break;

		DuplicateTokenEx(hToken, MAXIMUM_ALLOWED, NULL, SecurityIdentification, TokenPrimary, &hDuplicatedToken);
		CreateEnvironmentBlock(&lpEnv, hDuplicatedToken, FALSE);

		/ *
			WCHAR szDir[MAX_PATH] = L"\"";
		wcscpy_s(&szDir[2], MAX_PATH, process);
		int iLen = wcslen(szDir);
		if (NULL != cmd)
		{
			wcscpy_s(&szDir[iLen], MAX_PATH, L"\" \"");
			iLen = wcslen(szDir);
			wcscpy_s(&szDir[iLen], MAX_PATH, cmd);
		}
		iLen = wcslen(szDir);
		wcscpy_s(&szDir[iLen], MAX_PATH, L"\"");
		* /

			STARTUPINFO si = { 0 };
		PROCESS_INFORMATION pi = { 0 };
		si.cb = sizeof(STARTUPINFO);
		si.lpDesktop = (LPWSTR)L"winsta0\\default";
		si.dwFlags = STARTF_USESHOWWINDOW;
		si.wShowWindow = SW_HIDE;
		if (!CreateProcessAsUser(hToken, process, NULL / *const_cast<LPWSTR>(szDir) * / , 0, 0, FALSE, CREATE_UNICODE_ENVIRONMENT, lpEnv, NULL, &si, &pi))
			break;
		ret = TRUE;
	} while (0);
	if (INVALID_HANDLE_VALUE != hProcess)
		CloseHandle(hProcess);
	if (INVALID_HANDLE_VALUE != hToken)
		CloseHandle(hToken);
	if (INVALID_HANDLE_VALUE != hDuplicatedToken)
		CloseHandle(hDuplicatedToken);
	if (NULL != lpEnv)
		DestroyEnvironmentBlock(lpEnv);
	return ret;
}

*/
#define CTL_CODE( DeviceType, Function, Method, Access ) (((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method))
#define IOCTL_STORAGE_GET_DEVICE_NUMBER       CTL_CODE(0x0000002d, 0x0420, 0, 0)

typedef struct _STORAGE_DEVICE_NUMBER {
	DWORD		DeviceType;
	DWORD       DeviceNumber;
	DWORD       PartitionNumber;
} STORAGE_DEVICE_NUMBER, * PSTORAGE_DEVICE_NUMBER;
DWORD GetPhysicalDriveFromPartitionLetter(WCHAR letter)//通过盘符获取物理硬盘位置
{
	if (!((letter >= L'A' && letter <= L'Z') || (letter >= L'a' && letter <= L'z')))
		return DWORD(-1);
	if (letter >= L'a' && letter <= L'z')
		letter = (WCHAR)(letter - L'a' + L'A');
	HANDLE hDevice;
	DWORD readed = 0;
	STORAGE_DEVICE_NUMBER number{};
	WCHAR path[64] = {};
	wsprintf(path, L"\\\\.\\%c:", letter);
	// The query is read-only.  Requesting GENERIC_WRITE on a physical volume
	// needlessly requires elevation and makes security products treat TrayS as a
	// disk-management utility.
	hDevice = CreateFile(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL, OPEN_EXISTING, 0, NULL);
	if (hDevice != INVALID_HANDLE_VALUE) // cannot open the drive
	{
		if(DeviceIoControl(hDevice, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0,
			&number, sizeof(number), &readed, NULL) && readed >= sizeof(number))
		{		
			(void)CloseHandle(hDevice);
			return number.DeviceNumber;
		}
		CloseHandle(hDevice);
	}
	return DWORD(-1);
}

DWORD iCPU;
/////////////////////////////////////////////////////////////////////////////CPU占用率
FILETIME pre_idleTime;
FILETIME pre_kernelTime;
FILETIME pre_userTime;
__int64 CompareFileTime(FILETIME time1, FILETIME time2)
{
	ULARGE_INTEGER a{};
	ULARGE_INTEGER b{};
	a.HighPart = time1.dwHighDateTime;
	a.LowPart = time1.dwLowDateTime;
	b.HighPart = time2.dwHighDateTime;
	b.LowPart = time2.dwLowDateTime;
	return (b.QuadPart >= a.QuadPart) ? (__int64)(b.QuadPart - a.QuadPart) : -(__int64)(a.QuadPart - b.QuadPart);
}
typedef struct _PDH_RAW_COUNTER {
	volatile DWORD CStatus;
	FILETIME    TimeStamp;
	LONGLONG    FirstValue;
	LONGLONG    SecondValue;
	DWORD       MultiCount;
} PDH_RAW_COUNTER, * PPDH_RAW_COUNTER;
PDH_RAW_COUNTER m_last_rawData;

#define PDH_FMT_RAW          ((DWORD) 0x00000010)
#define PDH_FMT_ANSI         ((DWORD) 0x00000020)
#define PDH_FMT_UNICODE      ((DWORD) 0x00000040)
#define PDH_FMT_LONG         ((DWORD) 0x00000100)
#define PDH_FMT_DOUBLE       ((DWORD) 0x00000200)
#define PDH_FMT_LARGE        ((DWORD) 0x00000400)
#define PDH_FMT_NOSCALE      ((DWORD) 0x00001000)
#define PDH_FMT_1000         ((DWORD) 0x00002000)
#define PDH_FMT_NODATA       ((DWORD) 0x00004000)
#define PDH_FMT_NOCAP100     ((DWORD) 0x00008000)
#define PDH_CSTATUS_VALID_DATA ((DWORD)0x00000000)
#define PDH_CSTATUS_NEW_DATA   ((DWORD)0x00000001)
#define PERF_DETAIL_COSTLY   ((DWORD) 0x00010000)
#define PERF_DETAIL_STANDARD ((DWORD) 0x0000FFFF)
typedef HANDLE       PDH_HCOUNTER;
typedef HANDLE       PDH_HQUERY;
typedef HANDLE       PDH_HLOG;

typedef PDH_HCOUNTER HCOUNTER;
typedef PDH_HQUERY   HQUERY;

typedef struct _PDH_FMT_COUNTERVALUE {
	DWORD    CStatus;
	union {
		LONG        longValue;
		double      doubleValue;
		LONGLONG    largeValue;
		LPCSTR      AnsiStringValue;
		LPCWSTR     WideStringValue;
	};
} PDH_FMT_COUNTERVALUE, * PPDH_FMT_COUNTERVALUE;
HQUERY hQuery;
HCOUNTER hCounter;
HCOUNTER hDiskRead;
HCOUNTER hDiskWrite;
HCOUNTER hDiskTime;
DWORD counterType;
PDH_RAW_COUNTER rawData;
typedef ULONG(WINAPI* pfnPdhOpenQuery)(_In_opt_ LPCWSTR szDataSource, _In_ DWORD_PTR dwUserData, _Out_ PDH_HQUERY* phQuery);
typedef ULONG(WINAPI* pfnPdhAddCounter)(_In_ PDH_HQUERY hQuery, _In_ LPCWSTR szFullCounterPath, _In_ DWORD_PTR dwUserData, _Out_ PDH_HCOUNTER* phCounter);
typedef ULONG(WINAPI* pfnPdhCollectQueryData)(PDH_HQUERY hQuery);
//typedef ULONG(WINAPI* pfnPdhGetRawCounterValue)(PDH_HCOUNTER hCounter, LPDWORD lpdwType, PPDH_RAW_COUNTER pValue);
//typedef ULONG(WINAPI* pfnPdhCalculateCounterFromRawValue)(PDH_HCOUNTER hCounter, DWORD dwFormat, PPDH_RAW_COUNTER rawValue1, PPDH_RAW_COUNTER rawValue2, PPDH_FMT_COUNTERVALUE fmtValue);
typedef ULONG(WINAPI *pfnPdhRemoveCounter)(PDH_HCOUNTER hCounter);
typedef ULONG(WINAPI* pfnPdhCloseQuery)(PDH_HQUERY hQuery);
typedef ULONG(WINAPI* pfnPdhGetFormattedCounterValue)(PDH_HCOUNTER hCounter, DWORD dwFormat, LPDWORD lpdwType, PPDH_FMT_COUNTERVALUE pValue);
pfnPdhOpenQuery PdhOpenQuery;
pfnPdhAddCounter PdhAddCounter;
pfnPdhCollectQueryData PdhCollectQueryData;
pfnPdhRemoveCounter PdhRemoveCounter;
//pfnPdhGetRawCounterValue PdhGetRawCounterValue;
//pfnPdhCalculateCounterFromRawValue PdhCalculateCounterFromRawValue;
pfnPdhCloseQuery PdhCloseQuery;
pfnPdhGetFormattedCounterValue PdhGetFormattedCounterValue;
static SRWLOCK g_pdhLock = SRWLOCK_INIT;
static BOOL IsPdhValueValid(DWORD status);

// Windows exposes ACPI thermal zones through the PDH provider. This is a
// user-mode, read-only path supplied by the firmware/OS; it does not access
// MSRs or PCI configuration space and does not install a kernel driver.
typedef ULONG(WINAPI* pfnPdhExpandWildCardPath)(
	_In_opt_ LPCWSTR szDataSource,
	_In_ LPCWSTR szWildCardPath,
	_Out_writes_to_(*pcchPathListLength, *pcchPathListLength) LPWSTR mszExpandedPathList,
	_Inout_ LPDWORD pcchPathListLength,
	_In_ DWORD dwFlags);
static const ULONG kPdhMoreData = 0x800007D2u;
static const DWORD kMaxThermalCounters = 32;
static const DWORD kMaxThermalPathChars = 65536;
static HMODULE hThermalPDH = NULL;
static HQUERY hThermalQuery = NULL;
static HCOUNTER hThermalCounters[kMaxThermalCounters] = {};
static DWORD thermalCounterCount = 0;
static BOOL thermalCounterUsesHighPrecision = FALSE;
// A missing ACPI provider is a stable, supported outcome on some firmware.
// Cache the probe result until the user explicitly toggles temperature
// monitoring, rather than loading pdh.dll and expanding wildcards on every
// one-second sample.
static BOOL thermalProbeAttempted = FALSE;
static SRWLOCK g_thermalPdhLock = SRWLOCK_INIT;
static pfnPdhOpenQuery ThermalPdhOpenQuery = NULL;
static pfnPdhAddCounter ThermalPdhAddCounter = NULL;
static pfnPdhCollectQueryData ThermalPdhCollectQueryData = NULL;
static pfnPdhRemoveCounter ThermalPdhRemoveCounter = NULL;
static pfnPdhCloseQuery ThermalPdhCloseQuery = NULL;
static pfnPdhGetFormattedCounterValue ThermalPdhGetFormattedCounterValue = NULL;
static pfnPdhExpandWildCardPath ThermalPdhExpandWildCardPath = NULL;

static void CloseThermalQueryUnlocked()
{
	if (ThermalPdhRemoveCounter)
	{
		for (DWORD i = 0; i < thermalCounterCount; ++i)
		{
			if (hThermalCounters[i])
				ThermalPdhRemoveCounter(hThermalCounters[i]);
			hThermalCounters[i] = NULL;
		}
	}
	thermalCounterCount = 0;
	thermalCounterUsesHighPrecision = FALSE;
	if (ThermalPdhCloseQuery && hThermalQuery)
		ThermalPdhCloseQuery(hThermalQuery);
	hThermalQuery = NULL;
	if (hThermalPDH)
		FreeLibrary(hThermalPDH);
	hThermalPDH = NULL;
	ThermalPdhOpenQuery = NULL;
	ThermalPdhAddCounter = NULL;
	ThermalPdhCollectQueryData = NULL;
	ThermalPdhRemoveCounter = NULL;
	ThermalPdhCloseQuery = NULL;
	ThermalPdhGetFormattedCounterValue = NULL;
	ThermalPdhExpandWildCardPath = NULL;
}

static BOOL AddThermalCountersForPatternUnlocked(LPCWSTR pattern)
{
	if (!pattern || !ThermalPdhExpandWildCardPath || !ThermalPdhAddCounter || !hThermalQuery)
		return FALSE;
	DWORD pathChars = 0;
	ULONG status = ThermalPdhExpandWildCardPath(NULL, pattern, NULL, &pathChars, 0);
	if (status != kPdhMoreData && status != ERROR_MORE_DATA && status != ERROR_SUCCESS)
		return FALSE;
	if (pathChars < 2 || pathChars > kMaxThermalPathChars)
		return FALSE;
	SIZE_T bytes = static_cast<SIZE_T>(pathChars) * sizeof(WCHAR);
	WCHAR* expandedPaths = static_cast<WCHAR*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes));
	if (!expandedPaths)
		return FALSE;
	status = ThermalPdhExpandWildCardPath(NULL, pattern, expandedPaths, &pathChars, 0);
	BOOL added = FALSE;
	if (status == ERROR_SUCCESS)
	{
		DWORD offset = 0;
		while (offset + 1 < pathChars && expandedPaths[offset] != L'\0' &&
			thermalCounterCount < kMaxThermalCounters)
		{
			DWORD length = 0;
			while (offset + length < pathChars && expandedPaths[offset + length] != L'\0')
				++length;
			if (offset + length >= pathChars)
				break;
			LPCWSTR path = expandedPaths + offset;
			HCOUNTER counter = NULL;
			if (ThermalPdhAddCounter(hThermalQuery, path, NULL, &counter) == ERROR_SUCCESS && counter)
			{
				hThermalCounters[thermalCounterCount++] = counter;
				added = TRUE;
			}
			offset += length + 1;
		}
	}
	HeapFree(GetProcessHeap(), 0, expandedPaths);
	return added;
}

static BOOL AddThermalCounterFallbackUnlocked(LPCWSTR path)
{
	if (!path || !ThermalPdhAddCounter || !hThermalQuery || thermalCounterCount >= kMaxThermalCounters)
		return FALSE;
	HCOUNTER counter = NULL;
	if (ThermalPdhAddCounter(hThermalQuery, path, NULL, &counter) != ERROR_SUCCESS || !counter)
		return FALSE;
	hThermalCounters[thermalCounterCount++] = counter;
	return TRUE;
}

static BOOL EnsureThermalQueryUnlocked()
{
	if (thermalProbeAttempted)
		return hThermalQuery != NULL && thermalCounterCount != 0;
	thermalProbeAttempted = TRUE;
	if (hThermalQuery && thermalCounterCount != 0)
		return TRUE;
	if (hThermalPDH == NULL)
		hThermalPDH = LoadSystemLibrarySafe(L"pdh.dll");
	if (!hThermalPDH)
		return FALSE;
	ThermalPdhOpenQuery = reinterpret_cast<pfnPdhOpenQuery>(GetProcAddress(hThermalPDH, "PdhOpenQueryW"));
	ThermalPdhAddCounter = reinterpret_cast<pfnPdhAddCounter>(GetProcAddress(hThermalPDH, "PdhAddCounterW"));
	ThermalPdhCollectQueryData = reinterpret_cast<pfnPdhCollectQueryData>(GetProcAddress(hThermalPDH, "PdhCollectQueryData"));
	ThermalPdhRemoveCounter = reinterpret_cast<pfnPdhRemoveCounter>(GetProcAddress(hThermalPDH, "PdhRemoveCounter"));
	ThermalPdhGetFormattedCounterValue = reinterpret_cast<pfnPdhGetFormattedCounterValue>(GetProcAddress(hThermalPDH, "PdhGetFormattedCounterValue"));
	ThermalPdhCloseQuery = reinterpret_cast<pfnPdhCloseQuery>(GetProcAddress(hThermalPDH, "PdhCloseQuery"));
	ThermalPdhExpandWildCardPath = reinterpret_cast<pfnPdhExpandWildCardPath>(GetProcAddress(hThermalPDH, "PdhExpandWildCardPathW"));
	if (!ThermalPdhOpenQuery || !ThermalPdhAddCounter || !ThermalPdhCollectQueryData ||
		!ThermalPdhRemoveCounter || !ThermalPdhGetFormattedCounterValue || !ThermalPdhCloseQuery)
	{
		CloseThermalQueryUnlocked();
		return FALSE;
	}
	if (!hThermalQuery && ThermalPdhOpenQuery(NULL, 0, &hThermalQuery) != ERROR_SUCCESS)
	{
		CloseThermalQueryUnlocked();
		return FALSE;
	}
	thermalCounterUsesHighPrecision = AddThermalCountersForPatternUnlocked(
		L"\\Thermal Zone Information(*)\\High Precision Temperature");
	if (!thermalCounterUsesHighPrecision)
	{
		if (AddThermalCounterFallbackUnlocked(L"\\Thermal Zone Information(_Total)\\High Precision Temperature"))
			thermalCounterUsesHighPrecision = TRUE;
	}
	if (thermalCounterCount == 0)
	{
		thermalCounterUsesHighPrecision = FALSE;
		AddThermalCountersForPatternUnlocked(L"\\Thermal Zone Information(*)\\Temperature");
		if (thermalCounterCount == 0)
			AddThermalCounterFallbackUnlocked(L"\\Thermal Zone Information(_Total)\\Temperature");
	}
	if (thermalCounterCount == 0)
	{
		CloseThermalQueryUnlocked();
		return FALSE;
	}
	return TRUE;
}

static int GetAcpiCpuTemperature()
{
	AcquireSRWLockExclusive(&g_thermalPdhLock);
	int result = 0;
	if (EnsureThermalQueryUnlocked() && ThermalPdhCollectQueryData(hThermalQuery) == ERROR_SUCCESS)
	{
		double bestCelsius = -1.0;
		for (DWORD i = 0; i < thermalCounterCount; ++i)
		{
			if (!hThermalCounters[i])
				continue;
			PDH_FMT_COUNTERVALUE value{};
			DWORD valueType = 0;
			if (ThermalPdhGetFormattedCounterValue(hThermalCounters[i], PDH_FMT_DOUBLE, &valueType, &value) != ERROR_SUCCESS ||
				!IsPdhValueValid(value.CStatus) || !std::isfinite(value.doubleValue))
				continue;
			double celsius = thermalCounterUsesHighPrecision ? (value.doubleValue / 10.0) - 273.15 : value.doubleValue - 273.15;
			if ((celsius < 0.0 || celsius > 200.0) && value.doubleValue >= 2500.0 && value.doubleValue <= 5000.0)
				celsius = (value.doubleValue / 10.0) - 273.15;
			if (celsius >= 0.0 && celsius <= 200.0 && (bestCelsius < 0.0 || celsius > bestCelsius))
				bestCelsius = celsius;
		}
		if (bestCelsius >= 0.0 && bestCelsius <= 200.0)
			result = static_cast<int>(bestCelsius + 0.5);
	}
	ReleaseSRWLockExclusive(&g_thermalPdhLock);
	return result;
}

static BOOL HasAcpiTemperaturePath()
{
	AcquireSRWLockShared(&g_thermalPdhLock);
	BOOL available = hThermalQuery != NULL && thermalCounterCount != 0;
	ReleaseSRWLockShared(&g_thermalPdhLock);
	return available;
}

static BOOL IsPdhValueValid(DWORD status)
{
	return status == PDH_CSTATUS_VALID_DATA || status == PDH_CSTATUS_NEW_DATA;
}
static void ClosePDH()
{
	if (PdhRemoveCounter)
	{
		if (hCounter)
			PdhRemoveCounter(hCounter);
		if (hDiskRead)
			PdhRemoveCounter(hDiskRead);
		if (hDiskWrite)
			PdhRemoveCounter(hDiskWrite);
		if (hDiskTime)
			PdhRemoveCounter(hDiskTime);
	}
	hCounter = NULL;
	hDiskRead = NULL;
	hDiskWrite = NULL;
	hDiskTime = NULL;
	if (PdhCloseQuery && hQuery)
		PdhCloseQuery(hQuery);
	hQuery = NULL;
	if (hPDH)
		FreeLibrary(hPDH);
	hPDH = NULL;
	PdhOpenQuery = NULL;
	PdhAddCounter = NULL;
	PdhCollectQueryData = NULL;
	PdhRemoveCounter = NULL;
	PdhCloseQuery = NULL;
	PdhGetFormattedCounterValue = NULL;
}

static void SwitchPDHUnlocked(BOOL bOn)
{
	if (!bOn)
	{
		ClosePDH();
		return;
	}
	if (hPDH != NULL && hQuery != NULL)
		return;
	if (hPDH == NULL)
		hPDH = LoadSystemLibrarySafe(L"pdh.dll");
	if (hPDH == NULL)
		return;

	PdhOpenQuery = (pfnPdhOpenQuery)GetProcAddress(hPDH, "PdhOpenQueryW");
	PdhAddCounter = (pfnPdhAddCounter)GetProcAddress(hPDH, "PdhAddCounterW");
	PdhCollectQueryData = (pfnPdhCollectQueryData)GetProcAddress(hPDH, "PdhCollectQueryData");
	PdhRemoveCounter = (pfnPdhRemoveCounter)GetProcAddress(hPDH, "PdhRemoveCounter");
	PdhGetFormattedCounterValue = (pfnPdhGetFormattedCounterValue)GetProcAddress(hPDH, "PdhGetFormattedCounterValue");
	PdhCloseQuery = (pfnPdhCloseQuery)GetProcAddress(hPDH, "PdhCloseQuery");
	if (!PdhOpenQuery || !PdhAddCounter || !PdhCollectQueryData || !PdhRemoveCounter || !PdhGetFormattedCounterValue || !PdhCloseQuery)
	{
		ClosePDH();
		return;
	}

	if (PdhOpenQuery(NULL, 0, &hQuery) != ERROR_SUCCESS || hQuery == NULL)
	{
		ClosePDH();
		return;
	}
	// Task Manager and most monitoring tools expose processor time.  The old
	// Win10+ path used Processor Utility, which is frequency weighted and can
	// report values such as 2% while Task Manager shows 17% on modern systems.
	// Prefer Processor Time and retain Utility only as a legacy-counter fallback.
	if (PdhAddCounter(hQuery, L"\\Processor Information(_Total)\\% Processor Time", NULL, &hCounter) != ERROR_SUCCESS)
	{
		hCounter = NULL;
		PdhAddCounter(hQuery, L"\\Processor Information(_Total)\\% Processor Utility", NULL, &hCounter);
	}

	if (TraySave.szDisk == L'\0')
	{
		PdhAddCounter(hQuery, L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec", NULL, &hDiskRead);
		PdhAddCounter(hQuery, L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec", NULL, &hDiskWrite);
		PdhAddCounter(hQuery, L"\\PhysicalDisk(_Total)\\% Idle Time", NULL, &hDiskTime);
	}
	else
	{
		DWORD d = GetPhysicalDriveFromPartitionLetter(TraySave.szDisk);
		if (d != DWORD(-1))
		{
			WCHAR sz[256];
			wsprintf(sz, L"\\PhysicalDisk(%u %c:)\\Disk Read Bytes/sec", d, TraySave.szDisk);
			PdhAddCounter(hQuery, sz, NULL, &hDiskRead);
			wsprintf(sz, L"\\PhysicalDisk(%u %c:)\\Disk Write Bytes/sec", d, TraySave.szDisk);
			PdhAddCounter(hQuery, sz, NULL, &hDiskWrite);
			wsprintf(sz, L"\\PhysicalDisk(%u %c:)\\%% Idle Time", d, TraySave.szDisk);
			PdhAddCounter(hQuery, sz, NULL, &hDiskTime);
		}
	}
}
void SwitchPDH(BOOL bOn)
{
	AcquireSRWLockExclusive(&g_pdhLock);
	SwitchPDHUnlocked(bOn);
	ReleaseSRWLockExclusive(&g_pdhLock);
}
int GetPDH(BOOL bCPU, BOOL bDisk)
{
	AcquireSRWLockExclusive(&g_pdhLock);
	if (hPDH == NULL || hQuery == NULL)
		SwitchPDHUnlocked(TRUE);
	if (hPDH == NULL || hQuery == NULL || !PdhCollectQueryData || !PdhGetFormattedCounterValue)
	{
		int result = iCPU;
		ReleaseSRWLockExclusive(&g_pdhLock);
		return result;
	}
	if (PdhCollectQueryData(hQuery) == ERROR_SUCCESS)
	{
		PDH_FMT_COUNTERVALUE pdhValue{};
		DWORD dwValue = 0;
		if (bCPU)
		{
			/*
					PdhGetRawCounterValue(hCounter, &counterType, &rawData);
					PDH_FMT_COUNTERVALUE fmtValue;
					PdhCalculateCounterFromRawValue(hCounter, PDH_FMT_DOUBLE, &rawData, &m_last_rawData, &fmtValue);//计算使用率
					iCPU = (int)fmtValue.doubleValue;//传出数据
			*/
			if (hCounter && PdhGetFormattedCounterValue(hCounter, PDH_FMT_LONG, &dwValue, &pdhValue) == ERROR_SUCCESS && IsPdhValueValid(pdhValue.CStatus))
			{
				iCPU = pdhValue.longValue;
				if (iCPU < 0)
					iCPU = 0;
				else if (iCPU >= 100)
					iCPU = 99;
			}
		}
		if (bDisk)
		{
			if (hDiskRead)
			{
				if (TrayData && PdhGetFormattedCounterValue(hDiskRead, PDH_FMT_DOUBLE, &dwValue, &pdhValue) == ERROR_SUCCESS && IsPdhValueValid(pdhValue.CStatus) && std::isfinite(pdhValue.doubleValue))
					TrayData->diskreadbyte = pdhValue.doubleValue >= 0.0 ? pdhValue.doubleValue : 0.0;
			}
			if (hDiskWrite)
			{
				if (TrayData && PdhGetFormattedCounterValue(hDiskWrite, PDH_FMT_DOUBLE, &dwValue, &pdhValue) == ERROR_SUCCESS && IsPdhValueValid(pdhValue.CStatus) && std::isfinite(pdhValue.doubleValue))
					TrayData->diskwritebyte = pdhValue.doubleValue >= 0.0 ? pdhValue.doubleValue : 0.0;
			}
			if (hDiskTime)
			{
				if (TrayData && PdhGetFormattedCounterValue(hDiskTime, PDH_FMT_LONG, &dwValue, &pdhValue) == ERROR_SUCCESS && IsPdhValueValid(pdhValue.CStatus))
				{
					LONG idle = pdhValue.longValue;
					if (idle < 0) idle = 0;
					if (idle > 100) idle = 100;
					TrayData->disktime = 100 - idle;
				}
			}
			if (TrayData && TrayData->disktime >= 100)
				TrayData->disktime = 99;
		}
	}
	int result = iCPU;
	ReleaseSRWLockExclusive(&g_pdhLock);
	return result;
}
int GetCPUUseRate()
{
	if (TraySave.bMonitorPDH)
	{
		return GetPDH(TRUE,TraySave.bMonitorDisk);
	}
	else
	{
		AcquireSRWLockExclusive(&g_pdhLock);
		if (hPDH && !TraySave.bMonitorDisk)
			SwitchPDHUnlocked(FALSE);
		ReleaseSRWLockExclusive(&g_pdhLock);
		int nCPUUseRate = -1;
		FILETIME idleTime{};//空闲时间
		FILETIME kernelTime{};//核心态时间
		FILETIME userTime{};//用户态时间
		if (!GetSystemTimes(&idleTime, &kernelTime, &userTime))
			return iCPU;

		__int64 idle = CompareFileTime(pre_idleTime, idleTime);
		__int64 kernel = CompareFileTime(pre_kernelTime, kernelTime);
		__int64 user = CompareFileTime(pre_userTime, userTime);
		if (kernel + user != 0)
			nCPUUseRate = (int)((kernel + user - idle) * 100 / (kernel + user));
		pre_idleTime = idleTime;
		pre_kernelTime = kernelTime;
		pre_userTime = userTime;
		if (nCPUUseRate < 1)
			nCPUUseRate = iCPU;
		else if (nCPUUseRate >= 100)
			nCPUUseRate = 99;
		return nCPUUseRate;
	}
}
static void NormalizeTraySave()
{
	TraySave.AdpterName[ARRAYSIZE(TraySave.AdpterName) - 1] = '\0';
	TraySave.szTrafficOut[ARRAYSIZE(TraySave.szTrafficOut) - 1] = L'\0';
	TraySave.szTrafficIn[ARRAYSIZE(TraySave.szTrafficIn) - 1] = L'\0';
	TraySave.szTemperatureCPU[ARRAYSIZE(TraySave.szTemperatureCPU) - 1] = L'\0';
	TraySave.szTemperatureCPUUnit[ARRAYSIZE(TraySave.szTemperatureCPUUnit) - 1] = L'\0';
	TraySave.szTemperatureGPU[ARRAYSIZE(TraySave.szTemperatureGPU) - 1] = L'\0';
	TraySave.szTemperatureGPUUnit[ARRAYSIZE(TraySave.szTemperatureGPUUnit) - 1] = L'\0';
	TraySave.szUsageCPU[ARRAYSIZE(TraySave.szUsageCPU) - 1] = L'\0';
	TraySave.szUsageCPUUnit[ARRAYSIZE(TraySave.szUsageCPUUnit) - 1] = L'\0';
	TraySave.szUsageMEM[ARRAYSIZE(TraySave.szUsageMEM) - 1] = L'\0';
	TraySave.szUsageMEMUnit[ARRAYSIZE(TraySave.szUsageMEMUnit) - 1] = L'\0';
	TraySave.szDiskReadSec[ARRAYSIZE(TraySave.szDiskReadSec) - 1] = L'\0';
	TraySave.szDiskWriteSec[ARRAYSIZE(TraySave.szDiskWriteSec) - 1] = L'\0';
	TraySave.szDiskName[ARRAYSIZE(TraySave.szDiskName) - 1] = L'\0';
	TraySave.szPriceName1[ARRAYSIZE(TraySave.szPriceName1) - 1] = L'\0';
	TraySave.szPriceName2[ARRAYSIZE(TraySave.szPriceName2) - 1] = L'\0';
	TraySave.szPriceName3[ARRAYSIZE(TraySave.szPriceName3) - 1] = L'\0';
	TraySave.szPriceName4[ARRAYSIZE(TraySave.szPriceName4) - 1] = L'\0';
	TraySave.szOKXWeb[ARRAYSIZE(TraySave.szOKXWeb) - 1] = L'\0';
	TraySave.TraybarFont.lfFaceName[ARRAYSIZE(TraySave.TraybarFont.lfFaceName) - 1] = L'\0';
	TraySave.TipsFont.lfFaceName[ARRAYSIZE(TraySave.TipsFont.lfFaceName) - 1] = L'\0';
	if (TraySave.iUnit < 0 || TraySave.iUnit > 2)
		TraySave.iUnit = 2;
	if (TraySave.TraybarFontSize < -256 || TraySave.TraybarFontSize > 256 || TraySave.TraybarFontSize == 0)
		TraySave.TraybarFontSize = -14;
	if (TraySave.TipsFontSize < -256 || TraySave.TipsFontSize > 256 || TraySave.TipsFontSize == 0)
		TraySave.TipsFontSize = -14;
	for (int i = 0; i < ARRAYSIZE(TraySave.aMode); ++i)
	{
		if (TraySave.aMode[i] != ACCENT_DISABLED &&
			TraySave.aMode[i] != ACCENT_ENABLE_TRANSPARENTGRADIENT &&
			TraySave.aMode[i] != ACCENT_ENABLE_BLURBEHIND &&
			TraySave.aMode[i] != ACCENT_ENABLE_ACRYLICBLURBEHIND)
			TraySave.aMode[i] = ACCENT_DISABLED;
	}
}

void ReadReg()//读取设置
{
	WCHAR savePath[32768] = {};
	HANDLE hFile = INVALID_HANDLE_VALUE;
	if (GetApplicationFilePath(szTraySave, savePath, ARRAYSIZE(savePath)))
		hFile = CreateFileW(savePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile!= INVALID_HANDLE_VALUE)
	{
		TRAYSAVE loaded = TraySave;
		DWORD dwBytes = 0;
		if (ReadFile(hFile, &loaded, sizeof loaded, &dwBytes, NULL) &&
			dwBytes == sizeof loaded && loaded.Ver != 0 && loaded.Ver <= kTraySaveVersion)
		{
			TraySave = loaded;
		}
		CloseHandle(hFile);
	}
	// TrayS.dat is a raw struct for backwards compatibility.  A truncated or
	// hand-edited file can otherwise leave fixed-size strings unterminated and
	// make a later lstrlen/wsprintf walk past the structure.
	NormalizeTraySave();
	// Keep a corrupted/legacy value from creating a tight timer loop. The
	// previous default (11 ms) made Explorer redraw continuously on Win11.
	if (TraySave.FlushTime < 100 || TraySave.FlushTime > 5000)
		TraySave.FlushTime = 400;
	if (TraySave.iPos < 0 || TraySave.iPos > 2)
		TraySave.iPos = 1;
	if (TraySave.iMonitorSimple < 0 || TraySave.iMonitorSimple > 2)
		TraySave.iMonitorSimple = 0;
	if (TraySave.szDisk != L'\0' && (TraySave.szDisk < L'A' || TraySave.szDisk > L'Z') &&
		(TraySave.szDisk < L'a' || TraySave.szDisk > L'z'))
		TraySave.szDisk = L'\0';
	if (TraySave.szDisk >= L'a' && TraySave.szDisk <= L'z')
		TraySave.szDisk = (WCHAR)(TraySave.szDisk - L'a' + L'A');
}
void WriteReg()//写入设置
{
	TraySave.Ver = kTraySaveVersion;
	NormalizeTraySave();
	WCHAR savePath[32768] = {};
	HANDLE hFile = INVALID_HANDLE_VALUE;
	if (GetApplicationFilePath(szTraySave, savePath, ARRAYSIZE(savePath)))
		hFile = CreateFileW(savePath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile!= INVALID_HANDLE_VALUE)
	{
		DWORD dwBytes = 0;
		if (WriteFile(hFile, &TraySave, sizeof TraySave, &dwBytes, NULL) && dwBytes == sizeof TraySave)
			FlushFileBuffers(hFile);
		CloseHandle(hFile);
	}
}
void GetShellAllWnd()
{
	hTray = NULL;
	hReBarWnd = NULL;
	hStartWnd = NULL;
	hTrayNotifyWnd = NULL;
	hTaskWnd = NULL;
	hTaskListWnd = NULL;
	hWin11UI = NULL;
	hTrayClockWnd = NULL;
	for (int retry = 0; retry < 50 && !IsWindow(hTray); ++retry)
	{
		hTray = FindWindow(szShellTray, NULL);
		if (!IsWindow(hTray))
			Sleep(100);
	}
	if (!IsWindow(hTray))
		return;
	hReBarWnd = FindWindowEx(hTray, 0, L"ReBarWindow32", NULL);
	hStartWnd = FindWindowEx(hTray, 0, L"Start", NULL);
	hTrayNotifyWnd = FindWindowEx(hTray, 0, L"TrayNotifyWnd", NULL);
	if(hReBarWnd)
		hTaskWnd = FindWindowEx(hReBarWnd, NULL, L"MSTaskSwWClass", NULL);
	if(hTaskWnd)
		hTaskListWnd = FindWindowEx(hTaskWnd, NULL, L"MSTaskListWClass", NULL);
	if(hTaskWnd && !hTaskListWnd)
		hTaskListWnd = FindWindowEx(hTaskWnd, NULL, L"ToolbarWindow32", NULL);
	hWin11UI = FindWindowEx(hTray, 0, L"Windows.UI.Composition.DesktopWindowContentBridge", NULL);
	if(hTrayNotifyWnd)
		hTrayClockWnd = FindWindowEx(hTrayNotifyWnd, NULL, L"TrayClockWClass", NULL);
/*
	if (hWin11UI)
	{
		if (TraySave.cMonitorColor[0] == 0 && !TraySave.bMonitorFloat)
			TraySave.cMonitorColor[0] = RGB(1, 2, 3);
	}
*/
}
void CloseTaskBar()
{
	if (IsWindow(hTaskBar))
		DestroyWindow(hTaskBar);
	if (IsWindow(hTaskTips))
		DestroyWindow(hTaskTips);
	if (IsWindow(hPrice))
		DestroyWindow(hPrice);
	if(IsWindow(hTime))
		DestroyWindow(hTime);	
}
void OpenTimeDlg()
{
	if (!IsWindow(hTime) && TraySave.bSecond)
	{
		
		if (!hWin11UI)
		{
			hTime = ::CreateDialog(hInst, MAKEINTRESOURCE(IDD_TIME), NULL, (DLGPROC)TimeProc);
			if (hTime && hTrayClockWnd)
				SetParent(hTime, hTrayClockWnd);
		}
		else
		{
			hTime = ::CreateDialog(hInst, MAKEINTRESOURCE(IDD_TIME), NULL, (DLGPROC)TimeProc);
			if (!hTime)
				return;
			SetWindowLongPtr(hTime, GWL_EXSTYLE, GetWindowLongPtr(hTime, GWL_EXSTYLE) | WS_EX_LAYERED|WS_EX_TRANSPARENT);
/*
			if(bThemeMode&&rovi.dwBuildNumber>22000)
				SetLayeredWindowAttributes(hTime, RGB(254,254,255), 0, LWA_COLORKEY);
			else
*/
				SetLayeredWindowAttributes(hTime, RGB(0, 0, 1), 0, LWA_COLORKEY);
			if (hTray)
				SetParent(hTime, hTray);
		}
		if (hTime)
			ShowWindow(hTime, SW_SHOW);
	}
}

void OpenTaskBar()
{
	if (IsWindow(hTaskBar) == FALSE)
	{		
		if (TraySave.cMonitorColor[0] == RGB(1, 2, 3))
			TraySave.cMonitorColor[0] = RGB(0,0,1);
		hTaskBar = ::CreateDialog(hInst, MAKEINTRESOURCE(IDD_TASKBAR), NULL, (DLGPROC)TaskBarProc);
		if (hTaskBar)
		{			
			if (TraySave.bMonitorFloat||bFullScreen||(rovi.dwBuildNumber<=25000&&TraySave.bTrayStyle)||(rovi.dwMajorVersion==6&&rovi.dwMinorVersion==1))
			{
				if (TraySave.cMonitorColor[0] == RGB(0,0,1) || TraySave.cMonitorColor[0] == 0)
				{
					bShadow = TRUE;
					TraySave.bMonitorFuse = TRUE;
				}
				else
					bShadow = FALSE;
			}
			else
			{
				bShadow = FALSE;
				TraySave.bMonitorFuse = FALSE;
				if (TraySave.cMonitorColor[0] == 0)
					TraySave.cMonitorColor[0] = RGB(0, 0, 1);
				SetWindowLongPtr(hTaskBar, GWL_EXSTYLE, GetWindowLongPtr(hTaskBar, GWL_EXSTYLE) | WS_EX_LAYERED);
				SetLayeredWindowAttributes(hTaskBar, GetWindowPixel(hTray), 0, LWA_COLORKEY);

/*
					if (bThemeMode)
						SetLayeredWindowAttributes(hTaskBar, RGB(222, 222, 223), 128, LWA_COLORKEY | LWA_ALPHA);
					else
						SetLayeredWindowAttributes(hTaskBar, RGB(0, 0, 1), 128, LWA_COLORKEY | LWA_ALPHA);
*/				
//				else
//					SetParent(hTaskBar, GetForegroundWindow());
			}
			if(!TraySave.bMonitorFloat&&!bFullScreen)
				SetParent(hTaskBar, hTray);
			SetWH();
			if (!TraySave.bMonitorFuse && TraySave.bMonitorFloat)
			{
			}
			else
				SetWindowCompositionAttribute(hTaskBar, ACCENT_ENABLE_TRANSPARENT, 0x00111111);
			ShowWindow(hTaskBar, SW_SHOW);
			if (TraySave.bMonitorTransparent&&TraySave.bMonitorFloat)
				SetWindowLongPtr(hTaskBar, GWL_EXSTYLE, GetWindowLongPtr(hTaskBar, GWL_EXSTYLE) |WS_EX_LAYERED |WS_EX_TRANSPARENT);
			SetTimer(hTaskBar, 3, 1000, NULL);
			//			SetTimer(hTaskBar, 6, 100, NULL);
		}
	}
}
////////////////////////////////////////////获取CPU温度
static DWORD TemperatureForDisplay(float value)
{
	// LHM is an optional third-party boundary.  Reject NaN/inf and physically
	// implausible values before converting to the DWORD fields shared with the
	// UI; a float-to-integer conversion outside the representable range is
	// undefined behaviour.
	if (!std::isfinite(value) || value < 0.0f || value > 255.0f)
		return 0;
	return static_cast<DWORD>(value + 0.5f);
}

int GetCpuTemp(DWORD Core)
{
	UNREFERENCED_PARAMETER(Core);
	// Prefer the Windows/ACPI thermal-zone path. It remains available in the
	// default package and does not require LHM, WinRing0, PawnIO, MSR, or PCI
	// access. A missing ACPI zone is a normal unsupported-sensor condition.
	int acpiTemperature = GetAcpiCpuTemperature();
	if (hOHMA && GetTemperature)
	{
		float fCpu = -1.0f, fHdd = -1.0f, fGpu = -1.0f, fCpuPackge = -1.0f;
		int n = -1;
		if (TraySave.szDisk != L'\0')
		{
			n=GetPhysicalDriveFromPartitionLetter(TraySave.szDisk);
		}
		if (GetTemperature)
			GetTemperature(&fCpu,&fGpu,NULL,&fHdd,n,&fCpuPackge);
		if (TrayData)
		{
			TrayData->iHddTemperature = TemperatureForDisplay(fHdd);
			DWORD gpuTemperature = TemperatureForDisplay(fGpu);
			DWORD packageTemperature = TemperatureForDisplay(fCpuPackge);
			if (gpuTemperature != 0)
				TrayData->iTemperature2 = gpuTemperature;
			else if (packageTemperature != 0)
				TrayData->iTemperature2 = packageTemperature;
		}
		DWORD cpuTemperature = TemperatureForDisplay(fCpu);
		if (cpuTemperature != 0)
			return cpuTemperature <= INT_MAX ? (int)cpuTemperature : 0;
	}
	return acpiTemperature;
}
//////////////////////////////////////////////////载入温度DLL
static BOOL IsAmdProcessor()
{
	int cpuInfo[4] = {};
	__cpuid(cpuInfo, 0);
	char vendor[13] = {};
	CopyMemory(vendor, &cpuInfo[1], sizeof(DWORD));
	CopyMemory(vendor + sizeof(DWORD), &cpuInfo[3], sizeof(DWORD));
	CopyMemory(vendor + sizeof(DWORD) * 2, &cpuInfo[2], sizeof(DWORD));
	return lstrcmpA(vendor, "AuthenticAMD") == 0;
}

static BOOL IsEnvironmentFlagEnabled(LPCWSTR name)
{
	WCHAR value[8] = {};
	DWORD length = GetEnvironmentVariableW(name, value, ARRAYSIZE(value));
	return length > 0 && length < ARRAYSIZE(value) &&
		(value[0] == L'1' || value[0] == L'y' || value[0] == L'Y' || value[0] == L't' || value[0] == L'T');
}

static BOOL IsLhmOptIn()
{
	// LHM 0.9.4 embeds WinRing0.  The general flag is deliberately an
	// explicit opt-in for every CPU vendor.  Keep the older AMD-specific flag
	// as a compatibility alias for existing testers, but do not enable it on
	// Intel or other processors.
	if (IsEnvironmentFlagEnabled(L"TRAYS_ENABLE_LHM"))
		return TRUE;
	return IsAmdProcessor() && IsEnvironmentFlagEnabled(L"TRAYS_ENABLE_LHM_AMD");
}

static void FreeTemperatureDLLUnlocked();

void LoadTemperatureDLL()
{
	AcquireSRWLockExclusive(&g_temperatureLock);
	if (hOHMA || hNVDLL || hATIDLL || hThermalPDH || hThermalQuery)
	{
		FreeTemperatureDLLUnlocked();
	}
	// Do not fall back to TrayS's own WinRing0 path. Its legacy kernel driver is
	// blocked by current Windows security products and the direct AMD PCI/MSR
	// path has caused system hangs on Ryzen mobile and desktop platforms.
	// LibreHardwareMonitor 0.9.4 remains optional; it may still use its
	// embedded backend internally, so a future PawnIO migration is required to
	// remove every WinRing0 dependency. If the managed monitor is unavailable,
	// the Windows/ACPI path still supplies CPU/package temperature when exposed.
	bRing0 = FALSE;
	GetTemperature = NULL;
	bLhmDisabled = !IsLhmOptIn();
	// Initialise the safe ACPI path before deciding the UI layout. Failure is
	// harmless and means that this machine exposes no usable thermal zone.
	AcquireSRWLockExclusive(&g_thermalPdhLock);
	thermalProbeAttempted = FALSE;
	BOOL hasAcpiTemperaturePath = EnsureThermalQueryUnlocked();
	ReleaseSRWLockExclusive(&g_thermalPdhLock);
	// LHM 0.9.4 opens its embedded WinRing0 driver from Computer::Open().
	// Keep that path disabled by default on both AMD and Intel. GPU vendor APIs
	// below remain available in the safe default mode without installing a
	// kernel driver. A future PawnIO-based LHM upgrade can remove this opt-in.
	if (!bLhmDisabled)
		hOHMA = LoadApplicationLibrarySafe(L"OpenHardwareMonitorApi.dll");
	if (hOHMA)
	{
		GetTemperature = (pfnGetTemperature) GetProcAddress(hOHMA, "GetTemperature");
		if (!GetTemperature)
		{
			FreeLibrary(hOHMA);
			hOHMA = NULL;
		}
	}
	// bRing0 is retained as the legacy two-row temperature-layout flag. It now
	// means that either the safe ACPI path or the explicitly opted-in managed
	// path can provide a CPU/package sample; it never means a kernel driver was
	// loaded by TrayS.
	bRing0 = hasAcpiTemperaturePath || (hOHMA != NULL && GetTemperature != NULL);
#ifdef _WIN64
	hNVDLL = LoadSystemLibrarySafe(L"nvapi64.dll");
#else
	hNVDLL = LoadSystemLibrarySafe(L"nvapi.dll");
#endif
	if (hNVDLL)
	{
		NvAPI_QueryInterface = (NvAPI_QueryInterface_t)GetProcAddress(hNVDLL, "nvapi_QueryInterface");
		if (NvAPI_QueryInterface)
		{
			NvAPI_Initialize_t NvAPI_Initialize = (NvAPI_Initialize_t)NvAPI_QueryInterface(ID_NvAPI_Initialize);
			NvAPI_EnumPhysicalGPUs_t NvAPI_EnumPhysicalGPUs = (NvAPI_EnumPhysicalGPUs_t)NvAPI_QueryInterface(ID_NvAPI_EnumPhysicalGPUs);
			NvAPI_GPU_GetThermalSettings = (NvAPI_GPU_GetThermalSettings_t)NvAPI_QueryInterface(ID_NvAPI_GPU_GetThermalSettings);
			if (NvAPI_Initialize != NULL && NvAPI_EnumPhysicalGPUs != NULL && NvAPI_GPU_GetThermalSettings != NULL)
			{
				if (NvAPI_Initialize() == 0)
				{
					for (NvU32 PhysicalGpuIndex = 0; PhysicalGpuIndex < NVAPI_MAX_PHYSICAL_GPUS; PhysicalGpuIndex++)
					{
						hPhysicalGpu[PhysicalGpuIndex] = 0;
					}
					NvU32 physicalGpuCount = 0;
					if (NvAPI_EnumPhysicalGPUs(hPhysicalGpu, &physicalGpuCount) != 0)
						physicalGpuCount = 0;
					if (physicalGpuCount > static_cast<NvU32>(NVAPI_MAX_PHYSICAL_GPUS))
						physicalGpuCount = static_cast<NvU32>(NVAPI_MAX_PHYSICAL_GPUS);
				}
				else
				{
					FreeLibrary(hNVDLL);
					hNVDLL = NULL;
				}
			}
			else
			{
				FreeLibrary(hNVDLL);
				hNVDLL = NULL;
			}
		}
		else
		{
			FreeLibrary(hNVDLL);
			hNVDLL = NULL;
		}
	}
#ifdef _WIN64
	hATIDLL = LoadSystemLibrarySafe(L"atiadlxx.dll");
#else
	hATIDLL = LoadSystemLibrarySafe(L"atiadlxy.dll");
#endif
	if (hATIDLL)
	{
		ADL_Main_Control_Create = (ADL_MAIN_CONTROL_CREATE)GetProcAddress(hATIDLL, "ADL_Main_Control_Create");
		ADL_Main_Control_Destroy = (ADL_MAIN_CONTROL_DESTROY)GetProcAddress(hATIDLL, "ADL_Main_Control_Destroy");
		ADL_Adapter_NumberOfAdapters_Get = (ADL_ADAPTER_NUMBER_OF_ADAPTERS_GET)GetProcAddress(hATIDLL, "ADL_Adapter_NumberOfAdapters_Get");
		ADL_Adapter_Active_Get = (ADL_ADAPTER_ACTIVE_GET)GetProcAddress(hATIDLL, "ADL_Adapter_Active_Get");
		ADL_Overdrive5_Temperature_Get = (ADL_OVERDRIVE5_TEMPERATURE_GET)GetProcAddress(hATIDLL, "ADL_Overdrive5_Temperature_Get");
		if (NULL != ADL_Main_Control_Create && NULL != ADL_Main_Control_Destroy)
		{
			if (ADL_OK != ADL_Main_Control_Create(ADL_Main_Memory_Alloc, 1))
			{
				FreeLibrary(hATIDLL);
				hATIDLL = NULL;
			}
		}
		else
		{
			FreeLibrary(hATIDLL);
			hATIDLL = NULL;
		}
	}
	if (!hOHMA && !hNVDLL && !hATIDLL && !hasAcpiTemperaturePath)
		bRing0 = FALSE;
	ReleaseSRWLockExclusive(&g_temperatureLock);
}
///////////////////////////////////释放温度DLL
static void FreeTemperatureDLLUnlocked()
{
	AcquireSRWLockExclusive(&g_thermalPdhLock);
	CloseThermalQueryUnlocked();
	ReleaseSRWLockExclusive(&g_thermalPdhLock);
	if (hATIDLL)
	{
		if (ADL_Main_Control_Destroy)
			ADL_Main_Control_Destroy();
		FreeLibrary(hATIDLL);
		hATIDLL = NULL;
	}
	ADL_Main_Control_Create = NULL;
	ADL_Main_Control_Destroy = NULL;
	ADL_Adapter_NumberOfAdapters_Get = NULL;
	ADL_Adapter_Active_Get = NULL;
	ADL_Overdrive5_Temperature_Get = NULL;
	if (hNVDLL)
	{
		FreeLibrary(hNVDLL);
		hNVDLL = NULL;
	}
	NvAPI_QueryInterface = NULL;
	NvAPI_GPU_GetThermalSettings = NULL;
	for (DWORD i = 0; i < NVAPI_MAX_PHYSICAL_GPUS; ++i)
		hPhysicalGpu[i] = NULL;
	if (hOHMA)
	{
		FreeLibrary(hOHMA);
		hOHMA = NULL;
	}
	bRing0 = FALSE;
	GetTemperature = NULL;
}
void FreeTemperatureDLL()
{
	AcquireSRWLockExclusive(&g_temperatureLock);
	FreeTemperatureDLLUnlocked();
	ReleaseSRWLockExclusive(&g_temperatureLock);
}
///////////////////////////////////////////////打开读取设置
void OpenSetting()
{
	if (IsWindow(hSetting))
	{
		SetForegroundWindow(hSetting);
		return;
	}
	hSetting = ::CreateDialog(hInst, MAKEINTRESOURCE(IDD_SETTING), NULL, (DLGPROC)SettingProc);
	if (!hSetting)
	{
		return;
	}
	SendMessage(hSetting, WM_SETICON, ICON_BIG, (LPARAM)(HICON)iMain);
	SendMessage(hSetting, WM_SETICON, ICON_SMALL, (LPARAM)(HICON)iMain);
	CheckRadioButton(hSetting, IDC_RADIO_NORMAL, IDC_RADIO_MAXIMIZE, IDC_RADIO_NORMAL);
	iProject = iWindowMode;
	if (iProject == 0)
		CheckRadioButton(hSetting, IDC_RADIO_NORMAL, IDC_RADIO_MAXIMIZE, IDC_RADIO_NORMAL);
	else
		CheckRadioButton(hSetting, IDC_RADIO_NORMAL, IDC_RADIO_MAXIMIZE, IDC_RADIO_MAXIMIZE);
	if (TraySave.aMode[iProject] == ACCENT_DISABLED)
		CheckRadioButton(hSetting, IDC_RADIO_DEFAULT, IDC_RADIO_ACRYLIC, IDC_RADIO_DEFAULT);
	else if (TraySave.aMode[iProject] == ACCENT_ENABLE_TRANSPARENTGRADIENT)
		CheckRadioButton(hSetting, IDC_RADIO_DEFAULT, IDC_RADIO_ACRYLIC, IDC_RADIO_TRANSPARENT);
	else if (TraySave.aMode[iProject] == ACCENT_ENABLE_BLURBEHIND)
		CheckRadioButton(hSetting, IDC_RADIO_DEFAULT, IDC_RADIO_ACRYLIC, IDC_RADIO_BLURBEHIND);
	else if (TraySave.aMode[iProject] == ACCENT_ENABLE_ACRYLICBLURBEHIND)
		CheckRadioButton(hSetting, IDC_RADIO_DEFAULT, IDC_RADIO_ACRYLIC, IDC_RADIO_ACRYLIC);
	if (TraySave.iPos == 0)
		CheckRadioButton(hSetting, IDC_RADIO_LEFT, IDC_RADIO_RIGHT, IDC_RADIO_LEFT);
	else if (TraySave.iPos == 1)
		CheckRadioButton(hSetting, IDC_RADIO_LEFT, IDC_RADIO_RIGHT, IDC_RADIO_CENTER);
	else if (TraySave.iPos == 2)
		CheckRadioButton(hSetting, IDC_RADIO_LEFT, IDC_RADIO_RIGHT, IDC_RADIO_RIGHT);
	if (hWin11UI)
	{
		EnableWindow(GetDlgItem(hSetting, IDC_RADIO_LEFT), FALSE);
		EnableWindow(GetDlgItem(hSetting, IDC_RADIO_CENTER), FALSE);
		EnableWindow(GetDlgItem(hSetting, IDC_RADIO_RIGHT), FALSE);
//		EnableWindow(GetDlgItem(hSetting, IDC_CHECK_TOPMOST), TRUE);
	}
	if (LOWORD(TraySave.iUnit) == 0)
		CheckRadioButton(hSetting, IDC_RADIO_AUTO, IDC_RADIO_MB, IDC_RADIO_AUTO);
	else if (LOWORD(TraySave.iUnit) == 1)
		CheckRadioButton(hSetting, IDC_RADIO_AUTO, IDC_RADIO_MB, IDC_RADIO_KB);
	else if (LOWORD(TraySave.iUnit) == 2)
		CheckRadioButton(hSetting, IDC_RADIO_AUTO, IDC_RADIO_MB, IDC_RADIO_MB);
	if (HIWORD(TraySave.iUnit) == 0)
		CheckRadioButton(hSetting, IDC_RADIO_BYTE, IDC_RADIO_BIT, IDC_RADIO_BYTE);
	else
		CheckRadioButton(hSetting, IDC_RADIO_BYTE, IDC_RADIO_BIT, IDC_RADIO_BIT);
	CheckDlgButton(hSetting, IDC_CHECK_TRAYICON, TraySave.bTrayIcon);
	CheckDlgButton(hSetting, IDC_CHECK_MONITOR, TraySave.bMonitor);
	CheckDlgButton(hSetting, IDC_CHECK_TRAY_STYLE, TraySave.bTrayStyle);
	CheckDlgButton(hSetting, IDC_CHECK_TRAFFIC, TraySave.bMonitorTraffic);
	CheckDlgButton(hSetting, IDC_CHECK_MONITOR_UPDOWN, TraySave.bMonitorTrafficUpDown);
	CheckDlgButton(hSetting, IDC_CHECK_TEMPERATURE, TraySave.bMonitorTemperature);
	CheckDlgButton(hSetting, IDC_CHECK_USAGE, TraySave.bMonitorUsage);
	CheckDlgButton(hSetting, IDC_CHECK_DISK, TraySave.bMonitorDisk);
	CheckDlgButton(hSetting, IDC_CHECK_PRICE, TraySave.bMonitorPrice);
	CheckDlgButton(hSetting, IDC_CHECK_SOUND, TraySave.bSound);
	CheckDlgButton(hSetting, IDC_CHECK_MONITOR_PDH, TraySave.bMonitorPDH);
	CheckDlgButton(hSetting, IDC_CHECK_MONITOR_SIMPLE, TraySave.iMonitorSimple);
	CheckDlgButton(hSetting, IDC_CHECK_MONITOR_LEFT, TraySave.bMonitorLeft);
	CheckDlgButton(hSetting, IDC_CHECK_MONITOR_NEAR, TraySave.bNear);
	CheckDlgButton(hSetting, IDC_CHECK_MONITOR_FLOAT, TraySave.bMonitorFloat);
	CheckDlgButton(hSetting, IDC_CHECK_MONITOR_FLOAT_VROW, TraySave.bMonitorFloatVRow);
	CheckDlgButton(hSetting, IDC_CHECK_MONITOR_TIME, TraySave.bMonitorTime);
	CheckDlgButton(hSetting, IDC_CHECK_TIME, TraySave.bSecond);
	CheckDlgButton(hSetting, IDC_CHECK_TRANSPARENT, TraySave.bMonitorTransparent);
	CheckDlgButton(hSetting, IDC_CHECK_TIPS, TraySave.bMonitorTips);
	CheckDlgButton(hSetting, IDC_CHECK_FUSE, TraySave.bMonitorFuse);
	CheckDlgButton(hSetting, IDC_CHECK_TOPMOST, TraySave.bMonitorTopmost);
	SendDlgItemMessage(hSetting, IDC_SLIDER_ALPHA, TBM_SETRANGE, 0, MAKELPARAM(0, 255));
	SendDlgItemMessage(hSetting, IDC_SLIDER_ALPHA, TBM_SETPOS, TRUE, TraySave.bAlpha[iProject]);
	SendDlgItemMessage(hSetting, IDC_SLIDER_ALPHA_B, TBM_SETRANGE, 0, MAKELPARAM(0, 255));
	BYTE bAlphaB = TraySave.dAlphaColor[iProject] >> 24;
	SendDlgItemMessage(hSetting, IDC_SLIDER_ALPHA_B, TBM_SETPOS, TRUE, bAlphaB);
	SendDlgItemMessage(hSetting, IDC_CHECK_AUTORUN, BM_SETCHECK,
		IsUserAutoRunEnabledSafe(szAppName), NULL);
	bSettingInit = TRUE;
	SetDlgItemInt(hSetting, IDC_EDIT1, TraySave.dNumValues[0] / 1048576, 0);
	SetDlgItemInt(hSetting, IDC_EDIT2, TraySave.dNumValues[1] / 1048576, 0);
	SetDlgItemInt(hSetting, IDC_EDIT3, TraySave.dNumValues[2], 0);
	SetDlgItemInt(hSetting, IDC_EDIT4, TraySave.dNumValues[3], 0);
	SetDlgItemInt(hSetting, IDC_EDIT5, TraySave.dNumValues[4], 0);
	SetDlgItemInt(hSetting, IDC_EDIT6, TraySave.dNumValues[5], 0);
	SetDlgItemInt(hSetting, IDC_EDIT7, TraySave.dNumValues[6], 0);
	SetDlgItemInt(hSetting, IDC_EDIT8, TraySave.dNumValues[7], 0);
	SetDlgItemInt(hSetting, IDC_EDIT9, TraySave.dNumValues[8] / 1048576, 0);
	SetDlgItemInt(hSetting, IDC_EDIT10, TraySave.dNumValues[9], 0);
	SetDlgItemInt(hSetting, IDC_EDIT11, TraySave.dNumValues[10], 0);
	SetDlgItemInt(hSetting, IDC_EDIT12, TraySave.dNumValues[11], 0);
	SetDlgItemInt(hSetting, IDC_EDIT24, TraySave.dNumValues2[0], 0);
	SetDlgItemInt(hSetting, IDC_EDIT25, TraySave.dNumValues2[1], 0);
	SetDlgItemInt(hSetting, IDC_EDIT26, TraySave.dNumValues2[2], 0);
	SetDlgItemInt(hSetting, IDC_EDIT_TIME, TraySave.FlushTime, 0);
	SetDlgItemText(hSetting, IDC_EDIT14, TraySave.szTrafficOut);
	SetDlgItemText(hSetting, IDC_EDIT15, TraySave.szTrafficIn);
	SetDlgItemText(hSetting, IDC_EDIT16, TraySave.szTemperatureCPU);
	SetDlgItemText(hSetting, IDC_EDIT17, TraySave.szTemperatureGPU);
	SetDlgItemText(hSetting, IDC_EDIT18, TraySave.szTemperatureCPUUnit);
	SetDlgItemText(hSetting, IDC_EDIT19, TraySave.szTemperatureGPUUnit);
	SetDlgItemText(hSetting, IDC_EDIT20, TraySave.szUsageCPU);
	SetDlgItemText(hSetting, IDC_EDIT21, TraySave.szUsageMEM);
	SetDlgItemText(hSetting, IDC_EDIT22, TraySave.szUsageCPUUnit);
	SetDlgItemText(hSetting, IDC_EDIT23, TraySave.szUsageMEMUnit);
	SetDlgItemText(hSetting, IDC_EDIT27, TraySave.szDiskReadSec);
	SetDlgItemText(hSetting, IDC_EDIT28, TraySave.szDiskWriteSec);
	SetDlgItemText(hSetting, IDC_EDIT29, TraySave.szDiskName);
	bSettingInit = FALSE;
	oldColorButtonPoroc = (WNDPROC)SetWindowLongPtr(GetDlgItem(hSetting, IDC_BUTTON_COLOR), GWLP_WNDPROC, (LONG_PTR)ColorButtonProc);
	oldColorButtonPoroc = (WNDPROC)SetWindowLongPtr(GetDlgItem(hSetting, IDC_BUTTON_COLOR_BACKGROUND), GWLP_WNDPROC, (LONG_PTR)ColorButtonProc);
	oldColorButtonPoroc = (WNDPROC)SetWindowLongPtr(GetDlgItem(hSetting, IDC_BUTTON_COLOR_TRAFFIC_LOW), GWLP_WNDPROC, (LONG_PTR)ColorButtonProc);
	oldColorButtonPoroc = (WNDPROC)SetWindowLongPtr(GetDlgItem(hSetting, IDC_BUTTON_COLOR_TRAFFIC_MEDIUM), GWLP_WNDPROC, (LONG_PTR)ColorButtonProc);
	oldColorButtonPoroc = (WNDPROC)SetWindowLongPtr(GetDlgItem(hSetting, IDC_BUTTON_COLOR_TRAFFIC_HIGH), GWLP_WNDPROC, (LONG_PTR)ColorButtonProc);
	oldColorButtonPoroc = (WNDPROC)SetWindowLongPtr(GetDlgItem(hSetting, IDC_BUTTON_COLOR_LOW), GWLP_WNDPROC, (LONG_PTR)ColorButtonProc);
	oldColorButtonPoroc = (WNDPROC)SetWindowLongPtr(GetDlgItem(hSetting, IDC_BUTTON_COLOR_MEDUIM), GWLP_WNDPROC, (LONG_PTR)ColorButtonProc);
	oldColorButtonPoroc = (WNDPROC)SetWindowLongPtr(GetDlgItem(hSetting, IDC_BUTTON_COLOR_HIGH), GWLP_WNDPROC, (LONG_PTR)ColorButtonProc);	
	oldColorButtonPoroc = (WNDPROC)SetWindowLongPtr(GetDlgItem(hSetting, IDC_BUTTON_COLOR_PRICE_LOW), GWLP_WNDPROC, (LONG_PTR)ColorButtonProc);
	oldColorButtonPoroc = (WNDPROC)SetWindowLongPtr(GetDlgItem(hSetting, IDC_BUTTON_COLOR_PRICE_HIGH), GWLP_WNDPROC, (LONG_PTR)ColorButtonProc);
	ShowWindow(hSetting, SW_SHOW);
	UpdateWindow(hSetting);
	SetForegroundWindow(hSetting);
}

// TrayS now targets the Windows 10+ taskbar and composition APIs.  Refuse
// older systems before creating mappings, touching Explorer, or opening any
// hardware/temperature interface.  RtlGetVersion is used instead of the
// manifest-sensitive GetVersionEx family so a compatibility manifest cannot
// make an old system look newer than it is.
static BOOL InitializeSupportedWindowsVersion()
{
	ZeroMemory(&rovi, sizeof(rovi));
	rovi.dwOSVersionInfoSize = sizeof(rovi);
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	typedef LONG(WINAPI* pfnRtlGetVersion)(PRTL_OSVERSIONINFOW);
	pfnRtlGetVersion rtlGetVersion = ntdll == NULL ? NULL :
		(pfnRtlGetVersion)GetProcAddress(ntdll, "RtlGetVersion");
	if (rtlGetVersion == NULL || rtlGetVersion(&rovi) != 0)
	{
		// This API is present on supported Windows releases.  If a future
		// compatible host hides it, fail open without inventing an old version;
		// the rest of TrayS will still use its checked, read-only fallbacks.
		return TRUE;
	}
	return rovi.dwMajorVersion >= 10;
}

#ifndef _DEBUG
extern "C" void WinMainCRTStartup()
{
	ConfigureSafeDllSearch();
	LPWSTR lpCmdLine;
#else
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);
	ConfigureSafeDllSearch();

/*
	if (lpCmdLine[0] == L'c')////打开控制面板
	{
		(void)pShellExecute(NULL, L"open", L"control.exe", &lpCmdLine[1], NULL, SW_SHOW);
		return 0;
	}
	else if (lpCmdLine[0] == L'o')//用SHELLEXECUTE打开
	{
		(void)pShellExecute(NULL, L"open", &lpCmdLine[1], NULL, NULL, SW_SHOW);
		return 0;
	}
	else if (lpCmdLine[0] == L's')//打开任务计划
	{
		(void)pShellExecute(NULL, L"open", L"schtasks", &lpCmdLine[1], NULL, SW_HIDE);
		return 0;
	}
	if (IsUserAdmin())
	{
		LPWSTR lpServiceName = (LPWSTR)szAppName;
		InitService();
		SERVICE_TABLE_ENTRY st[] =
		{
			{ (LPWSTR)szAppName, (LPSERVICE_MAIN_FUNCTION)ServiceMain},
			{ NULL, NULL }
		};
		if (lstrcmpi(lpCmdLine, L"/install") == 0)
		{
			InstallService();
			return 0;
		}
		else if (lstrcmpi(lpCmdLine, L"/uninstall") == 0)
		{
			UninstallService();
			return 0;
		}
		else if (lstrcmpi(lpCmdLine, L"/start") == 0)
		{
			ServiceCtrlStart();
			return 0;
		}
		else if (lstrcmpi(lpCmdLine, L"/stop") == 0)
		{
			ServiceCtrlStop();
			return 0;
		}
		if (ServiceRunState() != SERVICE_RUNNING)
		{
			if (IsServiceInstalled())
			{
				if (ServiceRunState() == SERVICE_STOPPED)
					ServiceCtrlStart();
				StartServiceCtrlDispatcher(st);
				return 0;
			}
		}
		ServiceCtrlStop();
	}
*/
#endif
	if (!InitializeSupportedWindowsVersion())
	{
		MessageBoxW(NULL,
			L"此维护版 TrayS 仅支持 Windows 10 及更高版本。",
			L"TrayS",
			MB_OK | MB_ICONINFORMATION);
		ExitProcess(0);
	}
#ifdef NDEBUG
	HANDLE existingMap = OpenFileMapping(FILE_MAP_ALL_ACCESS, FALSE, szAppName);
	if (existingMap == NULL)/////////////////////////创建守护进程
	{
			HANDLE hMap = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(TRAYDATA), szAppName);
			if (hMap)
			{
				TrayData = (TRAYDATA*)MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(TRAYDATA));
				if (!TrayData)
				{
					CloseHandle(hMap);
					ExitProcess(ERROR_NOT_ENOUGH_MEMORY);
				}
				ZeroMemory(TrayData, sizeof(TRAYDATA));
				int iReset = 6;
				while (TrayData->bExit == FALSE && iReset != 0)
				{
					HANDLE hProcess = NULL;
					if (!RunProcess(0, 0, &hProcess) || !hProcess)
						break;
					TrimOwnWorkingSetSafe();
					WaitForSingleObject(hProcess, INFINITE);
					CloseHandle(hProcess);
					iReset--;
				}
				UnmapViewOfFile(TrayData);
				CloseHandle(hMap);
				ExitProcess(0);
				return;
			}
	}
	else
	{
		CloseHandle(existingMap);
	}
#endif
	hMap = OpenFileMapping(FILE_MAP_ALL_ACCESS, FALSE, szAppName);
	if (hMap)
	{
		TrayData = (TRAYDATA*)MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(TRAYDATA));						
	}
#ifdef NDEBUG
#else
	else
	{
		hMap = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(TRAYDATA), szAppName);
		if (hMap)
			TrayData = (TRAYDATA*)MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(TRAYDATA));
		if (!hMap || !TrayData)
		{
			if (TrayData)
				UnmapViewOfFile(TrayData);
			if (hMap)
				CloseHandle(hMap);
			ExitProcess(ERROR_NOT_ENOUGH_MEMORY);
		}
		ZeroMemory(TrayData, sizeof(TRAYDATA));
	}
#endif // !DAEMON
	if (!hMap || !TrayData)
		ExitProcess(ERROR_NOT_ENOUGH_MEMORY);
	pChangeWindowMessageFilter(WM_TRAYS, MSGFLT_ADD);
	pChangeWindowMessageFilter(WM_DROPFILES, MSGFLT_ADD);
	pChangeWindowMessageFilter(0x0049, MSGFLT_ADD);
	lpCmdLine = GetCommandLine();
	LPWSTR lpParameters = NULL;
	int iLen = lstrlen(lpCmdLine);
	int flag = 0;
	for (int i = 0; i < iLen; i++)
	{
		if (lpCmdLine[i] == L'\"')
		{
			++flag;
		}
		else if (flag == 2)
		{
			lpCmdLine = &lpCmdLine[i + 1];
			break;
		}
		else if (lpCmdLine[i] == L' ' && flag == 0)
		{
			lpCmdLine = &lpCmdLine[i + 1];
			if (lpCmdLine[0] == L'o')
			{
				for (int n = 1; n < iLen - i; n++)
				{
					if (lpCmdLine[n] == L' ')
					{
						lpCmdLine[n] = 0;
						lpParameters = &lpCmdLine[n + 1];
						break;
					}
				}
			}
			break;
		}
	}
	if (lpCmdLine[0] == L'c')////打开控制面板
	{
		(void)pShellExecute(NULL, L"open", L"control.exe", &lpCmdLine[1], NULL, SW_SHOW);
		ExitProcess(0);
	}
	else if (lpCmdLine[0] == L'o')//用SHELLEXECUTE打开
	{
		(void)pShellExecute(NULL, L"open", &lpCmdLine[1], lpParameters, NULL, SW_SHOW);
		ExitProcess(0);
	}
	// The old `s` command forwarded arbitrary arguments to schtasks.exe.  It
	// could create or delete tasks even when the tray UI was merely launched
	// with an unexpected command line.  Task scheduling is intentionally not a
	// supported operation in the default build.
	// The historical build could turn a SYSTEM-launched instance into a
	// service and then create/modify the TrayS service from the command line.
	// Keep that code available only for an explicitly maintained legacy build;
	// the default desktop build must never start, install, stop, or delete a
	// service as a side effect of launching the tray UI.
#if defined(TRAYS_ENABLE_LEGACY_SERVICE) && TRAYS_ENABLE_LEGACY_SERVICE
	if (IsUserAdmin()==3)//////////////////////////////////////////////////以SYSYTEM权限启动
	{
//		lpServiceName = (LPWSTR)szAppName;
		InitService();
		SERVICE_TABLE_ENTRY st[] =
		{
			{ (LPWSTR)szAppName, (LPSERVICE_MAIN_FUNCTION)ServiceMain},
			{ NULL, NULL }
		};
		if (lstrcmpi(lpCmdLine, L"/install") == 0)
		{
			InstallService();
			ExitProcess(0);
		}
		else if (lstrcmpi(lpCmdLine, L"/uninstall") == 0)
		{
			UninstallService();
			ExitProcess(0);
		}
		else if (lstrcmpi(lpCmdLine, L"/start") == 0)
		{
			ServiceCtrlStart();
			ExitProcess(0);
		}
		else if (lstrcmpi(lpCmdLine, L"/stop") == 0)
		{
			ServiceCtrlStop();
			ExitProcess(0);
		}
		if (ServiceRunState() != SERVICE_RUNNING)
		{
			if (IsServiceInstalled())
			{
				if (ServiceRunState() == SERVICE_STOPPED)
					ServiceCtrlStart();
				StartServiceCtrlDispatcher(st);
				ExitProcess(0);
			}
		}
		ServiceCtrlStop();
	}
#endif
	GetShellAllWnd();
	hInst = GetModuleHandle(NULL); // 将实例句柄存储在全局变量中
	ReadReg();
	if(!TraySave.bMonitorTips||!TraySave.bMonitor||TraySave.bMonitorTransparent)
		EnumWindows((WNDENUMPROC)FindSettingWindowFunc, 0);
	hMutex = CreateMutex(NULL, TRUE, L"_TrayS_");
	if (hMutex != NULL)
	{
		if (ERROR_ALREADY_EXISTS != GetLastError())
		{
			iMain = LoadIcon(hInst, MAKEINTRESOURCE(IDI_TRAYS));
			hDwmapi = LoadSystemLibrarySafe(L"dwmapi.dll");
		if (hDwmapi)
		{
			pDwmGetWindowAttribute = (pfnDwmGetWindowAttribute)GetProcAddress(hDwmapi, "DwmGetWindowAttribute");
		}
			// Keep the monitor at normal priority. Raising process priority can
			// starve Explorer or driver control threads during a transient stall.
			SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
			if (TraySave.bMonitorTemperature)
				LoadTemperatureDLL();
			pProcessTime = NULL;
			// TrayS only trims its own working set.  SeDebugPrivilege is not
			// required for that operation and unnecessarily broadens the process
			// token (which also makes security software flag the monitor).
			SYSTEM_INFO si;
			GetSystemInfo(&si);
			dNumProcessor = si.dwNumberOfProcessors;
			if (dNumProcessor == 0)
				dNumProcessor = 1;
			ppmu[0] = &pmu[0];
			ppmu[1] = &pmu[1];
			ppmu[2] = &pmu[2];
			ppmu[3] = &pmu[3];
			ppmu[4] = &pmu[4];
			ppmu[5] = &pmu[5];
			ppcu[0] = &pcu[0];
			ppcu[1] = &pcu[1];
			ppcu[2] = &pcu[2];
			ppcu[3] = &pcu[3];
			ppcu[4] = &pcu[4];
			ppcu[5] = &pcu[5];
//			g_hHeapWindowInfo = HeapCreate(NULL, 0, 0);			
// 
			// 执行应用程序初始化:
			if (!InitInstance(hInst, 0))
			{
#ifndef _DEBUG
				ExitProcess(0);
#else
				return 0;
#endif
			}
			MSG msg;
			// 主消息循环:
			while (GetMessage(&msg, nullptr, 0, 0))
			{
				if (!IsDialogMessage(hMain, &msg) && !IsDialogMessage(hSetting, &msg))
				{
					TranslateMessage(&msg);
					DispatchMessage(&msg);
				}
			}
//			WaitForSingleObject(hThread, INFINITE);
// Ask workers to leave their loops before releasing shared resources.
			// A third-party API call can take longer than the normal shutdown
			// window.  Never use TerminateThread here: killing a thread while it
			// owns an SRW lock or is inside a loaded DLL leaves the process unable
			// to clean up and can make the UI appear hung forever.
			bRealClose = TRUE;
			BOOL workersStopped = TRUE;
			if (hGetDataThread)
			{
				DWORD waitResult = WaitForSingleObject(hGetDataThread, 3000);
				if (waitResult == WAIT_OBJECT_0)
				{
					CloseHandle(hGetDataThread);
					hGetDataThread = NULL;
				}
				else
					workersStopped = FALSE;
			}
			if (hPriceThread)
			{
				DWORD waitResult = WaitForSingleObject(hPriceThread, 3000);
				if (waitResult == WAIT_OBJECT_0)
				{
					CloseHandle(hPriceThread);
					hPriceThread = NULL;
				}
				else
					workersStopped = FALSE;
			}
			if (!workersStopped)
			{
				// Do not unload PDH, iphlpapi, the temperature DLL, or the mapped
				// data while a worker may still be using them.  Process termination
				// is the bounded last-resort fallback and cannot leave a lock behind
				// for this process to wait on.
				ExitProcess(ERROR_TIMEOUT);
			}
			AcquireSRWLockExclusive(&g_processLock);
			ClearProcessSnapshotUnlocked();
			ReleaseSRWLockExclusive(&g_processLock);
//			CloseHandle(hMainThread);
			if (IsWindow(hSetting))
				DestroyWindow(hSetting);
			CloseTaskBar();
			if (IsWindow(hMain))
				DestroyWindow(hMain);
			pShell_NotifyIcon(NIM_DELETE, &nid);
			DestroyIcon(iMain);
			DeleteObject(hFont);
			//free(ipinfo);
			if (hDwmapi)
				FreeLibrary(hDwmapi);
			if (hOleacc)
				FreeLibrary(hOleacc);
			SwitchPDH(FALSE);
			// The data thread owns the network snapshots while it is running.
			// Stop it above, then take the same lock used by the UI before
			// releasing the heap-backed adapter data and iphlpapi module.
			AcquireSRWLockExclusive(&g_networkLock);
			ClearNetworkSnapshotUnlocked();
			if (hIphlpapi)
			{
				FreeLibrary(hIphlpapi);
				hIphlpapi = NULL;
			}
			GetAdaptersAddressesT = NULL;
			GetIfTableT = NULL;
			getIfTable2 = NULL;
			freeMibTable = NULL;
			ReleaseSRWLockExclusive(&g_networkLock);
//			HeapDestroy(g_hHeapWindowInfo);
			if (hMutex)
				CloseHandle(hMutex);
			FreeTemperatureDLL();
			UnmapViewOfFile(TrayData);
			CloseHandle(hMap);
			hMap = NULL;
			ReleaseDC(NULL, hDesktopDC);
		}
	}
	if (hMap)
		CloseHandle(hMap);
	ExitProcess((UINT)0);
}
DWORD WINAPI GetPriceThreadProc(PVOID pParam)//获取行情线程
{
	while (!bRealClose)
	{
		if (!TrayData)
			break;
		DWORD dStart = GetTickCount();
		if (TraySave.bMonitorPrice)
		{
			if (TraySave.iPriceInterface[0] == 0)
				GetOKXPrice(TraySave.szPriceName1, TraySave.szOKXWeb, &TrayData->fLastPrice1, &TrayData->fOpenPrice1, TrayData->szLastPrice1, NULL);
			else
				GetSinaPrice(TraySave.szPriceName1, &TrayData->fLastPrice1, &TrayData->fOpenPrice1, TrayData->szLastPrice1, NULL);
			TrayData->szLastPrice1[7] = L'\0';
			if (TraySave.iPriceInterface[1] == 0)
				GetOKXPrice(TraySave.szPriceName2, TraySave.szOKXWeb, &TrayData->fLastPrice2, &TrayData->fOpenPrice2, TrayData->szLastPrice2, NULL);
			else
				GetSinaPrice(TraySave.szPriceName2, &TrayData->fLastPrice2, &TrayData->fOpenPrice2, TrayData->szLastPrice2, NULL);
			TrayData->szLastPrice2[7] = L'\0';
			if (TraySave.bTwoFour)
			{
				if (TraySave.iPriceInterface[2] == 0)
					GetOKXPrice(TraySave.szPriceName3, TraySave.szOKXWeb, &TrayData->fLastPrice3, &TrayData->fOpenPrice3, TrayData->szLastPrice3, NULL);
				else
					GetSinaPrice(TraySave.szPriceName3, &TrayData->fLastPrice3, &TrayData->fOpenPrice3, TrayData->szLastPrice3, NULL);
				TrayData->szLastPrice3[7] = L'\0';
				if (TraySave.iPriceInterface[3] == 0)
					GetOKXPrice(TraySave.szPriceName4, TraySave.szOKXWeb, &TrayData->fLastPrice4, &TrayData->fOpenPrice4, TrayData->szLastPrice4, NULL);
				else
					GetSinaPrice(TraySave.szPriceName4, &TrayData->fLastPrice4, &TrayData->fOpenPrice4, TrayData->szLastPrice4, NULL);
				TrayData->szLastPrice4[7] = L'\0';
			}
		}
		DWORD dTime = GetTickCount() - dStart;
		if (dTime < 888)
			Sleep(888 - dTime);
	}
	return 0;
}
DWORD dwIPSize = 0;
DWORD dwMISize = 0;
int iGetAddressTime = 10;//10秒一次获取网卡信息
static void ClearProcessSnapshotUnlocked()
{
	if (pProcessTime)
		HeapFree(GetProcessHeap(), 0, pProcessTime);
	pProcessTime = NULL;
	pProcessTimeCapacity = 0;
	nProcess = 0;
	ZeroMemory(pmu, sizeof(pmu));
	ZeroMemory(pcu, sizeof(pcu));
}

static BOOL EnsureProcessTimeBufferUnlocked(int processCount)
{
	if (processCount < 0)
		processCount = 0;
	if (processCount > (int)kMaxProcessTimeEntries - 32)
		processCount = (int)kMaxProcessTimeEntries - 32;
	SIZE_T requested = (SIZE_T)processCount + 32;
	if (requested < 32)
		requested = 32;
	if (requested > kMaxProcessTimeEntries)
		requested = kMaxProcessTimeEntries;
	if (pProcessTime && pProcessTimeCapacity >= requested)
		return TRUE;
	PROCESSTIME* newBuffer = (PROCESSTIME*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
		requested * sizeof(PROCESSTIME));
	if (!newBuffer)
		return FALSE;
	if (pProcessTime)
	{
		SIZE_T copyCount = pProcessTimeCapacity < requested ? pProcessTimeCapacity : requested;
		if (copyCount != 0)
			CopyMemory(newBuffer, pProcessTime, copyCount * sizeof(PROCESSTIME));
		HeapFree(GetProcessHeap(), 0, pProcessTime);
	}
	pProcessTime = newBuffer;
	pProcessTimeCapacity = requested;
	return TRUE;
}

static void ClearNetworkSnapshotUnlocked()
{
	// GetIfTable2 allocates its result with the provider's allocator.  Release
	// it through the matching FreeMibTable entry while iphlpapi is still loaded;
	// merely dropping the pointer leaks the table and can leave a stale pointer
	// for a subsequent refresh.
	if (mit2 && freeMibTable)
		freeMibTable(mit2);
	mit2 = NULL;
	HeapFree(GetProcessHeap(), 0, piaa);
	piaa = NULL;
	HeapFree(GetProcessHeap(), 0, traffic);
	traffic = NULL;
	HeapFree(GetProcessHeap(), 0, mi);
	mi = NULL;
	nTraffic = 0;
	dwIPSize = 0;
	dwMISize = 0;
}

DWORD WINAPI GetDataThreadProc(PVOID pParam)//获取温度占用硬盘线程
{	
	while (!bRealClose)
	{
		if (!TrayData)
			break;
		DWORD dStart = GetTickCount();
		if (TraySave.bMonitor)
		{
			GlobalMemoryStatusEx(&MemoryStatusEx);
			if (TraySave.bMonitorDisk)
			{
				TraySave.bMonitorPDH = TRUE;
				if (TraySave.bMonitorUsage == FALSE)
					GetPDH(FALSE, TRUE);
			}
			if (TraySave.bMonitorUsage)
			{
				iCPU = GetCPUUseRate();
			}
			if (IsWindow(hTaskTips))
			{
				GetProcessMemUsage();
				GetProcessCpuUsage();
			}
			else
			{
				AcquireSRWLockExclusive(&g_processLock);
				if (pProcessTime != NULL)
					ClearProcessSnapshotUnlocked();
				ReleaseSRWLockExclusive(&g_processLock);
			}
			if (TraySave.bMonitorTemperature)
			{
				AcquireSRWLockShared(&g_temperatureLock);
				// Clear the previous sample first.  This matters when monitoring is
				// toggled at runtime or when an AMD machine intentionally skips the
				// optional LHM backend: an unavailable sensor must not leave an old
				// temperature visible indefinitely.
				TrayData->iTemperature1 = 0;
				TrayData->iTemperature2 = 0;
				TrayData->iHddTemperature = 0;
				// CPU temperature is not limited to the optional managed monitor:
				// GetCpuTemp first queries the safe Windows/ACPI path and only then
				// consults an explicitly enabled third-party backend.
				TrayData->iTemperature1 = GetCpuTemp(1);
				// Vendor APIs are a safe supplemental path when the managed monitor
				// has no GPU sensor (common with hybrid/older AMD drivers). They do
				// not read CPU MSRs and therefore do not reintroduce WinRing0 risk.
				if (!hOHMA || TrayData->iTemperature2 == 0)
				{
					int iATITemperature = 0;
					int iNVTemperature = 0;
					if (hNVDLL)
					{
						for (int GpuIndex = 0; GpuIndex < NVAPI_MAX_PHYSICAL_GPUS; GpuIndex++)
						{
							if (hPhysicalGpu[GpuIndex] != 0 && NvAPI_GPU_GetThermalSettings != NULL)
							{
								NV_GPU_THERMAL_SETTINGS currentTemp = {};
								currentTemp.version = NV_GPU_THERMAL_SETTINGS_VER;
								// NVAPI expects a zero-based thermal-sensor index here;
								// 15 is a target mask, not a valid sensor index on all
								// driver generations.
								if (NvAPI_GPU_GetThermalSettings(hPhysicalGpu[GpuIndex], 0, &currentTemp) == 0 &&
									currentTemp.count > 0 && currentTemp.count <= MAX_THERMAL_SENSORS_PER_GPU)
								{
									for (NvU32 sensorIndex = 0; sensorIndex < currentTemp.count; ++sensorIndex)
									{
										const auto& sensor = currentTemp.sensor[sensorIndex];
										if ((sensor.target == NVAPI_THERMAL_TARGET_GPU || sensor.target == NVAPI_THERMAL_TARGET_ALL) &&
											sensor.currentTemp >= 0 && sensor.currentTemp <= 255)
										{
										if (static_cast<int>(sensor.currentTemp) > iNVTemperature)
											iNVTemperature = static_cast<int>(sensor.currentTemp);
										}
									}
								}
							}
						}
					}

					if (hATIDLL && ADL_Overdrive5_Temperature_Get)
					{
						int adapterCount = 1;
						int reportedAdapterCount = 0;
						if (ADL_Adapter_NumberOfAdapters_Get &&
							ADL_Adapter_NumberOfAdapters_Get(&reportedAdapterCount) == ADL_OK &&
							reportedAdapterCount > 0 && reportedAdapterCount <= 256)
							adapterCount = reportedAdapterCount;
						for (int adapterIndex = 0; adapterIndex < adapterCount; ++adapterIndex)
						{
							if (ADL_Adapter_Active_Get)
							{
								int active = 0;
								if (ADL_Adapter_Active_Get(adapterIndex, &active) != ADL_OK || active == 0)
									continue;
							}
							adlTemperature.iSize = sizeof(ADLTemperature);
							adlTemperature.iTemperature = 0;
							if (ADL_Overdrive5_Temperature_Get(adapterIndex, 0, &adlTemperature) == ADL_OK &&
								adlTemperature.iTemperature >= 0 && adlTemperature.iTemperature <= 255000)
							{
								int adapterTemperature = adlTemperature.iTemperature / 1000;
								if (adapterTemperature > iATITemperature)
									iATITemperature = adapterTemperature;
							}
						}
					}
						if (iATITemperature < 0 || iATITemperature > 255)
							iATITemperature = 0;
						if (iATITemperature != 0 || iNVTemperature != 0)
					{
						if (iATITemperature > iNVTemperature)
							TrayData->iTemperature2 = iATITemperature;
						else
							TrayData->iTemperature2 = iNVTemperature;
					}
				}
				ReleaseSRWLockShared(&g_temperatureLock);
			}
			if (TraySave.bMonitorTraffic)
			{
				AcquireSRWLockExclusive(&g_networkLock);
				if (hIphlpapi == NULL)
				{
					hIphlpapi = LoadSystemLibrarySafe(L"iphlpapi.dll");
					if (hIphlpapi)
					{
						GetAdaptersAddressesT = (pfnGetAdaptersAddresses)GetProcAddress(hIphlpapi, "GetAdaptersAddresses");
						getIfTable2 = (pfnGetIfTable2)GetProcAddress(hIphlpapi, "GetIfTable2");
						freeMibTable = (pfnFreeMibTable)GetProcAddress(hIphlpapi, "FreeMibTable");
						// Windows Vista and later expose GetIfTable2, but a damaged
						// or compatibility iphlpapi may omit FreeMibTable.  Resolve
						// the legacy API as a fallback instead of calling a null pointer.
						if (getIfTable2 == NULL || freeMibTable == NULL)
							GetIfTableT = (pfnGetIfTable)GetProcAddress(hIphlpapi, "GetIfTable");
					}
				}
				if (hIphlpapi && GetAdaptersAddressesT && ((getIfTable2 && freeMibTable) || GetIfTableT))
				{
					PIP_ADAPTER_ADDRESSES paa = NULL;
					if (iGetAddressTime == 10)
					{
						// First query only asks for the required size. Passing the
						// previous heap pointer here is unsafe when the adapter list
						// has been rebuilt by Windows. Keep the returned size bounded
						// before using it in HeapAlloc.
						ULONG requiredIPSize = 0;
						ULONG addressStatus = GetAdaptersAddressesT(AF_UNSPEC, 0, NULL, NULL, &requiredIPSize);
						dwIPSize = 0;
						if (addressStatus == ERROR_BUFFER_OVERFLOW &&
							IsReasonableNetworkBufferSize(requiredIPSize, kMaxAdapterAddressBytes))
						{
							PIP_ADAPTER_ADDRESSES newPiaa = (PIP_ADAPTER_ADDRESSES)HeapAlloc(
								GetProcessHeap(), HEAP_ZERO_MEMORY, requiredIPSize);
							if (newPiaa)
							{
								ULONG newIPSize = requiredIPSize;
								ULONG newAddressStatus = GetAdaptersAddressesT(AF_UNSPEC, 0, NULL, newPiaa, &newIPSize);
								// Adapter changes can race the size probe (for example while a
								// Wi-Fi or VPN interface is reconnecting).  Allow one bounded
								// resize/retry, but never loop on a provider-controlled size.
								if (newAddressStatus == ERROR_BUFFER_OVERFLOW &&
									newIPSize > requiredIPSize &&
									IsReasonableNetworkBufferSize(newIPSize, kMaxAdapterAddressBytes))
								{
									PIP_ADAPTER_ADDRESSES resizedPiaa = (PIP_ADAPTER_ADDRESSES)HeapReAlloc(
										GetProcessHeap(), HEAP_ZERO_MEMORY, newPiaa, newIPSize);
									if (resizedPiaa)
									{
										newPiaa = resizedPiaa;
										requiredIPSize = newIPSize;
										newIPSize = requiredIPSize;
										newAddressStatus = GetAdaptersAddressesT(AF_UNSPEC, 0, NULL, newPiaa, &newIPSize);
									}
								}
								if (newAddressStatus == ERROR_SUCCESS &&
									newIPSize >= sizeof(IP_ADAPTER_ADDRESSES) &&
									newIPSize <= requiredIPSize)
								{
									int n = 0;
									DWORD adapterWalk = 0;
									for (paa = newPiaa; paa && adapterWalk++ < (DWORD)kMaxTrafficEntries * 4u && n < kMaxTrafficEntries; paa = paa->Next)
									{
										if (paa->IfType != IF_TYPE_SOFTWARE_LOOPBACK && paa->IfType != IF_TYPE_TUNNEL)
											++n;
									}
									// Keep counters only when adapter identities remain in the
									// same order. Windows may reorder this list after reconnect.
									BOOL preserveCounters = (n > 0 && n == nTraffic && traffic != NULL);
									if (preserveCounters)
									{
										int index = 0;
										adapterWalk = 0;
										for (PIP_ADAPTER_ADDRESSES candidate = newPiaa;
											candidate && adapterWalk++ < (DWORD)kMaxTrafficEntries * 4u && index < n;
											candidate = candidate->Next)
										{
											if (candidate->IfType == IF_TYPE_SOFTWARE_LOOPBACK || candidate->IfType == IF_TYPE_TUNNEL)
												continue;
											if (traffic[index].IfIndex == 0 || traffic[index].IfIndex != candidate->IfIndex)
											{
												preserveCounters = FALSE;
												break;
											}
											++index;
										}
										if (index != n)
											preserveCounters = FALSE;
									}
									// Allocate a matching counter array before replacing the
									// old snapshot. On failure, keep the old pair intact.
									TRAFFIC* newTraffic = traffic;
									BOOL replaceSnapshot = TRUE;
									if (n != nTraffic || (n == 0 && traffic != NULL) || (n > 0 && traffic == NULL))
									{
										newTraffic = NULL;
										if (n > 0)
											newTraffic = (TRAFFIC*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
												(SIZE_T)n * sizeof(TRAFFIC));
										if (n > 0 && newTraffic == NULL)
											replaceSnapshot = FALSE;
									}
									if (replaceSnapshot)
									{
										if (!preserveCounters && newTraffic == traffic && traffic != NULL)
											ZeroMemory(traffic, (SIZE_T)n * sizeof(TRAFFIC));
										if (newTraffic != traffic)
											HeapFree(GetProcessHeap(), 0, traffic);
										traffic = newTraffic;
										nTraffic = n;
										HeapFree(GetProcessHeap(), 0, piaa);
										piaa = newPiaa;
										dwIPSize = newIPSize;
										newPiaa = NULL;
									}
								}
								else if (newAddressStatus == ERROR_NO_DATA)
								{
									// All adapters disappeared between the two calls.
									ClearNetworkSnapshotUnlocked();
								}
							}
							HeapFree(GetProcessHeap(), 0, newPiaa);
						}
						else if (addressStatus == ERROR_SUCCESS || addressStatus == ERROR_NO_DATA ||
							(addressStatus == ERROR_BUFFER_OVERFLOW && requiredIPSize == 0))
						{
							// No adapters are currently reported, or the provider returned
							// an unusable size. Do not keep stale state forever.
							ClearNetworkSnapshotUnlocked();
						}
						iGetAddressTime = 0;
					}
					else
						iGetAddressTime++;
					if (nTraffic > 0 && piaa && traffic)
					{
						ULONG64 m_in_bytes = 0;
						ULONG64 m_out_bytes = 0;
						if (getIfTable2 && freeMibTable)
						{
							mit2 = NULL;
							if (getIfTable2(&mit2) == NO_ERROR && mit2)
							{
								DWORD ifEntryCount = mit2->NumEntries;
								if (ifEntryCount > kMaxIfTableEntries)
									ifEntryCount = kMaxIfTableEntries;
								for (DWORD i = 0; i < ifEntryCount; i++)
								{
									int l = 0;
									DWORD adapterWalk = 0;
									paa = &piaa[0];
									while (paa && adapterWalk++ < (DWORD)kMaxTrafficEntries * 4u)
									{
										if (paa->IfType != IF_TYPE_SOFTWARE_LOOPBACK && paa->IfType != IF_TYPE_TUNNEL)
										{
											if (l >= nTraffic)
												break;
											if (paa->IfIndex == mit2->Table[i].InterfaceIndex)
											{
												traffic[l].IfIndex = paa->IfIndex;
												traffic[l].in_byte = (mit2->Table[i].InOctets >= traffic[l].in_bytes) ?
													(mit2->Table[i].InOctets - traffic[l].in_bytes) : mit2->Table[i].InOctets;
												traffic[l].out_byte = (mit2->Table[i].OutOctets >= traffic[l].out_bytes) ?
													(mit2->Table[i].OutOctets - traffic[l].out_bytes) : mit2->Table[i].OutOctets;
												traffic[l].in_bytes = mit2->Table[i].InOctets;
												traffic[l].out_bytes = mit2->Table[i].OutOctets;
												traffic[l].IP4[0] = L'\0';
													PIP_ADAPTER_UNICAST_ADDRESS pUnicast = paa->FirstUnicastAddress;
													DWORD unicastWalk = 0;
													while (pUnicast && unicastWalk++ < 64)
												{
													if (pUnicast->Address.lpSockaddr && AF_INET == pUnicast->Address.lpSockaddr->sa_family)// IPV4 地址，使用 IPV4 转换
													{
														void* pAddr = &((sockaddr_in*)pUnicast->Address.lpSockaddr)->sin_addr;
														byte* bp = (byte*)pAddr;
														wsprintf(traffic[l].IP4, L"%d.%d.%d.%d", bp[0], bp[1], bp[2], bp[3]);
														break;
													}
													//											else if (AF_INET6 == pUnicast->Address.lpSockaddr->sa_family)// IPV6 地址，使用 IPV6 转换
													//												inet_ntop(PF_INET6, &((sockaddr_in6*)pUnicast->Address.lpSockaddr)->sin6_addr, IP, sizeof(IP));
													pUnicast = pUnicast->Next;
												}
												if (paa->FriendlyName)
													lstrcpynW(traffic[l].FriendlyName, paa->FriendlyName, ARRAYSIZE(traffic[l].FriendlyName));
												else
													traffic[l].FriendlyName[0] = L'\0';
												if (lstrlen(traffic[l].FriendlyName) > 19)
												{
													traffic[l].FriendlyName[16] = L'.';
													traffic[l].FriendlyName[17] = L'.';
													traffic[l].FriendlyName[18] = L'.';
													traffic[l].FriendlyName[19] = L'\0';
												}
												if (paa->AdapterName)
													lstrcpynA(traffic[l].AdapterName, paa->AdapterName, ARRAYSIZE(traffic[l].AdapterName));
												else
													traffic[l].AdapterName[0] = '\0';
												if (TraySave.AdpterName[0] == L'\0' ||
													(traffic[l].AdapterName[0] != '\0' && lstrcmpA(traffic[l].AdapterName, TraySave.AdpterName) == 0))
												{
													m_in_bytes = SaturatingAddUlong64(m_in_bytes, mit2->Table[i].InOctets);
													m_out_bytes = SaturatingAddUlong64(m_out_bytes, mit2->Table[i].OutOctets);
												}

											}
																++l;
																paa = paa->Next;
																}
								}
								}
							}
							if (mit2)
								freeMibTable(mit2);
							mit2 = NULL;
						}
						else if (GetIfTableT)
						{
							ULONG tableSize = IsReasonableNetworkBufferSize(dwMISize, kMaxIfTableBytes) ? dwMISize : 0;
							DWORD tableStatus = GetIfTableT(mi, &tableSize, FALSE);
							if (tableStatus == ERROR_INSUFFICIENT_BUFFER &&
								IsReasonableNetworkBufferSize(tableSize, kMaxIfTableBytes))
							{
								PMIB_IFTABLE newMi = (PMIB_IFTABLE)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, tableSize);
								if (newMi)
								{
									ULONG newTableSize = tableSize;
									if (GetIfTableT(newMi, &newTableSize, FALSE) == NO_ERROR &&
										newTableSize <= tableSize && IsValidIfTableBuffer(newMi, newTableSize))
									{
										HeapFree(GetProcessHeap(), 0, mi);
										mi = newMi;
										dwMISize = newTableSize;
										newMi = NULL;
									}
									HeapFree(GetProcessHeap(), 0, newMi);
								}
							}
							if (IsValidIfTableBuffer(mi, dwMISize))
							{
								DWORD ifEntryCount = mi->dwNumEntries;
								SIZE_T maxEntriesBySize = (dwMISize - FIELD_OFFSET(MIB_IFTABLE, table)) / sizeof(MIB_IFROW);
								if ((SIZE_T)ifEntryCount > maxEntriesBySize)
									ifEntryCount = (DWORD)maxEntriesBySize;
								if (ifEntryCount > kMaxIfTableEntries)
									ifEntryCount = kMaxIfTableEntries;
								for (DWORD i = 0; i < ifEntryCount; i++)
								{
								int l = 0;
								DWORD adapterWalk = 0;
								paa = &piaa[0];
								while (paa && adapterWalk++ < (DWORD)kMaxTrafficEntries * 4u)
								{
									if (paa->IfType != IF_TYPE_SOFTWARE_LOOPBACK && paa->IfType != IF_TYPE_TUNNEL)
									{
										if (l >= nTraffic)
											break;
										if (paa->IfIndex == mi->table[i].dwIndex)
										{
											traffic[l].IfIndex = paa->IfIndex;
											traffic[l].in_byte = (mi->table[i].dwInOctets >= traffic[l].in_bytes) ?
												(mi->table[i].dwInOctets - traffic[l].in_bytes) : mi->table[i].dwInOctets;
											traffic[l].out_byte = (mi->table[i].dwOutOctets >= traffic[l].out_bytes) ?
												(mi->table[i].dwOutOctets - traffic[l].out_bytes) : mi->table[i].dwOutOctets;
											traffic[l].in_bytes = mi->table[i].dwInOctets;
											traffic[l].out_bytes = mi->table[i].dwOutOctets;

											traffic[l].IP4[0] = L'\0';
											PIP_ADAPTER_UNICAST_ADDRESS pUnicast = paa->FirstUnicastAddress;
											DWORD unicastWalk = 0;
											//							char IP[130];
											while (pUnicast && unicastWalk++ < 64)
											{
												if (pUnicast->Address.lpSockaddr && AF_INET == pUnicast->Address.lpSockaddr->sa_family)// IPV4 地址，使用 IPV4 转换
												{
													void* pAddr = &((sockaddr_in*)pUnicast->Address.lpSockaddr)->sin_addr;
													byte* bp = (byte*)pAddr;
													wsprintf(traffic[l].IP4, L"%d.%d.%d.%d", bp[0], bp[1], bp[2], bp[3]);
													break;
												}
												//								else if (AF_INET6 == pUnicast->Address.lpSockaddr->sa_family)// IPV6 地址，使用 IPV6 转换
												//									inet_ntop(PF_INET6, &((sockaddr_in6*)pUnicast->Address.lpSockaddr)->sin6_addr, IP, sizeof(IP));
												pUnicast = pUnicast->Next;
											}
											//							MultiByteToWideChar(CP_ACP, 0, IP, 15, traffic[l].IP4, 15);
											if (paa->FriendlyName)
												lstrcpynW(traffic[l].FriendlyName, paa->FriendlyName, ARRAYSIZE(traffic[l].FriendlyName));
											else
												traffic[l].FriendlyName[0] = L'\0';
											if (lstrlen(traffic[l].FriendlyName) > 19)
											{
												traffic[l].FriendlyName[16] = L'.';
												traffic[l].FriendlyName[17] = L'.';
												traffic[l].FriendlyName[18] = L'.';
												traffic[l].FriendlyName[19] = L'\0';
											}
											if (paa->AdapterName)
												lstrcpynA(traffic[l].AdapterName, paa->AdapterName, ARRAYSIZE(traffic[l].AdapterName));
											else
												traffic[l].AdapterName[0] = '\0';
											//							wcsncpy_s(traffic[l].FriendlyName, 24, paa->FriendlyName,24);
											if (TraySave.AdpterName[0] == L'\0' ||
												(traffic[l].AdapterName[0] != '\0' && lstrcmpA(traffic[l].AdapterName, TraySave.AdpterName) == 0))
											{
														m_in_bytes = SaturatingAddUlong64(m_in_bytes, mi->table[i].dwInOctets);
														m_out_bytes = SaturatingAddUlong64(m_out_bytes, mi->table[i].dwOutOctets);
											}
										}
														++l;
									paa = paa->Next;
							}
								}
							}
							}
						}
						if (TrayData->m_last_in_bytes != 0)
						{
							// Interface counters can reset when an adapter reconnects.
							// Avoid displaying an enormous wrapped value in that case.
							TrayData->s_in_byte = (m_in_bytes >= TrayData->m_last_in_bytes) ?
								(m_in_bytes - TrayData->m_last_in_bytes) : m_in_bytes;
							TrayData->s_out_byte = (m_out_bytes >= TrayData->m_last_out_bytes) ?
								(m_out_bytes - TrayData->m_last_out_bytes) : m_out_bytes;
							/*
														s_in_bytes[iBytes] = s_in_byte / 1024;
														s_out_bytes[iBytes] = s_out_byte / 1024;
														if (iBytes == rNum-1)
															iBytes = 0;
														else
															++iBytes;
							*/
						}
						TrayData->m_last_out_bytes = m_out_bytes;
						TrayData->m_last_in_bytes = m_in_bytes;
					}
				}
				ReleaseSRWLockExclusive(&g_networkLock);
			}
			/*
					else
					{
						if (hIphlpapi)
						{
							FreeLibrary(hIphlpapi);
							hIphlpapi = NULL;
						}
					}
			*/
			if (TraySave.bMonitor)
			{
				if (IsWindowVisible(hTaskTips))
					::InvalidateRect(hTaskTips, NULL, TRUE);
				DWORD dm = GetSystemUsesLightThemeSafe();
				if (dm != bThemeMode)
				{
					DestroyWindow(hTime);
					DestroyWindow(hTaskBar);
					bThemeMode = dm;
				}

			}
		}
		DWORD dTime = GetTickCount() - dStart;
		if (dTime < 988)
			Sleep(988 - dTime);
	}
	return 0;
}
//
//   函数: InitInstance(HINSTANCE, int)
//
//   目标: 保存实例句柄并创建主窗口
//
//   注释:
//
//        在此函数中，我们在全局变量中保存实例句柄并
//        创建和显示主程序窗口。
//
BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
	hMain = ::CreateDialog(hInst, MAKEINTRESOURCE(IDD_MAIN), NULL, (DLGPROC)MainProc);
	if (!hMain)
	{
		return FALSE;
	}
	////////////////////////////////////////////////////////////当前DPI
	hDesktopDC = GetDC(NULL);
	HDC hdc = GetDC(hMain);
	iDPI = GetDeviceCaps(hdc, LOGPIXELSY);
	::ReleaseDC(hMain, hdc);
//	EnableNonClientDpiScaling(hMain);
	//////////////////////////////////////////////////////////////////创建程序全屏时消息
	APPBARDATA abd{};
	abd.cbSize = sizeof(abd);
	abd.hWnd = hMain;
	abd.uCallbackMessage = MSG_APPBAR_MSGID;
	pSHAppBarMessage(ABM_NEW, &abd);
	bThemeMode = GetSystemUsesLightThemeSafe();
	//////////////////////////////////////////////////////////////////////////////////设置通知栏图标
	nid.cbSize = sizeof NOTIFYICONDATA;
	nid.uID = WM_IAWENTRAY;
	nid.hWnd = hMain;
	nid.hIcon = iMain;
	nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	nid.uCallbackMessage = WM_IAWENTRAY;
	//			nid.dwInfoFlags = NIIF_INFO;
	LoadString(hInst, IDS_TIPS, nid.szTip, 88);
	if (TraySave.bTrayIcon)
		pShell_NotifyIcon(NIM_ADD, &nid);

	MemoryStatusEx.dwLength = sizeof MEMORYSTATUSEX;
	GlobalMemoryStatusEx(&MemoryStatusEx);
	if (TraySave.bMonitor)
	{
		AdjustWindowPos();
	}
	SetTimer(hMain, 3, TraySave.FlushTime, NULL);//自定时间处理监控窗口位置和任务栏透明
	SetTimer(hMain, 6, 1000, NULL);//每秒处理任务栏图标
	SetTimer(hMain, 11, 6000, NULL);//内存释放
	SetTimer(hMain, 3000, 3000, NULL);//每三秒重新读取系统窗口
	hGetDataThread = CreateThread(NULL, 0, GetDataThreadProc, 0, 0, 0);
	hPriceThread = CreateThread(NULL, 0, GetPriceThreadProc, 0, 0, 0);
	return TRUE;
}
BOOL Find(IAccessible* paccParent, int iRole, IAccessible** paccChild)//查找任务图标UI
{
	if (paccParent == NULL || paccChild == NULL)
		return FALSE;
	*paccChild = NULL;
	HRESULT hr;
	long numChildren = 0;
	unsigned long numFetched = 0;
	VARIANT varChild;
	int indexCount;
	IAccessible* pChild = NULL;
	IEnumVARIANT* pEnum = NULL;
	IDispatch* pDisp = NULL;
	BOOL found = false;
	//Get the IEnumVARIANT interface
	hr = paccParent->QueryInterface(IID_IEnumVARIANT, (PVOID*)&pEnum);
	if (pEnum)
		pEnum->Reset();
	// Get child count
	if (paccParent->get_accChildCount(&numChildren) != S_OK || numChildren <= 0)
	{
		if (pEnum)
			pEnum->Release();
		return FALSE;
	}
	for (indexCount = 1; indexCount <= numChildren && !found; indexCount++)
	{
		pChild = NULL;
		VariantInit(&varChild);
		if (pEnum)
		{
			hr = pEnum->Next(1, &varChild, &numFetched);
			if (hr != S_OK)
			{
				VariantClear(&varChild);
				break;
			}
		}
		else
		{
			varChild.vt = VT_I4;
			varChild.lVal = indexCount;
		}
		if (varChild.vt == VT_I4)
		{
			pDisp = NULL;
			hr = paccParent->get_accChild(varChild, &pDisp);
		}
		else
			pDisp = varChild.pdispVal;
		if (pDisp)
		{
			hr = pDisp->QueryInterface(IID_IAccessible, (void**)&pChild);
			if (varChild.vt == VT_I4)
				pDisp->Release();
		}
		if (pChild)
		{
			VARIANT self;
			VariantInit(&self);
			self.vt = VT_I4;
			self.lVal = CHILDID_SELF;
			VARIANT varState;
			VariantInit(&varState);
			if (pChild->get_accState(self, &varState) == S_OK &&
				varState.vt == VT_I4 && (varState.lVal & STATE_SYSTEM_INVISIBLE) == 0)
			{
				VARIANT varRole;
				VariantInit(&varRole);
				if (pChild->get_accRole(self, &varRole) == S_OK && varRole.vt == VT_I4 && varRole.lVal == iRole)
				{
					*paccChild = pChild;
					pChild = NULL;
					found = true;
				}
				VariantClear(&varRole);
			}
			VariantClear(&varState);
			VariantClear(&self);
		}
		if (!found && pChild)
		{
			//			found = Find(pCAcc, iRole, paccChild);
			//			if (*paccChild != pCAcc)
			pChild->Release();
		}
		VariantClear(&varChild);
	}
	if (pEnum)
		pEnum->Release();
	return found;
}
int oleft=0, otop=0;
int iIconsWidth=0;
void SetTaskBarPos(HWND hTaskListWnd, HWND hTrayWnd, HWND hTaskWnd, HWND hReBarWnd, BOOL bMainTray)//设置任务栏图标位置
{
	if (!IsWindow(hTaskListWnd) || !IsWindow(hTrayWnd) || !IsWindow(hTaskWnd) || !IsWindow(hReBarWnd))
		return;
	if (hOleacc == NULL)
	{
		hOleacc = LoadSystemLibrarySafe(L"oleacc.dll");
		if (hOleacc)
		{
			AccessibleObjectFromWindowT = (pfnAccessibleObjectFromWindow)GetProcAddress(hOleacc, "AccessibleObjectFromWindow");
			AccessibleChildrenT = (pfnAccessibleChildren)GetProcAddress(hOleacc, "AccessibleChildren");
		}
	}
	if (hOleacc == NULL || AccessibleObjectFromWindowT == NULL || AccessibleChildrenT == NULL)
		return;
	IAccessible* pAcc = NULL;
	if (AccessibleObjectFromWindowT(hTaskListWnd, OBJID_WINDOW, IID_IAccessible, (void**)&pAcc) != S_OK)
		return;
	IAccessible* paccChlid = NULL;
	BOOL found = Find(pAcc, 22, &paccChlid);
	pAcc->Release();
	if (!found || paccChlid == NULL)
		return;
	long childCount;
	long returnCount = 0;
	LONG left, top, width, height;
	LONG ol = 0, ot = 0;
	int tWidth = 0;
	int tHeight = 0;
	if (paccChlid)
	{
		if (paccChlid->get_accChildCount(&childCount) != S_OK || childCount <= 0 || childCount > 4096)
		{
			paccChlid->Release();
			return;
		}
		if (childCount > 0 && childCount <= 4096)
		{
			VARIANT* pArray = (VARIANT*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(VARIANT) * childCount);
			if (pArray && AccessibleChildrenT(paccChlid, 0L, childCount, pArray, &returnCount) == S_OK)
			{
				if (returnCount < 0 || returnCount > childCount)
					returnCount = 0;
				for (int x = 0; x < returnCount; x++)
				{
					VARIANT vtChild = pArray[x];
					{

						VARIANT varState;
						VariantInit(&varState);
						if (paccChlid->get_accState(vtChild, &varState) == S_OK &&
							varState.vt == VT_I4 && (varState.lVal & STATE_SYSTEM_INVISIBLE) == 0)
						{
							VARIANT varRole;
							VariantInit(&varRole);
							if (paccChlid->get_accRole(vtChild, &varRole) == S_OK &&
								varRole.vt == VT_I4 && (varRole.lVal == 0x2b || varRole.lVal == 0x39))
							{
								if (paccChlid->accLocation(&left, &top, &width, &height, vtChild) == S_OK)
								{
									if (ol != left)
									{
										tWidth += width;
										ol = left;
									}
									if (ot != top)
									{
										tHeight += height;
										ot = top;
									}
								}
							}
							VariantClear(&varRole);
						}
						VariantClear(&varState);
					}
				}
			}
			if (pArray)
			{
				for (LONG x = 0; x < returnCount; ++x)
					VariantClear(&pArray[x]);
			}
			HeapFree(GetProcessHeap(), 0, pArray);
		}
		paccChlid->Release();
	}
	else
		return;
	iIconsWidth = tWidth;
	if (tWidth <= 0 || tHeight <= 0)
		return;
	RECT lrc, src, trc;
	if (!GetWindowRect(hTaskListWnd, &lrc) || !GetWindowRect(hTrayWnd, &src) || !GetWindowRect(hTaskWnd, &trc))
		return;
	BOOL Vertical = FALSE;
	if (src.right - src.left < src.bottom - src.top)
		Vertical = TRUE;
	SendMessage(hReBarWnd, WM_SETREDRAW, TRUE, 0);
	int lr, tb;
	if (Vertical)
	{
		int t = trc.left - src.left;
		int b = src.bottom - trc.bottom;
		if (bMainTray && TraySave.bMonitor && TraySave.bMonitorFloat == FALSE)
		{
			if (TraySave.bMonitorLeft == FALSE)
				b += mHeight;
			else
				t += mHeight;
		}
		if (t > b)
			tb = t;
		else
			tb = b;
	}
	else
	{
		int l = trc.left - src.left;
		int r = src.right - trc.right;
		if (TraySave.bMonitor && bMainTray && TraySave.bMonitorFloat == FALSE)
		{
			if (TraySave.bMonitorLeft == FALSE)
				r += mWidth;
			else
				l += mWidth;
		}
		if (l > r)
			lr = l;
		else
			lr = r;
	}
	int nleft, ntop;
	if ((TraySave.iPos == 2 || (Vertical == FALSE && tWidth >= trc.right - trc.left - lr) || (Vertical && tHeight >= trc.bottom - trc.top - tb)) && TraySave.iPos != 0)
	{
		if (Vertical)
		{
			ntop = trc.bottom - trc.top - tHeight;
			if (TraySave.bMonitorLeft == FALSE && TraySave.bMonitor && bMainTray && TraySave.bMonitorFloat == FALSE)
				ntop -= mHeight + 2;
		}
		else
		{
			nleft = trc.right - trc.left - tWidth;
			if (TraySave.bMonitorLeft == FALSE && TraySave.bMonitor && bMainTray && TraySave.bMonitorFloat == FALSE)
				nleft -= mWidth + 2;
		}
	}
	else if (TraySave.iPos == 0)
	{
		if (TraySave.bMonitorLeft && TraySave.bMonitor && bMainTray && TraySave.bMonitorFloat == FALSE)
		{
			nleft = mWidth;
			ntop = mHeight;
		}
		else
		{
			nleft = 0;
			ntop = 0;
			if (TraySave.bMonitor == FALSE)
			{
				SetTimer(hMain, 11, 1000, NULL);
			}
		}
	}
	else if (TraySave.iPos == 1)
	{
		if (Vertical)
			ntop = src.top + (src.bottom - src.top) / 2 - trc.top - tHeight / 2;
		else
			nleft = src.left + (src.right - src.left) / 2 - trc.left - tWidth / 2;
		if (bMainTray)
		{
			if (Vertical)
				ntop -= 2;
			else
				nleft -= 2;
		}
	}
	if (Vertical)
	{
		if (bMainTray)
			otop = ntop;
		SetWindowPos(hTaskListWnd, 0, 0, ntop, lrc.right - lrc.left, lrc.bottom - lrc.top, SWP_NOSIZE | SWP_ASYNCWINDOWPOS | SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSENDCHANGING);
	}
	else
	{
		if (bMainTray)
			oleft = nleft;
		SetWindowPos(hTaskListWnd, 0, nleft, 0, lrc.right - lrc.left, lrc.bottom - lrc.top, SWP_NOSIZE | SWP_ASYNCWINDOWPOS | SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSENDCHANGING);
	}
	if (TraySave.iPos != 0)
		SendMessage(hReBarWnd, WM_SETREDRAW, FALSE, 0);
	ShowWindow(hTaskWnd, SW_SHOWNOACTIVATE);
}
int otleft, ottop;
void SetWH()
{
	mWidth = 0;
	mHeight = 0;
	if (!IsWindow(hMain))
		return;
	HDC mdc = GetDC(hMain);
	if (mdc == NULL)
		return;
	TraySave.TraybarFont.lfHeight = DPI(TraySave.TraybarFontSize);
	HFONT newFont = CreateFontIndirect(&TraySave.TraybarFont); //创建字体
	if (newFont == NULL)
	{
		ReleaseDC(hMain, mdc);
		return;
	}
	if (hFont)
		DeleteObject(hFont);
	hFont = newFont;
	HFONT oldFont = (HFONT)SelectObject(mdc, hFont);
	SIZE tSize{};
	WCHAR sz[16]=L"8";
	if (!::GetTextExtentPoint(mdc, sz, lstrlen(sz), &tSize) || tSize.cx <= 0 || tSize.cy <= 0)
	{
		tSize.cx = 8;
		tSize.cy = 16;
	}
	int measuredSpace = tSize.cx * 6 / 8;
	wSpace = measuredSpace > 0 ? measuredSpace : 1;
	wHeight = tSize.cy > 0 ? tSize.cy : 1;
	if (TraySave.bMonitorTraffic)
	{
		if (TraySave.iMonitorSimple == 1)
		{
			WCHAR szT[] = L"M↓:8.88";
			::GetTextExtentPoint(mdc, szT, lstrlen(szT), &tSize);
		}
		else if (TraySave.iMonitorSimple == 2)
		{
			WCHAR szT[] = L"M8.88";
			::GetTextExtentPoint(mdc, szT, lstrlen(szT), &tSize);
		}
		else
		{
			wsprintf(sz, L"M%s8.88", TraySave.szTrafficOut);
			::GetTextExtentPoint(mdc, sz, lstrlen(sz), &tSize);
		}
		wTraffic = tSize.cx + wSpace;
		mWidth += wTraffic;
		mHeight += wHeight * 2;
	}
	else
		wTraffic = 0;
	if (TraySave.bMonitorUsage)
	{
		if (TraySave.iMonitorSimple == 1)
			::GetTextExtentPoint(mdc, L"88%", lstrlen(L"88%"), &tSize);
		else if (TraySave.iMonitorSimple == 2)
			::GetTextExtentPoint(mdc, L"88", lstrlen(L"88"), &tSize);
		else
		{
			wsprintf(sz, L"%s88%s", TraySave.szUsageMEM, TraySave.szUsageMEMUnit);
			::GetTextExtentPoint(mdc, sz, lstrlen(sz), &tSize);
		}
		wUsage = tSize.cx + wSpace;
		mWidth += wUsage;
		mHeight += wHeight * 2;
	}
	else
		wUsage = 0;
	if (TraySave.bMonitorTemperature)
	{
		if (TraySave.iMonitorSimple == 1)
			::GetTextExtentPoint(mdc, L"88℃", lstrlen(L"88℃"), &tSize);
		else if (TraySave.iMonitorSimple == 2)
			::GetTextExtentPoint(mdc, L"88", lstrlen(L"88"), &tSize);
		else
		{
			wsprintf(sz, L"%s88%s", TraySave.szTemperatureGPU, TraySave.szTemperatureGPUUnit);
			::GetTextExtentPoint(mdc, sz, lstrlen(sz), &tSize);
		}
		wTemperature = tSize.cx + wSpace;
		mWidth += wTemperature;
		if (bRing0)
			mHeight += wHeight * 2;
		else
			mHeight += wHeight;
	}
	else
		wTemperature = 0;
	if (TraySave.bMonitorDisk)
	{
		if (TraySave.iMonitorSimple == 1)
		{
			WCHAR szT[] = L" MR:8.88";
			::GetTextExtentPoint(mdc, szT, lstrlen(szT), &tSize);
		}
		else if (TraySave.iMonitorSimple == 2)
		{
			WCHAR szT[] = L" M8.88";
			::GetTextExtentPoint(mdc, szT, lstrlen(szT), &tSize);
		}
		else
		{
			wsprintf(sz, L" M%s8.88", TraySave.szDiskReadSec);
			::GetTextExtentPoint(mdc, sz, lstrlen(sz), &tSize);
		}
		wDisk = tSize.cx + wSpace;
		if (hOHMA&&TraySave.bMonitorTemperature)
		{
			if (TraySave.bMonitorFloatVRow && TraySave.bMonitorFloat)
			{
			}
			else
				wDisk += wTemperature;
			mHeight += wHeight * 2;
		}
		mWidth += wDisk;
		mHeight += wHeight * 2;
	}
	else
		wDisk = 0;
	if (TraySave.bMonitorTime)
	{
		::GetTextExtentPoint(mdc, L"88:88:88", lstrlen(L"88:88:88"), &tSize);
		wTime = tSize.cx + wSpace;
		mWidth += wTime;
		mHeight += wHeight * 2;
	}
	else
		wTime = 0;
	if (TraySave.bMonitorPrice)
	{
		::GetTextExtentPoint(mdc, L"99999.8 8.88%", lstrlen(L"99999.8 8.88%"), &tSize);
		wPrice = tSize.cx + wSpace;
		if (TraySave.bTwoFour)
		{
			if (TraySave.bMonitorFloatVRow && TraySave.bMonitorFloat)
			{
			}
			else
				wPrice += wPrice;
			mHeight += wHeight * 2;
		}
		mWidth += wPrice;
		mHeight += wHeight * 2;
	}
	else
		wPrice = 0;
//	mWidth += 4;
//	mHeight += 4;
	if (oldFont && oldFont != (HFONT)HGDI_ERROR)
		SelectObject(mdc, oldFont);
	ReleaseDC(hMain, mdc);
	ottop = -1;
	otleft = -1;
}
void AdjustWindowPos()//设置信息窗口位置大小
{	
	if (IsWindow(hTray) == FALSE)//任务栏奔溃时重启
	{
		DestroyWindow(hTime);
		DestroyWindow(hTaskBar);
		bFullScreen = FALSE;
		GetShellAllWnd();
		APPBARDATA abd{};
		abd.cbSize = sizeof(abd);
		abd.hWnd = hMain;
		abd.uCallbackMessage = MSG_APPBAR_MSGID;
		pSHAppBarMessage(ABM_NEW, &abd);
	}
	if (!IsWindow(hTray))
		return;
	int dpi = pGetDpiForWindow(hTray);
	if (dpi != iDPI && dpi != 0)
	{
		iDPI = dpi;
		SendMessage(hMain, WM_DPICHANGED, dpi, dpi);
	}
	if (IsWindow(hTaskBar) == FALSE)
		OpenTaskBar();
	if (!IsWindow(hTaskBar))
		return;
	if (TraySave.bSecond && IsWindow(hTime) == FALSE)
		OpenTimeDlg();
		if (TraySave.bMonitorFloat)
	{
		RECT ScreenRect{};
		if (!GetScreenRectSafe(hTaskBar, &ScreenRect, FALSE))
			return;
		if (TraySave.bMonitorFloatVRow)
		{
			int iWidth = wTraffic;
			if (wTime > iWidth)
				iWidth = wTime;
			if (wTemperature > iWidth)
				iWidth = wTemperature;
			if (wDisk > iWidth)
				iWidth = wDisk;
			if (wUsage > iWidth)
				iWidth = wUsage;
			if (wPrice > iWidth)
				iWidth = wPrice;
			if (iWidth <= 0)
				iWidth = 1;
			int floatHeight = mHeight > 0 ? mHeight : 1;
			int maxX = ScreenRect.right - iWidth;
			int maxY = ScreenRect.bottom - floatHeight - 8;
			if (maxX < ScreenRect.left)
				maxX = ScreenRect.left;
			if (maxY < ScreenRect.top)
				maxY = ScreenRect.top;
			if (TraySave.dMonitorPoint.x < ScreenRect.left)
				TraySave.dMonitorPoint.x = ScreenRect.left;
			if (TraySave.dMonitorPoint.x > maxX)
				TraySave.dMonitorPoint.x = maxX;
			if (TraySave.dMonitorPoint.y < ScreenRect.top)
				TraySave.dMonitorPoint.y = ScreenRect.top;
			if (TraySave.dMonitorPoint.y > maxY)
				TraySave.dMonitorPoint.y = maxY;
			SetWindowPos(hTaskBar, HWND_TOPMOST, TraySave.dMonitorPoint.x, TraySave.dMonitorPoint.y, iWidth, floatHeight, SWP_NOACTIVATE);
			VTray = TRUE;
		}
		else
		{
			int floatWidth = mWidth > 0 ? mWidth : 1;
			int floatHeight = wHeight > 0 ? wHeight * 2 : 1;
			int maxX = ScreenRect.right - floatWidth;
			int maxY = ScreenRect.bottom - floatHeight;
			if (maxX < ScreenRect.left)
				maxX = ScreenRect.left;
			if (maxY < ScreenRect.top)
				maxY = ScreenRect.top;
			if (TraySave.dMonitorPoint.x < ScreenRect.left)
				TraySave.dMonitorPoint.x = ScreenRect.left;
			if (TraySave.dMonitorPoint.x > maxX)
				TraySave.dMonitorPoint.x = maxX;
			if (TraySave.dMonitorPoint.y < ScreenRect.top)
				TraySave.dMonitorPoint.y = ScreenRect.top;
			if (TraySave.dMonitorPoint.y > maxY)
				TraySave.dMonitorPoint.y = maxY;
			SetWindowPos(hTaskBar, HWND_TOPMOST, TraySave.dMonitorPoint.x, TraySave.dMonitorPoint.y, floatWidth, floatHeight, SWP_NOACTIVATE);
			VTray = FALSE;
		}		
	}
	else
	{
		/*
			RECT src,frc;
			if (!TraySave.bMonitorTopmost)
			{
				HWND fwnd = GetForegroundWindow();
				GetWindowRect(fwnd, &frc);
				GetScreenRect(GetForegroundWindow(), &src, FALSE);
				DWORD pid1, pid2;
				GetWindowThreadProcessId(hTray, &pid1);
				GetWindowThreadProcessId(fwnd, &pid2);
				if (EqualRect(&src, &frc) && pid1 != pid2)
				{
					ShowWindow(hTaskBar, SW_HIDE);
					return;
				}
			}
		*/
		RECT trayrc{};
		if (!GetWindowRect(hTray, &trayrc))
			return;
		if (!TraySave.bMonitorFloat && (!IsWindow(hTaskWnd) || !IsWindow(hTaskListWnd)))
			return;
		if (trayrc.right - trayrc.left > trayrc.bottom - trayrc.top)
			VTray = FALSE;
		else
			VTray = TRUE;
		if (VTray == FALSE)
		{
			int nleft = trayrc.left;
			if (hWin11UI)
			{
				RECT startrc, tasklistrc;
				startrc.left = 88;
				if (IsWindow(hStartWnd) && IsWindow(hTaskListWnd) && GetWindowRect(hStartWnd, &startrc))
				{
					if (GetWindowRect(hTaskListWnd, &tasklistrc))
					{
						BOOL bLeft = TraySave.bMonitorLeft;
						if (startrc.left == trayrc.left)
							bLeft = FALSE;
						if (TraySave.bNear)
						{
							if (bLeft)
							{
								nleft = startrc.left - mWidth;
							}
							else
							{
								nleft = tasklistrc.right;
							}
						}
						else
						{
							if (!bLeft)
							{
								RECT tnrc;
								if (IsWindow(hTrayNotifyWnd) && GetWindowRect(hTrayNotifyWnd, &tnrc))
									nleft = tnrc.left - mWidth;
							}
							else
							{
								nleft = trayrc.left;
							}
						}
					}
				}
			}
			else
			{
				if (TraySave.bNear)
				{
					RECT tasklistrc;
					if (GetWindowRect(hTaskListWnd, &tasklistrc))
					{
						if (TraySave.bMonitorLeft)
							nleft = tasklistrc.left - mWidth;
						else
							nleft = tasklistrc.left + iIconsWidth + 2;
					}
				}
				else
				{
					RECT taskrc;
					if (GetWindowRect(hTaskWnd, &taskrc))
					{
						if (TraySave.bMonitorLeft)
							nleft = taskrc.left + 2;
						else
							nleft = taskrc.right - mWidth;
					}
				}
			}
			int h = wHeight * 2;
			int ntop;
			if (trayrc.bottom - trayrc.top < h)
			{
				h = trayrc.bottom - trayrc.top - 2;
				if (h <= 0)
					h = 1;
				ntop = trayrc.top;
			}
			else
				ntop = (trayrc.bottom - trayrc.top - h) / 2 + trayrc.top;
			//		if (!hWin11UI)
			if(!bFullScreen)
				ntop -= trayrc.top;
/*
			if (hWin11UI)
				ntop += 1;
*/
			if (nleft != otleft || ottop != ntop)
			{
				/*
							HDC hdc = GetDC(hTaskBar);
							RECT crc;
							GetClientRect(hTaskBar, &crc);
							HBRUSH hb = CreateSolidBrush(RGB(0, 0, 0));
							FillRect(hdc, &crc, hb);
							DeleteObject(hb);
							ReleaseDC(hTaskBar, hdc);
				*/
				otleft = nleft;
				ottop = ntop;
				//			::InvalidateRect(hTaskBar, NULL, TRUE);
				//			if (!hWin11UI)
				if(bFullScreen)
					SetWindowPos(hTaskBar, HWND_TOPMOST, nleft, ntop, mWidth > 0 ? mWidth : 1, h, SWP_NOACTIVATE | SWP_NOREDRAW | SWP_SHOWWINDOW);
				else
					MoveWindow(hTaskBar, nleft, ntop, mWidth > 0 ? mWidth : 1, h, TRUE);
				
				//			else
				//				SetWindowPos(hTaskBar, HWND_TOPMOST, nleft, ntop, mWidth, h, SWP_NOACTIVATE | SWP_NOREDRAW | SWP_SHOWWINDOW);
			}
			//		else if(hWin11UI)
			//			SetWindowPos(hTaskBar, HWND_TOPMOST, nleft, ntop, mWidth, h, SWP_NOACTIVATE|SWP_NOREDRAW|SWP_NOSIZE|SWP_NOMOVE|SWP_SHOWWINDOW);
			if(bFullScreen)
				SetWindowPos(hTaskBar, HWND_TOPMOST, nleft, ntop, mWidth, h, SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOSIZE | SWP_NOMOVE | SWP_SHOWWINDOW);
		}
		else
		{
			int ntop;
			RECT taskrc{};
			if (!GetWindowRect(hTaskWnd, &taskrc))
				return;
			if (TraySave.bMonitorLeft)
				ntop = taskrc.top+2;
			else
				ntop = taskrc.bottom - mHeight;
			int nleft = 1;
			int w = trayrc.right - trayrc.left - 2;
			if (w <= 0)
				w = 1;
			int verticalHeight = mHeight > 0 ? mHeight : 1;
			if (bFullScreen)
				nleft = trayrc.left + 1;
			if (ntop != ottop || otleft != w)
			{
				/*
							HDC hdc = GetDC(hTaskBar);
							RECT crc;
							GetClientRect(hTaskBar, &crc);
							HBRUSH hb = CreateSolidBrush(RGB(0, 0, 0));
							FillRect(hdc, &crc, hb);
							DeleteObject(hb);
							ReleaseDC(hTaskBar, hdc);
				*/
				ottop = ntop;
				otleft = w;
				if (bFullScreen)
					SetWindowPos(hTaskBar, HWND_TOPMOST, nleft, ntop, w, verticalHeight, SWP_NOACTIVATE | SWP_NOREDRAW | SWP_SHOWWINDOW);
				else
					MoveWindow(hTaskBar, nleft, ntop, w, verticalHeight, TRUE);
			}
		}
	}
	if (TraySave.bSecond && IsWindow(hTime))
	{
		if (!hWin11UI)
		{
			if (GetAncestor(hTime, GA_PARENT) != hTrayClockWnd)
			{
				Sleep(1000);
				DestroyWindow(hTime);
//				SetParent(hTime, hTrayClockWnd);
			}
			else
			{
				if (VTray)
				{
					ShowWindow(hTime, SW_HIDE);
				}
				else
				{
					RECT rc{};
					if (!IsWindow(hTrayClockWnd) || !GetWindowRect(hTrayClockWnd, &rc))
						return;
					SetWindowPos(hTime, 0, 0, 0, rc.right - rc.left, (rc.bottom - rc.top) / 2, SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOREDRAW);
				}
			}
		}
		else
		{
			if (GetAncestor(hTime, GA_PARENT) != hTray)
			{
				Sleep(1000);
				DestroyWindow(hTime);
//				SetParent(hTime, hTray);
			}
			else
			{
				RECT rc{};
				if (!GetWindowRect(hTray, &rc))
					return;
				SetWindowPos(hTime, NULL, rc.right - rc.left - ((rc.bottom - rc.top) * 153/100), 1, rc.bottom - rc.top, (rc.bottom - rc.top-2) / 2, SWP_NOACTIVATE | SWP_NOREDRAW);
			}
		}
	}
}
void GetTrafficStr(WCHAR* sz, ULONG64 uByte, BOOL bBit, int iUnit)
{
	if (!sz)
		return;
	if (iUnit < 0 || iUnit > 2)
		iUnit = 0;
	if (bBit)
		uByte = SaturatingMultiplyUlong64(uByte, 8);
	if (((uByte < 1024 && !bBit) || (uByte < 1000 && bBit)) && iUnit==0)
		wsprintf(sz, L"%dB", TrafficDisplayInt(uByte));
	else if (((uByte < 1048576 && !bBit) || (uByte < 1000000 && bBit)) && iUnit != 2)
	{
		ULONG64 k_byte;
		if (bBit)
			k_byte = uByte / 10;
		else
			k_byte = SaturatingMultiplyUlong64(uByte, 100) / 1024;
		if (k_byte >= 10000)
			wsprintf(sz, L"%dK", TrafficDisplayInt(k_byte / 100));
		else if (k_byte >= 1000)
		{
			k_byte /= 10;
			wsprintf(sz, L"%d.%dK", TrafficDisplayInt(k_byte / 10), TrafficDisplayInt(k_byte % 10));
		}
		else
		{
			wsprintf(sz, L"%d.%dK", TrafficDisplayInt(k_byte / 100), TrafficDisplayInt(k_byte % 100));
		}
	}
	else if ((uByte < 1073741824 && !bBit) || (uByte < 1000000000 && bBit))
	{
		ULONG64 m_byte;
		if (bBit)
			m_byte = uByte / 10000;
		else
			m_byte = SaturatingMultiplyUlong64(uByte, 100) / 1048576;
		if (m_byte >= 10000)
			wsprintf(sz, L"%dM", TrafficDisplayInt(m_byte / 100));
		else if (m_byte >= 1000)
		{
			m_byte /= 10;
			wsprintf(sz, L"%d.%dM", TrafficDisplayInt(m_byte / 10), TrafficDisplayInt(m_byte % 10));
		}
		else
			wsprintf(sz, L"%d.%dM", TrafficDisplayInt(m_byte / 100), TrafficDisplayInt(m_byte % 100));
	}
	else if ((uByte < 1099511627776 && !bBit) || (uByte < 1000000000000 && bBit))
	{
		ULONG64 g_byte;
		if (bBit)
			g_byte = uByte / 10000000;
		else
			g_byte = SaturatingMultiplyUlong64(uByte, 100) / 1073741824;
		if (g_byte >= 10000)
			wsprintf(sz, L"%dG", TrafficDisplayInt(g_byte / 100));
		else if (g_byte >= 10)
		{
			g_byte /= 10;
			wsprintf(sz, L"%d.%dG", TrafficDisplayInt(g_byte / 10), TrafficDisplayInt(g_byte % 10));
		}
		else
			wsprintf(sz, L"%d.%dG", TrafficDisplayInt(g_byte / 100), TrafficDisplayInt(g_byte % 100));
	}
	else
	{
		ULONG64 t_byte;
		if (bBit)
			t_byte = uByte / 10000000000;
		else
			t_byte = SaturatingMultiplyUlong64(uByte, 100) / 1099511627776;
		if (t_byte >= 10000)
			wsprintf(sz, L"%dT", TrafficDisplayInt(t_byte / 100));
		else if (t_byte >= 10)
		{
			t_byte /= 10;
			wsprintf(sz, L"%d.%dT", TrafficDisplayInt(t_byte / 10), TrafficDisplayInt(t_byte % 10));
		}
		else
			wsprintf(sz, L"%d.%dT", TrafficDisplayInt(t_byte / 100), TrafficDisplayInt(t_byte % 100));
	}
	if (bBit)
		lstrlwr(sz, lstrlen(sz));
}
INT_PTR CALLBACK TaskTipsProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)//提示信息窗口过程
{
	switch (message)
	{
	case WM_INITDIALOG:
		return (INT_PTR)TRUE;
	case WM_MOUSEMOVE:
	{
		AcquireSRWLockShared(&g_networkLock);
		int trafficCount = nTraffic;
		ReleaseSRWLockShared(&g_networkLock);
		if (trafficCount < 0)
			trafficCount = 0;
		if (trafficCount > kMaxTrafficEntries)
			trafficCount = kMaxTrafficEntries;
		if (wTipsHeight <= 0)
			return TRUE;
		POINT pt;
		pt.x = GET_X_LPARAM(lParam);
		pt.y = GET_Y_LPARAM(lParam);
		RECT rc;
		GetClientRect(hDlg, &rc);
		rc.top = trafficCount * wTipsHeight;
		rc.bottom = (trafficCount + 12) * wTipsHeight;
		rc.left = rc.right * 100 / 178;
		rc.right = rc.right * 100 / 145;
		if (PtInRect(&rc, pt))
		{
			AcquireSRWLockExclusive(&g_processLock);
			inTipsProcessX = TRUE;
			ReleaseSRWLockExclusive(&g_processLock);
			::InvalidateRect(hDlg, NULL, TRUE);
		}
		else
		{
			AcquireSRWLockExclusive(&g_processLock);
			inTipsProcessX = FALSE;
			ReleaseSRWLockExclusive(&g_processLock);
		}
	}
	break;
	case WM_LBUTTONDOWN:
	{
		AcquireSRWLockShared(&g_networkLock);
		int trafficCount = nTraffic;
		ReleaseSRWLockShared(&g_networkLock);
		if (trafficCount < 0)
			trafficCount = 0;
		if (trafficCount > kMaxTrafficEntries)
			trafficCount = kMaxTrafficEntries;
		if (wTipsHeight <= 0)
			return TRUE;
		POINT pt;
		pt.x = GET_X_LPARAM(lParam);
		pt.y = GET_Y_LPARAM(lParam);
		if (pt.y == 0)
			pt.y = 1;
		if (pt.y < trafficCount * wTipsHeight)
			RunProcess(NULL, szNetCpl);
		else if (pt.y < (trafficCount + 12) * wTipsHeight)
		{
			RECT rc;
			GetClientRect(hDlg, &rc);
			rc.left = rc.right * 100 / 178;
			rc.right = rc.right * 100 / 145;
			if (PtInRect(&rc, pt))
			{				
				int x = 0;
				x = (pt.y / wTipsHeight) - trafficCount;
				if (x < 0 || x >= 12)
					return TRUE;
				DWORD pid = 0;
				AcquireSRWLockShared(&g_processLock);
				if (x < 6 && ppcu[x])
					pid = ppcu[x]->dwProcessID;
				else if (x >= 6 && x - 6 < 6 && ppmu[x - 6])
					pid = ppmu[x - 6]->dwProcessID;
				ReleaseSRWLockShared(&g_processLock);
				if (pid == 0)
					return TRUE;
				rc.left = rc.right * 145 / 156;
				if (PtInRect(&rc, pt))
				{
					HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
					if (hProc)
					{
						TerminateProcess(hProc, 0);
						CloseHandle(hProc);
					}
				}
				else
				{
					OpenProcessPath(pid);
				}
				inTipsProcessX = FALSE;
				GetCursorPos(&pt);
				SetCursorPos(pt.x + 88, pt.y);
			}
			else
			{
				if((pt.y / wTipsHeight) - trafficCount<6)
					RunProcess(NULL, szTaskmgr);
				else
					RunProcess(NULL, szPerfmon);
			}
		}
		else
		{
			RECT rc;
			GetClientRect(hDlg, &rc);
			if (pt.x < rc.right * 8 / 100)
			{
				bSetting = TRUE;
				SendMessage(hMain, WM_TRAYS, 0, 0);				
			}
			else if (pt.x > rc.right * 92 / 100)
			{
				bRealClose = TRUE;
				if (TrayData)
					TrayData->bExit = TRUE;
				SendMessage(hMain, WM_CLOSE, 0, 0);
			}
			else
			{
				if (pt.y < (trafficCount + 13) * wTipsHeight)
				{
					WCHAR wDrive[MAX_PATH];
					DWORD dwLen = GetLogicalDriveStrings(MAX_PATH, wDrive);
					if (dwLen != 0 && dwLen < ARRAYSIZE(wDrive))
					{
						DWORD driver_number = dwLen / 4;
						int driveArea = rc.right - rc.right * 8 / 100;
						int driveWidth = driver_number != 0 ? driveArea / (int)driver_number : 0;
						if (driver_number != 0 && driveWidth > 0 && pt.x >= 0)
						{
							DWORD x = (DWORD)(pt.x / driveWidth);
							if (x < driver_number)
							{
										WCHAR sz[24] = {};
								wsprintf(sz, L"o%s", &wDrive[x * 4]);
								RunProcess(NULL, sz);
							}
						}
					}
				}
				else
				{
					if(pt.x<rc.right/2)
						RunProcess(NULL, szCompmgmt);
					else
						RunProcess(NULL, szPowerCpl);
				}
			}
		}
		return TRUE;
	}
	break;
	case WM_ERASEBKGND:
		// The worker updates process data before it takes the network lock.  Keep
		// the same order for this handler, then copy the short-lived adapter
		// snapshot and release the network lock before doing any GDI or disk work.
		AcquireSRWLockExclusive(&g_processLock);
		SanitizeProcessDisplayDataUnlocked();
		HDC hdc = (HDC)wParam;//BeginPaint(hDlg, &ps);
		RECT rc{}, crc{};
		if (!hdc || !GetClientRect(hDlg, &rc) || rc.right <= rc.left || rc.bottom <= rc.top)
		{
			ReleaseSRWLockExclusive(&g_processLock);
			return TRUE;
		}
		crc = rc;
		HDC mdc = CreateCompatibleDC(hdc);
		TRAFFIC* trafficSnapshot = NULL;
		if (mdc)
		{
			HBITMAP hMemBmp = CreateCompatibleBitmap(hdc, rc.right - rc.left, rc.bottom - rc.top);
			if (!hMemBmp)
			{
				DeleteDC(mdc);
				ReleaseSRWLockExclusive(&g_processLock);
				return TRUE;
			}
			HBITMAP oldBmp = (HBITMAP)SelectObject(mdc, hMemBmp);
			if (!oldBmp || oldBmp == (HBITMAP)HGDI_ERROR)
			{
				DeleteObject(hMemBmp);
				DeleteDC(mdc);
				ReleaseSRWLockExclusive(&g_processLock);
				return TRUE;
			}
			int trafficCount = 0;
			AcquireSRWLockShared(&g_networkLock);
			trafficCount = nTraffic;
			if (trafficCount < 0)
				trafficCount = 0;
			if (trafficCount > kMaxTrafficEntries)
				trafficCount = kMaxTrafficEntries;
			if (trafficCount > 0 && traffic != NULL)
			{
				trafficSnapshot = (TRAFFIC*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
					(SIZE_T)trafficCount * sizeof(TRAFFIC));
				if (trafficSnapshot)
					CopyMemory(trafficSnapshot, traffic, (SIZE_T)trafficCount * sizeof(TRAFFIC));
				else
					trafficCount = 0;
			}
			ReleaseSRWLockShared(&g_networkLock);
			//		if (bErasebkgnd)
			{
				TraySave.TipsFont.lfHeight = DPI(TraySave.TipsFontSize);
				HFONT hTipsFont = CreateFontIndirect(&TraySave.TipsFont); //创建字体
				HFONT oldFont = NULL;
				if (hTipsFont)
					oldFont = (HFONT)SelectObject(mdc, hTipsFont);
				WCHAR sz[64];
				SetBkMode(mdc, TRANSPARENT);
				COLORREF rgb;
				rgb = RGB(192, 192, 192);
				SetTextColor(mdc, rgb);
				rc.bottom = wTipsHeight;
				HBRUSH hb = CreateSolidBrush(RGB(24, 24, 24));
				for (int i = 0; hb && i < trafficCount / 2 + 8; i++)
				{
					FillRect(mdc, &rc, hb);
					OffsetRect(&rc, 0, wTipsHeight * 2);
				}
				if (hb)
					DeleteObject(hb);
				HPEN hp = CreatePen(PS_DOT, 1, RGB(98, 98, 98));
				HPEN oldpen = NULL;
				BOOL penSelected = FALSE;
				if (hp)
				{
					oldpen = (HPEN)SelectObject(mdc, hp);
					if (oldpen && oldpen != (HPEN)HGDI_ERROR)
						penSelected = TRUE;
					else
					{
						DeleteObject(hp);
						hp = NULL;
					}
				}
				if (penSelected)
				{
				MoveToEx(mdc, crc.right * 10 / 31, 0, NULL);
				LineTo(mdc, crc.right * 10 / 31, wTipsHeight * trafficCount);

				MoveToEx(mdc, crc.right * 57 / 100, 0, NULL);
				LineTo(mdc, crc.right * 57 / 100, wTipsHeight* trafficCount);
				MoveToEx(mdc, crc.right * 66 / 100, 0, NULL);
				LineTo(mdc, crc.right * 66 / 100, wTipsHeight* trafficCount);
				MoveToEx(mdc, crc.right * 78 / 100, 0, NULL);
				LineTo(mdc, crc.right * 78 / 100, wTipsHeight * trafficCount);
				MoveToEx(mdc, crc.right * 87 / 100, 0, NULL);
				LineTo(mdc, crc.right * 87 / 100, wTipsHeight * trafficCount);

				MoveToEx(mdc, crc.right * 100 / 122, wTipsHeight * trafficCount, NULL);
				LineTo(mdc, crc.right * 100 / 122, wTipsHeight * (trafficCount + 12));
				MoveToEx(mdc, crc.right * 100 / 145, wTipsHeight * trafficCount, NULL);
				LineTo(mdc, crc.right * 100 / 145, wTipsHeight * (trafficCount + 12));
				MoveToEx(mdc, crc.right * 100 / 156, wTipsHeight * trafficCount, NULL);
				LineTo(mdc, crc.right * 100 / 156, wTipsHeight * (trafficCount + 12));
				MoveToEx(mdc, crc.right * 100 / 178, wTipsHeight* trafficCount, NULL);
				LineTo(mdc, crc.right * 100 / 178, wTipsHeight* (trafficCount + 12));


				MoveToEx(mdc, 0, wTipsHeight * trafficCount, NULL);
				LineTo(mdc, crc.right, wTipsHeight * trafficCount);
				MoveToEx(mdc, 0, wTipsHeight * (trafficCount + 6), NULL);
				LineTo(mdc, crc.right, wTipsHeight * (trafficCount + 6));
				MoveToEx(mdc, 0, wTipsHeight * (trafficCount + 12), NULL);
				LineTo(mdc, crc.right, wTipsHeight * (trafficCount + 12));
				MoveToEx(mdc, crc.right * 8 / 100, wTipsHeight * (trafficCount + 12), NULL);
				LineTo(mdc, crc.right * 8 / 100, wTipsHeight * (trafficCount + 12 + 2));
				MoveToEx(mdc, crc.right * 92 / 100, wTipsHeight * (trafficCount + 12), NULL);
				LineTo(mdc, crc.right * 92 / 100, wTipsHeight * (trafficCount + 12 + 2));
				}

				/*
							DeleteObject(hp);
							rc.bottom = wTipsHeight*trafficCount;
							rc.top = 1;
							rc.left = 5;
							rc.right = crc.right-5;
							DWORD max = 0;
							for (int in=0;in<rNum;in++)
							{
								if (max < s_in_bytes[in])
									max = s_in_bytes[in];
							}
							for (int out = 0; out < rNum; out++)
							{
								if (max < s_out_bytes[out])
									max = s_out_bytes[out];
							}
							if (max == 0)
								max = 100;
							hp = CreatePen(PS_DOT, 1, RGB(128, 255, 0));
							oldpen = (HPEN)SelectObject(mdc, hp);
							MoveToEx(mdc, rc.left, rc.bottom, NULL);
							for (int e=1;e<= rNum;e++)
							{
								int x = iBytes + e-1;
								if (x >= rNum)
									x -= rNum;
								LineTo(mdc, rc.left + (rc.right - rc.left) * e / rNum, rc.bottom - double((rc.bottom - rc.top) * s_out_bytes[x] / max ));
							}
							SelectObject(mdc, oldpen);
							DeleteObject(hp);

							hp = CreatePen(PS_SOLID, 1, RGB(255, 128, 0));
							oldpen = (HPEN)SelectObject(mdc, hp);
							MoveToEx(mdc, rc.left, rc.bottom, NULL);
							for (int e = 1; e <= rNum; e++)
							{
								int x = iBytes + e - 1;
								if (x >= rNum)
									x -= rNum;
								LineTo(mdc, rc.left + (rc.right - rc.left) *e / rNum, rc.bottom - double((rc.bottom - rc.top) * s_in_bytes[x] / max ));
							}
				*/

				if (penSelected)
					SelectObject(mdc, oldpen);
				if (hp)
					DeleteObject(hp);
				rc.top = 0;
				/*
							int cx;
							if (wTipsHeight < 24)
								cx = 16;
							else if (wTipsHeight < 32)
								cx = 24;
							else if (wTipsHeight < 48)
								cx = 32;
							else if (wTipsHeight < 64)
								cx = 48;
							else if (wTipsHeight < 128)
								cx = 64;
							else
								cx = 128;
				*/
				//			OffsetRect(&rc, 0, DPI(16)*3);
							//PIP_ADAPTER_INFO pai = &ipinfo[0];
				//			PIP_ADAPTER_ADDRESSES paa = &piaa[0];
				rc.bottom = wTipsHeight;
				for (int i = 0; i < trafficCount && trafficSnapshot != NULL; i++)
				{
					rc.left = 5;// +wTipsHeight;
					DrawText(mdc, trafficSnapshot[i].FriendlyName, lstrlen(trafficSnapshot[i].FriendlyName), &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
					/*
									HICON hIcon = GetIconForCSIDL(CSIDL_CONNECTIONS);

									UINT size = MAKELPARAM(cx, cx);
									WCHAR szGuid[64];// = L":::";
									MultiByteToWideChar(CP_ACP, 0, traffic[i].AdapterName, 64, szGuid , 64);
									SHFILEINFO shfi;
									SHGetFileInfo(szGuid, 0, &shfi, sizeof(shfi), SHGFI_ICON | SHGFI_USEFILEATTRIBUTES);
									hIcon = shfi.hIcon;

									if (wTipsHeight >= 16)
										DrawIconEx(mdc, 2, rc.top + (rc.bottom - rc.top - cx) / 2, hIcon, cx, cx, 0, NULL, DI_NORMAL);
									else
										DrawIconEx(mdc, 1, rc.top + 1, hIcon, wTipsHeight - 2, wTipsHeight - 2, 0, NULL, DI_NORMAL);
									DestroyIcon(hIcon);
					*/					
					rc.left = crc.right * 10 / 31;
					rc.right = crc.right * 57 / 100;
					DrawText(mdc, trafficSnapshot[i].IP4, lstrlen(trafficSnapshot[i].IP4), &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
					rc.left = crc.right * 57 / 100;
					rc.right = crc.right * 66 / 100 - 2;
					GetTrafficStr(sz, trafficSnapshot[i].in_bytes, HIWORD(TraySave.iUnit));
					DrawText(mdc, sz, lstrlen(sz), &rc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
					GetTrafficStr(sz, trafficSnapshot[i].in_byte, HIWORD(TraySave.iUnit));
					rc.left = crc.right * 66 / 100 + 2;
					rc.right = crc.right * 78 / 100-2;
					DrawText(mdc, L"↓:", 2, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
					DrawText(mdc, sz, lstrlen(sz), &rc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
					rc.left = crc.right * 78 / 100 + 2;
					rc.right = crc.right * 87 / 100 - 2;
					GetTrafficStr(sz, trafficSnapshot[i].out_bytes, HIWORD(TraySave.iUnit));
					DrawText(mdc, sz, lstrlen(sz), &rc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
					GetTrafficStr(sz, trafficSnapshot[i].out_byte, HIWORD(TraySave.iUnit));
					rc.left = crc.right * 87 / 100 + 2;
					rc.right = crc.right - 5;
					DrawText(mdc, L"↑:", 2, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
					DrawText(mdc, sz, lstrlen(sz), &rc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
					OffsetRect(&rc, 0, wTipsHeight);
				}
				rc.left = 5;// +wTipsHeight;
				rc.right = crc.right - 5;
				POINT pt;
				GetCursorPos(&pt);
				ScreenToClient(hDlg, &pt);
				for (int i = 0; i < 6; i++)
				{
					SetTextColor(mdc, RGB(192, 192, 0));
					DrawText(mdc, ppcu[i]->szExe, lstrlen(ppcu[i]->szExe), &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
					/*
									HICON hIcon = OpenProcessIcon(ppcu[i]->dwProcessID,cx);
									if(wTipsHeight>=16)
										DrawIconEx(mdc, 2, rc.top + (rc.bottom - rc.top - cx) / 2, hIcon, cx, cx, 0, NULL, DI_NORMAL);
									else
										DrawIconEx(mdc, 1, rc.top+1, hIcon, wTipsHeight-2, wTipsHeight-2, 0, NULL, DI_NORMAL);
									DestroyIcon(hIcon);
					*/
					int iCpuUsage = int(ppcu[i]->fCpuUsage * 100);
					wsprintf(sz, L"%d.%.2d%%", iCpuUsage / 100, iCpuUsage % 100);
					DrawText(mdc, sz, lstrlen(sz), &rc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
					RECT cr = rc;
					cr.left = crc.right * 100 / 145;
					cr.right = crc.right * 100 / 122;
					wsprintf(sz, L"%d", ppcu[i]->dwProcessID);
					DrawText(mdc, sz, lstrlen(sz), &cr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
					cr.left = crc.right * 100 / 156;
					cr.right = crc.right * 100 / 145;
					if (PtInRect(&cr, pt))
						SetTextColor(mdc, RGB(255, 255, 255));
					DrawText(mdc, L"X", 1, &cr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
					SetTextColor(mdc, RGB(192, 192, 0));
					cr.left = crc.right * 100 / 178;
					cr.right = crc.right * 100 / 156;
					if (PtInRect(&cr, pt))
						SetTextColor(mdc, RGB(255, 255, 255));
					DrawText(mdc, L"路径", 2, &cr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
					OffsetRect(&rc, 0, wTipsHeight);
				}
				for (int i = 0; i < 6; i++)
				{
					SetTextColor(mdc, RGB(0, 192, 192));
					DrawText(mdc, ppmu[i]->szExe, lstrlen(ppmu[i]->szExe), &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
					/*
									HICON hIcon = OpenProcessIcon(ppmu[i]->dwProcessID,cx);
									if(wTipsHeight>=16)
										DrawIconEx(mdc, 2, rc.top+(rc.bottom-rc.top-cx)/2, hIcon, cx, cx, 0, NULL, DI_NORMAL);
									else
										DrawIconEx(mdc, 1, rc.top+1, hIcon, wTipsHeight-2, wTipsHeight-2, 0, NULL, DI_NORMAL);
									DestroyIcon(hIcon);
					*/
					if (ppmu[i]->dwMemUsage >= 1048576000)
					{
						SIZE_T iMemUsage = (ppmu[i]->dwMemUsage * 100 / 1073741824);
						wsprintf(sz, L"%d.%.2dGB", iMemUsage / 100, iMemUsage % 100);
					}
					else
					{
						SIZE_T iMemUsage = (ppmu[i]->dwMemUsage * 100 / 1048576);
						wsprintf(sz, L"%d.%.2dMB", iMemUsage / 100, iMemUsage % 100);
					}
					DrawText(mdc, sz, lstrlen(sz), &rc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
					RECT cr = rc;
					cr.left = crc.right * 100 / 145;
					cr.right = crc.right * 100 / 122;
					wsprintf(sz, L"%d", ppmu[i]->dwProcessID);
					DrawText(mdc, sz, lstrlen(sz), &cr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
					cr.left = crc.right * 100 / 156;
					cr.right = crc.right * 100 / 145;
					if (PtInRect(&cr, pt))
						SetTextColor(mdc, RGB(255, 255, 255));
					DrawText(mdc, L"X", 1, &cr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
					SetTextColor(mdc, RGB(0, 192, 192));
					cr.left = crc.right * 100 / 178;
					cr.right = crc.right * 100 / 156;
					if (PtInRect(&cr, pt))
						SetTextColor(mdc, RGB(255, 255, 255));
					DrawText(mdc, L"路径", 2, &cr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
					OffsetRect(&rc, 0, wTipsHeight);
				}

				HBRUSH hb1 = NULL, hb2 = NULL, hb3 = NULL, hb4 = NULL;
				hb1 = CreateSolidBrush(RGB(168, 168, 168));
				hb2 = CreateSolidBrush(RGB(128, 0, 0));
				hb3 = CreateSolidBrush(RGB(0, 128, 198));
				hb4 = CreateSolidBrush(RGB(0, 148, 0));
				SetTextColor(mdc, RGB(255, 255, 255));
				/*
							HBRUSH hb;
							hb=CreateSolidBrush(RGB(0,38,0));
							FillRect(mdc, &rc, hb);
							DeleteObject(hb);
				*/
				WCHAR wDrive[MAX_PATH];
				DWORD dwLen = GetLogicalDriveStrings(MAX_PATH, wDrive);
				if (dwLen != 0 && dwLen < ARRAYSIZE(wDrive))
				{
					DWORD driver_number = dwLen / 4;
					if (driver_number == 0)
						driver_number = 1;
					rc.left = crc.right * 8 / 100 + 3;
					int dw = crc.right * 84 / 100 / (int)driver_number - 2;
					if (dw <= 0)
						dw = 1;
					rc.right = rc.left + dw;
					rc.top += 3;
					rc.bottom -= 1;
					for (DWORD nIndex = 0; nIndex < driver_number; nIndex++)
					{
						LPWSTR dName = wDrive + nIndex * 4;
						UINT64 lpFreeBytesAvailable = 0;
						UINT64 lpTotalNumberOfBytes = 0;
						UINT64 lpTotalNumberOfFreeBytes = 0;
						if (GetDriveType(dName) != DRIVE_CDROM && dName[0] != L'A')
						{
							if (GetDiskFreeSpaceEx(dName, (PULARGE_INTEGER)&lpFreeBytesAvailable, (PULARGE_INTEGER)&lpTotalNumberOfBytes, (PULARGE_INTEGER)&lpTotalNumberOfFreeBytes) && lpTotalNumberOfBytes != 0)
							{
								RECT frc = rc;
								if (nIndex + 1 == driver_number)
									rc.right = crc.right * 92 / 100 - 2;
								frc.right = ScaleRectFill(rc.left, rc.right,
									lpTotalNumberOfBytes - lpTotalNumberOfFreeBytes,
									lpTotalNumberOfBytes);
								if (hb1)
									FillRect(mdc, &rc, hb1);
								if (lpTotalNumberOfFreeBytes < lpTotalNumberOfBytes * 1 / 10)
								{
									if (hb2)
										FillRect(mdc, &frc, hb2);
								}
								else if (hb3)
									FillRect(mdc, &frc, hb3);
							}
							dName[2] = 0;
						}
						DrawText(mdc, dName, lstrlen(dName), &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
						OffsetRect(&rc, dw + 2, 0);
						if (rc.right - 3 > crc.right * 92 / 100)
							break;
					}
				}
				rc.top -= 2;
				rc.bottom -= 2;
				OffsetRect(&rc, 0, wTipsHeight);
				rc.left = crc.right * 50 / 100 + 1;
				rc.right = crc.right * 92 / 100 - 2;
				if (hb1)
					FillRect(mdc, &rc, hb1);

				rc.left = crc.right * 8 / 100 + 3;
				rc.right = crc.right * 50 / 100 - 1;
				if (hb1)
					FillRect(mdc, &rc, hb1);
				/*
							PROCESSOR_POWER_INFORMATION* pi = (PROCESSOR_POWER_INFORMATION*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof PROCESSOR_POWER_INFORMATION*dNumProcessor);
							if (pCallNtPowerInformation(ProcessorInformation, NULL, 0, &pi[0], sizeof PROCESSOR_POWER_INFORMATION * dNumProcessor) == 0)
							{
								int iCGhz = pi[0].CurrentMhz / 10;
								int iMGhz = pi[0].MaxMhz / 10;
								wsprintf(sz,  L"%d个逻辑处理器 当前频率%d.%.2dGHz 最大频率%d.%.2dGHz", dNumProcessor, iCGhz/100,iCGhz%100, iMGhz / 100, iMGhz % 100);
							}
							HeapFree(GetProcessHeap(), 0,pi);
							DrawText(mdc, sz, lstrlen(sz), &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
				*/

				DWORDLONG iaPage = MemoryStatusEx.ullAvailPageFile * 100 / 1073741824;
				DWORDLONG itPage = MemoryStatusEx.ullTotalPageFile * 100 / 1073741824;
				DWORDLONG ia = MemoryStatusEx.ullAvailPhys * 100 / 1073741824;
				DWORDLONG it = MemoryStatusEx.ullTotalPhys * 100 / 1073741824;

				RECT frc = rc;
				frc.right = frc.left;
				if (itPage != 0)
					frc.right = ScaleRectFill(rc.left, rc.right,
						itPage > iaPage ? itPage - iaPage : 0, itPage);
				if (iaPage < itPage * 2 / 10)
				{
					if (hb2)
						FillRect(mdc, &frc, hb2);
				}
				else if (hb4)
					FillRect(mdc, &frc, hb4);
				wsprintf(sz, L"虚拟内存:%d.%.2d/%d.%.2dGB", iaPage / 100, iaPage % 100, itPage / 100, itPage % 100);
				DrawText(mdc, sz, lstrlen(sz), &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
				rc.left = crc.right * 50 / 100 + 1;
				rc.right = crc.right * 92 / 100 - 2;
				frc = rc;
				frc.right = frc.left;
				if (it != 0)
					frc.right = ScaleRectFill(rc.left, rc.right,
						it > ia ? it - ia : 0, it);
				if (ia < it * 2 / 10)
				{
					if (hb2)
						FillRect(mdc, &frc, hb2);
				}
				else if (hb4)
					FillRect(mdc, &frc, hb4);
				if (hb1)
					DeleteObject(hb1);
				if (hb2)
					DeleteObject(hb2);
				if (hb3)
					DeleteObject(hb3);
				if (hb4)
					DeleteObject(hb4);
				wsprintf(sz, L"物理内存:%d.%.2d/%d.%.2dGB", ia / 100, ia % 100, it / 100, it % 100);
				DrawText(mdc, sz, lstrlen(sz), &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

				WCHAR set[] = L"设置";
				WCHAR sexit[] = L"退出";
				rc.top -= wTipsHeight;
				rc.right = crc.right * 8 / 100;
				rc.left = 0;
				DrawText(mdc, set, lstrlen(set), &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
				rc.right = crc.right;
				rc.left = crc.right * 92 / 100;
				DrawText(mdc, sexit, lstrlen(sexit), &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
				if (oldFont && oldFont != (HFONT)HGDI_ERROR)
					SelectObject(mdc, oldFont);
				if (hTipsFont)
					DeleteObject(hTipsFont);
			}
			GetClientRect(hDlg, &rc);
			BitBlt(hdc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, mdc, 0, 0, SRCCOPY);
			if (oldBmp && oldBmp != (HBITMAP)HGDI_ERROR)
				SelectObject(mdc, oldBmp);
			DeleteObject(hMemBmp);
			DeleteDC(mdc);
		}
		if (trafficSnapshot)
			HeapFree(GetProcessHeap(), 0, trafficSnapshot);
		ReleaseSRWLockExclusive(&g_processLock);
		return TRUE;
		break;
	}
	return (INT_PTR)FALSE;
}
void GetProcessCpuUsage()//获取进程CPU占用前五
{
	AcquireSRWLockExclusive(&g_processLock);
	if (!pProcessTime || pProcessTimeCapacity == 0 || dNumProcessor == 0)
	{
		ReleaseSRWLockExclusive(&g_processLock);
		return;
	}
	if (!inTipsProcessX)
	{
		ppcu[0] = &pcu[0];
		ppcu[1] = &pcu[1];
		ppcu[2] = &pcu[2];
		ppcu[3] = &pcu[3];
		ppcu[4] = &pcu[4];
		ppcu[5] = &pcu[5];
		memset(pcu, 0, sizeof pcu);
		pcu[0].fCpuUsage = 0;
		pcu[1].fCpuUsage = 0;
		pcu[2].fCpuUsage = 0;
		pcu[3].fCpuUsage = 0;
		pcu[4].fCpuUsage = 0;
		pcu[5].fCpuUsage = 0;
	}
	DWORD dCurID = GetCurrentProcessId();
	PROCESSENTRY32 pe;
	pe.dwSize = sizeof(PROCESSENTRY32);
	HANDLE hs = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (hs != INVALID_HANDLE_VALUE)
	{
		BOOL ret = Process32First(hs, &pe);
		while (ret)
		{
			if (pe.th32ProcessID != dCurID)
			{
				HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
				if (hProc)
				{
					int n = -1;
					for (SIZE_T i = 0; i < pProcessTimeCapacity; ++i)
					{
						if (pProcessTime[i].dwProcessID == pe.th32ProcessID)
						{
							n = (int)i;
							break;
						}
						if (n == -1 && pProcessTime[i].dwProcessID == 0)
							n = (int)i;
					}
					FILETIME createTime{}, exitTime{}, kernelTime{}, userTime{};
					if (n >= 0 && GetProcessTimes(hProc, &createTime, &exitTime, &kernelTime, &userTime))
					{
						ULARGE_INTEGER kernelTicks{};
						ULARGE_INTEGER userTicks{};
						kernelTicks.HighPart = kernelTime.dwHighDateTime;
						kernelTicks.LowPart = kernelTime.dwLowDateTime;
						userTicks.HighPart = userTime.dwHighDateTime;
						userTicks.LowPart = userTime.dwLowDateTime;
						float nProcCpuPercent = 0.0f;
						if (kernelTicks.QuadPart <= ~(ULONGLONG)0 - userTicks.QuadPart)
						{
							ULONGLONG totalTicks = kernelTicks.QuadPart + userTicks.QuadPart;
							ULONGLONG currentMilliseconds = totalTicks / 10000ULL;
							FILETIME previousCreateTime = pProcessTime[n].CreateTime;
							BOOL reusedPid = previousCreateTime.dwHighDateTime != createTime.dwHighDateTime ||
								previousCreateTime.dwLowDateTime != createTime.dwLowDateTime;
							LONGLONG previousMilliseconds = pProcessTime[n].g_slgProcessTimeOld.QuadPart;
							if (reusedPid || previousMilliseconds <= 0 || currentMilliseconds <= (ULONGLONG)previousMilliseconds)
							{
								nProcCpuPercent = 0.0f;
							}
							else
							{
								ULONGLONG elapsedMilliseconds = currentMilliseconds - (ULONGLONG)previousMilliseconds;
								nProcCpuPercent = (float)(((double)elapsedMilliseconds * 100.0) /
									(1000.0 * (double)dNumProcessor));
							}
							pProcessTime[n].CreateTime = createTime;
							pProcessTime[n].g_slgProcessTimeOld.QuadPart =
								currentMilliseconds <= (ULONGLONG)LLONG_MAX ? (LONGLONG)currentMilliseconds : 0;
							pProcessTime[n].dwProcessID = pe.th32ProcessID;
						}
						if (!std::isfinite(nProcCpuPercent) || nProcCpuPercent < 0.0f || nProcCpuPercent > 100.0f)
							nProcCpuPercent = 0.0f;
						if (!inTipsProcessX)
						{
							PROCESSCPUUSAGE* ppc;
							if (ppcu[0]->fCpuUsage <= nProcCpuPercent)
							{
								ppc = ppcu[5]; ppcu[5] = ppcu[4]; ppcu[4] = ppcu[3]; ppcu[3] = ppcu[2];
								ppcu[2] = ppcu[1]; ppcu[1] = ppcu[0]; ppcu[0] = ppc;
								ppcu[0]->dwProcessID = pe.th32ProcessID; ppcu[0]->fCpuUsage = nProcCpuPercent;
								lstrcpyn(ppcu[0]->szExe, pe.szExeFile, ARRAYSIZE(ppcu[0]->szExe));
							}
							else if (ppcu[1]->fCpuUsage <= nProcCpuPercent)
							{
								ppc = ppcu[5]; ppcu[5] = ppcu[4]; ppcu[4] = ppcu[3]; ppcu[3] = ppcu[2]; ppcu[2] = ppc;
								ppcu[1]->dwProcessID = pe.th32ProcessID; ppcu[1]->fCpuUsage = nProcCpuPercent;
								lstrcpyn(ppcu[1]->szExe, pe.szExeFile, ARRAYSIZE(ppcu[1]->szExe));
							}
							else if (ppcu[2]->fCpuUsage <= nProcCpuPercent)
							{
								ppc = ppcu[5]; ppcu[5] = ppcu[4]; ppcu[4] = ppcu[3]; ppcu[3] = ppc;
								ppcu[2]->dwProcessID = pe.th32ProcessID; ppcu[2]->fCpuUsage = nProcCpuPercent;
								lstrcpyn(ppcu[2]->szExe, pe.szExeFile, ARRAYSIZE(ppcu[2]->szExe));
							}
							else if (ppcu[3]->fCpuUsage <= nProcCpuPercent)
							{
								ppc = ppcu[5]; ppcu[5] = ppcu[4]; ppcu[4] = ppcu[3]; ppcu[3] = ppc;
								ppcu[3]->dwProcessID = pe.th32ProcessID; ppcu[3]->fCpuUsage = nProcCpuPercent;
								lstrcpyn(ppcu[3]->szExe, pe.szExeFile, ARRAYSIZE(ppcu[3]->szExe));
							}
							else if (ppcu[4]->fCpuUsage <= nProcCpuPercent)
							{
								ppc = ppcu[5]; ppcu[5] = ppcu[4]; ppcu[4] = ppc;
								ppcu[4]->dwProcessID = pe.th32ProcessID; ppcu[4]->fCpuUsage = nProcCpuPercent;
								lstrcpyn(ppcu[4]->szExe, pe.szExeFile, ARRAYSIZE(ppcu[4]->szExe));
							}
							else if (ppcu[5]->fCpuUsage <= nProcCpuPercent)
							{
								ppcu[5]->dwProcessID = pe.th32ProcessID; ppcu[5]->fCpuUsage = nProcCpuPercent;
								lstrcpyn(ppcu[5]->szExe, pe.szExeFile, ARRAYSIZE(ppcu[5]->szExe));
							}
						}
					}
					CloseHandle(hProc);
				}
			}
			ret = Process32Next(hs, &pe);
		}
		CloseHandle(hs);
	}
	ReleaseSRWLockExclusive(&g_processLock);
}
int GetProcessMemUsage()//获取进程内存占用前五
{
	AcquireSRWLockExclusive(&g_processLock);
	if (!inTipsProcessX)
	{
		ppmu[0] = &pmu[0];
		ppmu[1] = &pmu[1];
		ppmu[2] = &pmu[2];
		ppmu[3] = &pmu[3];
		ppmu[4] = &pmu[4];
		ppmu[5] = &pmu[5];
		memset(pmu, 0, sizeof pmu);
		pmu[0].dwMemUsage = 0;
		pmu[1].dwMemUsage = 0;
		pmu[2].dwMemUsage = 0;
		pmu[3].dwMemUsage = 0;
		pmu[4].dwMemUsage = 0;
		pmu[5].dwMemUsage = 0;
	}
	PROCESSENTRY32 pe;
	pe.dwSize = sizeof(PROCESSENTRY32);
	int n = 0;
	HANDLE hs = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (hs != INVALID_HANDLE_VALUE)
	{
		BOOL ret = Process32First(hs, &pe);
		while (ret)
		{
			if (n < INT_MAX)
				++n;
			if (lstrcmp(pe.szExeFile, L"Memory Compression") != 0 && !inTipsProcessX)
			{
				HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
				if (hProc)
				{
					PROCESS_MEMORY_COUNTERS_EX pmc{};
					if (GetProcessMemoryInfo(hProc, (PPROCESS_MEMORY_COUNTERS)&pmc, sizeof(pmc)))
					{
						//QueryWorkingSet()
						PROCESSMEMORYUSAGE* ppm;
						if (ppmu[0]->dwMemUsage <= pmc.WorkingSetSize)
						{
							ppm = ppmu[5];
							ppmu[5] = ppmu[4];
							ppmu[4] = ppmu[3];
							ppmu[3] = ppmu[2];
							ppmu[2] = ppmu[1];
							ppmu[1] = ppmu[0];
							ppmu[0] = ppm;
							ppmu[0]->dwProcessID = pe.th32ProcessID;
							ppmu[0]->dwMemUsage = pmc.WorkingSetSize;
							lstrcpyn(ppmu[0]->szExe, pe.szExeFile, 36);

						}
						else if (ppmu[1]->dwMemUsage <= pmc.WorkingSetSize)
						{
							ppm = ppmu[5];
							ppmu[5] = ppmu[4];
							ppmu[4] = ppmu[3];
							ppmu[3] = ppmu[2];
							ppmu[2] = ppmu[1];
							ppmu[1] = ppm;
							ppmu[1]->dwProcessID = pe.th32ProcessID;
							ppmu[1]->dwMemUsage = pmc.WorkingSetSize;
							lstrcpyn(ppmu[1]->szExe, pe.szExeFile, 36);

						}
						else if (ppmu[2]->dwMemUsage <= pmc.WorkingSetSize)
						{
							ppm = ppmu[5];
							ppmu[5] = ppmu[4];
							ppmu[4] = ppmu[3];
							ppmu[3] = ppmu[2];
							ppmu[2] = ppm;
							ppmu[2]->dwProcessID = pe.th32ProcessID;
							ppmu[2]->dwMemUsage = pmc.WorkingSetSize;
							lstrcpyn(ppmu[2]->szExe, pe.szExeFile, 36);

						}
						else if (ppmu[3]->dwMemUsage <= pmc.WorkingSetSize)
						{
							ppm = ppmu[5];
							ppmu[5] = ppmu[4];
							ppmu[4] = ppmu[3];
							ppmu[3] = ppm;
							ppmu[3]->dwProcessID = pe.th32ProcessID;
							ppmu[3]->dwMemUsage = pmc.WorkingSetSize;
							lstrcpyn(ppmu[3]->szExe, pe.szExeFile, 36);
						}
						else if (ppmu[4]->dwMemUsage <= pmc.WorkingSetSize)
						{
							ppm = ppmu[5];
							ppmu[5] = ppmu[4];
							ppmu[4] = ppm;
							ppmu[4]->dwProcessID = pe.th32ProcessID;
							ppmu[4]->dwMemUsage = pmc.WorkingSetSize;
							lstrcpyn(ppmu[4]->szExe, pe.szExeFile, 36);
						}
						else if (ppmu[5]->dwMemUsage <= pmc.WorkingSetSize)
						{
							ppmu[5]->dwProcessID = pe.th32ProcessID;
							ppmu[5]->dwMemUsage = pmc.WorkingSetSize;
							lstrcpyn(ppmu[5]->szExe, pe.szExeFile, 36);
						}
					}
					CloseHandle(hProc);
				}
			}
			ret = Process32Next(hs, &pe);
		}
		CloseHandle(hs);
	}
	nProcess = n;
	ReleaseSRWLockExclusive(&g_processLock);
	return n;
}
void DrawDisk(HDC mdc, LPRECT lpRect, double dwByte,BOOL bReadWrite)
{
	if (!mdc || !lpRect)
		return;
	// PDH values are floating point data from a provider boundary.  Reject
	// NaN/inf and clamp before converting f_byte*100 to int; an out-of-range
	// floating-point conversion is undefined and could corrupt the varargs
	// call below on a malformed counter response.
	if (!std::isfinite(dwByte) || dwByte < 0.0)
		dwByte = 0.0;
	const double maxDisplayBytes = ((double)INT_MAX - 1.0) * 1073741824.0 / 100.0;
	if (dwByte > maxDisplayBytes)
		dwByte = maxDisplayBytes;
	WCHAR szWriteS[] = L"X:";
	WCHAR szWriteS2[] = L"";
	WCHAR szReadS[] = L"R:";
	WCHAR szReadS2[] = L"";
	WCHAR* szT;
	if (bReadWrite)
	{
		if (TraySave.iMonitorSimple == 1)
			szT = szWriteS;
		else if (TraySave.iMonitorSimple == 2)
			szT = szWriteS2;
		else
			szT = TraySave.szDiskWriteSec;
	}
	else
	{
		if (TraySave.iMonitorSimple == 1)
			szT = szReadS;
		else if (TraySave.iMonitorSimple == 2)
			szT = szReadS2;
		else
			szT = TraySave.szDiskReadSec;
	}
	WCHAR sz[24];
	COLORREF rgb;
	if (dwByte/1024/1024 < TraySave.dNumValues2[0])
		rgb = TraySave.cMonitorColor[1];
	else if (dwByte/1024/1024 < TraySave.dNumValues2[1])
		rgb = TraySave.cMonitorColor[2];
	else
		rgb = TraySave.cMonitorColor[3];
	SetTextColor(mdc, rgb);
	double f_byte = dwByte;
	if (dwByte < 1048576000)
	{		
		f_byte /= 1048576;
		int m_byte = int(f_byte * 100);
		if (f_byte >= 100)
			wsprintf(sz, L"%dM",  m_byte / 100);
		else if (f_byte >= 10)
			wsprintf(sz, L"%d.%.1dM", m_byte / 100, (m_byte / 10) % 10);
		else
			wsprintf(sz, L"%d.%.2dM", m_byte / 100, m_byte % 100);
	}
	else
	{
		f_byte /= 1073741824;
		int g_byte = int(f_byte * 100);
		if (f_byte >= 100)
			wsprintf(sz, L"%dG", g_byte / 100);
		else if (f_byte >= 10)
			wsprintf(sz, L"%d.%.1dG", g_byte / 100, (g_byte / 10) % 10);
		else
			wsprintf(sz, L"%d.%.2dG", g_byte / 100, g_byte % 100);
	}
	DrawShadowText(mdc, szT, lstrlen(szT), lpRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
	DrawShadowText(mdc, sz, lstrlen(sz), lpRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
}
void DrawTraffic(HDC mdc, LPRECT lpRect, ULONG64 dwByte, BOOL bInOut)
{
	WCHAR szInS[] = L"↓:";
	WCHAR szInS2[] = L"";
	WCHAR szOutS[] = L"↑:";
	WCHAR szOutS2[] = L"";
	WCHAR* szT;
	if (bInOut)
	{
		if (TraySave.iMonitorSimple == 1)
			szT = szInS;
		else if (TraySave.iMonitorSimple == 2)
			szT = szInS2;
		else
			szT = TraySave.szTrafficIn;
	}
	else
	{
		if (TraySave.iMonitorSimple == 1)
			szT = szOutS;
		else if (TraySave.iMonitorSimple == 2)
			szT = szOutS2;
		else
			szT = TraySave.szTrafficOut;
	}
	WCHAR sz[24];
	COLORREF rgb;
	if (dwByte < TraySave.dNumValues[0])
		rgb = TraySave.cMonitorColor[1];
	else if (dwByte < TraySave.dNumValues[1])
		rgb = TraySave.cMonitorColor[2];
	else
		rgb = TraySave.cMonitorColor[3];
	SetTextColor(mdc, rgb);
	GetTrafficStr(sz, dwByte, HIWORD(TraySave.iUnit),LOWORD(TraySave.iUnit));
	DrawShadowText(mdc, szT, lstrlen(szT), lpRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
	DrawShadowText(mdc, sz, lstrlen(sz), lpRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
}
//extern "C" WINUSERAPI BOOL WINAPI TrackMouseEvent(LPTRACKMOUSEEVENT lpEventTrack);
BOOL bEvent = FALSE;//
BOOL SetTrackMouseEvent(HWND hWnd, DWORD dwFlags)
{
	TRACKMOUSEEVENT csTME;
	csTME.cbSize = sizeof(csTME);
	csTME.dwFlags = dwFlags;
	csTME.hwndTrack = hWnd;// 指定要 追踪 的窗口
	csTME.dwHoverTime = 300;  // 鼠标在按钮上停留超过 300ms ，才认为状态为 HOVER
	return TrackMouseEvent(&csTME);
}
INT_PTR CALLBACK TimeProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)//任务栏信息窗口过程
{
	switch (message)
	{
	case WM_INITDIALOG:
		return (INT_PTR)TRUE;
	case WM_ERASEBKGND:
	{
		//		PAINTSTRUCT ps;
		HDC hdc = (HDC)wParam;//BeginPaint(hDlg, &ps);		
		RECT rc{};
		if (!hdc || !GetClientRect(hDlg, &rc) || rc.right <= rc.left || rc.bottom <= rc.top)
			return TRUE;
		HDC mdc = CreateCompatibleDC(hdc);
		if (mdc)
		{
			HBITMAP hMemBmp = CreateCompatibleBitmap(hdc, rc.right - rc.left, rc.bottom - rc.top);
			if (!hMemBmp)
			{
				DeleteDC(mdc);
				return TRUE;
			}
			HBITMAP oldBmp = (HBITMAP)SelectObject(mdc, hMemBmp);
			if (!oldBmp || oldBmp == (HBITMAP)HGDI_ERROR)
			{
				DeleteObject(hMemBmp);
				DeleteDC(mdc);
				return TRUE;
			}

			COLORREF rgb(RGB(255, 255, 255));
			COLORREF cBack = RGB(0, 0, 1);
			if (bThemeMode != 0)
			{
				rgb = RGB(8, 8, 8);
				//			if(rovi.dwBuildNumber>22000)
				//				cBack = RGB(254, 254, 255);
			}
			if (hWin11UI)
			{
				HBRUSH hb = CreateSolidBrush(cBack);
				if (hb)
				{
					FillRect(mdc, &rc, hb);
					DeleteObject(hb);
				}
			}
			SYSTEMTIME systm;
			GetLocalTime(&systm);
			WCHAR sz[16];
			TCHAR szWeek[7][2] = { L"日",L"一",L"二",L"三",L"四",L"五",L"六" };

			int fsize;
			if (hWin11UI)
			{
				fsize = DPI(-11);
				wsprintf(sz, L"%.2d'%s", systm.wSecond, szWeek[systm.wDayOfWeek]);
			}
			else
			{
				fsize = DPI(-12);
				wsprintf(sz, L"%s%.2d:%.2d:%.2d", szWeek[systm.wDayOfWeek], systm.wHour, systm.wMinute, systm.wSecond);
			}
			HFONT hFont = CreateFont(fsize, 0, 0, 0, 0, false, false, false,
				DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
				CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
				DEFAULT_PITCH, L"微软雅黑");
			HFONT oldFont = NULL;
			if (hFont)
				oldFont = (HFONT)SelectObject(mdc, hFont);
			SetBkMode(mdc, TRANSPARENT);
			SetTextColor(mdc, rgb);
			if (!hWin11UI)
				DrawText(mdc, sz, lstrlen(sz), &rc, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
			else
				DrawText(mdc, sz, 4, &rc, DT_LEFT | DT_SINGLELINE | DT_BOTTOM);
			if (oldFont && oldFont != (HFONT)HGDI_ERROR)
				SelectObject(mdc, oldFont);
			if (hFont)
				DeleteObject(hFont);

			if (!hWin11UI)
			{
				BYTE* lpvBits = NULL;
				BITMAPINFO binfo;
				memset(&binfo, 0, sizeof(BITMAPINFO));
				binfo.bmiHeader.biBitCount = 32;     //每个像素多少位，也可直接写24(RGB)或者32(RGBA)
				binfo.bmiHeader.biCompression = 0;
				binfo.bmiHeader.biHeight = rc.bottom - rc.top;
				binfo.bmiHeader.biPlanes = 1;
				DWORD imageSize = 0;
				if (ComputeDibImageSize(rc.right - rc.left, rc.bottom - rc.top, &imageSize))
					binfo.bmiHeader.biSizeImage = imageSize;
				binfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
				binfo.bmiHeader.biWidth = rc.right - rc.left;
				if (binfo.bmiHeader.biSizeImage != 0)
					lpvBits = (BYTE*)HeapAlloc(GetProcessHeap(), 0, binfo.bmiHeader.biSizeImage);
				//		GetDIBits(mdc, hMemBmp, 0, rc.bottom - rc.top, lpvBits, &bmpInfo, DIB_RGB_COLORS);
				if (lpvBits && binfo.bmiHeader.biSizeImage >= 4)
				{
					if (GetDIBits(mdc, hMemBmp, 0, rc.bottom - rc.top, lpvBits, &binfo, DIB_RGB_COLORS) != 0)
					{
						for (DWORD i = 0; i + 4 <= binfo.bmiHeader.biSizeImage; i += 4)
						{
							if (lpvBits[i] > 3 || lpvBits[i + 1] != 0 || lpvBits[i + 2] != 0)
								lpvBits[i + 3] = 0x80;
						}
						SetDIBits(mdc, hMemBmp, 0, rc.bottom - rc.top, lpvBits, &binfo, DIB_RGB_COLORS);
					}
				}
				if (lpvBits)
					HeapFree(GetProcessHeap(), 0, lpvBits);
			}
			BitBlt(hdc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, mdc, 0, 0, SRCCOPY);
			if (oldBmp && oldBmp != (HBITMAP)HGDI_ERROR)
				SelectObject(mdc, oldBmp);
			DeleteObject(hMemBmp);
			DeleteDC(mdc);
		}
		return TRUE;
	}
	break;
	}
	return FALSE;
}

INT_PTR CALLBACK TaskBarProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)//任务栏信息窗口过程
{
	switch (message)
	{
	case WM_INITDIALOG:		
		return (INT_PTR)TRUE;
	case WM_COMMAND:
		if (LOWORD(wParam) >= IDC_SELECT_ALL && LOWORD(wParam) <= IDC_SELECT_ALL + 99)
		{
			if (LOWORD(wParam) == IDC_SELECT_ALL)
				TraySave.AdpterName[0] = L'\0';
			else
			{
				int x = LOWORD(wParam) - IDC_SELECT_ALL;
				AcquireSRWLockShared(&g_networkLock);
				PIP_ADAPTER_ADDRESSES paa = piaa;
				int n = 1;
				DWORD adapterWalk = 0;
				while (paa && adapterWalk++ < (DWORD)kMaxTrafficEntries * 4u)
				{
					if (paa->IfType != IF_TYPE_SOFTWARE_LOOPBACK && paa->IfType != IF_TYPE_TUNNEL)
					{
						if (n == x)
						{
							if (paa->AdapterName)
								lstrcpynA(TraySave.AdpterName, paa->AdapterName, ARRAYSIZE(TraySave.AdpterName));
							break;
						}
						n++;
					}
					paa = paa->Next;
				}
				ReleaseSRWLockShared(&g_networkLock);
			}
			WriteReg();
			AcquireSRWLockExclusive(&g_networkLock);
			if (TrayData)
			{
				TrayData->m_last_in_bytes = 0;
				TrayData->m_last_out_bytes = 0;
				TrayData->s_in_byte = 0;
				TrayData->s_out_byte = 0;
			}
			ReleaseSRWLockExclusive(&g_networkLock);
		}
		else if (LOWORD(wParam) >= IDC_DISK_ALL && LOWORD(wParam) <= IDC_DISK_ALL + 99)
		{
			if (LOWORD(wParam) == IDC_DISK_ALL)
				TraySave.szDisk = L'\0';
			else
			{
				TraySave.szDisk = LOWORD(wParam) - IDC_DISK_ALL;
			}
			WriteReg();
			SwitchPDH(FALSE);
			SwitchPDH(TRUE);
			if (TrayData)
			{
				TrayData->diskreadbyte = 0;
				TrayData->diskwritebyte = 0;
				TrayData->disktime = 0;
			}
		}
		break;
	case WM_MOUSEMOVE:
		if (bEvent == FALSE && (TraySave.bMonitorTips||TraySave.bMonitorPrice))
		{
			SetTrackMouseEvent(hTaskBar, TME_LEAVE | TME_HOVER);
			bEvent = TRUE;
		}
		break;
	case WM_MOUSEHOVER:
	{
		POINT pt;
		RECT rc;
		GetCursorPos(&pt);
		GetWindowRect(hTaskBar,&rc);
		BOOL bV = FALSE;
		if (rc.bottom - rc.top > rc.right - rc.left)
			bV = TRUE;
		if (TraySave.bMonitorPrice&&((pt.x > rc.right - wPrice&&!bV)||(pt.y>rc.bottom-wHeight*2-(TraySave.bTwoFour*wHeight*2) && bV)))
		{
			if (IsWindow(hTaskTips))
			{
				AcquireSRWLockExclusive(&g_processLock);
				ClearProcessSnapshotUnlocked();
				ReleaseSRWLockExclusive(&g_processLock);
				DestroyWindow(hTaskTips);
			}
			if (!IsWindow(hPrice))
			{
				hPrice = ::CreateDialog(hInst, MAKEINTRESOURCE(IDD_PRICE), NULL, (DLGPROC)PriceProc);
				if (hPrice && TraySave.bTwoFour)
				{
					RECT rc;
					GetWindowRect(hPrice, &rc);
					SetWindowPos(hPrice, HWND_TOPMOST, 0, 0, (rc.right - rc.left) * 2, rc.bottom - rc.top, SWP_NOMOVE);
				}
				if (hPrice)
					SendMessage(hPrice, WM_COMMAND, IDC_TWOFOUR, 888);
			}
		}
		else if (!IsWindowVisible(hTaskTips)&&TraySave.bMonitorTips)
		{
/*
			if (s_in_byte == 0)
				return FALSE;
*/			
			if (IsWindow(hPrice))
				DestroyWindow(hPrice);
			if (!IsWindow(hTaskTips))
			{
				hTaskTips = ::CreateDialog(hInst, MAKEINTRESOURCE(IDD_TIPS), NULL, (DLGPROC)TaskTipsProc);
				if (hTaskTips)
					SetLayeredWindowAttributes(hTaskTips, 0, 255, LWA_ALPHA);
			}
			if (!IsWindow(hTaskTips))
				break;
			AcquireSRWLockShared(&g_networkLock);
			int trafficCount = nTraffic;
			ReleaseSRWLockShared(&g_networkLock);
			int processCount = GetProcessMemUsage();
			AcquireSRWLockExclusive(&g_processLock);
			EnsureProcessTimeBufferUnlocked(processCount);
			ReleaseSRWLockExclusive(&g_processLock);
			GetProcessCpuUsage();
			HDC mdc = GetDC(hMain);
			SIZE tSize = { 160, 16 };
			if (mdc)
			{
				TraySave.TipsFont.lfHeight = DPI(TraySave.TipsFontSize);
				HFONT hTipsFont = CreateFontIndirect(&TraySave.TipsFont); //创建字体
				HFONT oldFont = NULL;
				if (hTipsFont)
					oldFont = (HFONT)SelectObject(mdc, hTipsFont);
				if (!GetTextExtentPoint(mdc, L"虚拟内存虚拟内存虚拟内存虚拟内存虚拟内存虚拟内存虚拟内存虚拟内存虚拟内存", 36, &tSize) ||
					tSize.cx <= 0 || tSize.cy <= 0)
					tSize = { 160, 16 };
				if (oldFont && oldFont != (HFONT)HGDI_ERROR)
					SelectObject(mdc, oldFont);
				if (hTipsFont)
					DeleteObject(hTipsFont);
				ReleaseDC(hMain, mdc);
			}
			int x, y, w, h;
			w = tSize.cx > 0 ? tSize.cx : 160;
			wTipsHeight = tSize.cy > 0 ? tSize.cy : 16;
			if (trafficCount < 0)
				trafficCount = 0;
			if (trafficCount > kMaxTrafficEntries)
				trafficCount = kMaxTrafficEntries;
			SIZE_T tipHeight = (SIZE_T)wTipsHeight * (SIZE_T)(trafficCount + 14);
			if (tipHeight == 0 || tipHeight > INT_MAX || w > INT_MAX)
			{
				ShowWindow(hTaskTips, SW_HIDE);
				return TRUE;
			}
			h = (int)tipHeight;
			RECT wrc{}, src{};
			if (!GetWindowRect(hDlg, &wrc) || !GetScreenRectSafe(hDlg, &src, TRUE))
				return TRUE;
			if (wrc.bottom + h > src.bottom)
				y = wrc.top - h;
			else
				y = wrc.bottom;
			if (wrc.right - (wrc.right - wrc.left) / 2 + w / 2 > src.right)
				x = src.right - w;
			else if (wrc.right - (wrc.right - wrc.left) / 2 - w / 2 < src.left)
				x = src.left;
			else
				x = wrc.right - (wrc.right - wrc.left) / 2 - w / 2;
			SetWindowPos(hTaskTips, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
			HRGN hRgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, 11, 11);
			if (hRgn)
			{
				if (!SetWindowRgn(hTaskTips, hRgn, FALSE))
					DeleteObject(hRgn);
			}
//			SetCursorPos(2498, 1398);
//			mouse_event(MOUSEEVENTF_MOVE, 2380, 1398, 0, 0);
//			mouse_event(MOUSEEVENTF_LEFTDOWN, 2500, 1398, 0, 0);
//			mouse_event(MOUSEEVENTF_LEFTUP, 2500, 1398, 0, 0);
		}
	}
	break;
	case WM_MOUSELEAVE:
		bEvent = FALSE;
		break;
	case  WM_RBUTTONDOWN:
	{
		POINT pt;
		RECT rc;
		GetCursorPos(&pt);
		ScreenToClient(hDlg, &pt);
		GetClientRect(hTaskBar, &rc);
		BOOL bV = FALSE;
		if (rc.bottom - rc.top > rc.right - rc.left)
		{
			bV = TRUE;
			rc.bottom = wHeight * 2;
		}
		else
			rc.right = wTraffic;
		if (TraySave.bMonitorTraffic&&PtInRect(&rc,pt))
		{
			ShowSelectMenu(TRUE);
			return true;
		}
		if (bV)
		{
			OffsetRect(&rc, 0, (TraySave.bMonitorTraffic + TraySave.bMonitorUsage + TraySave.bMonitorTemperature) * 2 * wHeight);
			if (!bRing0)
				OffsetRect(&rc, 0, -wHeight);
			if (hOHMA && TraySave.bMonitorTemperature)
				rc.bottom += wHeight * 2;
		}
		else
		{
			OffsetRect(&rc, wTraffic + wUsage + wTemperature, 0);
			rc.right = rc.left + wDisk;
		}
		if (TraySave.bMonitorDisk && PtInRect(&rc, pt))
		{
			ShowSelectMenu(FALSE);
			return true;
		}
		if (bV)
		{
			OffsetRect(&rc, 0, wHeight*2);
		}
		else
		{
			OffsetRect(&rc, wDisk, 0);
			rc.right = rc.left + wTime;
		}
		if (TraySave.bMonitorTime && PtInRect(&rc, pt))
			RunProcess(NULL, szTimeDateCpl);
		else
			OpenSetting();
		return TRUE;
	}
	break;
	case WM_LBUTTONDOWN:
	{
		if (TraySave.bMonitorFloat)
		{
			bTaskBarMoveing = TRUE;
			PostMessage(hDlg, WM_NCLBUTTONDOWN, HTCAPTION, lParam);
			SetTimer(hDlg, 11, 1000, NULL);
		}
		return TRUE;
		/*
				else
					RunProcess(szNetCpl);
		*/
	}
	break;
	case WM_LBUTTONUP:
		if (!TraySave.bMonitorFloat)
		{
/*
			ShowWindow(hDlg, SW_HIDE);
			Sleep(100);
			POINT pt;
			GetCursorPos(&pt);
			mouse_event(MOUSEEVENTF_LEFTDOWN, pt.x, pt.y, 0, 0);
			mouse_event(MOUSEEVENTF_LEFTUP, pt.x, pt.y, 0, 0);
			SetTimer(hDlg, 9, 3000, NULL);
			ShowWindow(hTaskTips,SW_HIDE);
			return TRUE;
*/
		}
		break;
	case WM_TIMER:
		if (wParam == 11)
		{
			if (!KEYDOWN(VK_LBUTTON))
			{
				if (TraySave.bMonitorFloat && bTaskBarMoveing)
				{
					RECT wrc;
					GetWindowRect(hDlg, &wrc);
					TraySave.dMonitorPoint.x = wrc.left;
					TraySave.dMonitorPoint.y = wrc.top;
					WriteReg();
					bTaskBarMoveing = FALSE;
					KillTimer(hDlg, wParam);
				}
			}
		}
		else if (wParam == 9)
		{
			KillTimer(hDlg, wParam);
			ShowWindow(hDlg, SW_SHOWNOACTIVATE);
		}
		else if (wParam == 5)////////////////////////////////////////////////光标移出弹出式菜单自动隐藏菜单
		{

			HWND hMenu = FindWindow(L"#32768", NULL);
			POINT pt;
			GetCursorPos(&pt);
			if (WindowFromPoint(pt) != hMenu)
			{
				KillTimer(hDlg, wParam);
				PostMessage(hMenu, WM_CLOSE, NULL, NULL);
			}
		}
		else if (wParam == 3)
		{
			if (TraySave.bSecond)
			{
				if (IsWindow(hTime))
					::InvalidateRect(hTime, NULL, TRUE);
			}
			if (IsWindow(hTaskBar))
				::InvalidateRect(hTaskBar, NULL, TRUE);
			if (TraySave.bMonitorPrice)
			{
				TrayData->iPriceUpDown[0] = 0;
				TrayData->iPriceUpDown[1] = 0;
				if (TrayData->fLastPrice1 != 0)
				{

					if (TraySave.HighRemind[0] != 0 && TraySave.HighRemind[0] < TrayData->fLastPrice1 && TraySave.bCheckHighRemind[0])
						TrayData->iPriceUpDown[0] = MB_ICONHAND;
					if (TraySave.HighRemind[1]!=0&&TraySave.HighRemind[1] < TrayData->fLastPrice1 && TraySave.bCheckHighRemind[1])
						TrayData->iPriceUpDown[0] = MB_ICONHAND;
					if (TraySave.HighRemind[2] != 0 && TraySave.HighRemind[2] < TrayData->fLastPrice1 && TraySave.bCheckHighRemind[2])
						TrayData->iPriceUpDown[0] = MB_ICONHAND;
					if (TraySave.LowRemind[0] != 0 && TraySave.LowRemind[0] > TrayData->fLastPrice1 && TraySave.bCheckLowRemind[0])
						TrayData->iPriceUpDown[0] = MB_ICONWARNING;
					if (TraySave.LowRemind[1] != 0 && TraySave.LowRemind[1] > TrayData->fLastPrice1 && TraySave.bCheckLowRemind[1])
						TrayData->iPriceUpDown[0] = MB_ICONWARNING;
					if (TraySave.LowRemind[2] != 0 && TraySave.LowRemind[2] > TrayData->fLastPrice1 && TraySave.bCheckLowRemind[2])
						TrayData->iPriceUpDown[0] = MB_ICONWARNING;
					if (TrayData->iPriceUpDown[0] != 0)
						MessageBeep(TrayData->iPriceUpDown[0]);
				}
				if (TrayData->fLastPrice2 != 0)
				{
					if (TraySave.HighRemind[3]!=0&&TraySave.HighRemind[3] < TrayData->fLastPrice2 && TraySave.bCheckHighRemind[3])
						TrayData->iPriceUpDown[1] = MB_ICONHAND;
					if (TraySave.HighRemind[4] != 0 && TraySave.HighRemind[4] < TrayData->fLastPrice2 && TraySave.bCheckHighRemind[4])
						TrayData->iPriceUpDown[1] = MB_ICONHAND;
					if (TraySave.HighRemind[5] != 0 && TraySave.HighRemind[5] < TrayData->fLastPrice2 && TraySave.bCheckHighRemind[5])
						TrayData->iPriceUpDown[1] = MB_ICONHAND;
					if (TraySave.LowRemind[3]!=0&&TraySave.LowRemind[3] > TrayData->fLastPrice2 && TraySave.bCheckLowRemind[3])
						TrayData->iPriceUpDown[1] = MB_ICONWARNING;
					if (TraySave.LowRemind[4] != 0 && TraySave.LowRemind[4] > TrayData->fLastPrice2 && TraySave.bCheckLowRemind[4])
						TrayData->iPriceUpDown[1] = MB_ICONWARNING;
					if (TraySave.LowRemind[5] != 0 && TraySave.LowRemind[5] > TrayData->fLastPrice2 && TraySave.bCheckLowRemind[5])
						TrayData->iPriceUpDown[1] = MB_ICONWARNING;
					if (TrayData->iPriceUpDown[1] != 0)
						MessageBeep(TrayData->iPriceUpDown[1]);
				}
				if (TraySave.bTwoFour)
				{
					TrayData->iPriceUpDown[2] = 0;
					TrayData->iPriceUpDown[3] = 0;
					if (TrayData->fLastPrice3 != 0)
					{

						if (TraySave.HighRemind[6] != 0 && TraySave.HighRemind[6] < TrayData->fLastPrice3 && TraySave.bCheckHighRemind[6])
							TrayData->iPriceUpDown[2] = MB_ICONHAND;
						if (TraySave.HighRemind[7] != 0 && TraySave.HighRemind[7] < TrayData->fLastPrice3 && TraySave.bCheckHighRemind[7])
							TrayData->iPriceUpDown[2] = MB_ICONHAND;
						if (TraySave.HighRemind[8] != 0 && TraySave.HighRemind[8] < TrayData->fLastPrice3 && TraySave.bCheckHighRemind[8])
							TrayData->iPriceUpDown[2] = MB_ICONHAND;
						if (TraySave.LowRemind[6] != 0 && TraySave.LowRemind[6] > TrayData->fLastPrice3 && TraySave.bCheckLowRemind[6])
							TrayData->iPriceUpDown[2] = MB_ICONWARNING;
						if (TraySave.LowRemind[7] != 0 && TraySave.LowRemind[7] > TrayData->fLastPrice3 && TraySave.bCheckLowRemind[7])
							TrayData->iPriceUpDown[2] = MB_ICONWARNING;
						if (TraySave.LowRemind[8] != 0 && TraySave.LowRemind[8] > TrayData->fLastPrice3 && TraySave.bCheckLowRemind[8])
							TrayData->iPriceUpDown[2] = MB_ICONWARNING;
						if (TrayData->iPriceUpDown[2] != 0)
							MessageBeep(TrayData->iPriceUpDown[2]);
					}
					if (TrayData->fLastPrice4 != 0)
					{
						if (TraySave.HighRemind[9] != 0 && TraySave.HighRemind[9] < TrayData->fLastPrice4 && TraySave.bCheckHighRemind[9])
							TrayData->iPriceUpDown[3] = MB_ICONHAND;
						if (TraySave.HighRemind[10] != 0 && TraySave.HighRemind[10] < TrayData->fLastPrice4 && TraySave.bCheckHighRemind[10])
							TrayData->iPriceUpDown[3] = MB_ICONHAND;
						if (TraySave.HighRemind[11] != 0 && TraySave.HighRemind[11] < TrayData->fLastPrice4 && TraySave.bCheckHighRemind[11])
							TrayData->iPriceUpDown[3] = MB_ICONHAND;
						if (TraySave.LowRemind[9] != 0 && TraySave.LowRemind[9] > TrayData->fLastPrice4 && TraySave.bCheckLowRemind[9])
							TrayData->iPriceUpDown[3] = MB_ICONWARNING;
						if (TraySave.LowRemind[10] != 0 && TraySave.LowRemind[10] > TrayData->fLastPrice4 && TraySave.bCheckLowRemind[10])
							TrayData->iPriceUpDown[3] = MB_ICONWARNING;
						if (TraySave.LowRemind[11] != 0 && TraySave.LowRemind[11] > TrayData->fLastPrice4 && TraySave.bCheckLowRemind[11])
							TrayData->iPriceUpDown[3] = MB_ICONWARNING;
						if (TrayData->iPriceUpDown[3] != 0)
							MessageBeep(TrayData->iPriceUpDown[3]);
					}
				}
			}
			if (TraySave.bSound)
			{
				if (TraySave.bMonitorTraffic)
					if (TraySave.dNumValues[8] != 0 && (TrayData->s_in_byte > TraySave.dNumValues[8] || TrayData->s_out_byte > TraySave.dNumValues[8]))
						MessageBeep(MB_ICONHAND);
				if (TraySave.bMonitorTemperature)
					if (TraySave.dNumValues[9] != 0 && ((DWORD)TrayData->iTemperature1 > TraySave.dNumValues[9] || (DWORD)TrayData->iTemperature2 > TraySave.dNumValues[9]))
						MessageBeep(MB_ICONHAND);
				if (TraySave.bMonitorUsage)
					if ((TraySave.dNumValues[10] != 0 && (DWORD)iCPU > TraySave.dNumValues[10]) || (TraySave.dNumValues[11] != 0 && MemoryStatusEx.dwMemoryLoad > TraySave.dNumValues[11]))
						MessageBeep(MB_ICONHAND);
				if (TraySave.bMonitorDisk)
					if (TraySave.dNumValues2[2] != 0 && (TrayData->diskreadbyte / 1024 / 1024 > TraySave.dNumValues2[2] || TrayData->diskwritebyte / 1024 / 1024 > TraySave.dNumValues2[2]))
						MessageBeep(MB_ICONHAND);
			}
			POINT pt;
			GetCursorPos(&pt);
			RECT brc;
			GetWindowRect(hTaskBar, &brc);
			if (PtInRect(&brc, pt))
			{
				if (rovi.dwBuildNumber >= 22000 && !TraySave.bMonitorFloat)//&&TraySave.cMonitorColor[0]==RGB(0,0,1))
				{
					if (!bFullScreen)
					{

						if (!KEYDOWN(VK_LBUTTON) && !IsWindow(hTaskTips) && !IsWindow(hPrice))
						{
							SendMessage(hTaskBar, WM_MOUSEHOVER, 0, 0);
						}
					}
				}
				break;
			}
			if (TraySave.bMonitorTips)
			{
				if (IsWindow(hTaskTips))
				{
					GetWindowRect(hTaskTips, &brc);
					if (!PtInRect(&brc, pt))
					{
						DestroyWindow(hTaskTips);
					}
				}
			}
			if (TraySave.bMonitorPrice)
			{
				if (IsWindow(hPrice))
				{
					GetWindowRect(hPrice, &brc);
					if (!PtInRect(&brc, pt))
						DestroyWindow(hPrice);
				}
			}
		}
	case WM_ERASEBKGND:
	{
		//		PAINTSTRUCT ps;
		HDC hdc = (HDC)wParam;//BeginPaint(hDlg, &ps);		
		RECT initialRect{};
		if (!hdc || !GetClientRect(hDlg, &initialRect) || initialRect.right <= initialRect.left || initialRect.bottom <= initialRect.top)
			return TRUE;
		HDC mdc = CreateCompatibleDC(hdc);
		if (mdc)
		{
			RECT rc = initialRect;
			HBITMAP hMemBmp = CreateCompatibleBitmap(hdc, rc.right - rc.left, rc.bottom - rc.top);
			if (!hMemBmp)
			{
				DeleteDC(mdc);
				return TRUE;
			}
			HBITMAP oldBmp = (HBITMAP)SelectObject(mdc, hMemBmp);
			if (!oldBmp || oldBmp == (HBITMAP)HGDI_ERROR)
			{
				DeleteObject(hMemBmp);
				DeleteDC(mdc);
				return TRUE;
			}
			//		if (TraySave.cMonitorColor[0] != 0)
			{
				HBRUSH hb = NULL;
				if (TraySave.cMonitorColor[0] != RGB(0, 0, 1)&& TraySave.cMonitorColor[0] != RGB(0, 0, 2))
					hb = CreateSolidBrush(TraySave.cMonitorColor[0]);
				else
				{

					/*
									if(bThemeMode&&!TraySave.bMonitorFloat&&rovi.dwBuildNumber>22000)
										hb=CreateSolidBrush(RGB(222,222,223));
									else
					*/
					if ((rovi.dwBuildNumber > 25000||!TraySave.bTrayStyle)&&!TraySave.bMonitorFloat)
					{
						COLORREF cPixel = GetWindowPixel(hTray);
						if (cPixel != oPixelColor)
						{
							oPixelColor = cPixel;
							SetLayeredWindowAttributes(hTaskBar, cPixel, 0, LWA_COLORKEY);
						}
						hb = CreateSolidBrush(oPixelColor);
					}
					else
						hb = CreateSolidBrush(RGB(0, 0, 1));
				}
				if (hb)
				{
					FillRect(mdc, &rc, hb);
					DeleteObject(hb);
				}
			}
			//		if (bErasebkgnd)
			{
//				InflateRect(&rc, -1, -1);
				if(VTray)
					InflateRect(&rc, -wSpace/2,0);
				HFONT oldFont = NULL;
				if (hFont)
					oldFont = (HFONT)SelectObject(mdc, hFont);
				WCHAR sz[16];
				SetBkMode(mdc, TRANSPARENT);
				COLORREF rgb;
				if (TraySave.bMonitorTraffic)
				{
					RECT crc = rc;
					if (VTray)
					{
						crc.bottom = wHeight;
						if (TraySave.bMonitorTrafficUpDown)
							DrawTraffic(mdc, &crc, TrayData->s_out_byte, FALSE);
						else
							DrawTraffic(mdc, &crc, TrayData->s_in_byte, TRUE);
						OffsetRect(&crc, 0, wHeight);
						if (TraySave.bMonitorTrafficUpDown)
							DrawTraffic(mdc, &crc, TrayData->s_in_byte, TRUE);
						else
							DrawTraffic(mdc, &crc, TrayData->s_out_byte, FALSE);
					}
					else
					{
						crc.right = crc.left + wTraffic;
						crc.bottom /= 2;
						InflateRect(&crc, -wSpace / 2, 0);
						if (TraySave.bMonitorTrafficUpDown)
							DrawTraffic(mdc, &crc, TrayData->s_out_byte, FALSE);
						else
							DrawTraffic(mdc, &crc, TrayData->s_in_byte, TRUE);
						OffsetRect(&crc, 0, crc.bottom);
						if (TraySave.bMonitorTrafficUpDown)
							DrawTraffic(mdc, &crc, TrayData->s_in_byte, TRUE);
						else
							DrawTraffic(mdc, &crc, TrayData->s_out_byte, FALSE);
					}
				}
				if (TraySave.bMonitorUsage)
				{
					if (iCPU <= TraySave.dNumValues[4])
						rgb = TraySave.cMonitorColor[4];
					else if (iCPU <= TraySave.dNumValues[5])
						rgb = TraySave.cMonitorColor[5];
					else
						rgb = TraySave.cMonitorColor[6];
					SetTextColor(mdc, rgb);
					/*
								if(bRing0)
									swprintf_s(sz, 16, L"%.2d%%", iCPU);
								else
					*/
					if (TraySave.iMonitorSimple == 1)
						wsprintf(sz, L"%.2d%%", iCPU);
					else if (TraySave.iMonitorSimple == 2)
						wsprintf(sz, L"%.2d", iCPU);
					else
						wsprintf(sz, L"%.2d%s",  iCPU, TraySave.szUsageCPUUnit);

					int sLen = lstrlen(sz);
					RECT crc = rc;
					if (VTray)
					{
						if (TraySave.bMonitorTraffic)
							crc.top = wHeight * 2;
						crc.bottom = crc.top + wHeight;
						
					}
					else
					{
						crc.left = crc.left + wTraffic;
						crc.right = crc.left + wUsage;
						crc.bottom /= 2;
						InflateRect(&crc, -(wSpace / 2), 0);
					}
					if (TraySave.iMonitorSimple == 0)
						DrawShadowText(mdc, TraySave.szUsageCPU, lstrlen(TraySave.szUsageCPU), &crc, DT_LEFT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
					DrawShadowText(mdc, sz, sLen, &crc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
					/*
								if(bRing0)
									swprintf_s(sz, 16, L"%.2d%%", MemoryStatusEx.dwMemoryLoad);
								else
					*/
					if (TraySave.iMonitorSimple == 1)
						wsprintf(sz, L"%.2d%%", MemoryStatusEx.dwMemoryLoad);
					else if (TraySave.iMonitorSimple == 2)
						wsprintf(sz, L"%.2d", MemoryStatusEx.dwMemoryLoad);
					else
						wsprintf(sz, L"%.2d%s", MemoryStatusEx.dwMemoryLoad, TraySave.szUsageMEMUnit);
					sLen = lstrlen(sz);
					if (MemoryStatusEx.dwMemoryLoad <= TraySave.dNumValues[6])
						rgb = TraySave.cMonitorColor[4];
					else if (MemoryStatusEx.dwMemoryLoad <= TraySave.dNumValues[7])
						rgb = TraySave.cMonitorColor[5];
					else
						rgb = TraySave.cMonitorColor[6];
					SetTextColor(mdc, rgb);
					if (VTray)
						OffsetRect(&crc, 0, wHeight);						
					else
						OffsetRect(&crc, 0, crc.bottom);
					if (TraySave.iMonitorSimple == 0)
						DrawShadowText(mdc, TraySave.szUsageMEM, lstrlen(TraySave.szUsageMEM), &crc, DT_LEFT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
					DrawShadowText(mdc, sz, (int)sLen, &crc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
				}
				if (TraySave.bMonitorTemperature)
				{
					RECT crc = rc;
					if (VTray)
					{
						if (TraySave.bMonitorTraffic)
							crc.top = wHeight * 2;
						if (TraySave.bMonitorUsage)
							crc.top += wHeight * 2;
						crc.bottom = crc.top + wHeight;
					}
					else
					{
						crc.left += wTraffic + wUsage;
						crc.right = crc.left + wTemperature;
						crc.bottom /= 2;
						InflateRect(&crc, -(wSpace / 2), 0);
					}
					if (bRing0)
					{
						if ((hATIDLL != NULL || hNVDLL != NULL )&& TrayData->iTemperature1 == 0 && TraySave.bMonitorDisk&&!hOHMA&&!HasAcpiTemperaturePath())
							TrayData->iTemperature1 = TrayData->disktime;
						if (TrayData->iTemperature1 <= TraySave.dNumValues[2])
							rgb = TraySave.cMonitorColor[4];
						else if (TrayData->iTemperature1 <= TraySave.dNumValues[3])
							rgb = TraySave.cMonitorColor[5];
						else
							rgb = TraySave.cMonitorColor[6];
						SetTextColor(mdc, rgb);
						if ((hATIDLL != NULL || hNVDLL != NULL )&& TrayData->iTemperature1 == TrayData->disktime && TraySave.bMonitorDisk&&!hOHMA&&!HasAcpiTemperaturePath())
						{
							if (TraySave.iMonitorSimple == 0)
							{
								DrawShadowText(mdc, TraySave.szDiskName, lstrlen(TraySave.szDiskName), &crc, DT_LEFT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
								wsprintf(sz, L"%.2d%s", TrayData->iTemperature1, TraySave.szUsageMEMUnit);
							}
							else if (TraySave.iMonitorSimple == 1)
								wsprintf(sz, L"%.2d%%", TrayData->iTemperature1);
							else
								wsprintf(sz, L"%.2d", TrayData->iTemperature1);
						}
						else
						{
							if (TraySave.iMonitorSimple == 1)
								wsprintf(sz, L"%.2d℃", TrayData->iTemperature1);
							else if (TraySave.iMonitorSimple == 2)
								wsprintf(sz, L"%.2d", TrayData->iTemperature1);
							else
							{
								DrawShadowText(mdc, TraySave.szTemperatureCPU, lstrlen(TraySave.szTemperatureCPU), &crc, DT_LEFT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
								wsprintf(sz, L"%.2d%s", TrayData->iTemperature1, TraySave.szTemperatureCPUUnit);
							}
						}
						DrawShadowText(mdc, sz, lstrlen(sz), &crc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
					}
					if (bRing0)
					{
						if (VTray)
							OffsetRect(&crc, 0, wHeight);
						else
							OffsetRect(&crc, 0, crc.bottom);
					}
					else
					{
						if (VTray)
						{
							//						crc.bottom += wHeight;
						}
						else
							crc.bottom += (crc.bottom - crc.top);
					}
					if (hATIDLL == NULL && hNVDLL == NULL && TraySave.bMonitorDisk&&!hOHMA&&!HasAcpiTemperaturePath()&&TrayData->disktime!=0)//如果没有独立显卡则显示磁盘使用率
						TrayData->iTemperature2 = TrayData->disktime;
					if (TrayData->iTemperature2 <= TraySave.dNumValues[2])
						rgb = TraySave.cMonitorColor[4];
					else if (TrayData->iTemperature2 <= TraySave.dNumValues[3])
						rgb = TraySave.cMonitorColor[5];
					else
						rgb = TraySave.cMonitorColor[6];
					SetTextColor(mdc, rgb);
					if (hATIDLL == NULL && hNVDLL == NULL && TraySave.bMonitorDisk&&!hOHMA&&!HasAcpiTemperaturePath())
					{
						if (TraySave.iMonitorSimple == 0)
						{
							DrawShadowText(mdc, TraySave.szDiskName, lstrlen(TraySave.szDiskName), &crc, DT_LEFT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
							wsprintf(sz, L"%.2d%s" , TrayData->iTemperature2, TraySave.szUsageMEMUnit);
						}
						else if (TraySave.iMonitorSimple == 1)
							wsprintf(sz, L"%.2d%%", TrayData->iTemperature2);
						else
							wsprintf(sz, L"%.2d", TrayData->iTemperature2);
					}
					else
					{
						if (TraySave.iMonitorSimple == 0)
						{
							DrawShadowText(mdc, TraySave.szTemperatureGPU, lstrlen(TraySave.szTemperatureGPU), &crc, DT_LEFT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
							wsprintf(sz, L"%.2d%s", TrayData->iTemperature2, TraySave.szTemperatureGPUUnit);
						}
						else if (TraySave.iMonitorSimple == 1)
							wsprintf(sz, L"%.2d℃", TrayData->iTemperature2);
						else
							wsprintf(sz, L"%.2d", TrayData->iTemperature2);
					}
					DrawShadowText(mdc, sz, lstrlen(sz), &crc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);

				}
				if (TraySave.bMonitorDisk)
				{
					RECT crc = rc;
					if (VTray)
					{
						if (TraySave.bMonitorTraffic)
							crc.top = wHeight * 2;
						if (TraySave.bMonitorTemperature)
						{
							crc.top += wHeight;
							if (bRing0)
								crc.top += wHeight;
						}
						if (TraySave.bMonitorUsage)
							crc.top += wHeight * 2;
						crc.bottom = crc.top + wHeight;
						if (hOHMA && TraySave.bMonitorTemperature)
						{
							if (TrayData->iHddTemperature <= TraySave.dNumValues[2])
								rgb = TraySave.cMonitorColor[4];
							else if (TrayData->iHddTemperature <= TraySave.dNumValues[3])
								rgb = TraySave.cMonitorColor[5];
							else
								rgb = TraySave.cMonitorColor[6];
							SetTextColor(mdc, rgb);
							if (TraySave.iMonitorSimple == 0)
							{
								DrawShadowText(mdc, TraySave.szDiskName, lstrlen(TraySave.szDiskName), &crc, DT_LEFT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
								wsprintf(sz, L"%.2d%s", TrayData->iHddTemperature, TraySave.szTemperatureCPUUnit);
							}
							else if (TraySave.iMonitorSimple == 1)
								wsprintf(sz, L"%.2d℃", TrayData->iHddTemperature);
							else
								wsprintf(sz, L"%.2d", TrayData->iHddTemperature);
							DrawShadowText(mdc, sz, lstrlen(sz), &crc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
							OffsetRect(&crc, 0, wHeight);
							if (TrayData->disktime <= TraySave.dNumValues[6])
								rgb = TraySave.cMonitorColor[4];
							else if (TrayData->disktime <= TraySave.dNumValues[7])
								rgb = TraySave.cMonitorColor[5];
							else
								rgb = TraySave.cMonitorColor[6];
							SetTextColor(mdc, rgb);
							if (TraySave.iMonitorSimple == 0)
							{
								DrawShadowText(mdc, TraySave.szDiskName, lstrlen(TraySave.szDiskName), &crc, DT_LEFT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
								wsprintf(sz, L"%.2d%s", TrayData->disktime, TraySave.szUsageMEMUnit);
							}
							else if (TraySave.iMonitorSimple == 1)
								wsprintf(sz, L"%.2d%%", TrayData->disktime);
							else
								wsprintf(sz, L"%.2d", TrayData->disktime);
							DrawShadowText(mdc, sz, lstrlen(sz), &crc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
							OffsetRect(&crc, 0, wHeight);
						}
						if (TraySave.bMonitorTrafficUpDown)
							DrawDisk(mdc, &crc, TrayData->diskreadbyte, FALSE);
						else
							DrawDisk(mdc, &crc, TrayData->diskwritebyte, TRUE);
						OffsetRect(&crc, 0, wHeight);
						if (TraySave.bMonitorTrafficUpDown)
							DrawDisk(mdc, &crc, TrayData->diskwritebyte, TRUE);
						else
							DrawDisk(mdc, &crc, TrayData->diskreadbyte, FALSE);
					}
					else
					{
						crc.left = crc.left + wTraffic + wTemperature + wUsage;
						crc.right = crc.left + wDisk;
						crc.bottom /= 2;
						if (hOHMA&&TraySave.bMonitorTemperature)
						{
							crc.right -= (wDisk-wTemperature);
							InflateRect(&crc, -(wSpace / 2), 0);
							if (TrayData->iHddTemperature <= TraySave.dNumValues[2])
								rgb = TraySave.cMonitorColor[4];
							else if (TrayData->iHddTemperature <= TraySave.dNumValues[3])
								rgb = TraySave.cMonitorColor[5];
							else
								rgb = TraySave.cMonitorColor[6];
							SetTextColor(mdc, rgb);
							if (TraySave.iMonitorSimple == 0)
							{
								DrawShadowText(mdc, TraySave.szDiskName, lstrlen(TraySave.szDiskName), &crc, DT_LEFT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
								wsprintf(sz, L"%.2d%s", TrayData->iHddTemperature, TraySave.szTemperatureCPUUnit);
							}
							else if (TraySave.iMonitorSimple == 1)
								wsprintf(sz, L"%.2d℃", TrayData->iHddTemperature);
							else
								wsprintf(sz, L"%.2d", TrayData->iHddTemperature);
							DrawShadowText(mdc, sz, lstrlen(sz), &crc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
							OffsetRect(&crc, 0, crc.bottom);
							if (TrayData->disktime <= TraySave.dNumValues[6])
								rgb = TraySave.cMonitorColor[4];
							else if (TrayData->disktime <= TraySave.dNumValues[7])
								rgb = TraySave.cMonitorColor[5];
							else
								rgb = TraySave.cMonitorColor[6];
							SetTextColor(mdc, rgb);
							if (TraySave.iMonitorSimple == 0)
							{
								DrawShadowText(mdc, TraySave.szDiskName, lstrlen(TraySave.szDiskName), &crc, DT_LEFT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
								wsprintf(sz, L"%.2d%s", TrayData->disktime, TraySave.szUsageMEMUnit);
							}
							else if (TraySave.iMonitorSimple == 1)
								wsprintf(sz, L"%.2d%%", TrayData->disktime);
							else
								wsprintf(sz, L"%.2d", TrayData->disktime);
							DrawShadowText(mdc, sz, lstrlen(sz), &crc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
							OffsetRect(&crc, wTemperature, -(crc.bottom - crc.top));
							crc.right = crc.left + (wDisk - wTemperature)-wSpace;
						}
						else
							InflateRect(&crc, -(wSpace / 2), 0);
						if (TraySave.bMonitorTrafficUpDown)
							DrawDisk(mdc, &crc, TrayData->diskreadbyte, FALSE);
						else
							DrawDisk(mdc, &crc, TrayData->diskwritebyte, TRUE);
						OffsetRect(&crc, 0, crc.bottom);
						if (TraySave.bMonitorTrafficUpDown)
							DrawDisk(mdc, &crc, TrayData->diskwritebyte, TRUE);
						else
							DrawDisk(mdc, &crc, TrayData->diskreadbyte, FALSE);
					}
				}
				if (TraySave.bMonitorTime)
				{
					SetTextColor(mdc, TraySave.cMonitorColor[1]);
					SYSTEMTIME systm;
					GetLocalTime(&systm);
					RECT crc = rc;
					TCHAR szWeek[7][2] = { L"日",L"一",L"二",L"三",L"四",L"五",L"六" };
					wsprintf(sz, L"%.2d/%.2d'%s", systm.wMonth, systm.wDay, szWeek[systm.wDayOfWeek]);
					int sLen = lstrlen(sz);
					if (VTray)
					{
						if (TraySave.bMonitorTraffic)
							crc.top = wHeight * 2;
						if (TraySave.bMonitorTemperature)
						{
							crc.top += wHeight;
							if (bRing0)
								crc.top += wHeight;
						}
						if (TraySave.bMonitorUsage)
							crc.top += wHeight * 2;
						if (TraySave.bMonitorDisk)
						{
							if(hOHMA && TraySave.bMonitorTemperature)
								crc.top += wHeight * 2;
							crc.top += wHeight * 2;
						}
						crc.bottom = crc.top + wHeight;
					}
					else
					{
						crc.left = crc.left + wTraffic + wTemperature + wUsage + wDisk;
						crc.right = crc.left + wTime;
						crc.bottom /= 2;
						InflateRect(&crc, -(wSpace / 2), 0);
					}
					if (VTray)
					{
						if(TraySave.iMonitorSimple == 0)
							DrawShadowText(mdc, L"D", 1, &crc, DT_LEFT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
						DrawShadowText(mdc, sz, sLen, &crc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
					}
					else
						DrawShadowText(mdc, sz, sLen, &crc, DT_CENTER | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
					wsprintf(sz, L"%.2d:%.2d:%.2d", systm.wHour, systm.wMinute, systm.wSecond);
					sLen = lstrlen(sz);
					if (VTray)
						OffsetRect(&crc, 0, wHeight);
					else
						OffsetRect(&crc, 0, crc.bottom);
					if (VTray)
					{
						if(TraySave.iMonitorSimple == 0)
							DrawShadowText(mdc, L"T", 1, &crc, DT_LEFT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
						DrawShadowText(mdc, sz, (int)sLen, &crc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
					}
				else
					DrawShadowText(mdc, sz, sLen, &crc, DT_CENTER | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
				}
				if (TraySave.bMonitorPrice)
				{
					RECT crc = rc;
					if (VTray)
					{
						if (TraySave.bMonitorTraffic)
							crc.top = wHeight * 2;
						if (TraySave.bMonitorTemperature)
						{
							crc.top += wHeight;
							if (bRing0)
								crc.top += wHeight;
						}
						if (TraySave.bMonitorUsage)
							crc.top += wHeight * 2;
						if (TraySave.bMonitorDisk)
						{
							if (hOHMA && TraySave.bMonitorTemperature)
								crc.top += wHeight * 2;
							crc.top += wHeight * 2;
						}
						if(TraySave.bMonitorTime)
							crc.top += wHeight * 2;
						crc.bottom = crc.top + wHeight;
					}
					else
					{
						crc.left = crc.left + wTraffic + wTemperature + wUsage + wDisk + wTime;
						crc.right = crc.left + wPrice;						
						crc.bottom /= 2;
						if (TraySave.bTwoFour)
						{
							crc.right -= wPrice / 2;
						}
						InflateRect(&crc, -(wSpace / 2), 0);
					}
					DrawPrice(mdc, &crc, TrayData->fLastPrice1, TrayData->fOpenPrice1, TrayData->szLastPrice1, TrayData->iPriceUpDown[0]);
					if (VTray)
						OffsetRect(&crc, 0, wHeight);
					else
						OffsetRect(&crc, 0, crc.bottom);
					DrawPrice(mdc, &crc, TrayData->fLastPrice2, TrayData->fOpenPrice2, TrayData->szLastPrice2, TrayData->iPriceUpDown[1]);
					if (TraySave.bTwoFour)
					{
						if (VTray)
							OffsetRect(&crc, 0, wHeight);
						else
							OffsetRect(&crc, wPrice/2, -(crc.bottom-crc.top));
						DrawPrice(mdc, &crc, TrayData->fLastPrice3, TrayData->fOpenPrice3, TrayData->szLastPrice3, TrayData->iPriceUpDown[2]);
						if (VTray)
							OffsetRect(&crc, 0, wHeight);
						else
							OffsetRect(&crc, 0, crc.bottom);
						DrawPrice(mdc, &crc, TrayData->fLastPrice4, TrayData->fOpenPrice4, TrayData->szLastPrice4, TrayData->iPriceUpDown[3]);
					}
				}
				if (oldFont && oldFont != (HFONT)HGDI_ERROR)
					SelectObject(mdc, oldFont);
			}
			//		GetClientRect(hDlg, &rc);
			if (VTray)
				InflateRect(&rc, wSpace / 2, 0);
			if(TraySave.bMonitorFuse)/////////////////背景融合
			{
				BYTE* lpvBits = NULL;

				BITMAPINFO binfo;
				memset(&binfo, 0, sizeof(BITMAPINFO));
				binfo.bmiHeader.biBitCount = 32;     //每个像素多少位，也可直接写24(RGB)或者32(RGBA)
				binfo.bmiHeader.biCompression = 0;
				binfo.bmiHeader.biHeight = rc.bottom - rc.top;
				binfo.bmiHeader.biPlanes = 1;
				DWORD imageSize = 0;
				if (ComputeDibImageSize(rc.right - rc.left, rc.bottom - rc.top, &imageSize))
					binfo.bmiHeader.biSizeImage = imageSize;
				binfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
				binfo.bmiHeader.biWidth = rc.right - rc.left;
				if (binfo.bmiHeader.biSizeImage != 0)
					lpvBits = (BYTE*)HeapAlloc(GetProcessHeap(), 0, binfo.bmiHeader.biSizeImage);
				//		GetDIBits(mdc, hMemBmp, 0, rc.bottom - rc.top, lpvBits, &bmpInfo, DIB_RGB_COLORS);
				if (lpvBits && binfo.bmiHeader.biSizeImage >= 4)
				{
					if (GetDIBits(mdc, hMemBmp, 0, rc.bottom - rc.top, lpvBits, &binfo, DIB_RGB_COLORS) != 0)
					{
						for (DWORD i = 0; i + 4 <= binfo.bmiHeader.biSizeImage; i += 4)
						{
							if (lpvBits[i] > 3 || lpvBits[i + 1] > 3 || lpvBits[i + 2] > 3)
							{
								if (TraySave.bMonitorFuse)
									lpvBits[i + 3] = 0x80;
								else if (TraySave.bMonitorFloat)
									lpvBits[i + 3] = 0xff;
							}
						}
						SetDIBits(mdc, hMemBmp, 0, rc.bottom - rc.top, lpvBits, &binfo, DIB_RGB_COLORS);
					}
				}
				if (lpvBits)
					HeapFree(GetProcessHeap(), 0, lpvBits);
			}
			BitBlt(hdc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, mdc, 0, 0, SRCCOPY);
			if (oldBmp && oldBmp != (HBITMAP)HGDI_ERROR)
				SelectObject(mdc, oldBmp);
			DeleteObject(hMemBmp);
			DeleteDC(mdc);
			//		EndPaint(hDlg, &ps);
		}
		return TRUE;
	}
	break;
	}
	return FALSE;
}
void DrawPrice(HDC mdc,LPRECT crc,float fLast,float fOpen,WCHAR *szLast,int iPriceUpDown)
{
	if (!mdc || !crc)
		return;
	WCHAR sz[64];
	if (!std::isfinite(fLast) || fLast < 0.0f)
		fLast = 0.0f;
	if (!std::isfinite(fOpen) || fOpen <= 0.0f)
		fOpen = 0.0f;
	float f = fOpen > 0.0f ? (fLast / fOpen) - 1.0f : 0.0f;
	if (!std::isfinite(f))
		f = 0.0f;
	// Keep the integer formatting below within a defined range even when an
	// exchange returns a transiently corrupted price.
	if (f > 1000.0f)
		f = 1000.0f;
	else if (f < -1.0f)
		f = -1.0f;
	int pf = 0;
	if (f < 0)
		SetTextColor(mdc, TraySave.cPriceColor[0]);
	else
		SetTextColor(mdc, TraySave.cPriceColor[1]);
	if (f >= 10)
	{
		pf = (int)(f * 100);
		wsprintf(sz, L"%2d%%", pf);
	}
	else if (f < 0)
	{
		f = -f;
	}
	if (f >= 1)
	{
		pf = (int)(f * 100);
		wsprintf(sz, L"%2d%%", pf);
	}
	else if (f >= 0.1)
	{
		pf = (int)(f * 1000);
		wsprintf(sz, L"%2d.%.1d%%", pf / 10, pf % 10);
	}
	else
	{
		if (fLast == 0)
			pf = 10000;
		else
			pf = (int)(f * 10000);
		wsprintf(sz, L"%2d.%.2d%%", pf / 100, pf % 100);
	}
	int sLen = lstrlen(sz);
	DrawShadowText(mdc, sz, sLen, crc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
	if (iPriceUpDown != 0)
	{
		if (iPriceUpDown == MB_ICONHAND)
			SetTextColor(mdc, TraySave.cPriceColor[3]);
		else
			SetTextColor(mdc, TraySave.cPriceColor[2]);
	}
	if (szLast)
		DrawShadowText(mdc, szLast, lstrlen(szLast), crc, DT_LEFT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);

}
INT_PTR CALLBACK MainProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)//主窗口过程
{
	UNREFERENCED_PARAMETER(lParam);
	switch (message)
	{
	case MSG_APPBAR_MSGID:
		if (wParam == ABN_FULLSCREENAPP)
		{
			if (TraySave.bMonitorTopmost && !TraySave.bMonitorFloat)
			{
				if (lParam == FALSE)
				{
					if (bFullScreen)
					{
						DestroyWindow(hTaskBar);
					}
				}
				else
				{
					if (!bFullScreen)
					{
						DestroyWindow(hTaskBar);
					}
				}
			}
			bFullScreen = (BOOL)lParam;
		}
		break;
	case WM_INITDIALOG:
		SetTimer(hDlg, 88, 8888,NULL);
		return (INT_PTR)TRUE;
		/*
			case WM_ENDSESSION:
				if (lParam == ENDSESSION_LOGOFF)
				{
					DestroyWindow(hTray);
					RunProcess(NULL);
					return TRUE;
				}
				break;
		*/
	case WM_TRAYS:
		if(bSetting)
			OpenSetting();
		break;
	case 0x02e0://WM_DPICHANGED:
	{
		iDPI = LOWORD(wParam);
		DestroyWindow(hTime);
		DestroyWindow(hTaskBar);
		Sleep(1000);
		SetWH();
/*
		bRealClose = TRUE;
		bResetRun = TRUE;
		PostQuitMessage(0);
*/
	}
	break;
	case WM_CLOSE:
	{
		KillTimer(hDlg, 6);
		KillTimer(hDlg, 3);
		SendMessage(hReBarWnd, WM_SETREDRAW, TRUE, 0);
		HWND hSecondaryTray;
		hSecondaryTray = FindWindow(szSecondaryTray, NULL);
		while (hSecondaryTray)
		{
			HWND hSReBarWnd = FindWindowEx(hSecondaryTray, 0, L"WorkerW", NULL);
			SendMessage(hSReBarWnd, WM_SETREDRAW, TRUE, 0);
			ShowWindow(hSReBarWnd, SW_SHOWNOACTIVATE);
			hSecondaryTray = FindWindowEx(NULL, hSecondaryTray, szSecondaryTray, NULL);
		}
		ShowWindow(hTaskListWnd, SW_SHOW);
		PostQuitMessage(0);
	}
	break;
	case WM_TIMER:
	{
		if(wParam==88)
		{
			KillTimer(hDlg,wParam);
			bSetting = TRUE;
		}
		else if(wParam==3000)
			GetShellAllWnd();
		else if (wParam == 11)//释放内存
		{
			KillTimer(hDlg, wParam);
			SetTimer(hDlg, wParam, 60000, NULL);
			TrimOwnWorkingSetSafe();
		}
		else if (wParam == 6)//处理任务栏图标与信息窗口
		{
			if (TraySave.bTrayStyle)
			{
				if ((TraySave.iPos != 0 || TraySave.bMonitor) && hWin11UI == NULL && rovi.dwBuildNumber < 22000)
				{
					//				if (TraySave.bTaskIcon == FALSE)
					{
						SetTaskBarPos(hTaskListWnd, hTray, hTaskWnd, hReBarWnd, TRUE);
					}
					HWND hSecondaryTray;
					hSecondaryTray = FindWindow(szSecondaryTray, NULL);
					while (hSecondaryTray)
					{
						HWND hSReBarWnd = FindWindowEx(hSecondaryTray, 0, L"WorkerW", NULL);
						if (hSReBarWnd)
						{
							HWND hSTaskListWnd = FindWindowEx(hSReBarWnd, NULL, L"MSTaskListWClass", NULL);
							if (hSTaskListWnd)
							{
								SetTaskBarPos(hSTaskListWnd, hSecondaryTray, hSReBarWnd, hSReBarWnd, FALSE);
							}
						}
						hSecondaryTray = FindWindowEx(NULL, hSecondaryTray, szSecondaryTray, NULL);
					}
				}
			}
		}
		else if (wParam == 3)//处理任务栏风格
		{
			if (TraySave.bMonitor)
			{
				if (!bTaskBarMoveing)
				{
					AdjustWindowPos();
				}
			}
			//			if (TraySave.aMode[0] != ACCENT_DISABLED || TraySave.aMode[1] != ACCENT_DISABLED)
			if(TraySave.bTrayStyle)
			{
				int oldWindowMode = iWindowMode;
				if (hTray)
				{
					if (iProject == 0)
						iWindowMode = 0;
					else if (iProject == 1)
						iWindowMode = 1;
					else
					{
						iWindowMode = 0;
						EnumWindows(IsZoomedFunc, (LPARAM)MonitorFromWindow(hTray, MONITOR_DEFAULTTONEAREST));
					}
					if (TraySave.aMode[iWindowMode] != ACCENT_DISABLED || oldWindowMode != iWindowMode)
					{
						SetWindowCompositionAttribute(hTray, TraySave.aMode[iWindowMode], TraySave.dAlphaColor[iWindowMode], hWin11UI != NULL);
//						HWND hTray11=FindWindowEx(hTray, 0, L"Windows.UI.Composition.DesktopWindowContentBridge",NULL);
//						SetWindowCompositionAttribute(hTray11, TraySave.aMode[iWindowMode], TraySave.dAlphaColor[iWindowMode]);
					}
					LONG_PTR exStyle = GetWindowLongPtr(hTray, GWL_EXSTYLE);
					exStyle |= WS_EX_LAYERED;
					SetWindowLongPtr(hTray, GWL_EXSTYLE, exStyle);
					SetLayeredWindowAttributes(hTray, 0, (BYTE)TraySave.bAlpha[iWindowMode], LWA_ALPHA);
				}
				HWND hSecondaryTray = FindWindow(szSecondaryTray, NULL);
				while (hSecondaryTray)
				{
					if (iProject == 0)
						iWindowMode = 0;
					else if (iProject == 1)
						iWindowMode = 1;
					else
					{
						iWindowMode = 0;
						EnumWindows(IsZoomedFunc, (LPARAM)MonitorFromWindow(hSecondaryTray, MONITOR_DEFAULTTONEAREST));
					}
					if (TraySave.aMode[iWindowMode] != ACCENT_DISABLED || oldWindowMode != iWindowMode)
						SetWindowCompositionAttribute(hSecondaryTray, TraySave.aMode[iWindowMode], TraySave.dAlphaColor[iWindowMode], hWin11UI != NULL);
					LONG_PTR exStyle = GetWindowLongPtr(hSecondaryTray, GWL_EXSTYLE);
					exStyle |= WS_EX_LAYERED;
					SetWindowLongPtr(hSecondaryTray, GWL_EXSTYLE, exStyle);
					SetLayeredWindowAttributes(hSecondaryTray, 0, (BYTE)TraySave.bAlpha[iWindowMode], LWA_ALPHA);
					hSecondaryTray = FindWindowEx(NULL, hSecondaryTray, szSecondaryTray, NULL);
				}
			}
			//			if (TraySave.aMode[0] == ACCENT_DISABLED && TraySave.aMode[1] == ACCENT_DISABLED)//默认则关闭定时器
			//				KillTimer(hDlg, 3);
		}
	}
	break;
	case WM_IAWENTRAY://////////////////////////////////////////////////////////////////////////////////通知栏左右键处理
	{
		if (LOWORD(lParam) == WM_LBUTTONDOWN || LOWORD(lParam) == WM_RBUTTONDOWN)
		{
			OpenSetting();
		}
		break;
	}
	break;
	}
	return FALSE;
}
INT_PTR CALLBACK SettingProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)//设置窗口过程
{
	UNREFERENCED_PARAMETER(lParam);
	switch (message)
	{
	case WM_INITDIALOG:
		return (INT_PTR)TRUE;
	case WM_NOTIFY:
		if (lParam == 0)
			break;
		switch (((LPNMHDR)lParam)->code)
		{
		case NM_CLICK:
		case NM_RETURN:
		{
			PNMLINK pNMLink = (PNMLINK)lParam;
			if (pNMLink == NULL || pNMLink->item.iLink != 0)
				break;
			HWND source = ((LPNMHDR)lParam)->hwndFrom;
			LPCWSTR url = NULL;
			if (source == GetDlgItem(hDlg, IDC_SYSLINK))
				url = L"https://github.com/Rhongomiant1227/TrayS";
			else if (source == GetDlgItem(hDlg, IDC_SYSLINK_COMPAT))
				url = L"https://github.com/Rhongomiant1227/TrayS/blob/master/COMPATIBILITY.md";
			if (url != NULL)
				(void)pShellExecute(NULL, L"open", url, NULL, NULL, SW_SHOW);
			break;
		}
		}
		break;
	case WM_HSCROLL://////////////////////////////////////////////////////////////////////////////////透明度处理
	{
		HWND hSlider = GetDlgItem(hDlg, IDC_SLIDER_ALPHA);
		HWND hSliderB = GetDlgItem(hDlg, IDC_SLIDER_ALPHA_B);
		if (hSlider == (HWND)lParam)
		{
			TraySave.bAlpha[iProject] = (int)SendDlgItemMessage(hDlg, IDC_SLIDER_ALPHA, TBM_GETPOS, 0, 0);
		}
		else if (hSliderB == (HWND)lParam)
		{
			DWORD bAlphaB = (int)SendDlgItemMessage(hDlg, IDC_SLIDER_ALPHA_B, TBM_GETPOS, 0, 0);
			bAlphaB = bAlphaB << 24;
			TraySave.dAlphaColor[iProject] = bAlphaB + (TraySave.dAlphaColor[iProject] & 0xffffff);
		}
		SetTimer(hDlg, 3, 500, NULL);
		break;
	}
	case WM_TIMER:
		if (wParam == 3)
		{
			KillTimer(hDlg, wParam);
			WriteReg();
		}
		break;
	case WM_COMMAND:
		if (HIWORD(wParam) == EN_CHANGE && !bSettingInit)
		{
			if (LOWORD(wParam) >= IDC_EDIT1 && LOWORD(wParam) <= IDC_EDIT12)
			{
				int index = LOWORD(wParam) - IDC_EDIT1;
				TraySave.dNumValues[index] = GetDlgItemInt(hDlg, LOWORD(wParam), NULL, 0);
				if (index == 0 || index == 1 || index == 8)
					TraySave.dNumValues[index] *= 1048576;
				SetTimer(hDlg, 3, 500, NULL);
			}
			else if (LOWORD(wParam) >= IDC_EDIT24 && LOWORD(wParam) <= IDC_EDIT26)
			{
				int index = LOWORD(wParam) - IDC_EDIT24;
				TraySave.dNumValues2[index] = GetDlgItemInt(hDlg, LOWORD(wParam), NULL, 0);
				SetTimer(hDlg, 3, 500, NULL);
			}
			else if (LOWORD(wParam) == IDC_EDIT_TIME)
			{
				TraySave.FlushTime = GetDlgItemInt(hDlg, LOWORD(wParam), NULL, 0);
				if (TraySave.aMode[0] != ACCENT_DISABLED || TraySave.aMode[1] != ACCENT_DISABLED)
					SetTimer(hMain, 3, TraySave.FlushTime, NULL);
				SetTimer(hDlg, 3, 500, NULL);
			}
			else if ((LOWORD(wParam) >= IDC_EDIT14 && LOWORD(wParam) <= IDC_EDIT23)||LOWORD(wParam)==IDC_EDIT27|| LOWORD(wParam) == IDC_EDIT29)
			{
				GetDlgItemText(hDlg, IDC_EDIT14, TraySave.szTrafficOut, 8);
				GetDlgItemText(hDlg, IDC_EDIT15, TraySave.szTrafficIn, 8);
				GetDlgItemText(hDlg, IDC_EDIT16, TraySave.szTemperatureCPU, 8);
				GetDlgItemText(hDlg, IDC_EDIT17, TraySave.szTemperatureGPU, 8);
				GetDlgItemText(hDlg, IDC_EDIT18, TraySave.szTemperatureCPUUnit, 4);
				GetDlgItemText(hDlg, IDC_EDIT19, TraySave.szTemperatureGPUUnit, 4);
				GetDlgItemText(hDlg, IDC_EDIT20, TraySave.szUsageCPU, 8);
				GetDlgItemText(hDlg, IDC_EDIT21, TraySave.szUsageMEM, 8);
				GetDlgItemText(hDlg, IDC_EDIT22, TraySave.szUsageCPUUnit, 4);
				GetDlgItemText(hDlg, IDC_EDIT23, TraySave.szUsageMEMUnit, 4);
				GetDlgItemText(hDlg, IDC_EDIT27, TraySave.szDiskReadSec, 8);
				GetDlgItemText(hDlg, IDC_EDIT28, TraySave.szDiskWriteSec, 8);
				GetDlgItemText(hDlg, IDC_EDIT29, TraySave.szDiskName, 8);
				SetTimer(hDlg, 3, 1500, NULL);
				if (TraySave.iMonitorSimple == 0)
				{
					SetWH();
					AdjustWindowPos();
				}
			}			
		}
		else if (LOWORD(wParam) >= IDC_RADIO_DEFAULT && LOWORD(wParam) <= IDC_RADIO_ACRYLIC)
		{
			iWindowMode = !iWindowMode;
			if (IsDlgButtonChecked(hDlg, IDC_RADIO_DEFAULT))
				TraySave.aMode[iProject] = ACCENT_DISABLED;
			else if (IsDlgButtonChecked(hDlg, IDC_RADIO_TRANSPARENT))
				TraySave.aMode[iProject] = ACCENT_ENABLE_TRANSPARENTGRADIENT;
			else if (IsDlgButtonChecked(hDlg, IDC_RADIO_BLURBEHIND))
				TraySave.aMode[iProject] = ACCENT_ENABLE_BLURBEHIND;
			else if (IsDlgButtonChecked(hDlg, IDC_RADIO_ACRYLIC))
				TraySave.aMode[iProject] = ACCENT_ENABLE_ACRYLICBLURBEHIND;
			WriteReg();
			if (TraySave.aMode[0] != ACCENT_DISABLED || TraySave.aMode[1] != ACCENT_DISABLED)
				SetTimer(hMain, 3, TraySave.FlushTime, NULL);
			//			else
			//				KillTimer(hMain, 3);

		}
		else if (LOWORD(wParam) >= IDC_RADIO_LEFT && LOWORD(wParam) <= IDC_RADIO_RIGHT)
		{
			if (IsDlgButtonChecked(hDlg, IDC_RADIO_LEFT))
			{
				TraySave.iPos = 0;
			}
			else if (IsDlgButtonChecked(hDlg, IDC_RADIO_CENTER))
			{
				TraySave.iPos = 1;
			}
			else if (IsDlgButtonChecked(hDlg, IDC_RADIO_RIGHT))
			{
				TraySave.iPos = 2;
			}
			WriteReg();
		}
		else if (LOWORD(wParam) >= IDC_RADIO_BYTE && LOWORD(wParam) <= IDC_RADIO_MB)
		{
			if (IsDlgButtonChecked(hDlg, IDC_RADIO_AUTO))
				TraySave.iUnit = 0;
			else if (IsDlgButtonChecked(hDlg, IDC_RADIO_KB))
				TraySave.iUnit = 1;
			else if (IsDlgButtonChecked(hDlg, IDC_RADIO_MB))
				TraySave.iUnit = 2;
			if (IsDlgButtonChecked(hDlg, IDC_RADIO_BIT))
				TraySave.iUnit |= 0x10000;
			WriteReg();
		}
		if (LOWORD(wParam) == IDC_RADIO_NORMAL || LOWORD(wParam) == IDC_RADIO_MAXIMIZE)
		{
			if (IsDlgButtonChecked(hDlg, IDC_RADIO_NORMAL))
				iProject = 0;
			else
				iProject = 1;
			if (TraySave.aMode[iProject] == ACCENT_DISABLED)
				CheckRadioButton(hSetting, IDC_RADIO_DEFAULT, IDC_RADIO_ACRYLIC, IDC_RADIO_DEFAULT);
			else if (TraySave.aMode[iProject] == ACCENT_ENABLE_TRANSPARENTGRADIENT)
				CheckRadioButton(hSetting, IDC_RADIO_DEFAULT, IDC_RADIO_ACRYLIC, IDC_RADIO_TRANSPARENT);
			else if (TraySave.aMode[iProject] == ACCENT_ENABLE_BLURBEHIND)
				CheckRadioButton(hSetting, IDC_RADIO_DEFAULT, IDC_RADIO_ACRYLIC, IDC_RADIO_BLURBEHIND);
			else if (TraySave.aMode[iProject] == ACCENT_ENABLE_ACRYLICBLURBEHIND)
				CheckRadioButton(hSetting, IDC_RADIO_DEFAULT, IDC_RADIO_ACRYLIC, IDC_RADIO_ACRYLIC);
			SendDlgItemMessage(hSetting, IDC_SLIDER_ALPHA, TBM_SETPOS, TRUE, TraySave.bAlpha[iProject]);
			BYTE bAlphaB = TraySave.dAlphaColor[iProject] >> 24;
			SendDlgItemMessage(hSetting, IDC_SLIDER_ALPHA_B, TBM_SETPOS, TRUE, bAlphaB);
			::InvalidateRect(GetDlgItem(hSetting, IDC_BUTTON_COLOR), NULL, FALSE);
		}
		else if (LOWORD(wParam) == IDC_CHECK_SOUND)
		{
			TraySave.bSound = IsDlgButtonChecked(hDlg, IDC_CHECK_SOUND);
			WriteReg();
		}
		else if (LOWORD(wParam) == IDC_CHECK_TIPS)
		{
			TraySave.bMonitorTips = IsDlgButtonChecked(hDlg, IDC_CHECK_TIPS);
			WriteReg();
		}
		else if (LOWORD(wParam) == IDC_CHECK_FUSE)
		{
			TraySave.bMonitorFuse = IsDlgButtonChecked(hDlg, IDC_CHECK_FUSE);
			WriteReg();
			CloseTaskBar();
		}
		else if (LOWORD(wParam) == IDC_CHECK_TRAYICON)
		{
			TraySave.bTrayIcon = IsDlgButtonChecked(hDlg, IDC_CHECK_TRAYICON);
			WriteReg();
			CloseTaskBar();
			if (TraySave.bTrayIcon)
				pShell_NotifyIcon(NIM_ADD, &nid);
			else
				pShell_NotifyIcon(NIM_DELETE, &nid);
		}
		else if (LOWORD(wParam) == IDC_CHECK_TRAY_STYLE)
		{
			TraySave.bTrayStyle = IsDlgButtonChecked(hDlg, IDC_CHECK_TRAY_STYLE);
			WriteReg();
			if (TraySave.bTrayStyle == FALSE)
			{
				SetWindowCompositionAttribute(hTray, ACCENT_DISABLED, 0, hWin11UI != NULL);
				SetLayeredWindowAttributes(hTray, 0, 255, LWA_ALPHA);
				InvalidateRect(hTray, NULL, TRUE);
			}
			CloseTaskBar();
		}
		else if (LOWORD(wParam) == IDC_CHECK_MONITOR)
		{
			TraySave.bMonitor = IsDlgButtonChecked(hDlg, IDC_CHECK_MONITOR);
			WriteReg();
			if (!TraySave.bMonitor)
			{
				CloseTaskBar();
			}
		}
		else if (LOWORD(wParam) == IDC_CHECK_TRAFFIC)
		{
			TraySave.bMonitorTraffic = IsDlgButtonChecked(hDlg, IDC_CHECK_TRAFFIC);
			WriteReg();
			SetWH();
			AdjustWindowPos();
		}
		else if (LOWORD(wParam) == IDC_CHECK_MONITOR_UPDOWN)
		{
			TraySave.bMonitorTrafficUpDown = IsDlgButtonChecked(hDlg, IDC_CHECK_MONITOR_UPDOWN);
			WriteReg();
			SetWH();
			AdjustWindowPos();
		}
		else if (LOWORD(wParam) == IDC_CHECK_TEMPERATURE)
		{
			TraySave.bMonitorTemperature = IsDlgButtonChecked(hDlg, IDC_CHECK_TEMPERATURE);
			if (TraySave.bMonitorTemperature)
				LoadTemperatureDLL();
			else
				FreeTemperatureDLL();
			WriteReg();
			SetWH();
			AdjustWindowPos();
		}
		else if (LOWORD(wParam) == IDC_CHECK_MONITOR_SIMPLE)
		{
			TraySave.iMonitorSimple = IsDlgButtonChecked(hDlg, IDC_CHECK_MONITOR_SIMPLE);
			WriteReg();
			SetWH();
			AdjustWindowPos();
		}
		else if (LOWORD(wParam) == IDC_CHECK_USAGE)
		{
			TraySave.bMonitorUsage = IsDlgButtonChecked(hDlg, IDC_CHECK_USAGE);
			WriteReg();
			SetWH();
			AdjustWindowPos();
		}
		else if (LOWORD(wParam) == IDC_CHECK_DISK)
		{
			TraySave.bMonitorDisk = IsDlgButtonChecked(hDlg, IDC_CHECK_DISK);
			WriteReg();
			SetWH();
			AdjustWindowPos();
		}
		else if (LOWORD(wParam) == IDC_CHECK_PRICE)
		{
			TraySave.bMonitorPrice = IsDlgButtonChecked(hDlg, IDC_CHECK_PRICE);			
			WriteReg();
			SetWH();
			AdjustWindowPos();
		}
		else if (LOWORD(wParam) == IDC_CHECK_MONITOR_PDH)
		{
			TraySave.bMonitorPDH = IsDlgButtonChecked(hDlg, IDC_CHECK_MONITOR_PDH);
			WriteReg();
		}
		else if (LOWORD(wParam) == IDC_CHECK_MONITOR_LEFT)
		{
			TraySave.bMonitorLeft = IsDlgButtonChecked(hDlg, IDC_CHECK_MONITOR_LEFT);
			WriteReg();
			AdjustWindowPos();
		}
		else if (LOWORD(wParam) == IDC_CHECK_MONITOR_NEAR)
		{
			TraySave.bNear = IsDlgButtonChecked(hDlg, IDC_CHECK_MONITOR_NEAR);
			WriteReg();
			AdjustWindowPos();
		}
		else if (LOWORD(wParam) == IDC_CHECK_MONITOR_FLOAT)
		{
			TraySave.bMonitorFloat = IsDlgButtonChecked(hDlg, IDC_CHECK_MONITOR_FLOAT);
			WriteReg();
			CloseTaskBar();
		}
		else if (LOWORD(wParam) == IDC_CHECK_MONITOR_FLOAT_VROW)
		{
			TraySave.bMonitorFloatVRow = IsDlgButtonChecked(hDlg, IDC_CHECK_MONITOR_FLOAT_VROW);
			WriteReg();
			CloseTaskBar();
		}
		else if (LOWORD(wParam) == IDC_CHECK_MONITOR_TIME)
		{
			TraySave.bMonitorTime = IsDlgButtonChecked(hDlg, IDC_CHECK_MONITOR_TIME);
			WriteReg();
			SetWH();
			AdjustWindowPos();
		}
		else if (LOWORD(wParam) == IDC_CHECK_TIME)
		{
			TraySave.bSecond = IsDlgButtonChecked(hDlg, IDC_CHECK_TIME);
			WriteReg();
			if(TraySave.bSecond)
				OpenTimeDlg();
			else
				DestroyWindow(hTime);
		}
		else if (LOWORD(wParam) == IDC_CHECK_TRANSPARENT)
		{
			TraySave.bMonitorTransparent = IsDlgButtonChecked(hDlg, IDC_CHECK_TRANSPARENT);
			if (TraySave.bMonitorTransparent)
				SetWindowLongPtr(hTaskBar, GWL_EXSTYLE, GetWindowLongPtr(hTaskBar, GWL_EXSTYLE) | WS_EX_TRANSPARENT | WS_EX_LAYERED);
			else
				SetWindowLongPtr(hTaskBar, GWL_EXSTYLE, GetWindowLongPtr(hTaskBar, GWL_EXSTYLE) & ~WS_EX_TRANSPARENT);
			WriteReg();
		}
		else if (LOWORD(wParam) == IDC_CHECK_TOPMOST)
		{
			TraySave.bMonitorTopmost = IsDlgButtonChecked(hDlg, IDC_CHECK_TOPMOST);
			WriteReg();
		}
		else if (LOWORD(wParam) == IDC_CHECK_AUTORUN)
		{
			if (IsDlgButtonChecked(hDlg, IDC_CHECK_AUTORUN))
				SetUserAutoRunSafe(TRUE, szAppName);
			else
				SetUserAutoRunSafe(FALSE, szAppName);
		}
		else if (LOWORD(wParam) == IDC_RESTORE_DEFAULT)
		{
			WCHAR savePath[32768] = {};
			if (GetApplicationFilePath(szTraySave, savePath, ARRAYSIZE(savePath)))
				DeleteFileW(savePath);
			//			RegDeleteKey(HKEY_CURRENT_USER, szSubKey);
			SendMessage(hDlg, WM_COMMAND, IDCANCEL, 0);
		}
		else if (LOWORD(wParam) == IDCANCEL)
		{
			/*
						SendMessage(hMain, WM_TIMER, 11, 1000);
						DestroyWindow(hDlg);
						return (INT_PTR)TRUE;
			*/
			//			SendMessage(hReBarWnd, WM_SETREDRAW, TRUE, 0);
			bRealClose = TRUE;
			SendMessage(hMain, WM_CLOSE, NULL, NULL);
			return (INT_PTR)TRUE;
		}
		else if (LOWORD(wParam) == IDC_CLOSE)
		{
			bRealClose = TRUE;
			TrayData->bExit = TRUE;
			SendMessage(hMain, WM_CLOSE, NULL, NULL);
		}
		else if (LOWORD(wParam) == IDC_BUTTON_SELECT_NET)
		{
			ShowSelectMenu(TRUE);
		}
		else if (LOWORD(wParam) == IDC_BUTTON_SELECT_DISK)
		{
			ShowSelectMenu(FALSE);
		}
		else if (LOWORD(wParam) == IDC_BUTTON_FONT || LOWORD(wParam) == IDC_BUTTON_TIPS_FONT)
		{
			typedef UINT_PTR(CALLBACK* LPCFHOOKPROC) (HWND, UINT, WPARAM, LPARAM);
			typedef struct tagCHOOSEFONTW {
				DWORD           lStructSize;
				HWND            hwndOwner;          // caller's window handle
				HDC             hDC;                // printer DC/IC or NULL
				LPLOGFONTW      lpLogFont;          // ptr. to a LOGFONT struct
				INT             iPointSize;         // 10 * size in points of selected font
				DWORD           Flags;              // enum. type flags
				COLORREF        rgbColors;          // returned text color
				LPARAM          lCustData;          // data passed to hook fn.
				LPCFHOOKPROC    lpfnHook;           // ptr. to hook function
				LPCWSTR         lpTemplateName;     // custom template name
				HINSTANCE       hInstance;          // instance handle of.EXE that
													//   contains cust. dlg. template
				LPWSTR          lpszStyle;          // return the style field here
													// must be LF_FACESIZE or bigger
				WORD            nFontType;          // same value reported to the EnumFonts
													//   call back with the extra FONTTYPE_
													//   bits added
				WORD            ___MISSING_ALIGNMENT__;
				INT             nSizeMin;           // minimum pt size allowed &
				INT             nSizeMax;           // max pt size allowed if
													//   CF_LIMITSIZE is used
			} CHOOSEFONT;
			TraySave.TraybarFont.lfHeight = TraySave.TraybarFontSize;
			TraySave.TipsFont.lfHeight = TraySave.TipsFontSize;
			CHOOSEFONT cf;
			cf.lStructSize = sizeof cf;
			cf.hwndOwner = hDlg;
			cf.hDC = NULL;
			if (LOWORD(wParam) == IDC_BUTTON_FONT)
				cf.lpLogFont = &TraySave.TraybarFont;
			else
				cf.lpLogFont = &TraySave.TipsFont;
			cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_EFFECTS;
			cf.nFontType = SCREEN_FONTTYPE;
			cf.rgbColors = RGB(0, 0, 0);
			typedef BOOL(WINAPI* pfnChooseFont)(CHOOSEFONT* lpcf);
			HMODULE hComdlg32 = LoadSystemLibrarySafe(L"comdlg32.dll");
			if (hComdlg32)
			{
				pfnChooseFont ChooseFont = (pfnChooseFont)GetProcAddress(hComdlg32, "ChooseFontW");
				if (ChooseFont)
				{
					if (ChooseFont(&cf))
					{
						if (LOWORD(wParam) == IDC_BUTTON_FONT)
						{
							TraySave.TraybarFontSize = TraySave.TraybarFont.lfHeight;
							otleft = -1;
							SetWH();
							AdjustWindowPos();
						}
						else
							TraySave.TipsFontSize = TraySave.TipsFont.lfHeight;
						WriteReg();
					}
				}
				FreeLibrary(hComdlg32);
			}
		}
		else if (LOWORD(wParam) == IDC_BUTTON_COLOR || (LOWORD(wParam) >= IDC_BUTTON_COLOR_BACKGROUND && LOWORD(wParam) <= IDC_BUTTON_COLOR_PRICE_HIGH))
		{
			CHOOSECOLOR stChooseColor;
			stChooseColor.lStructSize = sizeof(CHOOSECOLOR);
			stChooseColor.hwndOwner = hDlg;
			if (LOWORD(wParam) == IDC_BUTTON_COLOR)
			{
				stChooseColor.rgbResult = TraySave.dAlphaColor[iProject];
				stChooseColor.lpCustColors = (LPDWORD)&TraySave.dAlphaColor[iProject];
			}
			else
			{
				if (LOWORD(wParam) == IDC_BUTTON_COLOR_PRICE_HIGH || LOWORD(wParam) == IDC_BUTTON_COLOR_PRICE_LOW)
				{
					stChooseColor.rgbResult = TraySave.cPriceColor[LOWORD(wParam) - IDC_BUTTON_COLOR_PRICE_LOW];
					stChooseColor.lpCustColors = TraySave.cPriceColor;
				}
				else
				{
					stChooseColor.rgbResult = TraySave.cMonitorColor[LOWORD(wParam) - IDC_BUTTON_COLOR_BACKGROUND];
					stChooseColor.lpCustColors = TraySave.cMonitorColor;
				}
			}
			stChooseColor.Flags = CC_RGBINIT | CC_FULLOPEN;
			stChooseColor.lCustData = 0;
			stChooseColor.lpfnHook = NULL;
			stChooseColor.lpTemplateName = NULL;
			typedef BOOL(WINAPI* pfnChooseColor)(LPCHOOSECOLOR lpcc);
			HMODULE hComdlg32 = LoadSystemLibrarySafe(L"comdlg32.dll");
			if (hComdlg32)
			{
				pfnChooseColor ChooseColor = (pfnChooseColor)GetProcAddress(hComdlg32, "ChooseColorW");
				if (ChooseColor)
				{
					if (ChooseColor(&stChooseColor))
					{
						if (LOWORD(wParam) == IDC_BUTTON_COLOR)
						{
							TraySave.dAlphaColor[iProject] = stChooseColor.rgbResult;
							DWORD bAlphaB = (int)SendDlgItemMessage(hDlg, IDC_SLIDER_ALPHA_B, TBM_GETPOS, 0, 0);
							bAlphaB = bAlphaB << 24;
							TraySave.dAlphaColor[iProject] = bAlphaB + (TraySave.dAlphaColor[iProject] & 0xffffff);
						}
						else
						{
							if (LOWORD(wParam) == IDC_BUTTON_COLOR_PRICE_HIGH || LOWORD(wParam) == IDC_BUTTON_COLOR_PRICE_LOW)
								TraySave.cPriceColor[LOWORD(wParam - IDC_BUTTON_COLOR_PRICE_LOW)] = stChooseColor.rgbResult;
							else
								TraySave.cMonitorColor[LOWORD(wParam - IDC_BUTTON_COLOR_BACKGROUND)] = stChooseColor.rgbResult;
							if (TraySave.cMonitorColor[0] == 0 || TraySave.cMonitorColor[0] == RGB(0, 0, 1))
							{
								TraySave.cMonitorColor[0] = RGB(0, 0, 1);
//								if (TraySave.bMonitorFloat||rovi.dwBuildNumber<=22000)
								{
									bShadow = TRUE;
									TraySave.bMonitorFuse = TRUE;
								}
							}
							else
								bShadow = FALSE;
							CheckDlgButton(hSetting, IDC_CHECK_FUSE, TraySave.bMonitorFuse);
							CloseTaskBar();
						}
						::InvalidateRect(GetDlgItem(hMain, LOWORD(wParam)), NULL, FALSE);
					}
				}
				FreeLibrary(hComdlg32);
			}
			WriteReg();
			//			SendMessage(hMain, WM_TRAYS, NULL, NULL);
		}
		break;
	}
	return (INT_PTR)FALSE;
}
INT_PTR CALLBACK ColorButtonProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)//颜色按钮控件过程
{
	switch (message)
	{
	case WM_PAINT:
	{
		PAINTSTRUCT ps{};
		HDC hdc = BeginPaint(hWnd, &ps);
		if (!hdc)
		{
			EndPaint(hWnd, &ps);
			return TRUE;
		}
		RECT rc{};
		if (!GetClientRect(hWnd, &rc) || rc.right <= rc.left || rc.bottom <= rc.top)
		{
			EndPaint(hWnd, &ps);
			return TRUE;
		}
		HBRUSH hb = NULL;
		int id = GetDlgCtrlID(hWnd);
		if (id >= IDC_BUTTON_COLOR_BACKGROUND && id <= IDC_BUTTON_COLOR_PRICE_HIGH)
		{
			if(id>=IDC_BUTTON_COLOR_PRICE_LOW)
				hb = CreateSolidBrush(TraySave.cPriceColor[id - IDC_BUTTON_COLOR_PRICE_LOW]);
			else
				hb = CreateSolidBrush(TraySave.cMonitorColor[id - IDC_BUTTON_COLOR_BACKGROUND]);
		}
		else
			hb = CreateSolidBrush(TraySave.dAlphaColor[iProject] & 0xffffff);
		if (hb)
		{
			FillRect(hdc, &rc, hb);
			DeleteObject(hb);
		}
		SetBkMode(hdc, TRANSPARENT);
		SetTextColor(hdc, RGB(255, 255, 255));

		HFONT oldFont = NULL;
		if (hFont)
			oldFont = (HFONT)SelectObject(hdc, hFont);
		WCHAR sz[8];
		GetWindowText(hWnd, sz, 8);
		DrawShadowText(hdc, sz, lstrlen(sz), &rc, DT_CENTER | DT_VCENTER| DT_SINGLELINE,bColor,TRUE);
		if (oldFont && oldFont != (HFONT)HGDI_ERROR)
			SelectObject(hdc, oldFont);
		EndPaint(hWnd, &ps);
		return TRUE;
	}
	}
	return oldColorButtonPoroc ?
		CallWindowProc(oldColorButtonPoroc, hWnd, message, wParam, lParam) :
		DefWindowProc(hWnd, message, wParam, lParam);
}
INT_PTR CALLBACK PriceProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_INITDIALOG:
	{
		CheckDlgButton(hDlg, IDC_CHECK1, TraySave.bCheckHighRemind[0]);
		CheckDlgButton(hDlg, IDC_CHECK2, TraySave.bCheckHighRemind[1]);
		CheckDlgButton(hDlg, IDC_CHECK3, TraySave.bCheckHighRemind[2]);
		CheckDlgButton(hDlg, IDC_CHECK4, TraySave.bCheckHighRemind[3]);
		CheckDlgButton(hDlg, IDC_CHECK5, TraySave.bCheckHighRemind[4]);
		CheckDlgButton(hDlg, IDC_CHECK6, TraySave.bCheckHighRemind[5]);
		CheckDlgButton(hDlg, IDC_CHECK7, TraySave.bCheckLowRemind[0]);
		CheckDlgButton(hDlg, IDC_CHECK8, TraySave.bCheckLowRemind[1]);
		CheckDlgButton(hDlg, IDC_CHECK9, TraySave.bCheckLowRemind[2]);
		CheckDlgButton(hDlg, IDC_CHECK10, TraySave.bCheckLowRemind[3]);
		CheckDlgButton(hDlg, IDC_CHECK11, TraySave.bCheckLowRemind[4]);
		CheckDlgButton(hDlg, IDC_CHECK12, TraySave.bCheckLowRemind[5]);
		CheckDlgButton(hDlg, IDC_CHECK_13, TraySave.bCheckHighRemind[6]);
		CheckDlgButton(hDlg, IDC_CHECK_14, TraySave.bCheckHighRemind[7]);
		CheckDlgButton(hDlg, IDC_CHECK_15, TraySave.bCheckHighRemind[8]);
		CheckDlgButton(hDlg, IDC_CHECK_16, TraySave.bCheckHighRemind[9]);
		CheckDlgButton(hDlg, IDC_CHECK_17, TraySave.bCheckHighRemind[10]);
		CheckDlgButton(hDlg, IDC_CHECK_18, TraySave.bCheckHighRemind[11]);
		CheckDlgButton(hDlg, IDC_CHECK_19, TraySave.bCheckLowRemind[6]);
		CheckDlgButton(hDlg, IDC_CHECK_20, TraySave.bCheckLowRemind[7]);
		CheckDlgButton(hDlg, IDC_CHECK_21, TraySave.bCheckLowRemind[8]);
		CheckDlgButton(hDlg, IDC_CHECK_22, TraySave.bCheckLowRemind[9]);
		CheckDlgButton(hDlg, IDC_CHECK_23, TraySave.bCheckLowRemind[10]);
		CheckDlgButton(hDlg, IDC_CHECK_24, TraySave.bCheckLowRemind[11]);
		WCHAR sz[64];
		FloatToStr(TraySave.HighRemind[0], sz);
		SetDlgItemText(hDlg, IDC_EDIT1, sz);
		FloatToStr(TraySave.HighRemind[1], sz);
		SetDlgItemText(hDlg, IDC_EDIT2, sz);
		FloatToStr(TraySave.HighRemind[2], sz);
		SetDlgItemText(hDlg, IDC_EDIT3, sz);
		FloatToStr(TraySave.HighRemind[3], sz);
		SetDlgItemText(hDlg, IDC_EDIT4, sz);
		FloatToStr(TraySave.HighRemind[4], sz);
		SetDlgItemText(hDlg, IDC_EDIT5, sz);
		FloatToStr(TraySave.HighRemind[5], sz);
		SetDlgItemText(hDlg, IDC_EDIT6, sz);
		FloatToStr(TraySave.LowRemind[0],sz);		
		SetDlgItemText(hDlg, IDC_EDIT7, sz);
		FloatToStr(TraySave.LowRemind[1], sz);
		SetDlgItemText(hDlg, IDC_EDIT8, sz);
		FloatToStr(TraySave.LowRemind[2], sz);
		SetDlgItemText(hDlg, IDC_EDIT9, sz);
		FloatToStr(TraySave.LowRemind[3], sz);
		SetDlgItemText(hDlg, IDC_EDIT10, sz);
		FloatToStr(TraySave.LowRemind[4], sz);
		SetDlgItemText(hDlg, IDC_EDIT11, sz);
		FloatToStr(TraySave.LowRemind[5], sz);
		SetDlgItemText(hDlg, IDC_EDIT12, sz);
		FloatToStr(TraySave.HighRemind[6], sz);
		SetDlgItemText(hDlg, IDC_EDIT_13, sz);
		FloatToStr(TraySave.HighRemind[7], sz);
		SetDlgItemText(hDlg, IDC_EDIT_14, sz);
		FloatToStr(TraySave.HighRemind[8], sz);
		SetDlgItemText(hDlg, IDC_EDIT_15, sz);
		FloatToStr(TraySave.HighRemind[9], sz);
		SetDlgItemText(hDlg, IDC_EDIT_16, sz);
		FloatToStr(TraySave.HighRemind[10], sz);
		SetDlgItemText(hDlg, IDC_EDIT_17, sz);
		FloatToStr(TraySave.HighRemind[11], sz);
		SetDlgItemText(hDlg, IDC_EDIT_18, sz);
		FloatToStr(TraySave.LowRemind[6], sz);
		SetDlgItemText(hDlg, IDC_EDIT_19, sz);
		FloatToStr(TraySave.LowRemind[7], sz);
		SetDlgItemText(hDlg, IDC_EDIT_20, sz);
		FloatToStr(TraySave.LowRemind[8], sz);
		SetDlgItemText(hDlg, IDC_EDIT_21, sz);
		FloatToStr(TraySave.LowRemind[9], sz);
		SetDlgItemText(hDlg, IDC_EDIT_22, sz);
		FloatToStr(TraySave.LowRemind[10], sz);
		SetDlgItemText(hDlg, IDC_EDIT_23, sz);
		FloatToStr(TraySave.LowRemind[11], sz);
		SetDlgItemText(hDlg, IDC_EDIT_24, sz);
		SetDlgItemText(hDlg, IDC_EDIT18, TraySave.szPriceName1);
		SetDlgItemText(hDlg, IDC_EDIT19, TraySave.szPriceName2);
		SetDlgItemText(hDlg, IDC_EDIT37, TraySave.szPriceName3);
		SetDlgItemText(hDlg, IDC_EDIT38, TraySave.szPriceName4);
		SetDlgItemText(hDlg, IDC_EDIT_OKX_WEB, TraySave.szOKXWeb);
	}
		break;
	case WM_DESTROY:
		TraySave.bCheckHighRemind[0] = IsDlgButtonChecked(hDlg, IDC_CHECK1);
		TraySave.bCheckHighRemind[1] = IsDlgButtonChecked(hDlg, IDC_CHECK2);
		TraySave.bCheckHighRemind[2] = IsDlgButtonChecked(hDlg, IDC_CHECK3);
		TraySave.bCheckHighRemind[3] = IsDlgButtonChecked(hDlg, IDC_CHECK4);
		TraySave.bCheckHighRemind[4] = IsDlgButtonChecked(hDlg, IDC_CHECK5);
		TraySave.bCheckHighRemind[5] = IsDlgButtonChecked(hDlg, IDC_CHECK6);
		TraySave.bCheckHighRemind[6] = IsDlgButtonChecked(hDlg, IDC_CHECK_13);
		TraySave.bCheckHighRemind[7] = IsDlgButtonChecked(hDlg, IDC_CHECK_14);
		TraySave.bCheckHighRemind[8] = IsDlgButtonChecked(hDlg, IDC_CHECK_15);
		TraySave.bCheckHighRemind[9] = IsDlgButtonChecked(hDlg, IDC_CHECK_16);
		TraySave.bCheckHighRemind[10] = IsDlgButtonChecked(hDlg, IDC_CHECK_17);
		TraySave.bCheckHighRemind[11] = IsDlgButtonChecked(hDlg, IDC_CHECK_18);
		TraySave.bCheckLowRemind[0] = IsDlgButtonChecked(hDlg, IDC_CHECK7);
		TraySave.bCheckLowRemind[1] = IsDlgButtonChecked(hDlg, IDC_CHECK8);
		TraySave.bCheckLowRemind[2] = IsDlgButtonChecked(hDlg, IDC_CHECK9);
		TraySave.bCheckLowRemind[3] = IsDlgButtonChecked(hDlg, IDC_CHECK10);
		TraySave.bCheckLowRemind[4] = IsDlgButtonChecked(hDlg, IDC_CHECK11);
		TraySave.bCheckLowRemind[5] = IsDlgButtonChecked(hDlg, IDC_CHECK12);
		TraySave.bCheckLowRemind[6] = IsDlgButtonChecked(hDlg, IDC_CHECK_19);
		TraySave.bCheckLowRemind[7] = IsDlgButtonChecked(hDlg, IDC_CHECK_20);
		TraySave.bCheckLowRemind[8] = IsDlgButtonChecked(hDlg, IDC_CHECK_21);
		TraySave.bCheckLowRemind[9] = IsDlgButtonChecked(hDlg, IDC_CHECK_22);
		TraySave.bCheckLowRemind[10] = IsDlgButtonChecked(hDlg, IDC_CHECK_23);
		TraySave.bCheckLowRemind[11] = IsDlgButtonChecked(hDlg, IDC_CHECK_24);

		WCHAR sz[64];
		GetDlgItemText(hDlg, IDC_EDIT1, sz, 64);
		TraySave.HighRemind[0] = xwtof(sz);
		GetDlgItemText(hDlg, IDC_EDIT2, sz, 64);
		TraySave.HighRemind[1] = xwtof(sz);
		GetDlgItemText(hDlg, IDC_EDIT3, sz, 64);
		TraySave.HighRemind[2] = xwtof(sz);
		GetDlgItemText(hDlg, IDC_EDIT4, sz, 64);
		TraySave.HighRemind[3] = xwtof(sz);
		GetDlgItemText(hDlg, IDC_EDIT5, sz, 64);
		TraySave.HighRemind[4] = xwtof(sz);
		GetDlgItemText(hDlg, IDC_EDIT6, sz, 64);
		TraySave.HighRemind[5] = xwtof(sz);
		GetDlgItemText(hDlg, IDC_EDIT7, sz, 64);
		TraySave.LowRemind[0] = xwtof(sz);
		GetDlgItemText(hDlg, IDC_EDIT8, sz, 64);
		TraySave.LowRemind[1] = xwtof(sz);
		GetDlgItemText(hDlg, IDC_EDIT9, sz, 64);
		TraySave.LowRemind[2] = xwtof(sz);
		GetDlgItemText(hDlg, IDC_EDIT10, sz, 64);
		TraySave.LowRemind[3] = xwtof(sz);
		GetDlgItemText(hDlg, IDC_EDIT11, sz, 64);
		TraySave.LowRemind[4] = xwtof(sz);
		GetDlgItemText(hDlg, IDC_EDIT12, sz, 64);
		TraySave.LowRemind[5] = xwtof(sz);
		GetDlgItemText(hDlg, IDC_EDIT_13, sz, 64);
		TraySave.HighRemind[6] = xwtof(sz);
		GetDlgItemText(hDlg, IDC_EDIT_14, sz, 64);
		TraySave.HighRemind[7] = xwtof(sz);
		GetDlgItemText(hDlg, IDC_EDIT_15, sz, 64);
		TraySave.HighRemind[8] = xwtof(sz);
		GetDlgItemText(hDlg, IDC_EDIT_16, sz, 64);
		TraySave.HighRemind[9] = xwtof(sz);
		GetDlgItemText(hDlg, IDC_EDIT_17, sz, 64);
		TraySave.HighRemind[10] = xwtof(sz);
		GetDlgItemText(hDlg, IDC_EDIT_18, sz, 64);
		TraySave.HighRemind[11] = xwtof(sz);
		GetDlgItemText(hDlg, IDC_EDIT_19, sz, 64);
		TraySave.LowRemind[6] = xwtof(sz);
		GetDlgItemText(hDlg, IDC_EDIT_20, sz, 64);
		TraySave.LowRemind[7] = xwtof(sz);
		GetDlgItemText(hDlg, IDC_EDIT_21, sz, 64);
		TraySave.LowRemind[8] = xwtof(sz);
		GetDlgItemText(hDlg, IDC_EDIT_22, sz, 64);
		TraySave.LowRemind[9] = xwtof(sz);
		GetDlgItemText(hDlg, IDC_EDIT_23, sz, 64);
		TraySave.LowRemind[10] = xwtof(sz);
		GetDlgItemText(hDlg, IDC_EDIT_24, sz, 64);
		TraySave.LowRemind[11] = xwtof(sz);
		GetDlgItemText(hDlg, IDC_EDIT18, TraySave.szPriceName1, 64);
		GetDlgItemText(hDlg, IDC_EDIT19, TraySave.szPriceName2, 64);
		GetDlgItemText(hDlg, IDC_EDIT37, TraySave.szPriceName3, 64);
		GetDlgItemText(hDlg, IDC_EDIT38, TraySave.szPriceName4, 64);
		GetDlgItemText(hDlg, IDC_EDIT_OKX_WEB, TraySave.szOKXWeb, 32);
		if (GetOKXPrice(TraySave.szPriceName1, TraySave.szOKXWeb, &TrayData->fLastPrice1, &TrayData->fOpenPrice1, TrayData->szLastPrice1, NULL))
			TraySave.iPriceInterface[0] = FALSE;
		else if (GetSinaPrice(TraySave.szPriceName1, &TrayData->fLastPrice1, &TrayData->fOpenPrice1, TrayData->szLastPrice1, NULL))
			TraySave.iPriceInterface[0] = TRUE;
		if (GetOKXPrice(TraySave.szPriceName2, TraySave.szOKXWeb, &TrayData->fLastPrice2, &TrayData->fOpenPrice2, TrayData->szLastPrice2, NULL))
			TraySave.iPriceInterface[1] = FALSE;
		else if (GetSinaPrice(TraySave.szPriceName2, &TrayData->fLastPrice2, &TrayData->fOpenPrice2, TrayData->szLastPrice2, NULL))
			TraySave.iPriceInterface[1] = TRUE;
		if (GetOKXPrice(TraySave.szPriceName3, TraySave.szOKXWeb, &TrayData->fLastPrice3, &TrayData->fOpenPrice3, TrayData->szLastPrice3, NULL))
			TraySave.iPriceInterface[2] = FALSE;
		else if (GetSinaPrice(TraySave.szPriceName3, &TrayData->fLastPrice3, &TrayData->fOpenPrice3, TrayData->szLastPrice3, NULL))
			TraySave.iPriceInterface[2] = TRUE;
		if (GetOKXPrice(TraySave.szPriceName4, TraySave.szOKXWeb, &TrayData->fLastPrice4, &TrayData->fOpenPrice4, TrayData->szLastPrice4, NULL))
			TraySave.iPriceInterface[3] = FALSE;
		else if (GetSinaPrice(TraySave.szPriceName4, &TrayData->fLastPrice4, &TrayData->fOpenPrice4, TrayData->szLastPrice4, NULL))
			TraySave.iPriceInterface[3] = TRUE;
		WriteReg();
		break;	
	case WM_COMMAND:
		if (LOWORD(wParam) == IDC_TWOFOUR)
		{
			RECT rc;
			GetWindowRect(hDlg, &rc);
			int x, y, w, h;
			if (lParam != 888)
			{
				if (TraySave.bTwoFour)
				{
					rc.right = (rc.right - rc.left) / 2;
				}
				else
				{
					rc.right = (rc.right - rc.left) * 2;
				}
				rc.left = 0;
				TraySave.bTwoFour = !TraySave.bTwoFour;
				WriteReg();
				SetWH();
				AdjustWindowPos();
			}
			w = rc.right-rc.left;
			h = rc.bottom - rc.top;
			RECT wrc{}, src{};
			if (!IsWindow(hTaskBar) || !GetWindowRect(hTaskBar, &wrc) || !GetScreenRectSafe(hTaskBar, &src, TRUE))
				return TRUE;
			if (wrc.bottom + h > src.bottom)
				y = wrc.top - h;
			else
				y = wrc.bottom;
			if (wrc.right - (wrc.right - wrc.left) / 2 + w / 2 > src.right)
				x = src.right - w;
			else if (wrc.right - (wrc.right - wrc.left) / 2 - w / 2 < src.left)
				x = src.left;
			else
				x = wrc.right - (wrc.right - wrc.left) / 2 - w / 2;
			SetWindowPos(hPrice, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
		}
		break;
	}
	return(INT_PTR)FALSE;
}
void ShowSelectMenu(BOOL bNet)
{
	HMENU hMenu = LoadMenu(hInst, MAKEINTRESOURCEW(IDR_MENU));
	if (!hMenu)
		return;
	HMENU subMenu = NULL;
	if (bNet)
	{
		subMenu = GetSubMenu(hMenu, 0);
		if (subMenu)
		{
			AcquireSRWLockShared(&g_networkLock);
			PIP_ADAPTER_ADDRESSES paa = piaa;
			int n = 1;
			DWORD adapterWalk = 0;
			CheckMenuRadioItem(subMenu, IDC_SELECT_ALL, IDC_SELECT_ALL + 99, IDC_SELECT_ALL, MF_BYCOMMAND);
			while (paa && adapterWalk++ < (DWORD)kMaxTrafficEntries * 4u)
			{
				if (paa->IfType != IF_TYPE_SOFTWARE_LOOPBACK && paa->IfType != IF_TYPE_TUNNEL)
				{
					if (n <= 99)
					{
						AppendMenu(subMenu, MF_BYCOMMAND, IDC_SELECT_ALL + n, paa->FriendlyName ? paa->FriendlyName : L"");
						if (paa->AdapterName && lstrcmpA(paa->AdapterName, TraySave.AdpterName) == 0)
							CheckMenuRadioItem(subMenu, IDC_SELECT_ALL, IDC_SELECT_ALL + 99, IDC_SELECT_ALL + n, MF_BYCOMMAND);
					}
					n++;
				}
				paa = paa->Next;
			}
			ReleaseSRWLockShared(&g_networkLock);
		}
	}
	else
	{
		subMenu = GetSubMenu(hMenu, 1);
		if (!subMenu)
		{
			DestroyMenu(hMenu);
			return;
		}
		WCHAR wDrive[MAX_PATH];
		DWORD dwLen = GetLogicalDriveStrings(MAX_PATH, wDrive);
		if (dwLen && dwLen < ARRAYSIZE(wDrive))
		{
			CheckMenuRadioItem(subMenu, IDC_DISK_ALL, IDC_DISK_ALL + 99, IDC_DISK_ALL, MF_BYCOMMAND);
			DWORD driver_number = dwLen / 4;
			for (DWORD nIndex = 0; nIndex < driver_number; nIndex++)
			{
				LPWSTR dName = wDrive + nIndex * 4;
				if (GetDriveType(dName) != DRIVE_CDROM)
				{
					if (GetPhysicalDriveFromPartitionLetter(dName[0]) != -1)
					{
						dName[2] = 0;
						AppendMenu(subMenu, MF_BYCOMMAND, IDC_DISK_ALL + dName[0], dName);
						if (TraySave.szDisk == dName[0])
							CheckMenuRadioItem(subMenu, IDC_DISK_ALL, IDC_DISK_ALL + 99, IDC_DISK_ALL + dName[0], MF_BYCOMMAND);
					}
				}
			}
		}
	}
	POINT point;
	GetCursorPos(&point);
	SetTimer(hTaskBar, 5, 1200, NULL);
	if (subMenu)
		TrackPopupMenu(subMenu, TPM_LEFTALIGN, point.x, point.y, NULL, hTaskBar, NULL);
	DestroyMenu(hMenu);
}
