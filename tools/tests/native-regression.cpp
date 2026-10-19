// Exercise the current implementation with isolated windows and simulated
// mouse input. No TrayS startup, Explorer interaction, or driver loading.
#include "framework.h"
#include "TrayS.h"
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <vector>

static POINT reviewCursor{};
static SHORT WINAPI ReviewGetKeyState(int key)
{
    return key == VK_CONTROL || key == VK_LBUTTON ? (SHORT)0x8000 : 0;
}
static BOOL WINAPI ReviewGetCursorPos(LPPOINT point)
{
    *point = reviewCursor;
    return TRUE;
}
static BOOL WINAPI ReviewSetWindowPos(HWND window, HWND after, int x, int y, int width, int height, UINT flags)
{
    // Keep test windows hidden even when the product asks to show its overlay.
    return SetWindowPos(window, after, x, y, width, height, flags & ~SWP_SHOWWINDOW);
}
#define GetKeyState ReviewGetKeyState
#define GetCursorPos ReviewGetCursorPos
#define SetWindowPos ReviewSetWindowPos
#include "TrayS.cpp"
#undef GetKeyState
#undef GetCursorPos
#undef SetWindowPos

static void Check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

static LRESULT CALLBACK ReviewWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (window == hTaskBar && (message == WM_CAPTURECHANGED || message == WM_CANCELMODE || message == WM_DESTROY))
        TaskBarProc(window, message, wParam, lParam);
    return DefWindowProcW(window, message, wParam, lParam);
}

static HWND MakeWindow(DWORD style, HWND parent = NULL, int id = 0)
{
    HWND window = CreateWindowExW(0, L"TraySRegression", L"", style, 0, 0, 100, 40,
        parent, (HMENU)(INT_PTR)id, hInst, NULL);
    Check(window != NULL, "Cannot create isolated test window");
    return window;
}

static void SetupLayout(bool vertical, bool composition = false, bool fullscreen = false)
{
    TraySave.bMonitor = FALSE;
    TraySave.bMonitorFloat = FALSE;
    TraySave.bSecond = FALSE;
    TraySave.bNear = FALSE;
    TraySave.bMonitorLeft = TRUE;
    bFullScreen = fullscreen;
    hWin11UI = composition ? hTray : NULL;
    MoveWindow(hTray, -1600, -400, vertical ? 40 : 1600, vertical ? 900 : 40, FALSE);
    MoveWindow(hTaskWnd, vertical ? 0 : 12, vertical ? 20 : 0,
        vertical ? 40 : 1200, vertical ? 750 : 40, FALSE);
    MoveWindow(hTaskListWnd, 0, 0, 600, 40, FALSE);
    mWidth = 180;
    mHeight = 100;
    wHeight = 14;
    otleft = ottop = -99999;
}

static void CheckRect(int x, int y, int width, int height)
{
    RECT rect{};
    Check(GetWindowRect(hTaskBar, &rect) != FALSE, "Monitor rectangle is unavailable");
    if (rect.left != x || rect.top != y || rect.right - rect.left != width || rect.bottom - rect.top != height)
    {
        std::printf("  rectangle: %ld,%ld %ldx%ld; expected %d,%d %dx%d\n",
            rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, x, y, width, height);
        Check(false, "Taskbar position/size is incorrect");
    }
}

static void TestArgbResources()
{
    const int width = 128, height = 48;
    std::vector<BYTE> pixels(width * height * 4);
    HDC source = CreateCompatibleDC(NULL);
    Check(source != NULL, "Cannot create font DC");
    RECT textRect{ 0, 0, width, height };
    BeginArgbTextRender(pixels.data(), width, height, width * 4);
    Check(g_argbTextSurface.active != FALSE, "ARGB rendering did not begin");
    Check(TryDrawShadowTextArgb(source, L"TrayS 温度", -1, &textRect, DT_LEFT, RGB(0, 0, 0), TRUE) != FALSE,
        "ARGB text did not render");
    bool haveAlpha = false;
    for (size_t index = 3; index < pixels.size(); index += 4)
        haveAlpha = haveAlpha || pixels[index] != 0;
    Check(haveAlpha, "ARGB text has no visible alpha pixels");
    EndArgbTextRender();
    const DWORD before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    for (int iteration = 0; iteration < 2000; ++iteration)
    {
        BeginArgbTextRender(pixels.data(), width, height, width * 4);
        Check(g_argbTextSurface.active != FALSE, "Repeated ARGB allocation failed");
        TryDrawShadowTextArgb(source, L"TrayS", -1, &textRect, DT_LEFT, RGB(0, 0, 0), FALSE);
        EndArgbTextRender();
    }
    const DWORD after = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    std::printf("  GDI objects before=%lu after=%lu, 2000 render cycles\n", before, after);
    Check(after <= before, "ARGB rendering leaked GDI objects");
    BeginArgbTextRender(pixels.data(), 0, height, 0);
    Check(!g_argbTextSurface.active, "Invalid ARGB dimensions remained active");
    DeleteDC(source);
}

