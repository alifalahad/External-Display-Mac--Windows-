// =============================================================================
// ClipboardSync.cpp — Bidirectional clipboard synchronization (Windows)
// =============================================================================

#include "ClipboardSync.h"
#include <iostream>
#include <functional>

// ── Lifecycle ───────────────────────────────────────────────────────────────

ClipboardSync::~ClipboardSync() {
    stop();
}

void ClipboardSync::start(HINSTANCE hInstance) {
    if (m_running.load()) return;

    // Create a hidden message-only window for clipboard notifications
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = clipboardWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"ExternalDisplayClipboardSync";
    RegisterClassExW(&wc);

    m_clipboardWindow = CreateWindowExW(
        0, L"ExternalDisplayClipboardSync", L"",
        0, 0, 0, 0, 0,
        HWND_MESSAGE,  // Message-only window (no visible UI)
        nullptr, hInstance, nullptr
    );

    if (!m_clipboardWindow) {
        std::cerr << "[Clipboard] Failed to create notification window" << std::endl;
        return;
    }

    // Store 'this' pointer for the window procedure
    SetWindowLongPtrW(m_clipboardWindow, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    // Register for clipboard change notifications
    if (!AddClipboardFormatListener(m_clipboardWindow)) {
        std::cerr << "[Clipboard] Failed to register clipboard listener: " << GetLastError() << std::endl;
        DestroyWindow(m_clipboardWindow);
        m_clipboardWindow = nullptr;
        return;
    }

    m_running.store(true);
    std::cout << "[Clipboard] Monitoring started" << std::endl;
}

void ClipboardSync::stop() {
    if (!m_running.load()) return;
    m_running.store(false);

    if (m_clipboardWindow) {
        RemoveClipboardFormatListener(m_clipboardWindow);
        DestroyWindow(m_clipboardWindow);
        m_clipboardWindow = nullptr;
    }

    std::cout << "[Clipboard] Monitoring stopped" << std::endl;
}

// ── Message Processing ──────────────────────────────────────────────────────

void ClipboardSync::poll() {
    if (!m_clipboardWindow) return;

    // Process pending messages for our hidden clipboard window
    MSG msg;
    while (PeekMessageW(&msg, m_clipboardWindow, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

LRESULT CALLBACK ClipboardSync::clipboardWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_CLIPBOARDUPDATE) {
        auto* self = reinterpret_cast<ClipboardSync*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self && self->m_running.load()) {
            self->onClipboardUpdate();
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ── Clipboard Change Detection ──────────────────────────────────────────────

void ClipboardSync::onClipboardUpdate() {
    // Check if clipboard contains text
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return;

    std::string text = getClipboardTextUTF8();
    if (text.empty()) return;

    // Size guard
    if (text.size() > MAX_TEXT_SIZE) {
        std::cout << "[Clipboard] Skipping oversized clipboard (" << text.size() << " bytes)" << std::endl;
        return;
    }

    size_t hash = std::hash<std::string>{}(text);

    // Anti-loop: skip if this is content we just received from remote
    if (hash == m_lastRemoteHash) return;

    // Dedup: skip if we already sent this
    if (hash == m_lastSentHash) return;

    m_lastSentHash = hash;

    if (m_callback) {
        m_callback(text);
        std::cout << "[Clipboard] -> Sending to Mac: "
                  << text.substr(0, 50) << (text.size() > 50 ? "..." : "")
                  << " (" << text.size() << " bytes)" << std::endl;
    }
}

// ── Receive from Mac ────────────────────────────────────────────────────────

void ClipboardSync::receiveRemoteClipboard(const std::string& utf8Text) {
    if (utf8Text.empty()) return;

    size_t hash = std::hash<std::string>{}(utf8Text);

    // Dedup: don't set if it's the same as what we just sent
    if (hash == m_lastSentHash) return;

    // Remember this hash so we don't echo it back
    m_lastRemoteHash = hash;

    setClipboardTextUTF8(utf8Text);

    std::cout << "[Clipboard] <- Received from Mac: "
              << utf8Text.substr(0, 50) << (utf8Text.size() > 50 ? "..." : "")
              << " (" << utf8Text.size() << " bytes)" << std::endl;
}

// ── Win32 Clipboard Helpers ─────────────────────────────────────────────────

std::string ClipboardSync::getClipboardTextUTF8() {
    if (!OpenClipboard(m_clipboardWindow)) return "";

    std::string result;
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (hData) {
        auto* wideText = static_cast<const wchar_t*>(GlobalLock(hData));
        if (wideText) {
            // Convert UTF-16 → UTF-8
            int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wideText, -1, nullptr, 0, nullptr, nullptr);
            if (utf8Len > 0) {
                result.resize(utf8Len - 1);  // -1 to exclude null terminator
                WideCharToMultiByte(CP_UTF8, 0, wideText, -1, result.data(), utf8Len, nullptr, nullptr);
            }
            GlobalUnlock(hData);
        }
    }

    CloseClipboard();
    return result;
}

void ClipboardSync::setClipboardTextUTF8(const std::string& text) {
    // Convert UTF-8 → UTF-16
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (wideLen <= 0) return;

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, wideLen * sizeof(wchar_t));
    if (!hMem) return;

    auto* wideText = static_cast<wchar_t*>(GlobalLock(hMem));
    if (wideText) {
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wideText, wideLen);
        GlobalUnlock(hMem);

        if (OpenClipboard(m_clipboardWindow)) {
            EmptyClipboard();
            SetClipboardData(CF_UNICODETEXT, hMem);
            CloseClipboard();
        } else {
            GlobalFree(hMem);
        }
    } else {
        GlobalFree(hMem);
    }
}
