#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <strsafe.h>
#include <uxtheme.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "../res/resource.h"

namespace {

constexpr wchar_t kWindowClassName[] = L"MouseMoverMainWindow";
constexpr wchar_t kAppName[] = L"MouseMover";
constexpr wchar_t kIniSectionSchedule[] = L"Schedule";
constexpr wchar_t kIniSectionMovement[] = L"Movement";
constexpr wchar_t kIniSectionUi[] = L"Ui";
constexpr UINT kTrayMessage = WM_APP + 42;
constexpr UINT kTrayIconId = 1;
constexpr UINT_PTR kTickTimerId = 1;
constexpr UINT kTickMs = 100;
constexpr double kPi = 3.14159265358979323846;

constexpr int kMinIntervalMinutes = 1;
constexpr int kMaxIntervalMinutes = 1440;
constexpr int kMinDurationSeconds = 5;
constexpr int kMaxDurationSeconds = 10;
constexpr int kMinDistancePx = 1;
constexpr int kMaxDistancePx = 200;
constexpr int kMinStepMs = 50;
constexpr int kMaxStepMs = 1000;
constexpr int kMinSweepRadiusPx = 80;
constexpr int kSweepCycleSteps = 40;

constexpr COLORREF kLightBackground = RGB(248, 249, 251);
constexpr COLORREF kLightEditBackground = RGB(255, 255, 255);
constexpr COLORREF kLightText = RGB(32, 37, 44);
constexpr COLORREF kDarkBackground = RGB(28, 31, 36);
constexpr COLORREF kDarkEditBackground = RGB(43, 48, 56);
constexpr COLORREF kDarkText = RGB(242, 245, 248);

enum ControlId : int {
    IdIntervalEdit = 1001,
    IdDurationEdit,
    IdPatternCombo,
    IdReturnToStart,
    IdDistanceEdit,
    IdStepEdit,
    IdSaveButton,
    IdStartButton,
    IdStopButton,
    IdRunNowButton,
    IdHideButton,
    IdStatusText,
    IdThemeToggle,
    IdTrayShow = 2001,
    IdTrayStart,
    IdTrayStop,
    IdTrayRunNow,
    IdTrayExit
};

enum class MovementPattern {
    Jiggle = 0,
    Sweep = 1
};

enum class RunState {
    Stopped,
    Waiting,
    Moving
};

struct Settings {
    int intervalMinutes = 5;
    int durationSeconds = 8;
    int distancePx = 12;
    int stepMs = 120;
    MovementPattern pattern = MovementPattern::Jiggle;
    bool returnToStart = true;
    bool darkMode = false;
};

struct AppState {
    HINSTANCE instance = nullptr;
    HWND hwnd = nullptr;
    HFONT font = nullptr;
    HBRUSH backgroundBrush = nullptr;
    HBRUSH editBrush = nullptr;
    NOTIFYICONDATAW trayIcon = {};
    bool trayAdded = false;
    bool exitRequested = false;
    Settings settings;
    std::wstring settingsPath;
    RunState state = RunState::Stopped;
    bool scheduleEnabled = false;
    bool manualRun = false;
    ULONGLONG nextCycleAt = 0;
    ULONGLONG moveEndAt = 0;
    ULONGLONG nextStepAt = 0;
    POINT moveStart = {};
    int stepIndex = 0;
    std::mt19937 rng{ 0x4d6f7573U };
};

AppState g_app;

int ClampInt(const int value, const int minimum, const int maximum) {
    return std::max(minimum, std::min(maximum, value));
}

Settings SanitizeSettings(Settings settings) {
    settings.intervalMinutes = ClampInt(settings.intervalMinutes, kMinIntervalMinutes, kMaxIntervalMinutes);
    settings.durationSeconds = ClampInt(settings.durationSeconds, kMinDurationSeconds, kMaxDurationSeconds);
    settings.distancePx = ClampInt(settings.distancePx, kMinDistancePx, kMaxDistancePx);
    settings.stepMs = ClampInt(settings.stepMs, kMinStepMs, kMaxStepMs);
    if (settings.pattern != MovementPattern::Jiggle && settings.pattern != MovementPattern::Sweep) {
        settings.pattern = MovementPattern::Jiggle;
    }
    return settings;
}

std::wstring ToString(const int value) {
    wchar_t buffer[32] = {};
    StringCchPrintfW(buffer, ARRAYSIZE(buffer), L"%d", value);
    return buffer;
}

bool EnsureDirectoryExists(const std::wstring& path) {
    if (CreateDirectoryW(path.c_str(), nullptr) != FALSE) {
        return true;
    }

    return GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring GetSettingsPath() {
    PWSTR roamingPath = nullptr;
    std::wstring basePath;

    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roamingPath)) && roamingPath != nullptr) {
        basePath = roamingPath;
        CoTaskMemFree(roamingPath);
    } else {
        wchar_t buffer[MAX_PATH] = {};
        const DWORD length = GetEnvironmentVariableW(L"APPDATA", buffer, ARRAYSIZE(buffer));
        if (length > 0 && length < ARRAYSIZE(buffer)) {
            basePath = buffer;
        } else {
            basePath = L".";
        }
    }

    const std::wstring settingsDir = basePath + L"\\MouseMover";
    EnsureDirectoryExists(settingsDir);
    return settingsDir + L"\\settings.ini";
}