static void TestConfiguration()
{
    SetMonitorOffset(9999, -9999);
    Check(GetMonitorOffsetX() == 4096 && GetMonitorOffsetY() == -4096, "Offset limits are not enforced");
    SetMonitorOffset(-1523, 2048);
    TraySave.iUnit = MAKELONG(1, 1);
    WriteReg();
    Check(LOWORD(TraySave.iUnit) == 1 && HIWORD(TraySave.iUnit) == 1, "Saving lost the bits/bytes setting");
    SetMonitorOffset(0, 0);
    TraySave.iUnit = 0;
    ReadReg();
    Check(GetMonitorOffsetX() == -1523 && GetMonitorOffsetY() == 2048, "Signed offsets did not survive a config round trip");
    Check(LOWORD(TraySave.iUnit) == 1 && HIWORD(TraySave.iUnit) == 1, "Packed unit setting did not survive a config round trip");
    TraySave.iUnit = MAKELONG(65535, 65535);
    NormalizeTraySave();
    Check(TraySave.iUnit == 2, "Invalid unit settings were not normalized");
}

static void StartDrag(int x, int y)
{
    SetupLayout(false);
    SetMonitorOffset(x, y);
    reviewCursor = { -1450, -380 };
    TaskBarProc(hTaskBar, WM_LBUTTONDOWN, MK_CONTROL | MK_LBUTTON, 0);
    Check(gMonitorCalibrating && GetCapture() == hTaskBar, "Ctrl drag did not acquire capture");
}

static void TestDrag()
{
    StartDrag(125, -75);
    TaskBarProc(hTaskBar, WM_MOUSEMOVE, MK_LBUTTON, 0);
    Check(GetMonitorOffsetX() == 125 && GetMonitorOffsetY() == -75, "Ctrl drag jumped without mouse movement");
    reviewCursor.x += 37;
    reviewCursor.y -= 12;
    TaskBarProc(hTaskBar, WM_MOUSEMOVE, MK_LBUTTON, 0);
    Check(GetMonitorOffsetX() == 162 && GetMonitorOffsetY() == -87, "Ctrl drag did not follow the cursor delta");
    TaskBarProc(hTaskBar, WM_LBUTTONUP, 0, 0);
    Check(!gMonitorCalibrating && GetCapture() != hTaskBar, "Button-up left calibration active");
    SetMonitorOffset(0, 0);
    ReadReg();
    Check(GetMonitorOffsetX() == 162 && GetMonitorOffsetY() == -87, "Completed drag was not saved");
}

static void TestCaptureLoss()
{
    StartDrag(42, -61);
    ReleaseCapture();
    Check(!gMonitorCalibrating, "Losing capture left calibration active");
    StartDrag(56, 78);
    TaskBarProc(hTaskBar, WM_CANCELMODE, 0, 0);
    Check(!gMonitorCalibrating && GetCapture() != hTaskBar, "Cancel mode left calibration/capture active");
}

