#include "Update.h"

#include <wincrypt.h>
#include <winhttp.h>
#include <shellapi.h>
#include <sddl.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <climits>

namespace
{
const DWORD kApiResponseLimit = 256u * 1024u;
const ULONGLONG kPackageLimit = 128ull * 1024ull * 1024ull;
const DWORD kUpdateTimeoutMs = 5000;
const WCHAR kUpdateSettingName[] = L"TrayS.update.dat";
const WCHAR kGitHubDownloadPrefix[] = L"https://github.com/Rhongomiant1227/TrayS/releases/download/";

struct VersionNumber
{
	int major;
	int minor;
	int patch;
};

struct UpdateThreadContext
{
	HWND notifyWindow;
	BOOL automatic;
};

volatile LONG g_updateInProgress = 0;

typedef HINTERNET(WINAPI* PFN_WINHTTP_OPEN)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
typedef HINTERNET(WINAPI* PFN_WINHTTP_CONNECT)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
typedef HINTERNET(WINAPI* PFN_WINHTTP_OPEN_REQUEST)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR*, DWORD, DWORD_PTR);
typedef BOOL(WINAPI* PFN_WINHTTP_SEND_REQUEST)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);
typedef BOOL(WINAPI* PFN_WINHTTP_RECEIVE_RESPONSE)(HINTERNET, LPVOID);
typedef BOOL(WINAPI* PFN_WINHTTP_QUERY_DATA_AVAILABLE)(HINTERNET, LPDWORD);
typedef BOOL(WINAPI* PFN_WINHTTP_READ_DATA)(HINTERNET, LPVOID, DWORD, LPDWORD);
typedef BOOL(WINAPI* PFN_WINHTTP_CLOSE_HANDLE)(HINTERNET);
typedef BOOL(WINAPI* PFN_WINHTTP_SET_TIMEOUTS)(HINTERNET, int, int, int, int);
typedef BOOL(WINAPI* PFN_WINHTTP_QUERY_HEADERS)(HINTERNET, DWORD, LPCWSTR, LPVOID, LPDWORD, LPDWORD);

struct WinHttpApi
{
	HMODULE module;
	PFN_WINHTTP_OPEN open;
	PFN_WINHTTP_CONNECT connect;
	PFN_WINHTTP_OPEN_REQUEST openRequest;
	PFN_WINHTTP_SEND_REQUEST sendRequest;
	PFN_WINHTTP_RECEIVE_RESPONSE receiveResponse;
	PFN_WINHTTP_QUERY_DATA_AVAILABLE queryDataAvailable;
	PFN_WINHTTP_READ_DATA readData;
	PFN_WINHTTP_CLOSE_HANDLE closeHandle;
	PFN_WINHTTP_SET_TIMEOUTS setTimeouts;
	PFN_WINHTTP_QUERY_HEADERS queryHeaders;
};

