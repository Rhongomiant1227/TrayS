#include "PawnIoSetup.h"

#include <winhttp.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <sddl.h>
#include <objbase.h>
#include <vector>

namespace
{
const WCHAR kPawnIoHost[] = L"github.com";
const WCHAR kPawnIoDownloadPath[] = L"/namazso/PawnIO.Setup/releases/download/2.2.0/PawnIO_setup.exe";
const WCHAR kPawnIoSha256[] = L"1f519a22e47187f70a1379a48ca604981c4fcf694f4e65b734aaa74a9fba3032";
const WCHAR kPawnIoFileName[] = L"PawnIO_setup_2.2.0.exe";
const DWORD kPawnIoInstallerBytes = 3410960;
const ULONGLONG kDownloadDeadlineMs = 120000;

struct ScopedHandle
{
	HANDLE value;
	explicit ScopedHandle(HANDLE handle = INVALID_HANDLE_VALUE) : value(handle) {}
	~ScopedHandle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
	ScopedHandle(const ScopedHandle&) = delete;
	ScopedHandle& operator=(const ScopedHandle&) = delete;
};

struct HttpApi
{
	HMODULE module = NULL;
	decltype(&WinHttpOpen) open = NULL;
	decltype(&WinHttpConnect) connect = NULL;
	decltype(&WinHttpOpenRequest) openRequest = NULL;
	decltype(&WinHttpSetTimeouts) setTimeouts = NULL;
	decltype(&WinHttpSetOption) setOption = NULL;
	decltype(&WinHttpQueryOption) queryOption = NULL;
	decltype(&WinHttpSendRequest) sendRequest = NULL;
	decltype(&WinHttpReceiveResponse) receiveResponse = NULL;
	decltype(&WinHttpQueryHeaders) queryHeaders = NULL;
	decltype(&WinHttpReadData) readData = NULL;
	decltype(&WinHttpCloseHandle) closeHandle = NULL;