static void TestSliders()
{
    SetupLayout(false);
    hSetting = MakeWindow(WS_POPUP);
    HWND sliderX = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD, 0, 0, 100, 20,
        hSetting, (HMENU)IDC_SLIDER_OFFSET_X, hInst, NULL);
    HWND sliderY = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD, 0, 20, 100, 20,
        hSetting, (HMENU)IDC_SLIDER_OFFSET_Y, hInst, NULL);
    Check(sliderX && sliderY, "Cannot create calibration sliders");
    // Use a clipped/stale control to ensure editing one axis never reads the
    // other axis back from a control that cannot represent its saved value.
    SendMessageW(sliderX, TBM_SETRANGE, TRUE, MAKELPARAM(-300, 300));
    SendMessageW(sliderY, TBM_SETRANGE, TRUE, MAKELPARAM(-300, 300));
    SetMonitorOffset(20, -1900);
    UpdateMonitorOffsetControls(hSetting);
    SendMessageW(sliderX, TBM_SETPOS, TRUE, 150);
    SettingProc(hSetting, WM_HSCROLL, TB_THUMBPOSITION, (LPARAM)sliderX);
    Check(GetMonitorOffsetX() == 150 && GetMonitorOffsetY() == -1900, "X slider changed Y calibration");
    SetMonitorOffset(-1600, 35);
    UpdateMonitorOffsetControls(hSetting);
    SendMessageW(sliderY, TBM_SETPOS, TRUE, 100);
    SettingProc(hSetting, WM_HSCROLL, TB_THUMBPOSITION, (LPARAM)sliderY);
    Check(GetMonitorOffsetX() == -1600 && GetMonitorOffsetY() == 100, "Y slider changed X calibration");
    KillTimer(hSetting, 3);
    DestroyWindow(hSetting);
    hSetting = NULL;
}

static void TestLegacyLayout()
{
    SetupLayout(false);
    SetMonitorOffset(7, 13);
    AdjustWindowPos();
    CheckRect(-1579, -381, 180, 28);
    SetupLayout(true);
    AdjustWindowPos();
    CheckRect(-1592, -365, 38, 100);
    SetMonitorOffset(23, 13);
    AdjustWindowPos();
    CheckRect(-1576, -365, 38, 100);
}

static void TestPopupLayout()
{
    SetupLayout(true, true);
    SetMonitorOffset(17, -19);
    AdjustWindowPos();
    CheckRect(-1582, -397, 38, 100);
    SetupLayout(true, false, true);
    AdjustWindowPos();
    CheckRect(-1582, -397, 38, 100);
}

static float reviewCpuTemperature = 55.125f;
static int reviewAcpiSamples = 0;

static void ReviewGetTemperature(float* cpu, float* gpu, float* mainboard,
    float* disk, int diskIndex, float* package)
{
    if (cpu) *cpu = reviewCpuTemperature;
    if (package) *package = reviewCpuTemperature;
    if (gpu) *gpu = -1.0f;
    if (mainboard) *mainboard = -1.0f;
    if (disk) *disk = -1.0f;
}

static ULONG WINAPI ReviewCollectThermalData(PDH_HQUERY)
{
    ++reviewAcpiSamples;
    return ERROR_SUCCESS;
}

static ULONG WINAPI ReviewReadThermalCounter(PDH_HCOUNTER, DWORD, LPDWORD type,
    PPDH_FMT_COUNTERVALUE value)
{
    *type = 0;
    value->CStatus = PDH_CSTATUS_VALID_DATA;
    value->doubleValue = 303.15; // 30 C, deliberately different from the CPU.
    return ERROR_SUCCESS;
}

