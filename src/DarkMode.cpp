#include "DarkMode.h"

#include <dwmapi.h>
#include <uxtheme.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

namespace {

// Undocumented uxtheme entry points. Their numbers have been stable since
// Windows 10 1809 and are still exported by Windows 11.
enum UxthemeOrdinal {
    kAllowDarkModeForWindow = 104,
    kRefreshImmersiveColorPolicyState = 132,
    kSetPreferredAppMode = 135,
    kFlushMenuThemes = 136,
};

enum PreferredAppMode {
    kAppModeDefault = 0,
    kAppModeAllowDark = 1,
    kAppModeForceDark = 2,
    kAppModeForceLight = 3,
};

using SetPreferredAppModeFn = int(WINAPI*)(int);
using AllowDarkModeForWindowFn = BOOL(WINAPI*)(HWND, BOOL);
using FlushMenuThemesFn = void(WINAPI*)();
using RefreshImmersiveColorPolicyStateFn = void(WINAPI*)();

SetPreferredAppModeFn g_setPreferredAppMode = nullptr;
AllowDarkModeForWindowFn g_allowDarkModeForWindow = nullptr;
FlushMenuThemesFn g_flushMenuThemes = nullptr;
RefreshImmersiveColorPolicyStateFn g_refreshColorPolicy = nullptr;

bool g_dark = false;
HBRUSH g_darkBrush = nullptr;

constexpr COLORREF kDarkBackground = RGB(32, 32, 32);
constexpr COLORREF kDarkText = RGB(240, 240, 240);

bool ReadSystemDarkMode()
{
    DWORD lightTheme = 1;
    DWORD size = sizeof(lightTheme);
    HKEY key = nullptr;

    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, nullptr,
            reinterpret_cast<LPBYTE>(&lightTheme), &size);
        RegCloseKey(key);
    }

    return lightTheme == 0;
}

void ApplyProcessMode()
{
    if (g_setPreferredAppMode)
        g_setPreferredAppMode(g_dark ? kAppModeForceDark : kAppModeDefault);
    if (g_flushMenuThemes)
        g_flushMenuThemes();
    if (g_refreshColorPolicy)
        g_refreshColorPolicy();
}

template <typename T>
T LoadOrdinal(HMODULE module, int ordinal)
{
    return reinterpret_cast<T>(GetProcAddress(module, MAKEINTRESOURCEA(ordinal)));
}

void SetImmersiveTitleBar(HWND hWnd)
{
    BOOL dark = g_dark ? TRUE : FALSE;

    // Attribute 20 is current; 19 is the Windows 10 1809-1903 spelling.
    if (FAILED(DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
            &dark, sizeof(dark)))) {
        DwmSetWindowAttribute(hWnd, 19, &dark, sizeof(dark));
    }
}

} // namespace

namespace darkmode {

bool IsDark()
{
    return g_dark;
}

void Initialize()
{
    HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (uxtheme) {
        g_setPreferredAppMode = LoadOrdinal<SetPreferredAppModeFn>(uxtheme, kSetPreferredAppMode);
        g_allowDarkModeForWindow = LoadOrdinal<AllowDarkModeForWindowFn>(uxtheme, kAllowDarkModeForWindow);
        g_flushMenuThemes = LoadOrdinal<FlushMenuThemesFn>(uxtheme, kFlushMenuThemes);
        g_refreshColorPolicy = LoadOrdinal<RefreshImmersiveColorPolicyStateFn>(
            uxtheme, kRefreshImmersiveColorPolicyState);
    }

    g_dark = ReadSystemDarkMode();
    g_darkBrush = CreateSolidBrush(kDarkBackground);

    // Must happen before the first window is created, otherwise the controls
    // are already themed light and will not switch.
    ApplyProcessMode();
}

bool OnSystemThemeChanged()
{
    const bool dark = ReadSystemDarkMode();
    if (dark == g_dark)
        return false;

    g_dark = dark;
    ApplyProcessMode();
    return true;
}

void ApplyToWindow(HWND hWnd)
{
    if (!hWnd)
        return;

    SetImmersiveTitleBar(hWnd);

    if (g_allowDarkModeForWindow)
        g_allowDarkModeForWindow(hWnd, g_dark);
}

void ApplyToControl(HWND hWnd, const wchar_t* theme)
{
    if (!hWnd)
        return;

    if (g_allowDarkModeForWindow)
        g_allowDarkModeForWindow(hWnd, g_dark);

    SetWindowTheme(hWnd, g_dark ? theme : L"", nullptr);
}

COLORREF BackgroundColor()
{
    return g_dark ? kDarkBackground : GetSysColor(COLOR_WINDOW);
}

COLORREF TextColor()
{
    return g_dark ? kDarkText : GetSysColor(COLOR_WINDOWTEXT);
}

HBRUSH BackgroundBrush()
{
    if (g_dark && g_darkBrush)
        return g_darkBrush;
    return GetSysColorBrush(COLOR_WINDOW);
}

} // namespace darkmode