Settings LoadSettingsFromFile(const std::wstring& path) {
    Settings settings;
    settings.intervalMinutes = static_cast<int>(GetPrivateProfileIntW(kIniSectionSchedule, L"IntervalMinutes", settings.intervalMinutes, path.c_str()));
    settings.durationSeconds = static_cast<int>(GetPrivateProfileIntW(kIniSectionSchedule, L"DurationSeconds", settings.durationSeconds, path.c_str()));
    settings.distancePx = static_cast<int>(GetPrivateProfileIntW(kIniSectionMovement, L"DistancePx", settings.distancePx, path.c_str()));
    settings.stepMs = static_cast<int>(GetPrivateProfileIntW(kIniSectionMovement, L"StepMs", settings.stepMs, path.c_str()));
    const int pattern = static_cast<int>(GetPrivateProfileIntW(kIniSectionMovement, L"Pattern", static_cast<int>(settings.pattern), path.c_str()));
    settings.pattern = pattern == static_cast<int>(MovementPattern::Sweep) ? MovementPattern::Sweep : MovementPattern::Jiggle;
    settings.returnToStart = GetPrivateProfileIntW(kIniSectionMovement, L"ReturnToStart", settings.returnToStart ? 1U : 0U, path.c_str()) != 0U;
    settings.darkMode = GetPrivateProfileIntW(kIniSectionUi, L"DarkMode", settings.darkMode ? 1U : 0U, path.c_str()) != 0U;
    return SanitizeSettings(settings);
}

bool WriteIniInt(const std::wstring& path, const wchar_t* section, const wchar_t* key, const int value) {
    return WritePrivateProfileStringW(section, key, ToString(value).c_str(), path.c_str()) != FALSE;
}

bool SaveSettingsToFile(const std::wstring& path, const Settings& rawSettings) {
    const Settings settings = SanitizeSettings(rawSettings);
    bool ok = true;
    ok = WriteIniInt(path, kIniSectionSchedule, L"IntervalMinutes", settings.intervalMinutes) && ok;
    ok = WriteIniInt(path, kIniSectionSchedule, L"DurationSeconds", settings.durationSeconds) && ok;
    ok = WriteIniInt(path, kIniSectionMovement, L"DistancePx", settings.distancePx) && ok;
    ok = WriteIniInt(path, kIniSectionMovement, L"StepMs", settings.stepMs) && ok;
    ok = WriteIniInt(path, kIniSectionMovement, L"Pattern", static_cast<int>(settings.pattern)) && ok;
    ok = WriteIniInt(path, kIniSectionMovement, L"ReturnToStart", settings.returnToStart ? 1 : 0) && ok;
    ok = WriteIniInt(path, kIniSectionUi, L"DarkMode", settings.darkMode ? 1 : 0) && ok;
    return ok;
}

ULONGLONG IntervalMs(const Settings& settings) {
    return static_cast<ULONGLONG>(settings.intervalMinutes) * 60ULL * 1000ULL;
}

ULONGLONG DurationMs(const Settings& settings) {
    return static_cast<ULONGLONG>(settings.durationSeconds) * 1000ULL;
}

ULONGLONG RemainingMs(const ULONGLONG now, const ULONGLONG target) {
    return target > now ? target - now : 0ULL;
}

std::wstring FormatRemaining(const ULONGLONG milliseconds) {
    const ULONGLONG totalSeconds = (milliseconds + 999ULL) / 1000ULL;
    const ULONGLONG minutes = totalSeconds / 60ULL;
    const ULONGLONG seconds = totalSeconds % 60ULL;

    wchar_t buffer[64] = {};
    if (minutes > 0ULL) {
        StringCchPrintfW(buffer, ARRAYSIZE(buffer), L"%llu:%02llu", minutes, seconds);
    } else {
        StringCchPrintfW(buffer, ARRAYSIZE(buffer), L"%llu sec", seconds);
    }
    return buffer;
}