static void TestCpuTemperatureSources()
{
    // Inject both providers so this regression never opens hardware devices.
    // A real broker mapping may be present on the host running the harness;
    // mark this fixture as broker mode so GetCpuTemp exercises only the
    // injected managed/ACPI providers.
    BOOL previousBrokerMode = g_runningCpuSensorBroker;
    g_runningCpuSensorBroker = TRUE;
    TraySave.szDisk = L'\0';
    thermalProbeAttempted = TRUE;
    hThermalQuery = reinterpret_cast<HQUERY>(1);
    hThermalCounters[0] = reinterpret_cast<HCOUNTER>(1);
    thermalCounterCount = 1;
    thermalCounterUsesHighPrecision = FALSE;
    ThermalPdhCollectQueryData = ReviewCollectThermalData;
    ThermalPdhGetFormattedCounterValue = ReviewReadThermalCounter;
    hOHMA = reinterpret_cast<HMODULE>(1);
    GetTemperature = ReviewGetTemperature;
    reviewAcpiSamples = 0;
    reviewCpuTemperature = 55.125f;
    DWORD secondaryTemperature = 0;
    DWORD hddTemperature = 0;
    Check(GetCpuTemp(1, &secondaryTemperature, &hddTemperature) == 55,
        "CPU package sensor lost priority to the ACPI thermal zone");
    Check(HasFreshLocalCpuHardwareSample(), "A successful local CPU hardware sample was not recorded");
    Check(secondaryTemperature == 55 && hddTemperature == 0,
        "LHM secondary/disk readings were not returned to the caller");
    Check(reviewAcpiSamples == 0, "A usable CPU package unnecessarily sampled ACPI");
    reviewCpuTemperature = std::numeric_limits<float>::quiet_NaN();
    Check(GetCpuTemp(1) == 30, "Invalid hardware reading did not fall back to ACPI");
    Check(!HasFreshLocalCpuHardwareSample(), "A loaded DLL with an invalid CPU sample was reported as hardware-read");
    reviewCpuTemperature = std::numeric_limits<float>::infinity();
    Check(GetCpuTemp(1) == 30, "Infinite hardware reading did not fall back to ACPI");
    reviewCpuTemperature = -1.0f;
    Check(GetCpuTemp(1) == 30, "Missing hardware reading did not fall back to ACPI");
	Check(GetCpuTemp(1, NULL, NULL, FALSE) == 0,
		"Broker hardware-only path published an ACPI fallback as CPU temperature");
    hOHMA = NULL;
    GetTemperature = NULL;
	g_runningCpuSensorBroker = TRUE;
    Check(GetCpuTemp(1) == 30, "ACPI-only temperature became unavailable");
	Check(GetCpuTemp(1, NULL, NULL, FALSE) == 0,
		"Broker hardware-only path used ACPI when the managed monitor was unavailable");
    hThermalQuery = NULL;
    hThermalCounters[0] = NULL;
    thermalCounterCount = 0;
    ThermalPdhCollectQueryData = NULL;
    ThermalPdhGetFormattedCounterValue = NULL;
    Check(GetCpuTemp(1) == 0, "Missing providers produced a fake CPU temperature");

    hSetting = MakeWindow(WS_POPUP);
    MakeWindow(WS_CHILD, hSetting, IDC_LABEL_CPU_TEMPERATURE_STATUS);
    MakeWindow(WS_CHILD, hSetting, IDC_BUTTON_CPU_TEMPERATURE_ENABLE);
    TraySave.bMonitorTemperature = TRUE;
    g_pawnIoProbeError = ERROR_ACCESS_DENIED;
    wchar_t status[128]{};
    // A developer machine may already have the protected broker installed;
    // in that case hardware status is the correct guidance and the one-time
    // authorization text is intentionally not shown.
    BOOL brokerInstalledForFixture = IsCpuSensorBrokerTaskInstalled();
    DWORD brokerFixtureTemperature = 0;
    DWORD brokerFixtureError = ERROR_SUCCESS;
    // A broker from another architecture can expose the same per-user mapping
    // while this harness is running (the mapping layout is architecture-neutral).
    // Treat a fresh mapping sample as an already-available hardware path too;
    // otherwise the fixture would incorrectly demand the first-use guidance
    // while the status dialog is correctly showing the live broker reading.
    BOOL brokerSampleForFixture = TryReadCpuSensorBrokerState(
        &brokerFixtureTemperature, &brokerFixtureError);
    UpdateCpuTemperatureStatus(hSetting);
    GetDlgItemTextW(hSetting, IDC_LABEL_CPU_TEMPERATURE_STATUS, status, 128);
    if (!brokerInstalledForFixture && !brokerSampleForFixture)
        Check(wcsstr(status, L"一次授权") != NULL, "PawnIO access denied has no one-time broker guidance");
    if (brokerInstalledForFixture && g_cpuSensorBrokerTaskNeedsRepair)
        Check(IsWindowEnabled(GetDlgItem(hSetting, IDC_BUTTON_CPU_TEMPERATURE_ENABLE)) != FALSE,
            "A broker with a stale battery policy cannot be repaired explicitly from settings");
    g_pawnIoProbeError = ERROR_FILE_NOT_FOUND;
    UpdateCpuTemperatureStatus(hSetting);
    GetDlgItemTextW(hSetting, IDC_LABEL_CPU_TEMPERATURE_STATUS, status, 128);
	wchar_t setupButton[64]{};
	GetDlgItemTextW(hSetting, IDC_BUTTON_CPU_TEMPERATURE_ENABLE, setupButton, ARRAYSIZE(setupButton));
	Check(wcscmp(setupButton, L"下载安装") == 0 &&
		IsWindowEnabled(GetDlgItem(hSetting, IDC_BUTTON_CPU_TEMPERATURE_ENABLE)),
		"An installed broker hides/disables the missing PawnIO download action");
    if (!brokerInstalledForFixture && !brokerSampleForFixture)
        Check(wcsstr(status, L"安装 PawnIO") != NULL, "Missing PawnIO has no installation guidance");
    TraySave.bMonitorTemperature = FALSE;
    DestroyWindow(hSetting);
    hSetting = NULL;
    g_pawnIoProbeError = ERROR_SUCCESS;
    thermalProbeAttempted = FALSE;
    g_runningCpuSensorBroker = previousBrokerMode;
}