	BOOL Load()
	{
		module = LoadLibraryExW(L"winhttp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (!module) return FALSE;
#define TRAYS_PAWNIO_HTTP(field, name) field = reinterpret_cast<decltype(field)>(GetProcAddress(module, name))
		TRAYS_PAWNIO_HTTP(open, "WinHttpOpen");
		TRAYS_PAWNIO_HTTP(connect, "WinHttpConnect");
		TRAYS_PAWNIO_HTTP(openRequest, "WinHttpOpenRequest");
		TRAYS_PAWNIO_HTTP(setTimeouts, "WinHttpSetTimeouts");
		TRAYS_PAWNIO_HTTP(setOption, "WinHttpSetOption");
		TRAYS_PAWNIO_HTTP(queryOption, "WinHttpQueryOption");
		TRAYS_PAWNIO_HTTP(sendRequest, "WinHttpSendRequest");
		TRAYS_PAWNIO_HTTP(receiveResponse, "WinHttpReceiveResponse");
		TRAYS_PAWNIO_HTTP(queryHeaders, "WinHttpQueryHeaders");
		TRAYS_PAWNIO_HTTP(readData, "WinHttpReadData");
		TRAYS_PAWNIO_HTTP(closeHandle, "WinHttpCloseHandle");
#undef TRAYS_PAWNIO_HTTP
		return open && connect && openRequest && setTimeouts && setOption && queryOption &&
			sendRequest && receiveResponse && queryHeaders && readData && closeHandle;
	}
	~HttpApi() { if (module) FreeLibrary(module); }
};

struct HttpHandle
{
	HttpApi& api;
	HINTERNET value;
	HttpHandle(HttpApi& http, HINTERNET handle) : api(http), value(handle) {}
	~HttpHandle() { if (value) api.closeHandle(value); }
	HttpHandle(const HttpHandle&) = delete;
	HttpHandle& operator=(const HttpHandle&) = delete;
};

static BOOL IsCancelled(HANDLE cancelEvent)
{
	return cancelEvent && WaitForSingleObject(cancelEvent, 0) == WAIT_OBJECT_0;
}

static DWORD CreateSetupDirectory(BOOL elevated, std::wstring* directory)
{
	WCHAR root[32768] = {};
	DWORD length = elevated ? GetWindowsDirectoryW(root, ARRAYSIZE(root)) :
		GetTempPathW(ARRAYSIZE(root), root);
	if (!length || length >= ARRAYSIZE(root)) return ERROR_BAD_PATHNAME;
	std::wstring parent = root;
	if (parent.back() != L'\\') parent += L'\\';
	if (elevated) parent += L"Temp\\";
	GUID unique{};
	WCHAR suffix[40] = {};
	if (FAILED(CoCreateGuid(&unique)) || !StringFromGUID2(unique, suffix, ARRAYSIZE(suffix)))
		return ERROR_GEN_FAILURE;
	std::wstring path = parent + L"TrayS-PawnIO-" + suffix;
	std::wstring sddl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
	if (!elevated)
	{
		ScopedHandle token(NULL);
		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) return GetLastError();
		DWORD bytes = 0;
		GetTokenInformation(token.value, TokenUser, NULL, 0, &bytes);
		if (!bytes || bytes > 65536) return ERROR_INVALID_SID;
		std::vector<BYTE> user(bytes);
		if (!GetTokenInformation(token.value, TokenUser, user.data(), bytes, &bytes)) return GetLastError();
		LPWSTR sid = NULL;
		if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid))
			return GetLastError();
		sddl += std::wstring(L"(A;OICI;FA;;;") + sid + L")";
		LocalFree(sid);
	}
	PSECURITY_DESCRIPTOR descriptor = NULL;
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
		&descriptor, NULL)) return GetLastError();
	SECURITY_ATTRIBUTES security{ sizeof(security), descriptor, FALSE };
	BOOL created = CreateDirectoryW(path.c_str(), &security);
	DWORD error = created ? ERROR_SUCCESS : GetLastError();
	LocalFree(descriptor);
	if (created) *directory = path;
	return error;
}

static HANDLE OpenLockedInstaller(LPCWSTR path)
{
	return CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
		FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
}

