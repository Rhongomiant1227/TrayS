// Exercise the current implementation with isolated windows and simulated
// mouse input. No TrayS startup, Explorer interaction, or driver loading.
#include "framework.h"
#include "TrayS.h"
#include <cstdio>
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
        { "Composition and fullscreen vertical offsets", TestPopupLayout }
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