static void TestCpuTemperatureLayoutAvailability()
{
	Check(!HasCpuTemperatureDisplayRow(FALSE, FALSE, FALSE),
		"Unavailable CPU sources unexpectedly reserve a CPU temperature row");
	Check(HasCpuTemperatureDisplayRow(FALSE, FALSE, TRUE),
		"Installed CPU sensor broker does not reserve a row for its readings");
	Check(HasCpuTemperatureDisplayRow(TRUE, FALSE, FALSE),
		"ACPI CPU temperature source does not reserve a row");
	Check(HasCpuTemperatureDisplayRow(FALSE, TRUE, FALSE),
		"Local hardware monitor does not reserve a CPU temperature row");
}

static void TestCpuSensorBrokerSampleAndStatus()
{
	Check(IsCpuSensorBrokerSampleUsable(55, ERROR_SUCCESS),
		"A successful in-range CPU hardware sample was rejected");
	Check(!IsCpuSensorBrokerSampleUsable(28, ERROR_FILE_NOT_FOUND),
		"A stale ACPI fallback was accepted as a hardware sample");
	Check(!IsCpuSensorBrokerSampleUsable(55, ERROR_ACCESS_DENIED),
		"A sample carrying a broker access error was accepted");
	Check(!IsCpuSensorBrokerSampleUsable(0, ERROR_SUCCESS),
		"A missing CPU sample was accepted as a temperature");
	Check(!IsCpuSensorBrokerSampleUsable(300, ERROR_SUCCESS),
		"An out-of-range CPU sample was accepted");
	Check(wcscmp(GetCpuSensorBrokerStatusText(TRUE, TRUE, 28,
		ERROR_FILE_NOT_FOUND, TRUE), L"CPU：后台需更新") == 0,
		"A stale broker sample took priority over the required protected-binary repair");
	Check(wcscmp(GetCpuSensorBrokerStatusText(TRUE, TRUE, 28,
		ERROR_FILE_NOT_FOUND, FALSE), L"CPU：需安装 PawnIO") == 0,
		"A current broker reporting missing PawnIO did not show the installation prerequisite");
	Check(wcscmp(GetCpuSensorBrokerStatusText(TRUE, TRUE, 55,
		ERROR_SUCCESS, FALSE), L"CPU：硬件已读") == 0,
		"A valid CPU hardware sample was not reported as read");
	Check(wcscmp(GetCpuSensorBrokerStatusText(TRUE, FALSE, 0,
		ERROR_SUCCESS, TRUE), L"CPU：后台需更新") == 0,
		"A version-mismatched broker was not identified for repair");
	Check(GetCpuSensorBrokerStatusText(FALSE, FALSE, 0, ERROR_SUCCESS, FALSE) == NULL,
		"An absent broker unexpectedly supplied broker status");
	Check(AreCpuSensorBrokerVersionsEqual(0x00010007, 0x00080000,
		0x00010007, 0x00080000), "Matching broker and tray versions were rejected");
	Check(!AreCpuSensorBrokerVersionsEqual(0x00010007, 0x00080000,
		0x00010007, 0x00060000), "An outdated protected broker was treated as current");
}