POINT ClampToVirtualScreen(POINT point) {
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    point.x = ClampInt(point.x, left, left + width - 1);
    point.y = ClampInt(point.y, top, top + height - 1);
    return point;
}

int SweepRadiusPx(const Settings& settings) {
    return ClampInt(std::max(settings.distancePx, kMinSweepRadiusPx), kMinSweepRadiusPx, kMaxDistancePx);
}

POINT OffsetForSweep(const Settings& settings, const int stepIndex) {
    const int radius = SweepRadiusPx(settings);
    const double angle = static_cast<double>(stepIndex % kSweepCycleSteps) * (2.0 * kPi / static_cast<double>(kSweepCycleSteps));
    POINT offset = {};
    offset.x = static_cast<LONG>(std::lround(std::sin(angle) * static_cast<double>(radius)));
    offset.y = static_cast<LONG>(std::lround(std::sin(angle * 2.0) * static_cast<double>(radius) / 5.0));
    return offset;
}

POINT OffsetForJiggle(const Settings& settings, std::mt19937& rng) {
    std::uniform_int_distribution<int> distribution(-settings.distancePx, settings.distancePx);
    POINT offset = {};
    offset.x = distribution(rng);
    offset.y = distribution(rng);
    return offset;
}

POINT MovementOffset(const Settings& settings, const int stepIndex, std::mt19937& rng) {
    if (settings.pattern == MovementPattern::Sweep) {
        return OffsetForSweep(settings, stepIndex);
    }

    return OffsetForJiggle(settings, rng);
}

int MovementStepLimitPx(const Settings& settings) {
    if (settings.pattern == MovementPattern::Sweep) {
        return ClampInt(SweepRadiusPx(settings) / 4, 16, 60);
    }

    return std::max(1, settings.distancePx);
}

void MoveRelative(const int dx, const int dy) {
    if (dx == 0 && dy == 0) {
        return;
    }

    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &input, sizeof(input));
}

void MoveTowardPoint(const POINT target, const int maxStep) {
    POINT current = {};
    if (GetCursorPos(&current) == FALSE) {
        return;
    }

    const int dx = ClampInt(target.x - current.x, -maxStep, maxStep);
    const int dy = ClampInt(target.y - current.y, -maxStep, maxStep);
    MoveRelative(dx, dy);
}

void SmoothReturnToPoint(const POINT target) {
    POINT current = {};
    if (GetCursorPos(&current) == FALSE) {
        return;
    }

    constexpr int kReturnSteps = 10;
    for (int i = kReturnSteps; i > 0; --i) {
        if (GetCursorPos(&current) == FALSE) {
            break;
        }

        const int dx = (target.x - current.x) / i;
        const int dy = (target.y - current.y) / i;
        MoveRelative(dx, dy);
        Sleep(8);
    }

    SetCursorPos(target.x, target.y);
}

std::wstring StateName() {
    switch (g_app.state) {
    case RunState::Waiting:
        return L"Waiting";
    case RunState::Moving:
        return L"Moving";
    case RunState::Stopped:
    default:
        return L"Stopped";
    }
}

COLORREF BackgroundColor() {
    return g_app.settings.darkMode ? kDarkBackground : kLightBackground;
}

COLORREF EditBackgroundColor() {
    return g_app.settings.darkMode ? kDarkEditBackground : kLightEditBackground;
}

COLORREF TextColor() {
    return g_app.settings.darkMode ? kDarkText : kLightText;
}

void DeleteThemeBrushes() {
    if (g_app.backgroundBrush != nullptr) {
        DeleteObject(g_app.backgroundBrush);
        g_app.backgroundBrush = nullptr;
    }

    if (g_app.editBrush != nullptr) {
        DeleteObject(g_app.editBrush);
        g_app.editBrush = nullptr;
    }
}

void RebuildThemeBrushes() {
    DeleteThemeBrushes();
    g_app.backgroundBrush = CreateSolidBrush(BackgroundColor());
    g_app.editBrush = CreateSolidBrush(EditBackgroundColor());
}

void UpdateThemeToggleText() {
    HWND toggle = GetDlgItem(g_app.hwnd, IdThemeToggle);
    if (toggle != nullptr) {
        SetWindowTextW(toggle, g_app.settings.darkMode ? L"\x2600" : L"\x263E");
    }
}

BOOL CALLBACK ApplyThemeToChild(HWND child, LPARAM lParam) {
    const bool darkMode = lParam != 0;
    SetWindowTheme(child, darkMode ? L"DarkMode_Explorer" : nullptr, nullptr);
    InvalidateRect(child, nullptr, TRUE);
    return TRUE;
}

