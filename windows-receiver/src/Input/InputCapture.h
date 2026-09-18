#pragma once
// =============================================================================
// InputCapture.h — Captures mouse/keyboard input and sends to Mac
// =============================================================================
// Converts Win32 input events to normalized InputEventPayload and sends
// them to the Mac sender via the StreamReceiver's TCP connection.
// =============================================================================

#include "Network/Protocol.h"
#include <functional>
#include <chrono>
#include <cstdint>

class InputCapture {
public:
    /// Called to send an input event payload over TCP
    using SendCallback = std::function<void(const exdp::InputEventPayload&)>;

    void setSendCallback(SendCallback cb) { m_sendCallback = std::move(cb); }

    /// Set the video dimensions for coordinate mapping
    void setVideoDimensions(uint32_t w, uint32_t h) { m_videoW = w; m_videoH = h; }

    /// Enable/disable input capture
    void setEnabled(bool enabled) { m_enabled = enabled; }
    bool isEnabled() const { return m_enabled; }

    // ── Input event handlers (called from WndProc) ──────────────────────────

    void onMouseMove(int windowX, int windowY, int windowW, int windowH);
    void onMouseDown(int button, int windowX, int windowY, int windowW, int windowH);
    void onMouseUp(int button, int windowX, int windowY, int windowW, int windowH);
    void onMouseWheel(float deltaX, float deltaY, int windowX, int windowY, int windowW, int windowH);
    void onKeyDown(uint16_t vkCode);
    void onKeyUp(uint16_t vkCode);

private:
    void sendEvent(exdp::InputEventPayload& evt);
    uint32_t getModifiers() const;

    // Normalize window coordinates to 0.0–1.0
    void normalizeCoords(int wx, int wy, int ww, int wh, float& outX, float& outY);

    SendCallback m_sendCallback;
    uint32_t m_videoW = 0, m_videoH = 0;
    bool m_enabled = false;

    // Mouse move throttle (max 120 events/sec)
    std::chrono::steady_clock::time_point m_lastMouseMove;
    static constexpr int MOUSE_MOVE_INTERVAL_US = 8333; // ~120 Hz

    // Modifier state tracking
    bool m_shiftDown = false;
    bool m_ctrlDown = false;
    bool m_altDown = false;
    bool m_winDown = false;
};