static void TestTemperatureSamplePublication()
{
	TRAYDATA sample{};
	TrayData = &sample;
	PublishTemperatureSample(76, 32, 41);
	Check(sample.iTemperature1 == 76 && sample.iTemperature2 == 32 && sample.iHddTemperature == 41,
		"Completed CPU/GPU/disk temperature sample was not published");
	PublishTemperatureSample(0, 0, 0);
	Check(sample.iTemperature1 == 76 && sample.iTemperature2 == 0 && sample.iHddTemperature == 0,
		"Transient broker restart discarded the last valid CPU sample");
	TrayData = NULL;
}

static void TestCpuSensorAuthorizationRequestConditions()
{
	Check(ShouldAutoLaunchCpuSensorBrokerSetup(TRUE, ERROR_ACCESS_DENIED, FALSE, FALSE, FALSE),
		"Enabled monitoring did not request authorization after PawnIO denied access");
	Check(!ShouldAutoLaunchCpuSensorBrokerSetup(FALSE, ERROR_ACCESS_DENIED, FALSE, FALSE, FALSE),
		"Authorization was requested while temperature monitoring is disabled");
	Check(!ShouldAutoLaunchCpuSensorBrokerSetup(TRUE, ERROR_ACCESS_DENIED, TRUE, FALSE, FALSE),
		"Authorization was requested despite an installed broker task");
	Check(!ShouldAutoLaunchCpuSensorBrokerSetup(TRUE, ERROR_ACCESS_DENIED, FALSE, TRUE, FALSE),
		"A concurrent authorization request was not suppressed");
	Check(!ShouldAutoLaunchCpuSensorBrokerSetup(TRUE, ERROR_ACCESS_DENIED, FALSE, FALSE, TRUE),
		"A declined one-time authorization was requested again automatically");
	Check(!ShouldAutoLaunchCpuSensorBrokerSetup(TRUE, ERROR_FILE_NOT_FOUND, FALSE, FALSE, FALSE),
		"Missing PawnIO incorrectly triggered an administrator prompt");
}

static void TestPawnIoSetupOfferAndLifetime()
{
	Check(ShouldOfferPawnIoSetup(TRUE, ERROR_FILE_NOT_FOUND, FALSE, FALSE),
		"Missing PawnIO did not offer the in-app installation");
	Check(ShouldOfferPawnIoSetup(TRUE, ERROR_PATH_NOT_FOUND, FALSE, FALSE),
		"A missing device path did not offer installation");
	Check(!ShouldOfferPawnIoSetup(FALSE, ERROR_FILE_NOT_FOUND, FALSE, FALSE),
		"A driver was offered while temperature monitoring was disabled");
	Check(!ShouldOfferPawnIoSetup(TRUE, ERROR_ACCESS_DENIED, FALSE, FALSE),
		"An installed driver with restricted access was treated as missing");
	Check(!ShouldOfferPawnIoSetup(TRUE, ERROR_FILE_NOT_FOUND, TRUE, FALSE),
		"An in-progress installation allowed a duplicate prompt");
	Check(!ShouldOfferPawnIoSetup(TRUE, ERROR_FILE_NOT_FOUND, FALSE, TRUE),
		"A declined/failed offer was repeated automatically");
	Check(IsPawnIoSetupArchitectureSupported(PROCESSOR_ARCHITECTURE_AMD64) &&
		!IsPawnIoSetupArchitectureSupported(PROCESSOR_ARCHITECTURE_INTEL) &&
		!IsPawnIoSetupArchitectureSupported(PROCESSOR_ARCHITECTURE_ARM64),
		"The x64 installer can be selected on an unsupported native architecture");
	g_pawnIoSetupThread = CreateEventW(NULL, TRUE, FALSE, NULL);
	g_pawnIoSetupCancelEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	Check(g_pawnIoSetupThread && g_pawnIoSetupCancelEvent, "Cannot create setup lifetime fixtures");
	ReapPawnIoSetupWorker();
	Check(g_pawnIoSetupThread && g_pawnIoSetupCancelEvent,
		"An unfinished setup lost its cancellation or worker handle");
	SetEvent(g_pawnIoSetupThread);
	ReapPawnIoSetupWorker();
	Check(!g_pawnIoSetupThread && !g_pawnIoSetupCancelEvent,
		"Finished PawnIO setup leaked its worker or cancellation handle");
}