static DWORD VerifyLockedInstaller(HANDLE file, LPCWSTR path)
{
	BY_HANDLE_FILE_INFORMATION info{};
	if (!GetFileInformationByHandle(file, &info)) return GetLastError();
	if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
		info.nFileSizeHigh || info.nFileSizeLow != kPawnIoInstallerBytes)
		return ERROR_BAD_LENGTH;
	LARGE_INTEGER beginning{};
	if (!SetFilePointerEx(file, beginning, NULL, FILE_BEGIN)) return GetLastError();
	HCRYPTPROV provider = 0;
	HCRYPTHASH hash = 0;
	DWORD error = ERROR_SUCCESS;
	if (!CryptAcquireContextW(&provider, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) ||
		!CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) error = GetLastError();
	BYTE buffer[32768];
	while (error == ERROR_SUCCESS)
	{
		DWORD read = 0;
		if (!ReadFile(file, buffer, sizeof(buffer), &read, NULL)) { error = GetLastError(); break; }
		if (!read) break;
		if (!CryptHashData(hash, buffer, read, 0)) error = GetLastError();
	}
	BYTE digest[32] = {};
	DWORD digestBytes = sizeof(digest);
	if (error == ERROR_SUCCESS && (!CryptGetHashParam(hash, HP_HASHVAL, digest, &digestBytes, 0) ||
		digestBytes != sizeof(digest))) error = ERROR_INVALID_DATA;
	if (hash) CryptDestroyHash(hash);
	if (provider) CryptReleaseContext(provider, 0);
	if (error != ERROR_SUCCESS) return error;
	const WCHAR hex[] = L"0123456789abcdef";
	WCHAR actual[65] = {};
	for (DWORD i = 0; i < sizeof(digest); ++i)
	{
		actual[i * 2] = hex[digest[i] >> 4];
		actual[i * 2 + 1] = hex[digest[i] & 15];
	}
	if (lstrcmpW(actual, kPawnIoSha256) != 0) return static_cast<DWORD>(TRUST_E_BAD_DIGEST);
	HMODULE trustModule = LoadLibraryExW(L"wintrust.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!trustModule) return GetLastError();
	auto verify = reinterpret_cast<decltype(&WinVerifyTrust)>(GetProcAddress(trustModule, "WinVerifyTrust"));
	if (!verify) { FreeLibrary(trustModule); return ERROR_PROC_NOT_FOUND; }
	WINTRUST_FILE_INFO trustFile{};
	trustFile.cbStruct = sizeof(trustFile);
	trustFile.pcwszFilePath = path;
	trustFile.hFile = file;
	WINTRUST_DATA trust{};
	trust.cbStruct = sizeof(trust);
	trust.dwUIChoice = WTD_UI_NONE;
	// The exact release digest is pinned above. Validate its embedded signature
	// and certificate trust without initiating unbounded revocation downloads.
	trust.fdwRevocationChecks = WTD_REVOKE_NONE;
	trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
	trust.dwUnionChoice = WTD_CHOICE_FILE;
	trust.pFile = &trustFile;
	trust.dwStateAction = WTD_STATEACTION_VERIFY;
	GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
	LONG result = verify(reinterpret_cast<HWND>(INVALID_HANDLE_VALUE), &action, &trust);
	trust.dwStateAction = WTD_STATEACTION_CLOSE;
	verify(reinterpret_cast<HWND>(INVALID_HANDLE_VALUE), &action, &trust);
	FreeLibrary(trustModule);
	return static_cast<DWORD>(result);
}

static BOOL IsOfficialDownloadTarget(const std::wstring& url)
{
	const std::wstring original = std::wstring(L"https://") + kPawnIoHost + kPawnIoDownloadPath;
	return url == original || url.find(L"https://release-assets.githubusercontent.com/") == 0 ||
		url.find(L"https://objects.githubusercontent.com/") == 0;
}

static DWORD DownloadInstaller(LPCWSTR destination, HANDLE cancelEvent)
{
	HttpApi api;
	if (!api.Load()) return ERROR_PROC_NOT_FOUND;
	HttpHandle session(api, api.open(L"TrayS-PawnIO-setup", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
		WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
	if (!session.value) return GetLastError();
	if (!api.setTimeouts(session.value, 5000, 5000, 15000, 15000)) return GetLastError();
	DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
	if (!api.setOption(session.value, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols)))
		return GetLastError();
	HttpHandle connection(api, api.connect(session.value, kPawnIoHost, INTERNET_DEFAULT_HTTPS_PORT, 0));
	if (!connection.value) return GetLastError();
	HttpHandle request(api, api.openRequest(connection.value, L"GET", kPawnIoDownloadPath, NULL,
		WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
	if (!request.value) return GetLastError();
	DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
	if (!api.setOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects)))
		return GetLastError();
	ULONGLONG started = GetTickCount64();
	if (IsCancelled(cancelEvent)) return ERROR_CANCELLED;
	if (!api.sendRequest(request.value, L"Accept: application/octet-stream\r\n", (DWORD)-1L,
		NULL, 0, 0, 0) || !api.receiveResponse(request.value, NULL)) return GetLastError();
	DWORD status = 0, statusBytes = sizeof(status);
	if (!api.queryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
		WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusBytes, WINHTTP_NO_HEADER_INDEX)) return GetLastError();
	if (status != 200) return ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
	WCHAR finalUrl[8192] = {};
	DWORD urlBytes = sizeof(finalUrl);
	if (!api.queryOption(request.value, WINHTTP_OPTION_URL, finalUrl, &urlBytes)) return GetLastError();
	finalUrl[ARRAYSIZE(finalUrl) - 1] = 0;
	if (!IsOfficialDownloadTarget(finalUrl)) return ERROR_ACCESS_DENIED;
	ScopedHandle file(CreateFileW(destination, GENERIC_WRITE, 0, NULL, CREATE_NEW,
		FILE_ATTRIBUTE_TEMPORARY, NULL));
	if (file.value == INVALID_HANDLE_VALUE) return GetLastError();
	DWORD total = 0;
	for (;;)
	{
		if (IsCancelled(cancelEvent)) return ERROR_CANCELLED;
		if (GetTickCount64() - started >= kDownloadDeadlineMs) return ERROR_TIMEOUT;
		BYTE buffer[32768];
		DWORD read = 0, written = 0;
		if (!api.readData(request.value, buffer, sizeof(buffer), &read)) return GetLastError();
		if (!read) break;
		if (read > kPawnIoInstallerBytes - total) return ERROR_BAD_LENGTH;
		if (!WriteFile(file.value, buffer, read, &written, NULL) || written != read) return ERROR_WRITE_FAULT;
		total += read;
	}
	return total == kPawnIoInstallerBytes ? ERROR_SUCCESS : ERROR_BAD_LENGTH;
}
}