void ApplyTheme() {
    RebuildThemeBrushes();
    UpdateThemeToggleText();

    if (g_app.hwnd != nullptr) {
        const BOOL darkMode = g_app.settings.darkMode ? TRUE : FALSE;
        DwmSetWindowAttribute(g_app.hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
        SetWindowTheme(g_app.hwnd, g_app.settings.darkMode ? L"DarkMode_Explorer" : nullptr, nullptr);
        EnumChildWindows(g_app.hwnd, ApplyThemeToChild, g_app.settings.darkMode ? 1 : 0);
        InvalidateRect(g_app.hwnd, nullptr, TRUE);
    }
}

HBRUSH ApplyControlColors(const HDC dc, const bool editBackground) {
    SetTextColor(dc, TextColor());
    SetBkMode(dc, editBackground ? OPAQUE : TRANSPARENT);
    SetBkColor(dc, editBackground ? EditBackgroundColor() : BackgroundColor());
    return editBackground ? g_app.editBrush : g_app.backgroundBrush;
}

void SetControlFont(const HWND hwnd) {
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.font), TRUE);
}

HWND AddControl(const wchar_t* className, const wchar_t* text, const DWORD style, const DWORD exStyle, const int id, const int x, const int y, const int width, const int height) {
    HWND control = CreateWindowExW(
        exStyle,
        className,
        text,
        WS_CHILD | WS_VISIBLE | style,
        x,
        y,
        width,
        height,
        g_app.hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        g_app.instance,
        nullptr);

    if (control != nullptr) {
        SetControlFont(control);
    }

    return control;
}

void SetEditInt(const int id, const int value) {
    SetWindowTextW(GetDlgItem(g_app.hwnd, id), ToString(value).c_str());
}

int ReadEditInt(const int id, const int fallback) {
    wchar_t text[64] = {};
    GetWindowTextW(GetDlgItem(g_app.hwnd, id), text, ARRAYSIZE(text));

    wchar_t* end = nullptr;
    const long value = std::wcstol(text, &end, 10);
    if (end == text) {
        return fallback;
    }

    return static_cast<int>(value);
}

void ApplySettingsToControls(const Settings& settings) {
    SetEditInt(IdIntervalEdit, settings.intervalMinutes);
    SetEditInt(IdDurationEdit, settings.durationSeconds);
    SetEditInt(IdDistanceEdit, settings.distancePx);
    SetEditInt(IdStepEdit, settings.stepMs);
    SendMessageW(GetDlgItem(g_app.hwnd, IdPatternCombo), CB_SETCURSEL, static_cast<WPARAM>(settings.pattern == MovementPattern::Sweep ? 1 : 0), 0);
    SendMessageW(GetDlgItem(g_app.hwnd, IdReturnToStart), BM_SETCHECK, settings.returnToStart ? BST_CHECKED : BST_UNCHECKED, 0);
    UpdateThemeToggleText();
}

Settings ReadSettingsFromControls() {
    Settings settings = g_app.settings;
    settings.intervalMinutes = ReadEditInt(IdIntervalEdit, settings.intervalMinutes);
    settings.durationSeconds = ReadEditInt(IdDurationEdit, settings.durationSeconds);
    settings.distancePx = ReadEditInt(IdDistanceEdit, settings.distancePx);
    settings.stepMs = ReadEditInt(IdStepEdit, settings.stepMs);

    const LRESULT patternIndex = SendMessageW(GetDlgItem(g_app.hwnd, IdPatternCombo), CB_GETCURSEL, 0, 0);
    settings.pattern = patternIndex == 1 ? MovementPattern::Sweep : MovementPattern::Jiggle;
    settings.returnToStart = SendMessageW(GetDlgItem(g_app.hwnd, IdReturnToStart), BM_GETCHECK, 0, 0) == BST_CHECKED;

    return SanitizeSettings(settings);
}

void UpdateTrayTooltip() {
    if (!g_app.trayAdded) {
        return;
    }

    wchar_t tip[ARRAYSIZE(g_app.trayIcon.szTip)] = {};
    StringCchPrintfW(tip, ARRAYSIZE(tip), L"%s - %s", kAppName, StateName().c_str());
    StringCchCopyW(g_app.trayIcon.szTip, ARRAYSIZE(g_app.trayIcon.szTip), tip);
    Shell_NotifyIconW(NIM_MODIFY, &g_app.trayIcon);
}

