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

    // ── Accessors ───────────────────────────────────────────────────────────
    HWND GetHandle() const       { return hwnd_; }
    int  GetWidth() const        { return width_; }
    int  GetHeight() const       { return height_; }
    bool IsFullscreen() const    { return fullscreen_; }
    bool WasResized() const      { return resized_; }
    void ClearResizedFlag()      { resized_ = false; }

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

    // Video aspect ratio for resize constraint (0 = unconstrained)
    float videoAspect_ = 0.0f;

    // Saved windowed position for toggle
    RECT savedWindowRect_ = {};

    static constexpr const wchar_t* kClassName = L"ExternalDisplayReceiverClass";
};
