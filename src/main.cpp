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

constexpr int kClientWidth = 500;
constexpr int kClientHeight = 426;
constexpr int kPanelRadius = 18;
constexpr int kButtonRadius = 12;
constexpr int kButtonSubclassId = 1;
constexpr DWORD kDwmWindowCornerPreference = 33;
constexpr int kDwmWindowCornerRound = 2;

constexpr int kScheduleX = 20;
constexpr int kScheduleY = 18;
constexpr int kScheduleW = 460;
constexpr int kScheduleH = 116;
constexpr int kMovementX = 20;
constexpr int kMovementY = 148;
constexpr int kMovementW = 460;
constexpr int kMovementH = 144;
constexpr int kStatusX = 20;
constexpr int kStatusY = 306;
constexpr int kStatusW = 460;
constexpr int kStatusH = 48;

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

enum class ButtonRole {
    Neutral,
    Primary,
    Secondary,
    Danger,
    Ghost
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
    HFONT titleFont = nullptr;
    HBRUSH backgroundBrush = nullptr;
    HBRUSH surfaceBrush = nullptr;
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
    int hoveredButtonId = 0;
    int pressedButtonId = 0;
    std::mt19937 rng{ 0x4d6f7573U };
};

AppState g_app;

struct ThemePalette {
    COLORREF background;
    COLORREF surface;
    COLORREF editBackground;
    COLORREF text;
    COLORREF mutedText;
    COLORREF border;
    COLORREF accent;
    COLORREF accentHover;
    COLORREF accentPressed;
    COLORREF accentSoft;
    COLORREF accentText;
    COLORREF danger;
    COLORREF dangerHover;
    COLORREF dangerPressed;
    COLORREF neutralButton;
    COLORREF neutralHover;
    COLORREF neutralPressed;
    COLORREF disabledFill;
    COLORREF disabledText;
    COLORREF focus;
};

struct ButtonPalette {
    COLORREF fill;
    COLORREF hoverFill;
    COLORREF pressedFill;
    COLORREF border;
    COLORREF text;
};

RECT MakeRect(const int x, const int y, const int width, const int height) {
    return RECT{ x, y, x + width, y + height };
}

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

const ThemePalette& CurrentPalette() {
    static const ThemePalette light = {
        RGB(245, 247, 250),
        RGB(255, 255, 255),
        RGB(255, 255, 255),
        RGB(31, 41, 55),
        RGB(100, 116, 139),
        RGB(221, 226, 235),
        RGB(37, 99, 235),
        RGB(29, 78, 216),
        RGB(30, 64, 175),
        RGB(235, 242, 255),
        RGB(255, 255, 255),
        RGB(220, 38, 38),
        RGB(185, 28, 28),
        RGB(153, 27, 27),
        RGB(248, 250, 252),
        RGB(241, 245, 249),
        RGB(226, 232, 240),
        RGB(229, 231, 235),
        RGB(148, 163, 184),
        RGB(59, 130, 246)
    };

    static const ThemePalette dark = {
        RGB(24, 27, 32),
        RGB(35, 40, 47),
        RGB(47, 54, 64),
        RGB(241, 245, 249),
        RGB(160, 174, 192),
        RGB(67, 76, 89),
        RGB(96, 165, 250),
        RGB(59, 130, 246),
        RGB(37, 99, 235),
        RGB(32, 52, 84),
        RGB(255, 255, 255),
        RGB(248, 113, 113),
        RGB(239, 68, 68),
        RGB(220, 38, 38),
        RGB(45, 52, 62),
        RGB(55, 64, 76),
        RGB(67, 76, 89),
        RGB(41, 48, 57),
        RGB(100, 116, 139),
        RGB(147, 197, 253)
    };

    return g_app.settings.darkMode ? dark : light;
}

COLORREF BackgroundColor() {
    return CurrentPalette().background;
}

COLORREF SurfaceColor() {
    return CurrentPalette().surface;
}

COLORREF EditBackgroundColor() {
    return CurrentPalette().editBackground;
}

COLORREF TextColor() {
    return CurrentPalette().text;
}

COLORREF BorderColor() {
    return CurrentPalette().border;
}

void DeleteThemeBrushes() {
    if (g_app.backgroundBrush != nullptr) {
        DeleteObject(g_app.backgroundBrush);
        g_app.backgroundBrush = nullptr;
    }

    if (g_app.surfaceBrush != nullptr) {
        DeleteObject(g_app.surfaceBrush);
        g_app.surfaceBrush = nullptr;
    }

    if (g_app.editBrush != nullptr) {
        DeleteObject(g_app.editBrush);
        g_app.editBrush = nullptr;
    }
}