void UpdateStatusText() {
    std::wstring status;
    const ULONGLONG now = GetTickCount64();

    if (g_app.state == RunState::Stopped) {
        status = L"Stopped. Configure settings, then Start or Run Now.";
    } else if (g_app.state == RunState::Waiting) {
        status = L"Waiting. Next movement in " + FormatRemaining(RemainingMs(now, g_app.nextCycleAt)) + L".";
    } else {
        status = L"Moving. " + FormatRemaining(RemainingMs(now, g_app.moveEndAt)) + L" remaining.";
        if (g_app.manualRun) {
            status += L" Manual run.";
        }
    }

    SetWindowTextW(GetDlgItem(g_app.hwnd, IdStatusText), status.c_str());
    UpdateTrayTooltip();
}

void UpdateButtons() {
    const bool moving = g_app.state == RunState::Moving;
    EnableWindow(GetDlgItem(g_app.hwnd, IdStartButton), g_app.scheduleEnabled ? FALSE : TRUE);
    EnableWindow(GetDlgItem(g_app.hwnd, IdStopButton), g_app.state == RunState::Stopped ? FALSE : TRUE);
    EnableWindow(GetDlgItem(g_app.hwnd, IdRunNowButton), moving ? FALSE : TRUE);
}

bool SaveCurrentSettings() {
    g_app.settings = ReadSettingsFromControls();
    ApplySettingsToControls(g_app.settings);
    return SaveSettingsToFile(g_app.settingsPath, g_app.settings);
}

void ToggleTheme() {
    g_app.settings = ReadSettingsFromControls();
    g_app.settings.darkMode = !g_app.settings.darkMode;
    ApplySettingsToControls(g_app.settings);
    SaveSettingsToFile(g_app.settingsPath, g_app.settings);
    ApplyTheme();
    UpdateStatusText();
}

void StartMovementCycle(const bool manualRun) {
    if (GetCursorPos(&g_app.moveStart) == FALSE) {
        return;
    }

    const ULONGLONG now = GetTickCount64();
    g_app.state = RunState::Moving;
    g_app.manualRun = manualRun;
    g_app.stepIndex = 0;
    g_app.nextStepAt = now;
    g_app.moveEndAt = now + DurationMs(g_app.settings);
    UpdateButtons();
    UpdateStatusText();
}

void StartSchedule() {
    SaveCurrentSettings();
    g_app.scheduleEnabled = true;

    if (g_app.state != RunState::Moving) {
        g_app.state = RunState::Waiting;
        g_app.nextCycleAt = GetTickCount64() + IntervalMs(g_app.settings);
    }

    UpdateButtons();
    UpdateStatusText();
}

void StopAll() {
    g_app.scheduleEnabled = false;
    g_app.manualRun = false;
    g_app.state = RunState::Stopped;
    UpdateButtons();
    UpdateStatusText();
}

void RunNow() {
    SaveCurrentSettings();
    if (g_app.state != RunState::Moving) {
        StartMovementCycle(true);
    }
}

void FinishMovementCycle() {
    if (g_app.settings.returnToStart) {
        SmoothReturnToPoint(ClampToVirtualScreen(g_app.moveStart));
    }

    const bool completedManualRun = g_app.manualRun;
    g_app.manualRun = false;

    if (g_app.scheduleEnabled) {
        g_app.state = RunState::Waiting;

        const ULONGLONG now = GetTickCount64();
        if (!completedManualRun || g_app.nextCycleAt <= now) {
            g_app.nextCycleAt = now + IntervalMs(g_app.settings);
        }
    } else {
        g_app.state = RunState::Stopped;
    }

    UpdateButtons();
    UpdateStatusText();
}

void PerformMovementStep() {
    POINT offset = MovementOffset(g_app.settings, g_app.stepIndex, g_app.rng);
    POINT target = {};
    target.x = g_app.moveStart.x + offset.x;
    target.y = g_app.moveStart.y + offset.y;
    target = ClampToVirtualScreen(target);

    MoveTowardPoint(target, MovementStepLimitPx(g_app.settings));
    ++g_app.stepIndex;
}

void Tick() {
    const ULONGLONG now = GetTickCount64();

    if (g_app.state == RunState::Waiting && g_app.scheduleEnabled && now >= g_app.nextCycleAt) {
        StartMovementCycle(false);
    }

    if (g_app.state == RunState::Moving) {
        if (now >= g_app.moveEndAt) {
            FinishMovementCycle();
            return;
        }

        if (now >= g_app.nextStepAt) {
            PerformMovementStep();
            g_app.nextStepAt = now + static_cast<ULONGLONG>(g_app.settings.stepMs);
        }
    }

    UpdateStatusText();
}

void ShowMainWindow() {
    ShowWindow(g_app.hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_app.hwnd);
}

