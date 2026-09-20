// =============================================================================
// InputInjector.swift — Injects mouse/keyboard events via CGEvent
// =============================================================================
// Receives InputEventPayload from Windows receiver and creates corresponding
// macOS CGEvent objects to simulate input on the Mac.
//
// Coordinate mapping: Windows sends normalized (0.0–1.0) coords.
// We map these to the Mac display pixel coordinates.
//
// Key mapping: Windows VK codes → macOS kVK keycodes.
// Ctrl→Cmd remapping for natural Mac feel (Ctrl+C → Cmd+C).
// =============================================================================

import Foundation
import CoreGraphics
import Carbon.HIToolbox

final class InputInjector {

    // ── Configuration ───────────────────────────────────────────────────────

    /// The display to inject events to (main display by default)
    var displayWidth: CGFloat = 1920
    var displayHeight: CGFloat = 1200

    /// Display origin offset (for virtual display coordinate mapping)
    /// When a virtual display is positioned to the right of the main display,
    /// these values offset mouse events to the correct screen region.
    var displayOriginX: CGFloat = 0
    var displayOriginY: CGFloat = 0

    /// Track mouse position for delta-based events
    private var lastMousePos = CGPoint(x: 960, y: 600)

    /// Track button state for drag detection
    private var leftButtonDown = false
    private var rightButtonDown = false

    // ── Event Source ────────────────────────────────────────────────────────

    private let eventSource: CGEventSource?

    init() {
        eventSource = CGEventSource(stateID: .combinedSessionState)
        eventSource?.localEventsSuppressionInterval = 0.0
    }

    // ── Process Input Event ─────────────────────────────────────────────────

    func processEvent(_ payload: InputEventPayload) {
        guard let eventType = InputEventType(rawValue: payload.eventType) else {
            return
        }

        switch eventType {
        case .mouseMove:
            handleMouseMove(payload)
        case .mouseDown:
            handleMouseDown(payload)
        case .mouseUp:
            handleMouseUp(payload)
        case .scroll:
            handleScroll(payload)
        case .keyDown:
            handleKeyDown(payload)
        case .keyUp:
            handleKeyUp(payload)
        }
    }

    // ── Mouse Handling ──────────────────────────────────────────────────────

    private func handleMouseMove(_ p: InputEventPayload) {
        let point = normalizedToDisplay(x: p.x, y: p.y)
        lastMousePos = point

        let type: CGEventType = leftButtonDown ? .leftMouseDragged :
                                rightButtonDown ? .rightMouseDragged : .mouseMoved

        guard let event = CGEvent(mouseEventSource: eventSource,
                                   mouseType: type,
                                   mouseCursorPosition: point,
                                   mouseButton: .left) else { return }
        event.post(tap: .cghidEventTap)
    }

    private func handleMouseDown(_ p: InputEventPayload) {
        let point = normalizedToDisplay(x: p.x, y: p.y)
        lastMousePos = point

        let button = p.button
        if button == 0 {
            // Left click
            leftButtonDown = true
            guard let event = CGEvent(mouseEventSource: eventSource,
                                       mouseType: .leftMouseDown,
                                       mouseCursorPosition: point,
                                       mouseButton: .left) else { return }
            event.post(tap: .cghidEventTap)
        } else if button == 1 {
            // Right click
            rightButtonDown = true
            guard let event = CGEvent(mouseEventSource: eventSource,
                                       mouseType: .rightMouseDown,
                                       mouseCursorPosition: point,
                                       mouseButton: .right) else { return }
            event.post(tap: .cghidEventTap)
        } else if button == 2 {
            // Middle click
            guard let event = CGEvent(mouseEventSource: eventSource,
                                       mouseType: .otherMouseDown,
                                       mouseCursorPosition: point,
                                       mouseButton: .center) else { return }
            event.post(tap: .cghidEventTap)
        }
    }

    private func handleMouseUp(_ p: InputEventPayload) {
        let point = normalizedToDisplay(x: p.x, y: p.y)
        lastMousePos = point

        let button = p.button
        if button == 0 {
            leftButtonDown = false
            guard let event = CGEvent(mouseEventSource: eventSource,
                                       mouseType: .leftMouseUp,
                                       mouseCursorPosition: point,
                                       mouseButton: .left) else { return }
            event.post(tap: .cghidEventTap)
        } else if button == 1 {
            rightButtonDown = false
            guard let event = CGEvent(mouseEventSource: eventSource,
                                       mouseType: .rightMouseUp,
                                       mouseCursorPosition: point,
                                       mouseButton: .right) else { return }
            event.post(tap: .cghidEventTap)
        } else if button == 2 {
            guard let event = CGEvent(mouseEventSource: eventSource,
                                       mouseType: .otherMouseUp,
                                       mouseCursorPosition: point,
                                       mouseButton: .center) else { return }
            event.post(tap: .cghidEventTap)
        }
    }