void RebuildThemeBrushes() {
    DeleteThemeBrushes();
    g_app.backgroundBrush = CreateSolidBrush(BackgroundColor());
    g_app.surfaceBrush = CreateSolidBrush(SurfaceColor());
    g_app.editBrush = CreateSolidBrush(EditBackgroundColor());
}

HFONT CreateSegoeUiFont(const HWND hwnd, const int pointSize, const int weight) {
    const UINT dpi = hwnd != nullptr ? GetDpiForWindow(hwnd) : USER_DEFAULT_SCREEN_DPI;
    return CreateFontW(
        -MulDiv(pointSize, static_cast<int>(dpi), 72),
        0,
        0,
        0,
        weight,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");
}

void DeleteUiFonts() {
    const HFONT stockFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    if (g_app.font != nullptr && g_app.font != stockFont) {
        DeleteObject(g_app.font);
    }
    if (g_app.titleFont != nullptr && g_app.titleFont != stockFont && g_app.titleFont != g_app.font) {
        DeleteObject(g_app.titleFont);
    }

    g_app.font = nullptr;
    g_app.titleFont = nullptr;
}

void CreateUiFonts() {
    DeleteUiFonts();
    g_app.font = CreateSegoeUiFont(g_app.hwnd, 9, FW_NORMAL);
    g_app.titleFont = CreateSegoeUiFont(g_app.hwnd, 10, FW_SEMIBOLD);

    if (g_app.font == nullptr) {
        g_app.font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }
    if (g_app.titleFont == nullptr) {
        g_app.titleFont = g_app.font;
    }
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
        const int cornerPreference = kDwmWindowCornerRound;
        DwmSetWindowAttribute(
            g_app.hwnd,
            static_cast<DWMWINDOWATTRIBUTE>(kDwmWindowCornerPreference),
            &cornerPreference,
            sizeof(cornerPreference));
        SetWindowTheme(g_app.hwnd, g_app.settings.darkMode ? L"DarkMode_Explorer" : nullptr, nullptr);
        EnumChildWindows(g_app.hwnd, ApplyThemeToChild, g_app.settings.darkMode ? 1 : 0);
        InvalidateRect(g_app.hwnd, nullptr, TRUE);
    }
}

HBRUSH ApplySurfaceControlColors(const HDC dc) {
    SetTextColor(dc, TextColor());
    SetBkMode(dc, TRANSPARENT);
    SetBkColor(dc, SurfaceColor());
    return g_app.surfaceBrush;
}

HBRUSH ApplyEditControlColors(const HDC dc) {
    SetTextColor(dc, TextColor());
    SetBkMode(dc, OPAQUE);
    SetBkColor(dc, EditBackgroundColor());
    return g_app.editBrush;
}

void DrawRoundedRect(const HDC dc, const RECT& rect, const int radius, const COLORREF fill, const COLORREF border, const int borderWidth = 1) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, borderWidth, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);

    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);

    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

ButtonRole RoleForButton(const int id) {
    switch (id) {
    case IdStartButton:
        return ButtonRole::Primary;
    case IdRunNowButton:
        return ButtonRole::Secondary;
    case IdStopButton:
        return ButtonRole::Danger;
    case IdThemeToggle:
        return ButtonRole::Ghost;
    case IdSaveButton:
    case IdHideButton:
    default:
        return ButtonRole::Neutral;
    }
}

ButtonPalette PaletteForButtonRole(const ButtonRole role) {
    const ThemePalette& theme = CurrentPalette();
    switch (role) {
    case ButtonRole::Primary:
        return ButtonPalette{ theme.accent, theme.accentHover, theme.accentPressed, theme.accent, theme.accentText };
    case ButtonRole::Secondary:
        return ButtonPalette{ theme.accentSoft, theme.neutralHover, theme.neutralPressed, theme.accent, theme.accent };
    case ButtonRole::Danger:
        return ButtonPalette{ theme.danger, theme.dangerHover, theme.dangerPressed, theme.danger, RGB(255, 255, 255) };
    case ButtonRole::Ghost:
        return ButtonPalette{ theme.surface, theme.neutralHover, theme.neutralPressed, theme.border, theme.mutedText };
    case ButtonRole::Neutral:
    default:
        return ButtonPalette{ theme.neutralButton, theme.neutralHover, theme.neutralPressed, theme.border, theme.text };
    }
}

