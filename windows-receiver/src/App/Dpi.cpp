// =============================================================================
// Dpi.cpp — Real pixels on scaled laptop screens
// =============================================================================
// The APIs are looked up at run time: they arrived in Windows 8.1 and 10.

#include "App/Dpi.h"

namespace {

template <typename Fn>
Fn Lookup(HMODULE module, const char* name) {
    if (!module) return nullptr;
    // Through a generic function pointer: GCC warns on a direct FARPROC cast
    return reinterpret_cast<Fn>(reinterpret_cast<void (*)()>(GetProcAddress(module, name)));
}

} // namespace

const char* EnablePerMonitorDpi() {
    // Windows 10 1703+: per-monitor v2 also scales the title bar and dialogs'
    // non-client parts when a window moves to a monitor with another scale
    using SetContext = BOOL(WINAPI*)(HANDLE);
    if (auto set = Lookup<SetContext>(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")) {
        if (set(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)))) return "per-monitor v2";   // ..._PER_MONITOR_AWARE_V2
        if (set(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-3)))) return "per-monitor";      // ..._PER_MONITOR_AWARE
    }
    // Windows 8.1
    using SetAwareness = HRESULT(WINAPI*)(int);
    if (auto set = Lookup<SetAwareness>(LoadLibraryW(L"shcore.dll"), "SetProcessDpiAwareness")) {
        if (SUCCEEDED(set(2))) return "per-monitor (Windows 8.1)";                            // PROCESS_PER_MONITOR_DPI_AWARE
    }
    return SetProcessDPIAware() ? "system" : "not DPI-aware";
}

LOGFONTW MessageFontForDpi(UINT dpi) {
    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    // Windows 10 1607+: the metrics of any DPI
    using ForDpi = BOOL(WINAPI*)(UINT, UINT, PVOID, UINT, UINT);
    static const ForDpi forDpi = Lookup<ForDpi>(GetModuleHandleW(L"user32.dll"), "SystemParametersInfoForDpi");
    if (forDpi && forDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi)) return ncm.lfMessageFont;
    // Otherwise the system DPI's metrics, scaled
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    const UINT systemDpi = DpiForWindow(nullptr);
    if (systemDpi != dpi) ncm.lfMessageFont.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, static_cast<int>(dpi),
                                                             static_cast<int>(systemDpi));
    return ncm.lfMessageFont;
}

void AdjustWindowRectForDpi(RECT& rect, DWORD style, DWORD exStyle, UINT dpi) {
    using ForDpi = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static const ForDpi forDpi = Lookup<ForDpi>(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi");
    if (!forDpi || !forDpi(&rect, style, FALSE, exStyle, dpi)) AdjustWindowRectEx(&rect, style, FALSE, exStyle);
}

UINT DpiForWindow(HWND hwnd) {
    using GetDpi = UINT(WINAPI*)(HWND);
    static const GetDpi getDpi = Lookup<GetDpi>(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (hwnd && getDpi) {
        UINT dpi = getDpi(hwnd);
        if (dpi) return dpi;
    }
    HDC screen = GetDC(nullptr);
    int dpi = screen ? GetDeviceCaps(screen, LOGPIXELSX) : 0;
    if (screen) ReleaseDC(nullptr, screen);
    return dpi > 0 ? static_cast<UINT>(dpi) : 96;
}

UINT RefreshHzForWindow(HWND hwnd) {
    HMONITOR monitor = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY)
                            : MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW info = {};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return 0;
    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) return 0;
    // 0 and 1 mean "the hardware's default": unknown
    return mode.dmDisplayFrequency > 1 ? mode.dmDisplayFrequency : 0;
}
