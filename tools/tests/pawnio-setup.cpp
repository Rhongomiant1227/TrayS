// Read-only installer checks. This executable never invokes the installer or
// opens a PawnIO device. Optional network coverage uses the production fetch.
#include "PawnIoSetup.cpp"
#include <cstdio>

static int Fail(const char* message, DWORD error)
{
	std::printf("FAIL: %s (0x%08lx)\n", message, error);
	return 1;
}

int wmain(int argc, wchar_t** argv)
{
	if (argc < 2) return Fail("Pass the pinned official installer path", ERROR_INVALID_PARAMETER);
	DWORD result = VerifyPawnIoInstaller(argv[1]);
	if (result != ERROR_SUCCESS) return Fail("Official SHA-256/Authenticode validation", result);
	std::puts("PASS: official installer passed pinned SHA-256 and Authenticode validation.");
	{
		ScopedHandle locked(OpenLockedInstaller(argv[1]));
		if (locked.value == INVALID_HANDLE_VALUE) return Fail("Lock installer", GetLastError());
		ScopedHandle writeAttempt(CreateFileW(argv[1], GENERIC_WRITE, FILE_SHARE_READ, NULL,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL));
		if (writeAttempt.value != INVALID_HANDLE_VALUE)
			return Fail("Verified installer was writable while locked", ERROR_INVALID_DATA);
	}
	std::puts("PASS: validation lock prevents concurrent file writes/replacement.");
	std::wstring directory;
	result = CreateSetupDirectory(FALSE, &directory);
	if (result != ERROR_SUCCESS) return Fail("Create test cache", result);
	std::wstring corrupted = directory + L"\\" + kPawnIoFileName;
	if (!CopyFileW(argv[1], corrupted.c_str(), TRUE)) return Fail("Copy fixture", GetLastError());
	{
		ScopedHandle file(CreateFileW(corrupted.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL));
		if (file.value == INVALID_HANDLE_VALUE) return Fail("Open corruption fixture", GetLastError());
		BYTE changed = 0;
		DWORD done = 0;
		if (!ReadFile(file.value, &changed, 1, &done, NULL) || done != 1) return Fail("Read fixture", GetLastError());
		changed ^= 1;
		SetFilePointer(file.value, 0, NULL, FILE_BEGIN);
		if (!WriteFile(file.value, &changed, 1, &done, NULL) || done != 1) return Fail("Corrupt fixture", GetLastError());
	}
	result = VerifyPawnIoInstaller(corrupted.c_str());
	if (result != static_cast<DWORD>(TRUST_E_BAD_DIGEST)) return Fail("Same-size tampered installer accepted", result);
	RemovePawnIoDownload(directory, corrupted);
	if (GetFileAttributesW(directory.c_str()) != INVALID_FILE_ATTRIBUTES) return Fail("Cache cleanup", ERROR_DIR_NOT_EMPTY);
	std::puts("PASS: a same-size tampered installer was rejected and the cache was removed.");
	if (argc >= 3 && lstrcmpW(argv[2], L"--download") == 0)
	{
		std::wstring downloadDirectory, downloadPath;
		volatile LONG phase = PAWNIO_SETUP_IDLE;
		result = DownloadVerifiedPawnIoInstaller(&downloadDirectory, &downloadPath, &phase, NULL);
		RemovePawnIoDownload(downloadDirectory, downloadPath);
		if (result != ERROR_SUCCESS) return Fail("Production HTTPS download/validation", result);
		if (GetFileAttributesW(downloadDirectory.c_str()) != INVALID_FILE_ATTRIBUTES)
			return Fail("Download cache cleanup", ERROR_DIR_NOT_EMPTY);
		std::puts("PASS: production HTTPS download, digest, signature and cleanup.");
		ScopedHandle cancel(CreateEventW(NULL, TRUE, TRUE, NULL));
		phase = PAWNIO_SETUP_IDLE;
		result = DownloadVerifiedPawnIoInstaller(&downloadDirectory, &downloadPath, &phase, cancel.value);
		if (result != ERROR_CANCELLED) return Fail("Cancelled download did not stop", result);
		if (GetFileAttributesW(downloadDirectory.c_str()) != INVALID_FILE_ATTRIBUTES)
			return Fail("Cancelled download cache cleanup", ERROR_DIR_NOT_EMPTY);
		std::puts("PASS: a cancelled download stops before a request and removes its cache.");
	}
	std::puts("PAWNIO_SETUP_VALIDATION_PASS (no driver installed or modified)");
	return 0;
}
