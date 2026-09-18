// =============================================================================
// InputCapture.cpp — Input capture implementation
// =============================================================================

#include "Input/InputCapture.h"
#include <Windows.h>

void InputCapture::normalizeCoords(int wx, int wy, int ww, int wh,
                                    float& outX, float& outY) {
    // Clamp to window bounds
    if (ww <= 0 || wh <= 0) { outX = outY = 0; return; }
    float nx = (float)wx / (float)ww;
    float ny = (float)wy / (float)wh;
    outX = (nx < 0) ? 0 : (nx > 1.0f) ? 1.0f : nx;
    outY = (ny < 0) ? 0 : (ny > 1.0f) ? 1.0f : ny;
}

uint32_t InputCapture::getModifiers() const {
    uint32_t mods = exdp::MOD_NONE;
    if (m_shiftDown) mods |= exdp::MOD_SHIFT;
    if (m_ctrlDown)  mods |= exdp::MOD_CTRL;
    if (m_altDown)   mods |= exdp::MOD_ALT;
    if (m_winDown)   mods |= exdp::MOD_WIN;
    return mods;
}

void InputCapture::sendEvent(exdp::InputEventPayload& evt) {
    evt.modifiers = getModifiers();
    if (m_sendCallback) m_sendCallback(evt);
}

void InputCapture::onMouseMove(int windowX, int windowY, int windowW, int windowH) {
    if (!m_enabled) return;

    // Throttle mouse move events
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        now - m_lastMouseMove).count();
    if (elapsed < MOUSE_MOVE_INTERVAL_US) return;
    m_lastMouseMove = now;

    exdp::InputEventPayload evt;
    evt.eventType = (uint8_t)exdp::InputEventType::MouseMove;
    normalizeCoords(windowX, windowY, windowW, windowH, evt.x, evt.y);
    sendEvent(evt);
}

void InputCapture::onMouseDown(int button, int windowX, int windowY,
                                int windowW, int windowH) {
    if (!m_enabled) return;
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
