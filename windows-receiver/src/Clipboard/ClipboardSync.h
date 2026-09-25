// =============================================================================
// ClipboardSync.h — Bidirectional clipboard synchronization (Windows)
// =============================================================================
// Monitors the Windows clipboard for text changes and provides callbacks.
// Receives clipboard text from the Mac and sets it locally.
//
// Anti-loop protection prevents infinite clipboard bouncing.
// =============================================================================

#pragma once

#include <string>
#include <functional>
#include <atomic>
#include <mutex>
#include <thread>
#include <cstdint>
#include <windows.h>

class ClipboardSync {
public:
    /// Called when local clipboard text changes — provides UTF-8 text
    using ClipboardCallback = std::function<void(const std::string& utf8Text)>;

    ClipboardSync() = default;
    ~ClipboardSync();

    /// Start monitoring clipboard (creates hidden window for clipboard notifications)
    void start(HINSTANCE hInstance);

    /// Stop monitoring
    void stop();

    /// Set callback for local clipboard changes
    void setClipboardCallback(ClipboardCallback cb) { m_callback = std::move(cb); }

    /// Receive clipboard text from Mac — sets it on the Windows clipboard
    void receiveRemoteClipboard(const std::string& utf8Text);

    /// Poll from main thread (processes clipboard window messages)
    void poll();

    bool isRunning() const { return m_running.load(); }

private:
    /// Hidden window procedure for clipboard notifications
    static LRESULT CALLBACK clipboardWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    /// Handle clipboard update notification
    void onClipboardUpdate();

    /// Get current clipboard text as UTF-8
    std::string getClipboardTextUTF8();

    /// Set clipboard text from UTF-8 string
    void setClipboardTextUTF8(const std::string& text);

    // ── State ───────────────────────────────────────────────────────────────

    std::atomic<bool> m_running{false};
    ClipboardCallback m_callback;

    HWND m_clipboardWindow = nullptr;

    /// Hash of last content we SET (from remote) — anti-loop
    size_t m_lastRemoteHash = 0;

    /// Hash of last content we SENT to remote — dedup
    size_t m_lastSentHash = 0;

    /// Max text size to sync (1 MB)
    static constexpr size_t MAX_TEXT_SIZE = 1'000'000;
};