static void TestCpuSensorAuthorizationIsSerialized()
{
	HANDLE inFlightSetup = CreateEventW(NULL, TRUE, FALSE, NULL);
	Check(inFlightSetup != NULL, "Cannot create authorization state fixture");
	g_cpuSensorBrokerSetupProcess = inFlightSetup;
	g_cpuSensorBrokerSetupInProgress = TRUE;
	Check(LaunchCpuSensorBrokerSetup(NULL), "An already-running setup was treated as a failure");
	Check(g_cpuSensorBrokerSetupProcess == inFlightSetup && g_cpuSensorBrokerSetupInProgress,
		"A second authorization request replaced the in-flight setup process");
	RefreshCpuSensorBrokerSetupState(TRUE);
	Check(g_cpuSensorBrokerSetupProcess == inFlightSetup && g_cpuSensorBrokerSetupInProgress,
		"The existing task caused an in-progress broker repair to be treated as finished");
	SetEvent(inFlightSetup);
	RefreshCpuSensorBrokerSetupState(TRUE);
	Check(!g_cpuSensorBrokerSetupProcess && !g_cpuSensorBrokerSetupInProgress,
		"Completed broker repair did not release its process handle and reset setup state");
}

int main()
{
    hInst = GetModuleHandleW(NULL);
    WNDCLASSW windowClass{};
    windowClass.hInstance = hInst;
    windowClass.lpfnWndProc = ReviewWindowProc;
    windowClass.lpszClassName = L"TraySRegression";
    Check(RegisterClassW(&windowClass) != 0, "Cannot register regression window class");
    INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_BAR_CLASSES };
    Check(InitCommonControlsEx(&controls) != FALSE, "Cannot initialize trackbars");
    hTray = MakeWindow(WS_POPUP);
    hTaskWnd = MakeWindow(WS_CHILD, hTray);
    hTaskListWnd = MakeWindow(WS_CHILD, hTaskWnd);
    hTaskBar = MakeWindow(WS_CHILD, hTray);
    int failures = 0;
    struct Test { const char* name; void (*run)(); };
    const Test tests[] = {
        { "ARGB text and GDI lifetime", TestArgbResources },
        { "Configuration bounds and round trip", TestConfiguration },
        { "Ctrl drag cursor deltas and save", TestDrag },
        { "Calibration capture loss/cancel", TestCaptureLoss },
        { "Independent calibration sliders", TestSliders },
        { "Legacy taskbar on negative monitor origin", TestLegacyLayout },
        { "Composition and fullscreen vertical offsets", TestPopupLayout },
        { "CPU package priority, ACPI fallback and permission guidance", TestCpuTemperatureSources },
        { "CPU sensor broker keeps the temperature row visible", TestCpuTemperatureLayoutAvailability },
		{ "CPU sensor broker validates sample errors and version status", TestCpuSensorBrokerSampleAndStatus },
        { "Temperature samples publish after providers finish", TestTemperatureSamplePublication },
        { "CPU sensor authorization prompt is automatic and one-time", TestCpuSensorAuthorizationRequestConditions },
		{ "PawnIO download offer, architecture and worker lifetime", TestPawnIoSetupOfferAndLifetime },
        { "CPU sensor authorization cannot spawn duplicate prompts", TestCpuSensorAuthorizationIsSerialized }
    };
    for (const auto& test : tests)
    {
        try { test.run(); std::printf("PASS: %s\n", test.name); }
        catch (const std::exception& error) { ++failures; std::printf("FAIL: %s: %s\n", test.name, error.what()); }
        if (GetCapture()) ReleaseCapture();
        gMonitorCalibrating = FALSE;
    }
    DestroyWindow(hTaskBar);
    hTaskBar = NULL;
    DestroyWindow(hTray);
    if (IsWindow(hSetting)) DestroyWindow(hSetting);
    std::printf("Native regression: %d failed out of %zu groups\n", failures, sizeof(tests) / sizeof(tests[0]));
    return failures ? 1 : 0;
}
