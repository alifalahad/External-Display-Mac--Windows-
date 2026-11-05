#pragma once
// =============================================================================
// Window.h — Win32 borderless fullscreen window with toggle support
// =============================================================================
// Creates a borderless popup window covering the entire primary monitor.
// Supports F11 to toggle between fullscreen and windowed mode.
// F2 to toggle stats overlay. ESC to exit.
// Aspect ratio locking: when a video aspect ratio is set, window resize
// is constrained to maintain that ratio.
// =============================================================================

#include <Windows.h>
#include <cstdint>

class InputCapture;  // Forward declaration

class Window {
public:
    // Creates the window. If fullscreen=true, covers the primary monitor.
    Window(const wchar_t* title, bool fullscreen = true);
    ~Window();

    // Non-copyable, non-movable
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Process Win32 messages. Returns false when the window should close.
    bool ProcessMessages();

    // Toggle between fullscreen and windowed mode
    void ToggleFullscreen();

    // ── Aspect Ratio Lock ───────────────────────────────────────────────────
    // Set to lock resize to video's aspect ratio. Set to 0 to unlock.
    void SetVideoAspectRatio(float ratio) { videoAspect_ = ratio; }
    float GetVideoAspectRatio() const     { return videoAspect_; }

    // ── Stats Toggle ────────────────────────────────────────────────────────
    bool ShowStats() const { return showStats_; }

    // ── Input Capture ──────────────────────────────────────────────────────────
    void SetInputCapture(InputCapture* ic) { inputCapture_ = ic; }

    // ── Accessors ───────────────────────────────────────────────────────────
    HWND GetHandle() const       { return hwnd_; }
    int  GetWidth() const        { return width_; }
    int  GetHeight() const       { return height_; }
    bool IsFullscreen() const    { return fullscreen_; }
    bool WasResized() const      { return resized_; }
    void ClearResizedFlag()      { resized_ = false; }

    // True once after the system resumes from sleep (Phase 15: reconnect immediately)
    bool TakeResumeEvent()       { bool r = resumed_; resumed_ = false; return r; }

    // Last number key 1-9 pressed (0 = none), cleared on read — picks a Mac
    int TakeDigitKey()           { int d = digitKey_; digitKey_ = 0; return d; }

    // ── Pointer (docs/SMOOTHNESS.md, step 1) ──────────────────────────────────
    // Over the client area: `cursor`, or Windows' arrow when nullptr; nothing
    // when hide. Applied at once if the pointer is over the window.
    void SetClientCursor(HCURSOR cursor, bool hide);
    // The mouse moved over the client area since the last call (client pixels)
    bool TakeMouseMove(int& x, int& y) {
        if (!mouseMoved_) return false;
        mouseMoved_ = false;
        x = mouseX_;
        y = mouseY_;
        return true;
    }
    // F4 pressed: pointer drawn here ↔ pointer in the video
    bool TakePointerToggle()     { bool t = pointerToggle_; pointerToggle_ = false; return t; }

    // ── Display path (docs/SMOOTHNESS.md, step 3) ──────────────────────────────
    // DPI of the window's monitor (96 = 100 %): the receiver is per-monitor
    // DPI-aware, so sizes are real pixels and the UI scales itself
    UINT GetDpi() const          { return dpi_; }
    // F5 pressed: present on the next refresh ↔ at once
    bool TakePresentToggle()     { bool t = presentToggle_; presentToggle_ = false; return t; }

    // ── 120 Hz (docs/SMOOTHNESS.md, step 7) ─────────────────────────────────────
    // True once after the window moved to another monitor or a display's mode
    // changed: its refresh rate may be different
    bool TakeScreenChange()      { bool c = screenChange_; screenChange_ = false; return c; }

private:
    void RegisterWindowClass();
    void CreateFullscreenWindow(const wchar_t* title);
    void CreateWindowedWindow(const wchar_t* title);

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    HWND hwnd_       = nullptr;
    int  width_      = 0;
    int  height_     = 0;
    bool fullscreen_ = true;
    bool resized_    = false;
    bool showStats_  = true;
    bool resumed_    = false;
    int  digitKey_   = 0;

    // Pointer over the client area
    HCURSOR CurrentClientCursor() const;
    HCURSOR clientCursor_ = nullptr;   // not owned
    bool hideCursor_      = false;
    bool mouseMoved_      = false;
    int  mouseX_ = -1, mouseY_ = -1;
    bool pointerToggle_   = false;

    UINT dpi_             = 96;
    bool presentToggle_   = false;

    HMONITOR monitor_     = nullptr;   // the window's monitor at the last move
    bool screenChange_    = false;

    // Video aspect ratio for resize constraint (0 = unconstrained)
    float videoAspect_ = 0.0f;

    // Input capture (non-owning)
    InputCapture* inputCapture_ = nullptr;

    // Saved windowed position for toggle
    RECT savedWindowRect_ = {};

    static constexpr const wchar_t* kClassName = L"ExternalDisplayReceiverClass";
};
