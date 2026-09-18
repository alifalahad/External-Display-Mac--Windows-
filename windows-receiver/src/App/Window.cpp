// =============================================================================
// Window.cpp — Win32 borderless fullscreen window implementation
// =============================================================================

#include "App/Window.h"
#include "Input/InputCapture.h"
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

    // ── Aspect ratio lock during resize ─────────────────────────────────
    case WM_SIZING:
        if (self && self->videoAspect_ > 0.0f && !self->fullscreen_) {
            auto* rect = reinterpret_cast<RECT*>(lp);

            // Get window frame size (difference between window and client area)
            RECT clientRect;
            GetClientRect(hwnd, &clientRect);
            RECT windowRect;
            GetWindowRect(hwnd, &windowRect);
            int frameW = (windowRect.right - windowRect.left) - (clientRect.right - clientRect.left);
            int frameH = (windowRect.bottom - windowRect.top) - (clientRect.bottom - clientRect.top);

            // Current client size from the proposed rect
            int clientW = (rect->right - rect->left) - frameW;
            int clientH = (rect->bottom - rect->top) - frameH;
            if (clientW < 1) clientW = 1;
            if (clientH < 1) clientH = 1;

            float aspect = self->videoAspect_;

            // Adjust based on which edge is being dragged
            switch (wp) {
            case WMSZ_LEFT:
            case WMSZ_RIGHT:
                // Horizontal drag → adjust height to match
                clientH = (int)(clientW / aspect + 0.5f);
                rect->bottom = rect->top + clientH + frameH;
                break;
            case WMSZ_TOP:
            case WMSZ_BOTTOM:
                // Vertical drag → adjust width to match
                clientW = (int)(clientH * aspect + 0.5f);
                rect->right = rect->left + clientW + frameW;
                break;
            case WMSZ_TOPLEFT:
            case WMSZ_TOPRIGHT:
            case WMSZ_BOTTOMLEFT:
            case WMSZ_BOTTOMRIGHT:
            default:
                // Corner drag → use the larger dimension as anchor
                if ((float)clientW / (float)clientH > aspect) {
                    clientH = (int)(clientW / aspect + 0.5f);
                } else {
                    clientW = (int)(clientH * aspect + 0.5f);
                }
                // Adjust the appropriate edges
                if (wp == WMSZ_TOPLEFT || wp == WMSZ_BOTTOMLEFT) {
                    rect->left = rect->right - clientW - frameW;
                } else {
                    rect->right = rect->left + clientW + frameW;
                }
                if (wp == WMSZ_TOPLEFT || wp == WMSZ_TOPRIGHT) {
                    rect->top = rect->bottom - clientH - frameH;
                } else {
                    rect->bottom = rect->top + clientH + frameH;
                }
                break;
            }
            return TRUE;
        }
        break;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        if (wp == VK_F11 && self) {
            self->ToggleFullscreen();
            return 0;
        }
        if (wp == VK_F2 && self) {
            self->showStats_ = !self->showStats_;
            return 0;
        }
        if (wp == VK_F3 && self && self->inputCapture_) {
            bool newState = !self->inputCapture_->isEnabled();
            self->inputCapture_->setEnabled(newState);
            printf("[Input] %s\n", newState ? "ENABLED (F3)" : "DISABLED (F3)");
            if (newState) {
                // Hide cursor when input capture is active
                ShowCursor(FALSE);
            } else {
                ShowCursor(TRUE);
            }
            return 0;
        }
        // Forward to input capture
        if (self && self->inputCapture_) {
            self->inputCapture_->onKeyDown((uint16_t)wp);
        }
        break;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (self && self->inputCapture_) {
            self->inputCapture_->onKeyUp((uint16_t)wp);
        }
        break;

    // ── Mouse input forwarding ────────────────────────────────────────────
    case WM_MOUSEMOVE:
        if (self && self->inputCapture_) {
            self->inputCapture_->onMouseMove(
                LOWORD(lp), HIWORD(lp), self->width_, self->height_);
        }
        break;

    case WM_LBUTTONDOWN:
        if (self && self->inputCapture_) {
            self->inputCapture_->onMouseDown(
                0, LOWORD(lp), HIWORD(lp), self->width_, self->height_);
        }
        break;
    case WM_LBUTTONUP:
        if (self && self->inputCapture_) {
            self->inputCapture_->onMouseUp(
                0, LOWORD(lp), HIWORD(lp), self->width_, self->height_);
        }
        break;
    case WM_RBUTTONDOWN:
        if (self && self->inputCapture_) {
            self->inputCapture_->onMouseDown(
                1, LOWORD(lp), HIWORD(lp), self->width_, self->height_);
        }
        break;
    case WM_RBUTTONUP:
        if (self && self->inputCapture_) {
            self->inputCapture_->onMouseUp(
                1, LOWORD(lp), HIWORD(lp), self->width_, self->height_);
        }
        break;
    case WM_MBUTTONDOWN:
        if (self && self->inputCapture_) {
            self->inputCapture_->onMouseDown(
                2, LOWORD(lp), HIWORD(lp), self->width_, self->height_);
        }
        break;
    case WM_MBUTTONUP:
        if (self && self->inputCapture_) {
            self->inputCapture_->onMouseUp(
                2, LOWORD(lp), HIWORD(lp), self->width_, self->height_);
        }
        break;
    case WM_MOUSEWHEEL:
        if (self && self->inputCapture_) {
            float delta = (float)GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA;
            POINT pt = { LOWORD(lp), HIWORD(lp) };
            ScreenToClient(hwnd, &pt);
            self->inputCapture_->onMouseWheel(
                0, delta, pt.x, pt.y, self->width_, self->height_);
        }
        break;
    case WM_MOUSEHWHEEL:
        if (self && self->inputCapture_) {
            float delta = (float)GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA;
            POINT pt = { LOWORD(lp), HIWORD(lp) };
            ScreenToClient(hwnd, &pt);
            self->inputCapture_->onMouseWheel(
                delta, 0, pt.x, pt.y, self->width_, self->height_);
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}
