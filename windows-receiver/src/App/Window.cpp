// =============================================================================
// Window.cpp — Win32 borderless fullscreen window implementation
// =============================================================================

#include "App/Window.h"
#include <stdexcept>

Window::Window(const wchar_t* title, bool fullscreen)
    : fullscreen_(fullscreen)
{
    RegisterWindowClass();

    if (fullscreen) {
        CreateFullscreenWindow(title);
    } else {
        CreateWindowedWindow(title);
    }

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
}

Window::~Window() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    UnregisterClassW(kClassName, GetModuleHandleW(nullptr));
}

void Window::RegisterWindowClass() {
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kClassName;

    if (!RegisterClassExW(&wc)) {
        throw std::runtime_error("Failed to register window class");
    }
}

void Window::CreateFullscreenWindow(const wchar_t* title) {
    // Get primary monitor dimensions
    HMONITOR hMonitor = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(hMonitor, &mi);

    width_  = mi.rcMonitor.right  - mi.rcMonitor.left;
    height_ = mi.rcMonitor.bottom - mi.rcMonitor.top;

    // Save a reasonable windowed rect for toggle
    savedWindowRect_ = {
        mi.rcMonitor.left + 100,
        mi.rcMonitor.top + 100,
        mi.rcMonitor.left + 100 + 1280,
        mi.rcMonitor.top + 100 + 720
    };

    // WS_POPUP = borderless, no title bar
    hwnd_ = CreateWindowExW(
        WS_EX_APPWINDOW,
        kClassName,
        title,
        WS_POPUP | WS_VISIBLE,
        mi.rcMonitor.left,
        mi.rcMonitor.top,
        width_,
        height_,
        nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        this  // Pass 'this' for GWLP_USERDATA setup in WM_NCCREATE
    );

    if (!hwnd_) {
        throw std::runtime_error("Failed to create fullscreen window");
    }
}

void Window::CreateWindowedWindow(const wchar_t* title) {
    width_  = 1280;
    height_ = 720;

    RECT rc = { 0, 0, width_, height_ };
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);

    int adjustedWidth  = rc.right - rc.left;
    int adjustedHeight = rc.bottom - rc.top;

    hwnd_ = CreateWindowExW(
        0,
        kClassName,
        title,
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        adjustedWidth,
        adjustedHeight,
        nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        this
    );

    if (!hwnd_) {
        throw std::runtime_error("Failed to create windowed window");
    }

    // Save windowed rect for toggle
    GetWindowRect(hwnd_, &savedWindowRect_);
}

bool Window::ProcessMessages() {
    MSG msg = {};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            return false;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}

void Window::ToggleFullscreen() {
    if (fullscreen_) {
        // Switch to windowed
        SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(hwnd_, HWND_NOTOPMOST,
            savedWindowRect_.left,
            savedWindowRect_.top,
            savedWindowRect_.right  - savedWindowRect_.left,
            savedWindowRect_.bottom - savedWindowRect_.top,
            SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        fullscreen_ = false;
    } else {
        // Save current windowed position
        GetWindowRect(hwnd_, &savedWindowRect_);

        // Switch to fullscreen
        HMONITOR hMonitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(hMonitor, &mi);

        SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd_, HWND_TOP,
            mi.rcMonitor.left,
            mi.rcMonitor.top,
            mi.rcMonitor.right  - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top,
            SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        fullscreen_ = true;
    }
}

LRESULT CALLBACK Window::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Window* self = nullptr;

    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<Window*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    switch (msg) {
    case WM_SIZE:
        if (self && wp != SIZE_MINIMIZED) {
            int newWidth  = LOWORD(lp);
            int newHeight = HIWORD(lp);
            if (newWidth > 0 && newHeight > 0 &&
                (newWidth != self->width_ || newHeight != self->height_)) {
                self->width_   = newWidth;
                self->height_  = newHeight;
                self->resized_ = true;
            }
        }
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        if (wp == VK_F11 && self) {
            self->ToggleFullscreen();
            return 0;
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}
