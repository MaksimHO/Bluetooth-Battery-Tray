#pragma once
#include <windows.h>

// Dark mode support for the device list window.
//
// The colors themselves are trivial, but native controls (buttons, list view,
// scroll bars, menus) only switch to their dark variants when the process opts
// in through uxtheme's undocumented ordinals — the same ones Explorer uses.
// All of them are looked up dynamically: if a future Windows build removes
// them, the window simply stays light instead of failing to start.
namespace darkmode {

// Reads the "app mode" from the personalization settings, so the window
// follows the same switch as Settings and Explorer.
bool IsDark();

// Call once before creating any window or common control.
void Initialize();

// Re-reads the system setting after WM_SETTINGCHANGE ("ImmersiveColorSet").
// Returns true when the effective mode actually changed.
bool OnSystemThemeChanged();

void ApplyToWindow(HWND hWnd);
void ApplyToControl(HWND hWnd, const wchar_t* theme = L"DarkMode_Explorer");

COLORREF BackgroundColor();
COLORREF TextColor();
HBRUSH BackgroundBrush();

} // namespace darkmode