    // ── Scroll Handling ─────────────────────────────────────────────────────

    private func handleScroll(_ p: InputEventPayload) {
        // CGEvent scroll uses integer values; multiply for finer control
        let scrollY = Int32(p.scrollDeltaY * 10)
        let scrollX = Int32(p.scrollDeltaX * 10)

        guard let event = CGEvent(scrollWheelEvent2Source: eventSource,
                                   units: .line,
                                   wheelCount: 2,
                                   wheel1: scrollY,
                                   wheel2: scrollX,
                                   wheel3: 0) else { return }
        event.post(tap: .cghidEventTap)
    }

    // ── Keyboard Handling ───────────────────────────────────────────────────

    private func handleKeyDown(_ p: InputEventPayload) {
        guard let macKeyCode = windowsVKToMacKeyCode(p.keyCode) else { return }

        guard let event = CGEvent(keyboardEventSource: eventSource,
                                   virtualKey: CGKeyCode(macKeyCode),
                                   keyDown: true) else { return }
        applyModifiers(event, modifiers: p.modifiers)
        event.post(tap: .cghidEventTap)
    }

    private func handleKeyUp(_ p: InputEventPayload) {
        guard let macKeyCode = windowsVKToMacKeyCode(p.keyCode) else { return }

        guard let event = CGEvent(keyboardEventSource: eventSource,
                                   virtualKey: CGKeyCode(macKeyCode),
                                   keyDown: false) else { return }
        applyModifiers(event, modifiers: p.modifiers)
        event.post(tap: .cghidEventTap)
    }

    private func applyModifiers(_ event: CGEvent, modifiers: UInt32) {
        var flags = CGEventFlags()

        // Windows Ctrl → Mac Cmd (natural mapping)
        if modifiers & InputModifiers.ctrl.rawValue != 0 {
            flags.insert(.maskCommand)
        }
        // Windows Shift → Mac Shift
        if modifiers & InputModifiers.shift.rawValue != 0 {
            flags.insert(.maskShift)
        }
        // Windows Alt → Mac Option
        if modifiers & InputModifiers.alt.rawValue != 0 {
            flags.insert(.maskAlternate)
        }
        // Windows Win → Mac Control
        if modifiers & InputModifiers.win.rawValue != 0 {
            flags.insert(.maskControl)
        }

        event.flags = flags
    }

    // ── Coordinate Mapping ──────────────────────────────────────────────────

    private func normalizedToDisplay(x: Float, y: Float) -> CGPoint {
        return CGPoint(
            x: displayOriginX + CGFloat(x) * displayWidth,
            y: displayOriginY + CGFloat(y) * displayHeight
        )
    }

    // ── Windows VK → macOS Key Code Mapping ─────────────────────────────────