BOOL IsPawnIoMissingError(DWORD error)
{
	return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

BOOL IsPawnIoSetupArchitectureSupported(WORD nativeArchitecture)
{
	// The application and the pinned official installer both target AMD64.
	return nativeArchitecture == PROCESSOR_ARCHITECTURE_AMD64;
}

BOOL IsPawnIoSetupSupported()
{
	SYSTEM_INFO info{};
	GetNativeSystemInfo(&info);
	return IsPawnIoSetupArchitectureSupported(info.wProcessorArchitecture);
}

BOOL ShouldOfferPawnIoSetup(BOOL monitoringEnabled, DWORD deviceError,
	BOOL setupInProgress, BOOL alreadyOffered)
{
	return monitoringEnabled && IsPawnIoMissingError(deviceError) && !setupInProgress && !alreadyOffered;
}

DWORD VerifyPawnIoInstaller(LPCWSTR path)
{
	if (!path || !*path) return ERROR_INVALID_PARAMETER;
	ScopedHandle file(OpenLockedInstaller(path));
	if (file.value == INVALID_HANDLE_VALUE) return GetLastError();
	return VerifyLockedInstaller(file.value, path);
}

void RemovePawnIoDownload(const std::wstring& directory, const std::wstring& path)
{
	// Delete only our one known file in its GUID directory; never recurse.
	if (!directory.empty() && path == directory + L"\\" + kPawnIoFileName)
	{
		DeleteFileW(path.c_str());
		RemoveDirectoryW(directory.c_str());
	}
}

DWORD DownloadVerifiedPawnIoInstaller(std::wstring* directory, std::wstring* path,
	volatile LONG* phase, HANDLE cancelEvent)
{
	if (!directory || !path || !phase) return ERROR_INVALID_PARAMETER;
	if (!IsPawnIoSetupSupported()) return ERROR_NOT_SUPPORTED;
	directory->clear();
	path->clear();
	DWORD error = CreateSetupDirectory(FALSE, directory);
	if (error != ERROR_SUCCESS) return error;
	*path = *directory + L"\\" + kPawnIoFileName;
	InterlockedExchange(phase, PAWNIO_SETUP_DOWNLOADING);
	error = DownloadInstaller(path->c_str(), cancelEvent);
	if (error == ERROR_SUCCESS && IsCancelled(cancelEvent)) error = ERROR_CANCELLED;
	if (error == ERROR_SUCCESS)
	{
		InterlockedExchange(phase, PAWNIO_SETUP_VERIFYING);
		error = VerifyPawnIoInstaller(path->c_str());
	}
	if (error != ERROR_SUCCESS) RemovePawnIoDownload(*directory, *path);
	return error;
}

DWORD InstallVerifiedPawnIoDriver(LPCWSTR sourcePath)
{
	if (!sourcePath || !*sourcePath) return ERROR_INVALID_PARAMETER;
	if (!IsPawnIoSetupSupported()) return ERROR_NOT_SUPPORTED;
	ScopedHandle token(NULL);
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) return GetLastError();
	TOKEN_ELEVATION elevation{};
	DWORD bytes = 0;
	if (!GetTokenInformation(token.value, TokenElevation, &elevation, sizeof(elevation), &bytes) ||
		!elevation.TokenIsElevated) return ERROR_ELEVATION_REQUIRED;
	ScopedHandle source(OpenLockedInstaller(sourcePath));
	if (source.value == INVALID_HANDLE_VALUE) return GetLastError();
	DWORD error = VerifyLockedInstaller(source.value, sourcePath);
	if (error != ERROR_SUCCESS) return error;
	std::wstring directory;
	error = CreateSetupDirectory(TRUE, &directory);
	if (error != ERROR_SUCCESS) return error;
	const std::wstring staged = directory + L"\\" + kPawnIoFileName;
	{
		// Lock this protected directory against rename/replacement until the
		// verified installer exits. Ordinary users have no access to its DACL.
		ScopedHandle directoryLock(CreateFileW(directory.c_str(), FILE_LIST_DIRECTORY,
			FILE_SHARE_READ, NULL, OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL));
		if (directoryLock.value == INVALID_HANDLE_VALUE) error = GetLastError();
		if (error == ERROR_SUCCESS)
		{
			ScopedHandle output(CreateFileW(staged.c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW,
				FILE_ATTRIBUTE_NORMAL, NULL));
			if (output.value == INVALID_HANDLE_VALUE) error = GetLastError();
			LARGE_INTEGER beginning{};
			if (error == ERROR_SUCCESS && !SetFilePointerEx(source.value, beginning, NULL, FILE_BEGIN))
				error = GetLastError();
			while (error == ERROR_SUCCESS)
			{
				BYTE buffer[32768];
				DWORD read = 0, written = 0;
				if (!ReadFile(source.value, buffer, sizeof(buffer), &read, NULL)) { error = GetLastError(); break; }
				if (!read) break;
				if (!WriteFile(output.value, buffer, read, &written, NULL) || written != read)
					error = ERROR_WRITE_FAULT;
			}
		}
		if (error == ERROR_SUCCESS)
		{
			ScopedHandle verified(OpenLockedInstaller(staged.c_str()));
			if (verified.value == INVALID_HANDLE_VALUE) error = GetLastError();
			else error = VerifyLockedInstaller(verified.value, staged.c_str());
			if (error == ERROR_SUCCESS)
			{
				std::wstring command = L"\"" + staged + L"\" -install -silent";
				WCHAR workingDirectory[MAX_PATH] = {};
				DWORD length = GetSystemDirectoryW(workingDirectory, ARRAYSIZE(workingDirectory));
				STARTUPINFOW startup{ sizeof(startup) };
				startup.dwFlags = STARTF_USESHOWWINDOW;
				startup.wShowWindow = SW_HIDE;
				PROCESS_INFORMATION process{};
				if (!length || length >= ARRAYSIZE(workingDirectory)) error = ERROR_BAD_PATHNAME;
				else if (!CreateProcessW(staged.c_str(), &command[0], NULL, NULL, FALSE,
					CREATE_NO_WINDOW, NULL, workingDirectory, &startup, &process)) error = GetLastError();
				else
				{
					ScopedHandle processHandle(process.hProcess), threadHandle(process.hThread);
					// Never terminate an in-progress kernel-driver installation.
					if (WaitForSingleObject(processHandle.value, INFINITE) != WAIT_OBJECT_0 ||
						!GetExitCodeProcess(processHandle.value, &error)) error = ERROR_INSTALL_FAILURE;
				}
			}
		}
	}
	RemovePawnIoDownload(directory, staged);
	return error;
}