HICON LoadAppIcon(const HINSTANCE instance, const int width, const int height) {
    HICON icon = static_cast<HICON>(LoadImageW(
        instance,
        MAKEINTRESOURCEW(IDI_APP_ICON),
        IMAGE_ICON,
        width,
        height,
        LR_DEFAULTCOLOR | LR_SHARED));

    if (icon == nullptr) {
        icon = LoadIconW(nullptr, IDI_APPLICATION);
    }

    return icon;
}

void AddTrayIcon() {
    ZeroMemory(&g_app.trayIcon, sizeof(g_app.trayIcon));
    g_app.trayIcon.cbSize = sizeof(g_app.trayIcon);
    g_app.trayIcon.hWnd = g_app.hwnd;
    g_app.trayIcon.uID = kTrayIconId;
    g_app.trayIcon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_app.trayIcon.uCallbackMessage = kTrayMessage;
    g_app.trayIcon.hIcon = LoadAppIcon(g_app.instance, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    StringCchCopyW(g_app.trayIcon.szTip, ARRAYSIZE(g_app.trayIcon.szTip), L"MouseMover - Stopped");

    if (Shell_NotifyIconW(NIM_ADD, &g_app.trayIcon) != FALSE) {
        g_app.trayAdded = true;
        g_app.trayIcon.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &g_app.trayIcon);
    }
}

void RemoveTrayIcon() {
    if (g_app.trayAdded) {
        Shell_NotifyIconW(NIM_DELETE, &g_app.trayIcon);
        g_app.trayAdded = false;
    }
}

void ShowTrayMenu() {
    POINT cursor = {};
    GetCursorPos(&cursor);

    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }

    AppendMenuW(menu, MF_STRING, IdTrayShow, L"Show Settings");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (g_app.scheduleEnabled ? MF_GRAYED : 0U), IdTrayStart, L"Start");
    AppendMenuW(menu, MF_STRING | (g_app.state == RunState::Stopped ? MF_GRAYED : 0U), IdTrayStop, L"Stop");
    AppendMenuW(menu, MF_STRING | (g_app.state == RunState::Moving ? MF_GRAYED : 0U), IdTrayRunNow, L"Run Now");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IdTrayExit, L"Exit");

    SetForegroundWindow(g_app.hwnd);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, g_app.hwnd, nullptr);
    DestroyMenu(menu);

    if (command != 0U) {
        SendMessageW(g_app.hwnd, WM_COMMAND, static_cast<WPARAM>(command), 0);
    }
}

void CreateControls() {
    g_app.font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

    AddControl(L"BUTTON", L"Schedule", BS_GROUPBOX, 0, -1, 14, 12, 412, 94);
    AddControl(L"BUTTON", L"", WS_TABSTOP | BS_PUSHBUTTON, 0, IdThemeToggle, 386, 24, 30, 26);
    AddControl(L"STATIC", L"Interval (minutes)", 0, 0, -1, 32, 42, 130, 22);
    AddControl(L"EDIT", L"", WS_TABSTOP | ES_NUMBER | WS_BORDER | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, IdIntervalEdit, 176, 38, 72, 24);
    AddControl(L"STATIC", L"1 - 1440", 0, 0, -1, 260, 42, 90, 22);
    AddControl(L"STATIC", L"Duration (seconds)", 0, 0, -1, 32, 74, 130, 22);
    AddControl(L"EDIT", L"", WS_TABSTOP | ES_NUMBER | WS_BORDER | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, IdDurationEdit, 176, 70, 72, 24);
    AddControl(L"STATIC", L"5 - 10", 0, 0, -1, 260, 74, 90, 22);

    AddControl(L"BUTTON", L"Movement", BS_GROUPBOX, 0, -1, 14, 118, 412, 126);
    AddControl(L"STATIC", L"Pattern", 0, 0, -1, 32, 150, 130, 22);
    HWND combo = AddControl(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST, 0, IdPatternCombo, 176, 146, 178, 120);
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Subtle jiggle"));
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Visible sweep"));
    AddControl(L"BUTTON", L"Return to start after each cycle", WS_TABSTOP | BS_AUTOCHECKBOX, 0, IdReturnToStart, 176, 178, 222, 24);
    AddControl(L"STATIC", L"Distance / radius (px)", 0, 0, -1, 32, 208, 140, 22);
    AddControl(L"EDIT", L"", WS_TABSTOP | ES_NUMBER | WS_BORDER | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, IdDistanceEdit, 176, 204, 72, 24);
    AddControl(L"STATIC", L"Step speed (ms)", 0, 0, -1, 260, 208, 104, 22);
    AddControl(L"EDIT", L"", WS_TABSTOP | ES_NUMBER | WS_BORDER | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, IdStepEdit, 362, 204, 48, 24);

    AddControl(L"STATIC", L"Stopped. Configure settings, then Start or Run Now.", 0, 0, IdStatusText, 20, 258, 406, 24);
    AddControl(L"BUTTON", L"Save Settings", WS_TABSTOP | BS_PUSHBUTTON, 0, IdSaveButton, 20, 294, 98, 30);
    AddControl(L"BUTTON", L"Start", WS_TABSTOP | BS_DEFPUSHBUTTON, 0, IdStartButton, 126, 294, 72, 30);
    AddControl(L"BUTTON", L"Stop", WS_TABSTOP | BS_PUSHBUTTON, 0, IdStopButton, 206, 294, 72, 30);
    AddControl(L"BUTTON", L"Run Now", WS_TABSTOP | BS_PUSHBUTTON, 0, IdRunNowButton, 286, 294, 78, 30);
    AddControl(L"BUTTON", L"Hide", WS_TABSTOP | BS_PUSHBUTTON, 0, IdHideButton, 372, 294, 54, 30);

    ApplySettingsToControls(g_app.settings);
    UpdateButtons();
    UpdateStatusText();
}