void DrawFocusRing(const HDC dc, RECT rect, const int radius) {
    InflateRect(&rect, -3, -3);
    HPEN pen = CreatePen(PS_SOLID, 1, CurrentPalette().focus);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));

    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);

    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void DrawOwnerButton(const DRAWITEMSTRUCT& item) {
    const int id = static_cast<int>(item.CtlID);
    const bool enabled = (item.itemState & ODS_DISABLED) == 0;
    const bool pressed = enabled && ((item.itemState & ODS_SELECTED) != 0 || g_app.pressedButtonId == id);
    const bool hovered = enabled && g_app.hoveredButtonId == id;
    const bool focused = enabled && (item.itemState & ODS_FOCUS) != 0;

    ButtonPalette colors = PaletteForButtonRole(RoleForButton(id));
    if (!enabled) {
        colors.fill = CurrentPalette().disabledFill;
        colors.hoverFill = colors.fill;
        colors.pressedFill = colors.fill;
        colors.border = CurrentPalette().border;
        colors.text = CurrentPalette().disabledText;
    }

    const COLORREF fill = pressed ? colors.pressedFill : (hovered ? colors.hoverFill : colors.fill);
    RECT rect = item.rcItem;
    DrawRoundedRect(item.hDC, rect, kButtonRadius, fill, colors.border);

    wchar_t text[128] = {};
    GetWindowTextW(item.hwndItem, text, ARRAYSIZE(text));

    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, colors.text);
    HGDIOBJ oldFont = SelectObject(item.hDC, g_app.font);
    if (pressed) {
        OffsetRect(&rect, 0, 1);
    }
    DrawTextW(item.hDC, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(item.hDC, oldFont);

    if (focused) {
        DrawFocusRing(item.hDC, item.rcItem, kButtonRadius);
    }
}

void InvalidateButton(const HWND hwnd) {
    if (hwnd != nullptr) {
        InvalidateRect(hwnd, nullptr, TRUE);
    }
}

LRESULT CALLBACK ButtonSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    const int id = GetDlgCtrlID(hwnd);
    switch (message) {
    case WM_MOUSEMOVE:
        if (g_app.hoveredButtonId != id) {
            g_app.hoveredButtonId = id;
            InvalidateButton(hwnd);

            TRACKMOUSEEVENT track = {};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd;
            TrackMouseEvent(&track);
        }
        break;
    case WM_MOUSELEAVE:
        if (g_app.hoveredButtonId == id) {
            g_app.hoveredButtonId = 0;
            InvalidateButton(hwnd);
        }
        break;
    case WM_LBUTTONDOWN:
        g_app.pressedButtonId = id;
        InvalidateButton(hwnd);
        break;
    case WM_LBUTTONUP:
    case WM_CANCELMODE:
        if (g_app.pressedButtonId == id) {
            g_app.pressedButtonId = 0;
            InvalidateButton(hwnd);
        }
        break;
    case WM_ENABLE:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateButton(hwnd);
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, ButtonSubclassProc, kButtonSubclassId);
        break;
    default:
        break;
    }

    return DefSubclassProc(hwnd, message, wParam, lParam);
}

void InstallButtonSubclass(const HWND hwnd) {
    if (hwnd != nullptr) {
        SetWindowSubclass(hwnd, ButtonSubclassProc, kButtonSubclassId, 0);
    }
}

COLORREF StatusAccentColor() {
    switch (g_app.state) {
    case RunState::Waiting:
        return CurrentPalette().accent;
    case RunState::Moving:
        return CurrentPalette().danger;
    case RunState::Stopped:
    default:
        return CurrentPalette().mutedText;
    }
}

void DrawPanelTitle(const HDC dc, const RECT& panel, const wchar_t* title) {
    RECT titleRect = panel;
    titleRect.left += 18;
    titleRect.top += 12;
    titleRect.bottom = titleRect.top + 24;
    titleRect.right -= 18;

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, TextColor());
    HGDIOBJ oldFont = SelectObject(dc, g_app.titleFont);
    DrawTextW(dc, title, -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, oldFont);
}

void DrawPanel(const HDC dc, const RECT& rect, const wchar_t* title) {
    DrawRoundedRect(dc, rect, kPanelRadius, SurfaceColor(), BorderColor());
    if (title != nullptr && title[0] != L'\0') {
        DrawPanelTitle(dc, rect, title);
    }
}