static BOOL LoadWinHttpApi(WinHttpApi* api)
{
	if (!api)
		return FALSE;
	ZeroMemory(api, sizeof(*api));
	api->module = LoadLibraryExW(L"winhttp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!api->module)
		return FALSE;
	api->open = (PFN_WINHTTP_OPEN)GetProcAddress(api->module, "WinHttpOpen");
	api->connect = (PFN_WINHTTP_CONNECT)GetProcAddress(api->module, "WinHttpConnect");
	api->openRequest = (PFN_WINHTTP_OPEN_REQUEST)GetProcAddress(api->module, "WinHttpOpenRequest");
	api->sendRequest = (PFN_WINHTTP_SEND_REQUEST)GetProcAddress(api->module, "WinHttpSendRequest");
	api->receiveResponse = (PFN_WINHTTP_RECEIVE_RESPONSE)GetProcAddress(api->module, "WinHttpReceiveResponse");
	api->queryDataAvailable = (PFN_WINHTTP_QUERY_DATA_AVAILABLE)GetProcAddress(api->module, "WinHttpQueryDataAvailable");
	api->readData = (PFN_WINHTTP_READ_DATA)GetProcAddress(api->module, "WinHttpReadData");
	api->closeHandle = (PFN_WINHTTP_CLOSE_HANDLE)GetProcAddress(api->module, "WinHttpCloseHandle");
	api->setTimeouts = (PFN_WINHTTP_SET_TIMEOUTS)GetProcAddress(api->module, "WinHttpSetTimeouts");
	api->queryHeaders = (PFN_WINHTTP_QUERY_HEADERS)GetProcAddress(api->module, "WinHttpQueryHeaders");
	if (!api->open || !api->connect || !api->openRequest || !api->sendRequest ||
		!api->receiveResponse || !api->queryDataAvailable || !api->readData ||
		!api->closeHandle || !api->setTimeouts)
	{
		FreeLibrary(api->module);
		ZeroMemory(api, sizeof(*api));
		return FALSE;
	}
	return TRUE;
}

static void UnloadWinHttpApi(WinHttpApi* api)
{
	if (api && api->module)
	{
		FreeLibrary(api->module);
		ZeroMemory(api, sizeof(*api));
	}
}

static BOOL GetCurrentExePath(WCHAR* path, DWORD pathChars)
{
	if (!path || pathChars < 2)
		return FALSE;
	DWORD length = GetModuleFileNameW(NULL, path, pathChars);
	return length != 0 && length < pathChars;
}

static BOOL BuildSiblingPath(LPCWSTR name, WCHAR* path, DWORD pathChars)
{
	if (!name || !name[0] || !path || pathChars < 2 || !GetCurrentExePath(path, pathChars))
		return FALSE;
	DWORD length = lstrlenW(path);
	while (length > 0 && path[length - 1] != L'\\' && path[length - 1] != L'/')
		--length;
	DWORD nameLength = lstrlenW(name);
	if (length == 0 || nameLength >= pathChars - length)
		return FALSE;
	CopyMemory(path + length, name, (nameLength + 1) * sizeof(WCHAR));
	return TRUE;
}

static BOOL ParseVersionString(LPCWSTR text, VersionNumber* version)
{
	if (!text || !version || text[0] != L'v')
		return FALSE;
	const WCHAR* cursor = text + 1;
	int values[3] = {};
	for (int index = 0; index < 3; ++index)
	{
		if (cursor[0] < L'0' || cursor[0] > L'9')
			return FALSE;
		int value = 0;
		int digits = 0;
		while (cursor[0] >= L'0' && cursor[0] <= L'9')
		{
			if (++digits > 3)
				return FALSE;
			value = value * 10 + (cursor[0] - L'0');
			++cursor;
		}
		values[index] = value;
		if (index != 2)
		{
			if (*cursor != L'.')
				return FALSE;
			++cursor;
		}
	}
	if (*cursor != L'\0')
		return FALSE;
	version->major = values[0];
	version->minor = values[1];
	version->patch = values[2];
	return TRUE;
}

static int CompareVersions(const VersionNumber& left, const VersionNumber& right)
{
	if (left.major != right.major)
		return left.major < right.major ? -1 : 1;
	if (left.minor != right.minor)
		return left.minor < right.minor ? -1 : 1;
	if (left.patch != right.patch)
		return left.patch < right.patch ? -1 : 1;
	return 0;
}

static BOOL GetCurrentVersion(VersionNumber* version)
{
	if (!version)
		return FALSE;
	version->major = TRAYS_VERSION_MAJOR;
	version->minor = TRAYS_VERSION_MINOR;
	version->patch = TRAYS_VERSION_PATCH;
	WCHAR path[32768] = {};
	if (!GetCurrentExePath(path, ARRAYSIZE(path)))
		return TRUE;
	HMODULE versionDll = LoadLibraryExW(L"version.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!versionDll)
		return TRUE;
	typedef DWORD(WINAPI* PFN_GET_SIZE)(LPCWSTR, LPDWORD);
	typedef BOOL(WINAPI* PFN_GET_INFO)(LPCWSTR, DWORD, DWORD, LPVOID);
	typedef BOOL(WINAPI* PFN_QUERY)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
	PFN_GET_SIZE getSize = (PFN_GET_SIZE)GetProcAddress(versionDll, "GetFileVersionInfoSizeW");
	PFN_GET_INFO getInfo = (PFN_GET_INFO)GetProcAddress(versionDll, "GetFileVersionInfoW");
	PFN_QUERY query = (PFN_QUERY)GetProcAddress(versionDll, "VerQueryValueW");
	DWORD ignored = 0;
	DWORD size = getSize ? getSize(path, &ignored) : 0;
	if (getInfo && query && size > 0 && size <= 1024u * 1024u)
	{
		std::vector<BYTE> data(size);
		if (getInfo(path, 0, size, data.data()))
		{
			VS_FIXEDFILEINFO* fixed = NULL;
			UINT fixedLength = 0;
			if (query(data.data(), L"\\", (LPVOID*)&fixed, &fixedLength) && fixed && fixedLength >= sizeof(VS_FIXEDFILEINFO))
			{
				version->major = HIWORD(fixed->dwFileVersionMS);
				version->minor = LOWORD(fixed->dwFileVersionMS);
				version->patch = HIWORD(fixed->dwFileVersionLS);
			}
		}
	}
	FreeLibrary(versionDll);
	return TRUE;
}

static std::string JsonUnescapeString(const std::string& source)
{
	std::string result;
	for (size_t index = 0; index < source.size(); ++index)
	{
		if (source[index] != '\\' || index + 1 >= source.size())
		{
			result.push_back(source[index]);
			continue;
		}
		char escaped = source[++index];
		switch (escaped)
		{
		case '"': result.push_back('"'); break;
		case '\\': result.push_back('\\'); break;
		case '/': result.push_back('/'); break;
		case 'b': result.push_back('\b'); break;
		case 'f': result.push_back('\f'); break;
		case 'n': result.push_back('\n'); break;
		case 'r': result.push_back('\r'); break;
		case 't': result.push_back('\t'); break;
		default: result.push_back(escaped); break;
		}
	}
	return result;
}

static BOOL FindJsonString(const std::string& json, const char* key, size_t begin, size_t end, std::string* value, size_t* valueEnd)
{
	if (!value || !key || begin >= json.size())
		return FALSE;
	if (end == 0 || end > json.size())
		end = json.size();
	std::string marker = "\"";
	marker += key;
	marker += "\"";
	size_t keyPosition = json.find(marker, begin);
	if (keyPosition == std::string::npos || keyPosition >= end)
		return FALSE;
	size_t colon = json.find(':', keyPosition + marker.size());
	if (colon == std::string::npos || colon >= end)
		return FALSE;
	size_t quote = json.find('"', colon + 1);
	if (quote == std::string::npos || quote >= end)
		return FALSE;
	size_t cursor = quote + 1;
	BOOL escaped = FALSE;
	for (; cursor < end; ++cursor)
	{
		char c = json[cursor];
		if (escaped)
		{
			escaped = FALSE;
			continue;
		}
		if (c == '\\')
		{
			escaped = TRUE;
			continue;
		}
		if (c == '"')
			break;
	}
	if (cursor >= end)
		return FALSE;
	*value = JsonUnescapeString(json.substr(quote + 1, cursor - quote - 1));
	if (valueEnd)
		*valueEnd = cursor + 1;
	return TRUE;
}

static BOOL Utf8ToWide(const std::string& source, WCHAR* destination, DWORD destinationChars)
{
	if (!destination || destinationChars < 2 || source.empty() || source.size() > INT_MAX)
		return FALSE;
	int converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, source.data(), (int)source.size(), destination, (int)destinationChars - 1);
	if (converted <= 0)
		return FALSE;
	destination[converted] = L'\0';
	return TRUE;
}

static BOOL ReadResponse(WinHttpApi* api, HINTERNET request, std::string* response, ULONGLONG maximum)
{
	if (!api || !request || !response)
		return FALSE;
	response->clear();
	for (;;)
	{
		DWORD available = 0;
		if (!api->queryDataAvailable(request, &available))
			return FALSE;
		if (available == 0)
			return TRUE;
		if ((ULONGLONG)response->size() + available > maximum)
			return FALSE;
		size_t oldSize = response->size();
		response->resize(oldSize + available);
		DWORD read = 0;
		if (!api->readData(request, &(*response)[oldSize], available, &read))
			return FALSE;
		if (read < available)
			response->resize(oldSize + read);
		if (read == 0)
			return TRUE;
	}
}

static BOOL SendGitHubRequest(LPCWSTR path, std::string* response, ULONGLONG maximum)
{
	WinHttpApi api{};
	if (!LoadWinHttpApi(&api))
		return FALSE;
	BOOL success = FALSE;
	HINTERNET session = api.open(L"TrayS-maintenance", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	HINTERNET connection = session ? api.connect(session, TRAYS_UPDATE_API_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0) : NULL;
	HINTERNET request = connection ? api.openRequest(connection, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE, 0) : NULL;
	if (request)
	{
		api.setTimeouts(request, kUpdateTimeoutMs, kUpdateTimeoutMs, kUpdateTimeoutMs, kUpdateTimeoutMs);
		LPCWSTR headers = L"Accept: application/vnd.github+json\r\nUser-Agent: TrayS-maintenance\r\n";
		if (api.sendRequest(request, headers, (DWORD)-1L, NULL, 0, 0, 0) && api.receiveResponse(request, NULL))
		{
			DWORD status = 200;
			DWORD statusSize = sizeof(status);
			if (api.queryHeaders)
				api.queryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
			success = status >= 200 && status < 300 && ReadResponse(&api, request, response, maximum);
		}
		api.closeHandle(request);
	}
	if (connection)
		api.closeHandle(connection);
	if (session)
		api.closeHandle(session);
	UnloadWinHttpApi(&api);
	return success;
}

static BOOL ParseSha256(const std::string& text, WCHAR* output)
{
	if (!output || text.size() != 71 || text.compare(0, 7, "sha256:") != 0)
		return FALSE;
	for (size_t i = 7; i < text.size(); ++i)
	{
		char c = text[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
			return FALSE;
		output[i - 7] = (WCHAR)((c >= 'A' && c <= 'F') ? (c - 'A' + 'a') : c);
	}
	output[64] = L'\0';
	return TRUE;
}

// Returns 1 when a matching newer release is ready to download, 0 when the
// installed version is current, and -1 when the response is unavailable or
// fails validation. Keeping these states distinct lets manual checks explain
// a network/API failure instead of falsely claiming that the app is current.
static int QueryRelease(TRAYS_UPDATE_INFO* info)
{
	if (!info)
		return -1;
	std::string json;
	if (!SendGitHubRequest(TRAYS_UPDATE_API_PATH, &json, kApiResponseLimit))
		return -1;
	std::string tag;
	if (!FindJsonString(json, "tag_name", 0, json.size(), &tag, NULL) || tag.size() > 31)
		return -1;
	WCHAR tagWide[32] = {};
	if (!Utf8ToWide(tag, tagWide, ARRAYSIZE(tagWide)))
		return -1;
	VersionNumber current{}, latest{};
	GetCurrentVersion(&current);
	if (!ParseVersionString(tagWide, &latest))
		return -1;
	if (CompareVersions(latest, current) <= 0)
		return 0;
	WCHAR versionText[32] = {};
	wsprintfW(versionText, L"%d.%d.%d", latest.major, latest.minor, latest.patch);
#ifdef _WIN64
	LPCWSTR architecture = L"x64";
#else
	LPCWSTR architecture = L"x86";
#endif
	WCHAR expectedName[128] = {};
	wsprintfW(expectedName, L"TrayS_%s_%s.zip", versionText, architecture);
	std::string expectedNameUtf8;
	int utf8Length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, expectedName, -1, NULL, 0, NULL, NULL);
	if (utf8Length <= 1)
		return -1;
	std::vector<char> utf8Name((size_t)utf8Length);
	WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, expectedName, -1, utf8Name.data(), utf8Length, NULL, NULL);
	expectedNameUtf8.assign(utf8Name.data());
	size_t cursor = json.find("\"assets\"");
	if (cursor == std::string::npos)
		return -1;
	std::string assetName;
	std::string url;
	std::string digest;
	BOOL found = FALSE;
	while (cursor < json.size())
	{
		size_t nameKey = json.find("\"name\"", cursor);
		if (nameKey == std::string::npos)
			break;
		size_t nextNameKey = json.find("\"name\"", nameKey + 6);
		size_t assetEnd = nextNameKey == std::string::npos ? json.size() : nextNameKey;
		size_t nameEnd = 0;
		std::string candidate;
		if (FindJsonString(json, "name", nameKey, assetEnd, &candidate, &nameEnd) && candidate == expectedNameUtf8)
		{
			if (FindJsonString(json, "browser_download_url", nameEnd, assetEnd, &url, NULL) &&
				FindJsonString(json, "digest", nameEnd, assetEnd, &digest, NULL))
			{
				assetName = candidate;
				found = TRUE;
				break;
			}
		}
		cursor = nameKey + 6;
	}
	const size_t downloadPrefixLength = ARRAYSIZE(kGitHubDownloadPrefix) - 1;
	if (!found || url.size() == 0 || url.compare(0, downloadPrefixLength, "https://github.com/Rhongomiant1227/TrayS/releases/download/") != 0)
		return -1;
	if (!ParseSha256(digest, info->sha256))
		return -1;
	if (!Utf8ToWide(tag, info->version, ARRAYSIZE(info->version)) ||
		!Utf8ToWide(assetName, info->assetName, ARRAYSIZE(info->assetName)) ||
		!Utf8ToWide(url, info->downloadUrl, ARRAYSIZE(info->downloadUrl)))
		return -1;
	return 1;
}

static BOOL HashFileSha256(LPCWSTR path, WCHAR output[65])
{
	if (!path || !output)
		return FALSE;
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return FALSE;
	HCRYPTPROV provider = 0;
	HCRYPTHASH hash = 0;
	BOOL success = CryptAcquireContextW(&provider, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
		CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash);
	BYTE buffer[64 * 1024];
	if (success)
	{
		for (;;)
		{
			DWORD read = 0;
			if (!ReadFile(file, buffer, sizeof(buffer), &read, NULL))
			{
				success = FALSE;
				break;
			}
			if (read == 0)
				break;
			if (!CryptHashData(hash, buffer, read, 0))
			{
				success = FALSE;
				break;
			}
		}
	}
	BYTE digest[32] = {};
	DWORD digestSize = sizeof(digest);
	if (success)
		success = CryptGetHashParam(hash, HP_HASHVAL, digest, &digestSize, 0) && digestSize == sizeof(digest);
	if (success)
	{
		for (DWORD i = 0; i < digestSize; ++i)
			wsprintfW(output + i * 2, L"%02x", digest[i]);
		output[64] = L'\0';
	}
	if (hash)
		CryptDestroyHash(hash);
	if (provider)
		CryptReleaseContext(provider, 0);
	CloseHandle(file);
	return success;
}

static BOOL GetTempZipPath(WCHAR* path, DWORD pathChars)
{
	if (!path || pathChars < MAX_PATH)
		return FALSE;
	WCHAR tempPath[MAX_PATH] = {};
	DWORD length = GetTempPathW(ARRAYSIZE(tempPath), tempPath);
	if (length == 0 || length >= ARRAYSIZE(tempPath))
		return FALSE;
	WCHAR temporary[MAX_PATH] = {};
	if (GetTempFileNameW(tempPath, L"TrS", 0, temporary) == 0)
		return FALSE;
	DeleteFileW(temporary);
	if (lstrlenW(temporary) + 5 >= (int)pathChars)
		return FALSE;
	lstrcpyW(path, temporary);
	lstrcatW(path, L".zip");
	return TRUE;
}

static BOOL DownloadAsset(LPCWSTR url, LPCWSTR destination)
{
	const size_t downloadPrefixLength = ARRAYSIZE(kGitHubDownloadPrefix) - 1;
	if (!url || !destination || wcsncmp(url, kGitHubDownloadPrefix, downloadPrefixLength) != 0)
		return FALSE;
	WCHAR requestPath[4096] = {};
	if (lstrlenW(url + downloadPrefixLength) == 0 || lstrlenW(url + downloadPrefixLength) >= ARRAYSIZE(requestPath))
		return FALSE;
	lstrcpyW(requestPath, url + downloadPrefixLength);
	if (wcsstr(requestPath, L"..") != NULL || wcsstr(requestPath, L"?") != NULL || wcsstr(requestPath, L"#") != NULL)
		return FALSE;
	WCHAR fullPath[4096] = L"/Rhongomiant1227/TrayS/releases/download/";
	if (lstrlenW(fullPath) + lstrlenW(requestPath) >= ARRAYSIZE(fullPath))
		return FALSE;
	lstrcatW(fullPath, requestPath);
	WinHttpApi api{};
	if (!LoadWinHttpApi(&api))
		return FALSE;
	BOOL success = FALSE;
	HINTERNET session = api.open(L"TrayS-maintenance", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	HINTERNET connection = session ? api.connect(session, L"github.com", INTERNET_DEFAULT_HTTPS_PORT, 0) : NULL;
	HINTERNET request = connection ? api.openRequest(connection, L"GET", fullPath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE, 0) : NULL;
	HANDLE file = INVALID_HANDLE_VALUE;
	if (request)
	{
		api.setTimeouts(request, kUpdateTimeoutMs, kUpdateTimeoutMs, kUpdateTimeoutMs, kUpdateTimeoutMs);
		LPCWSTR headers = L"Accept: application/octet-stream\r\nUser-Agent: TrayS-maintenance\r\n";
		if (api.sendRequest(request, headers, (DWORD)-1L, NULL, 0, 0, 0) && api.receiveResponse(request, NULL))
		{
			file = CreateFileW(destination, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
			ULONGLONG total = 0;
			if (file != INVALID_HANDLE_VALUE)
			{
				success = TRUE;
				for (;;)
				{
					DWORD available = 0;
					if (!api.queryDataAvailable(request, &available)) { success = FALSE; break; }
					if (available == 0) break;
					if (total + available > kPackageLimit) { success = FALSE; break; }
					BYTE buffer[64 * 1024];
					DWORD remaining = available;
					while (remaining > 0)
					{
						DWORD want = remaining > sizeof(buffer) ? (DWORD)sizeof(buffer) : remaining;
						DWORD read = 0;
						if (!api.readData(request, buffer, want, &read) || read == 0) { success = FALSE; remaining = 0; break; }
						DWORD written = 0;
						if (!WriteFile(file, buffer, read, &written, NULL) || written != read) { success = FALSE; remaining = 0; break; }
						remaining -= read;
						total += read;
					}
					if (!success) break;
				}
			}
		}
	}
	if (file != INVALID_HANDLE_VALUE)
		CloseHandle(file);
	if (request) api.closeHandle(request);
	if (connection) api.closeHandle(connection);
	if (session) api.closeHandle(session);
	UnloadWinHttpApi(&api);
	if (!success)
		DeleteFileW(destination);
	return success;
}

static void SetUpdateError(TRAYS_UPDATE_MESSAGE* message, LPCWSTR text)
{
	if (!message)
		return;
	message->event = TRAYS_UPDATE_EVENT_ERROR;
	if (text)
		lstrcpynW(message->error, text, ARRAYSIZE(message->error));
}

static DWORD WINAPI UpdateThreadProc(LPVOID parameter)
{
	UpdateThreadContext context = *(UpdateThreadContext*)parameter;
	HeapFree(GetProcessHeap(), 0, parameter);
	TRAYS_UPDATE_MESSAGE* message = (TRAYS_UPDATE_MESSAGE*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(TRAYS_UPDATE_MESSAGE));
	if (!message)
	{
		InterlockedExchange(&g_updateInProgress, 0);
		return 0;
	}
	message->info.automatic = context.automatic;
	int releaseState = QueryRelease(&message->info);
	if (releaseState == 0)
	{
		message->event = TRAYS_UPDATE_EVENT_NO_UPDATE;
	}
	else if (releaseState < 0)
	{
		SetUpdateError(message, L"无法访问 GitHub 更新信息，或 Release 元数据未通过校验。");
	}
	else
	{
		if (!GetTempZipPath(message->info.downloadPath, ARRAYSIZE(message->info.downloadPath)) ||
			!DownloadAsset(message->info.downloadUrl, message->info.downloadPath))
		{
			SetUpdateError(message, L"无法下载匹配当前架构的更新包。");
		}
		else
		{
			WCHAR actual[65] = {};
			if (!HashFileSha256(message->info.downloadPath, actual) || lstrcmpiW(actual, message->info.sha256) != 0)
			{
				DeleteFileW(message->info.downloadPath);
				message->info.downloadPath[0] = L'\0';
				SetUpdateError(message, L"更新包 SHA-256 校验失败，已拒绝安装。");
			}
			else
				message->event = TRAYS_UPDATE_EVENT_READY;
		}
	}
	if (!IsWindow(context.notifyWindow) || !PostMessageW(context.notifyWindow, WM_APP_TRAYS_UPDATE, 0, (LPARAM)message))
	{
		FreeTraySUpdateMessage(message, TRUE);
	}
	return 0;
}

static BOOL ParseHexSha256(LPCWSTR text)
{
	if (!text || lstrlenW(text) != 64)
		return FALSE;
	for (int i = 0; i < 64; ++i)
	{
		WCHAR c = text[i];
		if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F')))
			return FALSE;
	}
	return TRUE;
}

static std::wstring PowerShellQuote(LPCWSTR text)
{
	std::wstring value = L"'";
	if (text)
	{
		for (const WCHAR* cursor = text; *cursor; ++cursor)
		{
			if (*cursor == L'\'') value += L"''";
			else value.push_back(*cursor);
		}
	}
	value += L"'";
	return value;
}

static BOOL WriteUtf16File(LPCWSTR path, const std::wstring& text)
{
	HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return FALSE;
	const WORD bom = 0xFEFF;
	DWORD written = 0;
	BOOL success = WriteFile(file, &bom, sizeof(bom), &written, NULL) && written == sizeof(bom);
	if (success && !text.empty())
		success = WriteFile(file, text.data(), (DWORD)(text.size() * sizeof(WCHAR)), &written, NULL) && written == text.size() * sizeof(WCHAR);
	CloseHandle(file);
	return success;
}

static std::wstring QuoteWindowsArgument(const std::wstring& argument)
{
	std::wstring quoted = L"\"";
	size_t backslashes = 0;
	for (WCHAR character : argument)
	{
		if (character == L'\\')
		{
			++backslashes;
			continue;
		}
		if (character == L'\"')
		{
			quoted.append(backslashes * 2 + 1, L'\\');
			quoted.push_back(character);
			backslashes = 0;
			continue;
		}
		quoted.append(backslashes, L'\\');
		backslashes = 0;
		quoted.push_back(character);
	}
	quoted.append(backslashes * 2, L'\\');
	quoted.push_back(L'\"');
	return quoted;
}

static BOOL CanWriteUpdateTargetDirectory(LPCWSTR targetPath)
{
	if (!targetPath || !targetPath[0])
		return FALSE;
	std::wstring directory(targetPath);
	size_t separator = directory.find_last_of(L"\\/");
	if (separator == std::wstring::npos)
		return FALSE;
	directory.resize(separator + 1);
	for (DWORD attempt = 0; attempt < 4; ++attempt)
	{
		WCHAR suffix[96] = {};
		wsprintfW(suffix, L".TrayS-update-probe-%lu-%lu.tmp", GetCurrentProcessId(), GetTickCount() + attempt);
		std::wstring probe = directory + suffix;
		HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
			CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
		if (file != INVALID_HANDLE_VALUE)
		{
			CloseHandle(file);
			return TRUE;
		}
		DWORD error = GetLastError();
		if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
			return FALSE;
	}
	return FALSE;
}

static BOOL GetPowerShellPath(WCHAR* path, DWORD pathChars)
{
	if (!path || pathChars < MAX_PATH)
		return FALSE;
	DWORD length = GetSystemDirectoryW(path, pathChars);
	if (length == 0 || length >= pathChars || length + ARRAYSIZE(L"\\WindowsPowerShell\\v1.0\\powershell.exe") >= pathChars)
		return FALSE;
	lstrcatW(path, L"\\WindowsPowerShell\\v1.0\\powershell.exe");
	return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static BOOL GrantUpdateHelperReadAccess(LPCWSTR path)
{
	if (!path || !path[0])
		return FALSE;
	PSECURITY_DESCRIPTOR descriptor = NULL;
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
		L"D:P(A;;GA;;;OW)(A;;GR;;;BU)(A;;GA;;;BA)(A;;GA;;;SY)", SDDL_REVISION_1, &descriptor, NULL))
		return FALSE;
	BOOL success = SetFileSecurityW(path, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor);
	LocalFree(descriptor);
	return success;
}

static BOOL LaunchPowerShellUpdate(LPCWSTR zipPath, LPCWSTR targetPath, LPCWSTR expectedHash,
	LPCWSTR expectedVersion, DWORD parentProcessId)
{
	if (!zipPath || !targetPath || !expectedHash || !expectedVersion || parentProcessId == 0)
		return FALSE;
	WCHAR tempPath[MAX_PATH] = {};
	DWORD tempLength = GetTempPathW(ARRAYSIZE(tempPath), tempPath);
	if (tempLength == 0 || tempLength >= ARRAYSIZE(tempPath))
		return FALSE;
	WCHAR script[MAX_PATH] = {};
	if (GetTempFileNameW(tempPath, L"TrS", 0, script) == 0)
		return FALSE;

	std::wstring ps;
	ps += L"$ErrorActionPreference='Stop'\r\n";
	ps += L"$zip=" + PowerShellQuote(zipPath) + L"\r\n";
	ps += L"$target=" + PowerShellQuote(targetPath) + L"\r\n";
	ps += L"$expectedHash=" + PowerShellQuote(expectedHash) + L"\r\n";
	ps += L"$expectedVersion=" + PowerShellQuote(expectedVersion) + L"; $expectedVersion=$expectedVersion.TrimStart('v')\r\n";
	ps += L"$parentPid=" + std::to_wstring(parentProcessId) + L"\r\n";
	ps += L"$scriptPath=$MyInvocation.MyCommand.Path; $extract=Join-Path ([IO.Path]::GetTempPath()) ('TrayS-update-'+[guid]::NewGuid().ToString('N')); $staged=$target+'.update-'+[guid]::NewGuid().ToString('N'); $backup=$target+'.bak-'+[guid]::NewGuid().ToString('N'); $replacementAttempted=$false\r\n";
	ps += L"try {\r\n";
	ps += L"  $parent=Get-Process -Id $parentPid -ErrorAction SilentlyContinue; if ($parent) { if (-not $parent.WaitForExit(120000)) { throw 'TrayS did not close in time; the existing version was left in place.' }; $parent.Dispose() }\r\n";
	ps += L"  if ((Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedHash.ToLowerInvariant()) { throw 'The downloaded package checksum does not match.' }\r\n";
	ps += L"  New-Item -ItemType Directory -Path $extract -Force | Out-Null\r\n";
	ps += L"  Expand-Archive -LiteralPath $zip -DestinationPath $extract -Force\r\n";
	ps += L"  $candidate=@(Get-ChildItem -LiteralPath $extract -Filter 'TrayS.exe' -File -Recurse); if ($candidate.Count -ne 1) { throw 'The package must contain exactly one TrayS.exe.' }\r\n";
	ps += L"  $version=[Diagnostics.FileVersionInfo]::GetVersionInfo($candidate[0].FullName).FileVersion; if (-not $version.StartsWith($expectedVersion + '.')) { throw ('Package version mismatch: '+$version) }\r\n";
	ps += L"  $bytes=[IO.File]::ReadAllBytes($candidate[0].FullName); if ($bytes.Length -lt 64 -or [BitConverter]::ToUInt16($bytes,0) -ne 0x5a4d) { throw 'The update executable has an invalid DOS header.' }; $pe=[BitConverter]::ToInt32($bytes,0x3c); if ($pe -lt 64 -or $pe -gt $bytes.Length-6 -or [BitConverter]::ToUInt32($bytes,$pe) -ne 0x4550) { throw 'The update executable has an invalid PE header.' }; $machine=[BitConverter]::ToUInt16($bytes,$pe+4)\r\n";
	ps += L"  $want=if ([IntPtr]::Size -eq 8) { 0x8664 } else { 0x14c }; if ($machine -ne $want) { throw 'The update package architecture does not match this installation.' }\r\n";
	ps += L"  Copy-Item -LiteralPath $candidate[0].FullName -Destination $staged\r\n";
	ps += L"  if ((Get-FileHash -LiteralPath $staged -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $candidate[0].FullName -Algorithm SHA256).Hash) { throw 'The staged executable failed verification.' }\r\n";
	ps += L"  Copy-Item -LiteralPath $target -Destination $backup\r\n";
	ps += L"  if ((Get-FileHash -LiteralPath $backup -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash) { throw 'The existing executable could not be backed up safely.' }\r\n";
	ps += L"  $replacementAttempted=$true; Move-Item -LiteralPath $staged -Destination $target -Force\r\n";
	ps += L"  if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $candidate[0].FullName -Algorithm SHA256).Hash) { throw 'The installed executable failed verification.' }\r\n";
	ps += L"  Start-Process -FilePath $target -WorkingDirectory ([IO.Path]::GetDirectoryName($target)) | Out-Null\r\n";
	ps += L"  $replacementAttempted=$false; Remove-Item -LiteralPath $backup -Force -ErrorAction SilentlyContinue; Remove-Item -LiteralPath $zip -Force -ErrorAction SilentlyContinue; Remove-Item -LiteralPath $extract -Recurse -Force -ErrorAction SilentlyContinue; exit 0\r\n";
	ps += L"} catch {\r\n";
	ps += L"  $detail=$_.Exception.Message; $failureState='旧版本文件未被修改。'; $rollbackSucceeded=$false; if ($replacementAttempted) { if (Test-Path -LiteralPath $backup) { try { Copy-Item -LiteralPath $backup -Destination $target -Force; if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $backup -Algorithm SHA256).Hash) { throw 'restored executable failed verification' }; $rollbackSucceeded=$true; $failureState='旧版本已恢复。' } catch { $failureState=('自动恢复失败，旧版本备份仍在：'+$backup); $detail += ('; rollback failed: '+$_.Exception.Message) } } else { $failureState='自动恢复失败，找不到旧版本备份。'; $detail += '; rollback backup is missing' } }; if (-not $replacementAttempted -or $rollbackSucceeded) { Remove-Item -LiteralPath $backup -Force -ErrorAction SilentlyContinue }; Remove-Item -LiteralPath $staged -Force -ErrorAction SilentlyContinue; Remove-Item -LiteralPath $extract -Recurse -Force -ErrorAction SilentlyContinue\r\n";
	ps += L"  try { Add-Type -TypeDefinition 'using System; using System.Runtime.InteropServices; public static class TraySUpdateNative { [DllImport(\"user32.dll\", CharSet=CharSet.Unicode)] public static extern int MessageBox(IntPtr hWnd, string text, string caption, uint type); }' -ErrorAction SilentlyContinue; [void][TraySUpdateNative]::MessageBox([IntPtr]::Zero, ('TrayS 更新失败：'+$failureState+'`n`n'+$detail), 'TrayS 更新', 0x30) } catch {}\r\n";
	ps += L"  exit 1\r\n";
	ps += L"} finally { Remove-Item -LiteralPath $scriptPath -Force -ErrorAction SilentlyContinue }\r\n";
	if (!WriteUtf16File(script, ps))
	{
		DeleteFileW(script);
		return FALSE;
	}
	WCHAR systemPath[MAX_PATH] = {};
	if (!GetPowerShellPath(systemPath, ARRAYSIZE(systemPath)))
	{
		DeleteFileW(script);
		return FALSE;
	}
	std::wstring parameters = L"-NoProfile -ExecutionPolicy Bypass -File " + QuoteWindowsArgument(script);
	std::wstring target(targetPath);
	std::wstring targetDirectory = target.substr(0, target.find_last_of(L"\\/"));
	BOOL launched = FALSE;
	if (CanWriteUpdateTargetDirectory(targetPath))
	{
		std::wstring commandLine = QuoteWindowsArgument(systemPath) + L" " + parameters;
		STARTUPINFOW startup{};
		startup.cb = sizeof(startup);
		startup.dwFlags = STARTF_USESHOWWINDOW;
		startup.wShowWindow = SW_HIDE;
		PROCESS_INFORMATION process{};
		launched = CreateProcessW(systemPath, &commandLine[0], NULL, NULL, FALSE,
			CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, NULL, targetDirectory.c_str(), &startup, &process);
		if (launched)
		{
			CloseHandle(process.hThread);
			CloseHandle(process.hProcess);
		}
	}
	else
	{
		// An administrator may provide different credentials in the UAC prompt.
		// Grant the helper inputs read access so that account can open them.
		if (!GrantUpdateHelperReadAccess(script) || !GrantUpdateHelperReadAccess(zipPath))
		{
			DeleteFileW(script);
			return FALSE;
		}
		SHELLEXECUTEINFOW execute{};
		execute.cbSize = sizeof(execute);
		execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
		execute.lpVerb = L"runas";
		execute.lpFile = systemPath;
		execute.lpParameters = parameters.c_str();
		execute.lpDirectory = targetDirectory.c_str();
		execute.nShow = SW_HIDE;
		launched = ShellExecuteExW(&execute);
		if (execute.hProcess)
			CloseHandle(execute.hProcess);
	}
	if (!launched)
		DeleteFileW(script);
	return launched;
}
}

BOOL ReadTraySAutoUpdateSetting()
{
	WCHAR path[32768] = {};
	if (!BuildSiblingPath(kUpdateSettingName, path, ARRAYSIZE(path)))
		return TRUE;
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_HIDDEN, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return TRUE;
	char value[8] = {};
	DWORD read = 0;
	BOOL result = ReadFile(file, value, sizeof(value), &read, NULL) && read == 1 && (value[0] == '1' || value[0] == '0') ? value[0] == '1' : TRUE;
	CloseHandle(file);
	return result;
}

BOOL WriteTraySAutoUpdateSetting(BOOL enabled)
{
	WCHAR path[32768] = {};
	if (!BuildSiblingPath(kUpdateSettingName, path, ARRAYSIZE(path)))
		return FALSE;
	WCHAR temporary[32768] = {};
	wsprintfW(temporary, L"%s.tmp.%lu", path, GetCurrentProcessId());
	HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return FALSE;
	char value = enabled ? '1' : '0';
	DWORD written = 0;
	BOOL result = WriteFile(file, &value, 1, &written, NULL) && written == 1;
	FlushFileBuffers(file);
	CloseHandle(file);
	if (result)
		result = MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
	if (!result)
		DeleteFileW(temporary);
	return result;
}

BOOL StartTraySUpdateCheck(HWND notifyWindow, BOOL automatic)
{
	if (!IsWindow(notifyWindow) || InterlockedCompareExchange(&g_updateInProgress, 1, 0) != 0)
		return FALSE;
	UpdateThreadContext* context = (UpdateThreadContext*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(UpdateThreadContext));
	if (!context)
	{
		InterlockedExchange(&g_updateInProgress, 0);
		return FALSE;
	}
	context->notifyWindow = notifyWindow;
	context->automatic = automatic;
	HANDLE thread = CreateThread(NULL, 0, UpdateThreadProc, context, 0, NULL);
	if (!thread)
	{
		HeapFree(GetProcessHeap(), 0, context);
		InterlockedExchange(&g_updateInProgress, 0);
		return FALSE;
	}
	CloseHandle(thread);
	return TRUE;
}

BOOL LaunchTraySUpdateApplier(const TRAYS_UPDATE_INFO* info, DWORD parentProcessId, LPCWSTR targetPath)
{
	if (!info || parentProcessId == 0 || !targetPath || !targetPath[0] ||
		std::find(info->version, info->version + ARRAYSIZE(info->version), L'\0') == info->version + ARRAYSIZE(info->version) ||
		std::find(info->downloadPath, info->downloadPath + ARRAYSIZE(info->downloadPath), L'\0') == info->downloadPath + ARRAYSIZE(info->downloadPath) ||
		!info->downloadPath[0] || !ParseHexSha256(info->sha256))
		return FALSE;
	VersionNumber parsedVersion{};
	if (!ParseVersionString(info->version, &parsedVersion))
		return FALSE;
	if (info->sha256[64] != L'\0')
		return FALSE;
	WCHAR fullTargetPath[32768] = {};
	DWORD fullTargetLength = GetFullPathNameW(targetPath, ARRAYSIZE(fullTargetPath), fullTargetPath, NULL);
	if (fullTargetLength == 0 || fullTargetLength >= ARRAYSIZE(fullTargetPath))
		return FALSE;
	DWORD attributes = GetFileAttributesW(fullTargetPath);
	if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
		return FALSE;
	return LaunchPowerShellUpdate(info->downloadPath, fullTargetPath, info->sha256, info->version, parentProcessId);
}

void FreeTraySUpdateMessage(TRAYS_UPDATE_MESSAGE* message, BOOL deleteDownloadedFile)
{
	if (!message)
		return;
	if (deleteDownloadedFile && message->info.downloadPath[0])
		DeleteFileW(message->info.downloadPath);
	HeapFree(GetProcessHeap(), 0, message);
	InterlockedExchange(&g_updateInProgress, 0);
}
