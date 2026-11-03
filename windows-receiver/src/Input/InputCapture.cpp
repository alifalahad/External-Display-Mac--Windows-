// =============================================================================
// InputCapture.cpp — Input capture implementation
// =============================================================================

// Windows.h must be included first to avoid macro conflicts
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "Input/InputCapture.h"
#include "Input/VideoViewport.h"

void InputCapture::normalizeCoords(int wx, int wy, int ww, int wh,
                                    float& outX, float& outY) {
    // Through the same letterbox/pillarbox rectangle the renderer draws into
    WindowToVideo(wx, wy, ww, wh, m_videoAspect, outX, outY);
}

uint32_t InputCapture::getModifiers() const {
    uint32_t mods = exdp::INPUT_MOD_NONE;
    if (m_shiftDown) mods |= exdp::INPUT_MOD_SHIFT;
    if (m_ctrlDown)  mods |= exdp::INPUT_MOD_CTRL;
    if (m_altDown)   mods |= exdp::INPUT_MOD_ALT;
    if (m_winDown)   mods |= exdp::INPUT_MOD_WIN;
    return mods;
}

void InputCapture::sendEvent(exdp::InputEventPayload& evt) {
    evt.modifiers = getModifiers();
    if (m_sendCallback) m_sendCallback(evt);
}

void InputCapture::onMouseMove(int windowX, int windowY, int windowW, int windowH) {
    if (!m_enabled) return;

    // Throttle mouse move events; the latest held-back one is sent later
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        now - m_lastMouseMove).count();
    if (elapsed < MOUSE_MOVE_INTERVAL_US) {
        m_movePending = true;
        m_pendingX = windowX;
        m_pendingY = windowY;
        m_pendingW = windowW;
        m_pendingH = windowH;
        return;
    }
    m_lastMouseMove = now;
    m_movePending = false;

    exdp::InputEventPayload evt;
    evt.eventType = (uint8_t)exdp::InputEventType::MouseMove;
    normalizeCoords(windowX, windowY, windowW, windowH, evt.x, evt.y);
    sendEvent(evt);
}

void InputCapture::flushPendingMove() {
    if (!m_movePending) return;
    if (!m_enabled) {
        m_movePending = false;
        return;
    }
    if (pendingMoveDueInUs() == 0) onMouseMove(m_pendingX, m_pendingY, m_pendingW, m_pendingH);
}

int64_t InputCapture::pendingMoveDueInUs() const {
    if (!m_movePending) return -1;
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - m_lastMouseMove).count();
    return elapsed >= MOUSE_MOVE_INTERVAL_US ? 0 : MOUSE_MOVE_INTERVAL_US - elapsed;
}

void InputCapture::onMouseDown(int button, int windowX, int windowY,
                                int windowW, int windowH) {
    if (!m_enabled) return;
    m_movePending = false;   // the click carries the position
    exdp::InputEventPayload evt;
    evt.eventType = (uint8_t)exdp::InputEventType::MouseDown;
    evt.button = (uint8_t)button;
    normalizeCoords(windowX, windowY, windowW, windowH, evt.x, evt.y);
    sendEvent(evt);
}

void InputCapture::onMouseUp(int button, int windowX, int windowY,
                              int windowW, int windowH) {
    if (!m_enabled) return;
    exdp::InputEventPayload evt;
    evt.eventType = (uint8_t)exdp::InputEventType::MouseUp;
    evt.button = (uint8_t)button;
    normalizeCoords(windowX, windowY, windowW, windowH, evt.x, evt.y);
    sendEvent(evt);
}

void InputCapture::onMouseWheel(float deltaX, float deltaY,
                                 int windowX, int windowY,
                                 int windowW, int windowH) {
    if (!m_enabled) return;
    exdp::InputEventPayload evt;
    evt.eventType = (uint8_t)exdp::InputEventType::Scroll;
    normalizeCoords(windowX, windowY, windowW, windowH, evt.x, evt.y);
    evt.scrollDeltaX = deltaX;
    evt.scrollDeltaY = deltaY;
    sendEvent(evt);
}

void InputCapture::onKeyDown(uint16_t vkCode) {
    if (!m_enabled) return;

    // Track modifier state
    if (vkCode == VK_SHIFT || vkCode == VK_LSHIFT || vkCode == VK_RSHIFT) m_shiftDown = true;
    if (vkCode == VK_CONTROL || vkCode == VK_LCONTROL || vkCode == VK_RCONTROL) m_ctrlDown = true;
    if (vkCode == VK_MENU || vkCode == VK_LMENU || vkCode == VK_RMENU) m_altDown = true;
    if (vkCode == VK_LWIN || vkCode == VK_RWIN) m_winDown = true;

    // Don't forward modifier-only keys as separate events (they're tracked in modifiers field)
    // But DO forward them so Mac can show modifier state
    exdp::InputEventPayload evt;
    evt.eventType = (uint8_t)exdp::InputEventType::KeyDown;
    evt.keyCode = vkCode;
    sendEvent(evt);
}

void InputCapture::onKeyUp(uint16_t vkCode) {
    if (!m_enabled) return;

    // Track modifier state
    if (vkCode == VK_SHIFT || vkCode == VK_LSHIFT || vkCode == VK_RSHIFT) m_shiftDown = false;
    if (vkCode == VK_CONTROL || vkCode == VK_LCONTROL || vkCode == VK_RCONTROL) m_ctrlDown = false;
    if (vkCode == VK_MENU || vkCode == VK_LMENU || vkCode == VK_RMENU) m_altDown = false;
    if (vkCode == VK_LWIN || vkCode == VK_RWIN) m_winDown = false;

    exdp::InputEventPayload evt;
    evt.eventType = (uint8_t)exdp::InputEventType::KeyUp;
    evt.keyCode = vkCode;
    sendEvent(evt);
}