    // swiftlint:disable cyclomatic_complexity function_body_length
    private func windowsVKToMacKeyCode(_ vk: UInt16) -> UInt16? {
        switch vk {
        // Letters A-Z (Windows VK_A=0x41 → VK_Z=0x5A)
        case 0x41: return UInt16(kVK_ANSI_A)
        case 0x42: return UInt16(kVK_ANSI_B)
        case 0x43: return UInt16(kVK_ANSI_C)
        case 0x44: return UInt16(kVK_ANSI_D)
        case 0x45: return UInt16(kVK_ANSI_E)
        case 0x46: return UInt16(kVK_ANSI_F)
        case 0x47: return UInt16(kVK_ANSI_G)
        case 0x48: return UInt16(kVK_ANSI_H)
        case 0x49: return UInt16(kVK_ANSI_I)
        case 0x4A: return UInt16(kVK_ANSI_J)
        case 0x4B: return UInt16(kVK_ANSI_K)
        case 0x4C: return UInt16(kVK_ANSI_L)
        case 0x4D: return UInt16(kVK_ANSI_M)
        case 0x4E: return UInt16(kVK_ANSI_N)
        case 0x4F: return UInt16(kVK_ANSI_O)
        case 0x50: return UInt16(kVK_ANSI_P)
        case 0x51: return UInt16(kVK_ANSI_Q)
        case 0x52: return UInt16(kVK_ANSI_R)
        case 0x53: return UInt16(kVK_ANSI_S)
        case 0x54: return UInt16(kVK_ANSI_T)
        case 0x55: return UInt16(kVK_ANSI_U)
        case 0x56: return UInt16(kVK_ANSI_V)
        case 0x57: return UInt16(kVK_ANSI_W)
        case 0x58: return UInt16(kVK_ANSI_X)
        case 0x59: return UInt16(kVK_ANSI_Y)
        case 0x5A: return UInt16(kVK_ANSI_Z)

        // Numbers 0-9 (Windows VK_0=0x30 → VK_9=0x39)
        case 0x30: return UInt16(kVK_ANSI_0)
        case 0x31: return UInt16(kVK_ANSI_1)
        case 0x32: return UInt16(kVK_ANSI_2)
        case 0x33: return UInt16(kVK_ANSI_3)
        case 0x34: return UInt16(kVK_ANSI_4)
        case 0x35: return UInt16(kVK_ANSI_5)
        case 0x36: return UInt16(kVK_ANSI_6)
        case 0x37: return UInt16(kVK_ANSI_7)
        case 0x38: return UInt16(kVK_ANSI_8)
        case 0x39: return UInt16(kVK_ANSI_9)

        // Function keys
        case 0x70: return UInt16(kVK_F1)
        case 0x71: return UInt16(kVK_F2)
        case 0x72: return UInt16(kVK_F3)
        case 0x73: return UInt16(kVK_F4)
        case 0x74: return UInt16(kVK_F5)
        case 0x75: return UInt16(kVK_F6)
        case 0x76: return UInt16(kVK_F7)
        case 0x77: return UInt16(kVK_F8)
        case 0x78: return UInt16(kVK_F9)
        case 0x79: return UInt16(kVK_F10)
        case 0x7A: return UInt16(kVK_F11)
        case 0x7B: return UInt16(kVK_F12)

        // Special keys
        case 0x0D: return UInt16(kVK_Return)       // Enter
        case 0x20: return UInt16(kVK_Space)        // Space
        case 0x08: return UInt16(kVK_Delete)       // Backspace
        case 0x2E: return UInt16(kVK_ForwardDelete)// Delete
        case 0x09: return UInt16(kVK_Tab)          // Tab
        case 0x1B: return UInt16(kVK_Escape)       // Escape
        case 0x14: return UInt16(kVK_CapsLock)     // Caps Lock

        // Arrow keys
        case 0x25: return UInt16(kVK_LeftArrow)    // Left
        case 0x26: return UInt16(kVK_UpArrow)      // Up
        case 0x27: return UInt16(kVK_RightArrow)   // Right
        case 0x28: return UInt16(kVK_DownArrow)    // Down

        // Navigation
        case 0x24: return UInt16(kVK_Home)         // Home
        case 0x23: return UInt16(kVK_End)          // End
        case 0x21: return UInt16(kVK_PageUp)       // Page Up
        case 0x22: return UInt16(kVK_PageDown)     // Page Down

        // Punctuation & symbols
        case 0xBA: return UInt16(kVK_ANSI_Semicolon)    // ;:
        case 0xBB: return UInt16(kVK_ANSI_Equal)        // =+
        case 0xBC: return UInt16(kVK_ANSI_Comma)        // ,<
        case 0xBD: return UInt16(kVK_ANSI_Minus)        // -_
        case 0xBE: return UInt16(kVK_ANSI_Period)       // .>
        case 0xBF: return UInt16(kVK_ANSI_Slash)        // /?
        case 0xC0: return UInt16(kVK_ANSI_Grave)        // `~
        case 0xDB: return UInt16(kVK_ANSI_LeftBracket)  // [{
        case 0xDC: return UInt16(kVK_ANSI_Backslash)    // \|
        case 0xDD: return UInt16(kVK_ANSI_RightBracket) // ]}
        case 0xDE: return UInt16(kVK_ANSI_Quote)        // '"

        // Modifier keys (forwarded but also tracked via modifier flags)
        case 0x10, 0xA0, 0xA1: return UInt16(kVK_Shift)     // VK_SHIFT/LSHIFT/RSHIFT
        case 0x11, 0xA2, 0xA3: return UInt16(kVK_Command)   // VK_CONTROL → Cmd
        case 0x12, 0xA4, 0xA5: return UInt16(kVK_Option)    // VK_MENU/ALT → Option
        case 0x5B, 0x5C:       return UInt16(kVK_Control)   // VK_LWIN/RWIN → Ctrl

        default: return nil
        }
    }
    // swiftlint:enable cyclomatic_complexity function_body_length
}