void ExitApplication() {
    g_app.exitRequested = true;
    SaveCurrentSettings();
    DestroyWindow(g_app.hwnd);
}

LRESULT HandleCommand(const WPARAM wParam) {
    switch (LOWORD(wParam)) {
    case IdSaveButton:
        SaveCurrentSettings();
        if (g_app.state == RunState::Waiting) {
            g_app.nextCycleAt = GetTickCount64() + IntervalMs(g_app.settings);
        }
        UpdateStatusText();
        return 0;
    case IdStartButton:
    case IdTrayStart:
        StartSchedule();
        return 0;
    case IdStopButton:
    case IdTrayStop:
        StopAll();
        return 0;
    case IdRunNowButton:
    case IdTrayRunNow:
        RunNow();
        return 0;
    case IdHideButton:
        ShowWindow(g_app.hwnd, SW_HIDE);
        return 0;
    case IdThemeToggle:
        ToggleTheme();
        return 0;
    case IdTrayShow:
        ShowMainWindow();
        return 0;
    case IdTrayExit:
        ExitApplication();
        return 0;
    default:
        return 1;
    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        g_app.hwnd = hwnd;
        g_app.settingsPath = GetSettingsPath();
        g_app.settings = LoadSettingsFromFile(g_app.settingsPath);
        RebuildThemeBrushes();
        CreateControls();
        ApplyTheme();
        AddTrayIcon();
        SetTimer(hwnd, kTickTimerId, kTickMs, nullptr);
        return 0;
    case WM_ERASEBKGND: {
        RECT rect = {};
        GetClientRect(hwnd, &rect);
        FillRect(reinterpret_cast<HDC>(wParam), &rect, g_app.backgroundBrush);
        return 1;
    }
    case WM_CTLCOLORDLG:
        return reinterpret_cast<LRESULT>(g_app.backgroundBrush);
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        return reinterpret_cast<LRESULT>(ApplyControlColors(reinterpret_cast<HDC>(wParam), false));
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return reinterpret_cast<LRESULT>(ApplyControlColors(reinterpret_cast<HDC>(wParam), true));
    case WM_COMMAND:
        if (HandleCommand(wParam) == 0) {
            return 0;
        }
        break;
    case WM_TIMER:
        if (wParam == kTickTimerId) {
            Tick();
            return 0;
        }
        break;
    case kTrayMessage:
        switch (LOWORD(lParam)) {
        case WM_CONTEXTMENU:
        case WM_RBUTTONUP:
            ShowTrayMenu();
            return 0;
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case WM_LBUTTONDBLCLK:
            ShowMainWindow();
            return 0;
        default:
            break;
        }
        return 0;
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) {
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (!g_app.exitRequested) {
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        break;
    case WM_DESTROY:
        KillTimer(hwnd, kTickTimerId);
        RemoveTrayIcon();
        DeleteThemeBrushes();
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool RegisterWindowClass(const HINSTANCE instance) {
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadAppIcon(instance, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kWindowClassName;
    windowClass.hIconSm = LoadAppIcon(instance, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));

    return RegisterClassExW(&windowClass) != 0;
}

HWND CreateMainWindow(const HINSTANCE instance) {
    RECT rect = { 0, 0, 456, 374 };
    AdjustWindowRectEx(&rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);

    return CreateWindowExW(
        0,
        kWindowClassName,
        kAppName,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr,
        nullptr,
        instance,
        nullptr);
}

bool HasArgument(const wchar_t* argument) {
    int argc = 0;
    PWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return false;
    }

    const std::unique_ptr<PWSTR, decltype(&LocalFree)> guard(argv, LocalFree);
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], argument) == 0) {
            return true;
        }
    }

    return false;
}