void DrawStatusPanel(const HDC dc, const RECT& rect) {
    DrawRoundedRect(dc, rect, kPanelRadius, SurfaceColor(), BorderColor());

    RECT indicator = MakeRect(rect.left + 18, rect.top + 18, 10, 10);
    HBRUSH brush = CreateSolidBrush(StatusAccentColor());
    HPEN pen = CreatePen(PS_SOLID, 1, StatusAccentColor());
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    Ellipse(dc, indicator.left, indicator.top, indicator.right, indicator.bottom);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void PaintWindow(const HDC dc) {
    RECT client = {};
    GetClientRect(g_app.hwnd, &client);
    FillRect(dc, &client, g_app.backgroundBrush);

    DrawPanel(dc, MakeRect(kScheduleX, kScheduleY, kScheduleW, kScheduleH), L"Schedule");
    DrawPanel(dc, MakeRect(kMovementX, kMovementY, kMovementW, kMovementH), L"Movement");
    DrawStatusPanel(dc, MakeRect(kStatusX, kStatusY, kStatusW, kStatusH));
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

HWND AddButton(const wchar_t* text, const int id, const int x, const int y, const int width, const int height) {
    HWND button = AddControl(L"BUTTON", text, WS_TABSTOP | BS_OWNERDRAW, 0, id, x, y, width, height);
    InstallButtonSubclass(button);
    return button;
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
    RECT statusRect = MakeRect(kStatusX, kStatusY, kStatusW, kStatusH);
    InvalidateRect(g_app.hwnd, &statusRect, FALSE);
    UpdateTrayTooltip();
}

void UpdateButtons() {
    const bool moving = g_app.state == RunState::Moving;
    EnableWindow(GetDlgItem(g_app.hwnd, IdStartButton), g_app.scheduleEnabled ? FALSE : TRUE);
    EnableWindow(GetDlgItem(g_app.hwnd, IdStopButton), g_app.state == RunState::Stopped ? FALSE : TRUE);
    EnableWindow(GetDlgItem(g_app.hwnd, IdRunNowButton), moving ? FALSE : TRUE);
    InvalidateButton(GetDlgItem(g_app.hwnd, IdStartButton));
    InvalidateButton(GetDlgItem(g_app.hwnd, IdStopButton));
    InvalidateButton(GetDlgItem(g_app.hwnd, IdRunNowButton));
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
    CreateUiFonts();

    AddButton(L"", IdThemeToggle, 432, 32, 32, 30);
    AddControl(L"STATIC", L"Interval (minutes)", 0, 0, -1, 38, 62, 138, 22);
    AddControl(L"EDIT", L"", WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, IdIntervalEdit, 190, 58, 78, 25);
    AddControl(L"STATIC", L"1 - 1440", 0, 0, -1, 284, 62, 90, 22);
    AddControl(L"STATIC", L"Duration (seconds)", 0, 0, -1, 38, 98, 138, 22);
    AddControl(L"EDIT", L"", WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, IdDurationEdit, 190, 94, 78, 25);
    AddControl(L"STATIC", L"5 - 10", 0, 0, -1, 284, 98, 90, 22);

    AddControl(L"STATIC", L"Pattern", 0, 0, -1, 38, 192, 138, 22);
    HWND combo = AddControl(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST, 0, IdPatternCombo, 190, 188, 190, 120);
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Subtle jiggle"));
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Visible sweep"));
    AddControl(L"BUTTON", L"Return to start after each cycle", WS_TABSTOP | BS_AUTOCHECKBOX, 0, IdReturnToStart, 190, 224, 250, 24);
    AddControl(L"STATIC", L"Distance / radius (px)", 0, 0, -1, 38, 258, 142, 22);
    AddControl(L"EDIT", L"", WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, IdDistanceEdit, 190, 254, 78, 25);
    AddControl(L"STATIC", L"Step speed (ms)", 0, 0, -1, 286, 258, 98, 22);
    AddControl(L"EDIT", L"", WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, IdStepEdit, 396, 254, 68, 25);

    AddControl(L"STATIC", L"Stopped. Configure settings, then Start or Run Now.", SS_CENTERIMAGE, 0, IdStatusText, 58, 318, 404, 24);
    AddButton(L"Save Settings", IdSaveButton, 20, 372, 112, 34);
    AddButton(L"Start", IdStartButton, 140, 372, 76, 34);
    AddButton(L"Stop", IdStopButton, 224, 372, 76, 34);
    AddButton(L"Run Now", IdRunNowButton, 308, 372, 88, 34);
    AddButton(L"Hide", IdHideButton, 404, 372, 76, 34);

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
    case WM_PAINT: {
        PAINTSTRUCT paint = {};
        HDC dc = BeginPaint(hwnd, &paint);
        PaintWindow(dc);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_CTLCOLORDLG:
        return reinterpret_cast<LRESULT>(g_app.backgroundBrush);
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        return reinterpret_cast<LRESULT>(ApplySurfaceControlColors(reinterpret_cast<HDC>(wParam)));
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return reinterpret_cast<LRESULT>(ApplyEditControlColors(reinterpret_cast<HDC>(wParam)));
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (item != nullptr && item->CtlType == ODT_BUTTON) {
            DrawOwnerButton(*item);
            return TRUE;
        }
        break;
    }
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
        DeleteUiFonts();
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
    constexpr DWORD windowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT rect = { 0, 0, kClientWidth, kClientHeight };
    AdjustWindowRectEx(&rect, windowStyle, FALSE, 0);

    return CreateWindowExW(
        0,
        kWindowClassName,
        kAppName,
        windowStyle,
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
