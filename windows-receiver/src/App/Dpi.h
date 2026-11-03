#pragma once
// =============================================================================
// Dpi.h — Real pixels on scaled laptop screens (docs/SMOOTHNESS.md, step 3)
// =============================================================================
// Without DPI awareness Windows renders the receiver at a lower resolution and
// stretches it: on a 1920×1080 panel at 150 % the picture is drawn at
// 1280×720. That is blurry, the compositor has to scale every frame, and
// frames cannot go straight to the screen ("independent flip"). Per-monitor
// DPI awareness gives the swap chain the panel's real pixels; the status card,
// overlay and pairing dialog are scaled up by the receiver itself.
// =============================================================================

#include <Windows.h>

/// Call first in WinMain, before any window exists. Returns the mode in effect.
const char* EnablePerMonitorDpi();

/// DPI of the monitor the window is on (96 = 100 %); the system DPI for nullptr
UINT DpiForWindow(HWND hwnd);

/// A length at 96 DPI, in pixels at `dpi`
inline int ScaleForDpi(int value, UINT dpi) { return MulDiv(value, static_cast<int>(dpi), 96); }

/// Windows' message font at `dpi` (dialogs drawn by the receiver itself)
LOGFONTW MessageFontForDpi(UINT dpi);

/// AdjustWindowRectEx with the frame sizes of `dpi`
void AdjustWindowRectForDpi(RECT& rect, DWORD style, DWORD exStyle, UINT dpi);