bool TestSettingsRoundTrip() {
    wchar_t tempPath[MAX_PATH] = {};
    if (GetTempPathW(ARRAYSIZE(tempPath), tempPath) == 0) {
        return false;
    }

    const std::wstring testDir = std::wstring(tempPath) + L"MouseMoverSelfTest";
    EnsureDirectoryExists(testDir);
    const std::wstring path = testDir + L"\\settings.ini";
    DeleteFileW(path.c_str());

    Settings settings;
    settings.intervalMinutes = 11;
    settings.durationSeconds = 9;
    settings.distancePx = 33;
    settings.stepMs = 250;
    settings.pattern = MovementPattern::Sweep;
    settings.returnToStart = false;
    settings.darkMode = true;

    if (!SaveSettingsToFile(path, settings)) {
        return false;
    }

    const Settings loaded = LoadSettingsFromFile(path);
    return loaded.intervalMinutes == 11
        && loaded.durationSeconds == 9
        && loaded.distancePx == 33
        && loaded.stepMs == 250
        && loaded.pattern == MovementPattern::Sweep
        && !loaded.returnToStart
        && loaded.darkMode;
}

bool TestSettingsClamping() {
    Settings settings;
    settings.intervalMinutes = -10;
    settings.durationSeconds = 99;
    settings.distancePx = 0;
    settings.stepMs = 1;
    settings.pattern = static_cast<MovementPattern>(99);

    settings = SanitizeSettings(settings);
    return settings.intervalMinutes == kMinIntervalMinutes
        && settings.durationSeconds == kMaxDurationSeconds
        && settings.distancePx == kMinDistancePx
        && settings.stepMs == kMinStepMs
        && settings.pattern == MovementPattern::Jiggle;
}

bool TestMovementOffsets() {
    Settings settings;
    settings.distancePx = 20;
    settings.pattern = MovementPattern::Sweep;
    POINT start = OffsetForSweep(settings, 0);
    POINT right = OffsetForSweep(settings, kSweepCycleSteps / 4);
    POINT wobble = OffsetForSweep(settings, kSweepCycleSteps / 8);
    POINT left = OffsetForSweep(settings, (kSweepCycleSteps * 3) / 4);
    if (start.x != 0 || start.y != 0 || right.x != kMinSweepRadiusPx || right.y != 0 || wobble.y <= 0 || left.x != -kMinSweepRadiusPx) {
        return false;
    }

    if (MovementStepLimitPx(settings) < 16) {
        return false;
    }

    settings.pattern = MovementPattern::Jiggle;
    std::mt19937 rng{ 1234U };
    for (int i = 0; i < 100; ++i) {
        POINT offset = MovementOffset(settings, i, rng);
        if (offset.x < -settings.distancePx || offset.x > settings.distancePx
            || offset.y < -settings.distancePx || offset.y > settings.distancePx) {
            return false;
        }
    }

    return true;
}

bool TestSchedulerMath() {
    Settings settings;
    settings.intervalMinutes = 5;
    settings.durationSeconds = 8;

    return IntervalMs(settings) == 300000ULL
        && DurationMs(settings) == 8000ULL
        && RemainingMs(10ULL, 30ULL) == 20ULL
        && RemainingMs(30ULL, 10ULL) == 0ULL;
}

int RunSelfTest() {
    return TestSettingsRoundTrip()
        && TestSettingsClamping()
        && TestMovementOffsets()
        && TestSchedulerMath()
        ? 0
        : 1;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    if (HasArgument(L"--self-test")) {
        return RunSelfTest();
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);

    INITCOMMONCONTROLSEX controls = {};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    g_app.instance = instance;
    g_app.rng.seed(static_cast<unsigned int>(GetTickCount64()));

    if (!RegisterWindowClass(instance)) {
        MessageBoxW(nullptr, L"Unable to register the MouseMover window class.", kAppName, MB_ICONERROR | MB_OK);
        return 1;
    }

    HWND hwnd = CreateMainWindow(instance);
    if (hwnd == nullptr) {
        MessageBoxW(nullptr, L"Unable to create the MouseMover window.", kAppName, MB_ICONERROR | MB_OK);
        return 1;
    }

    ShowWindow(hwnd, showCommand);
    UpdateWindow(hwnd);

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return static_cast<int>(message.wParam);
}
