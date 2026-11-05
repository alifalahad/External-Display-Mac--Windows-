// =============================================================================
// FrameRate.swift — How many frames per second to stream (docs/SMOOTHNESS.md, step 7)
// =============================================================================
// Every wait for a refresh — the Mac's display before capture, the PC's
// screen before a frame shows — is half a frame on average: 8 ms at 60 Hz,
// 4 ms at 120 Hz. The stream runs at 120 FPS only when every part can:
//   the preset       High (Balanced 60, Battery Saver 30)
//   the PC's screen  119 Hz or more (RECEIVER_CAPS; an older receiver: 60)
//   the display      refreshes at 120 Hz (the virtual display is set to it)
//   the encoder      keeps up at this size (EncoderLoad). On an M2 a frame
//                    takes ~8 ms at 1920×1080 and ~10 ms at 2560×1440: 120 FPS
//                    leaves 8.3 ms.
// =============================================================================

import Foundation

enum FrameRate {
    /// What a PC's screen can show (0 Hz: an older receiver, which never said).
    /// A 48 Hz power-saving panel still shows 60 FPS better than 30.
    static func receiverLimit(refreshHz: UInt32) -> Int {
        switch refreshHz {
        case 0:      return 60
        case 119...: return 120
        case 31...:  return 60
        default:     return 30
        }
    }

    /// What a captured display delivers (0 Hz: unknown)
    static func displayLimit(refreshHz: Double) -> Int {
        refreshHz >= 119 ? 120 : 60
    }

    /// The stream's frame rate
    static func choose(preset: QualityPreset, receiverRefreshHz: UInt32, displayRefreshHz: Double,
                       encoderKeepsUp: Bool) -> Int {
        let fps = min(preset.maxFps, receiverLimit(refreshHz: receiverRefreshHz),
                      displayLimit(refreshHz: displayRefreshHz))
        return fps > 60 && !encoderKeepsUp ? 60 : fps
    }

    /// Why the stream runs at what choose() said, for the window and the log
    static func describe(preset: QualityPreset, receiverRefreshHz: UInt32, displayRefreshHz: Double,
                         encoderKeepsUp: Bool) -> String {
        let fps = choose(preset: preset, receiverRefreshHz: receiverRefreshHz,
                         displayRefreshHz: displayRefreshHz, encoderKeepsUp: encoderKeepsUp)
        let screen120 = receiverLimit(refreshHz: receiverRefreshHz) >= 120
        if fps >= 120 { return "\(fps) FPS" }
        if preset.maxFps < 120 {
            return screen120 ? "\(fps) FPS — High streams 120 to this PC's \(receiverRefreshHz) Hz screen"
                             : "\(fps) FPS"
        }
        if receiverRefreshHz == 0 { return "\(fps) FPS — the PC has not said its screen's refresh rate" }
        if !screen120 { return "\(fps) FPS — the PC's screen runs at \(receiverRefreshHz) Hz" }
        if displayLimit(refreshHz: displayRefreshHz) < 120 {
            return "\(fps) FPS — the captured display refreshes at \(Int(displayRefreshHz.rounded())) Hz"
        }
        return "\(fps) FPS — the encoder cannot keep up with 120 at this size"
    }
}

/// Does the encoder keep up with the frame rate? Fed per frame, asked once a
/// second. Above some size VideoToolbox's encode call blocks until the frame
/// is done; when that takes longer than a frame interval the capture queue
/// falls behind (2560×1440 at 120 FPS on an M2: 92 frames a second, unevenly).
/// Below it the call returns at once and a slow encoder shows as late output.
struct EncoderLoad {
    struct Second: Equatable {
        var calls = 0
        var avgCallMs = 0.0
        var slowShare = 0.0     // calls that blocked for a frame interval or longer
        var lateShare = 0.0     // frames out three frame intervals or more after going in
    }

    static let minimumCalls = 30      // fewer frames (a still screen): no verdict
    static let behindShare = 0.25
    static let secondsBehind = 2

    private var calls = 0, slowCalls = 0
    private var callMsSum = 0.0
    private var outputs = 0, lateOutputs = 0
    private var behindInARow = 0

    mutating func recordCall(ms: Double, intervalMs: Double) {
        calls += 1
        callMsSum += ms
        if ms >= intervalMs { slowCalls += 1 }
    }

    mutating func recordOutput(latencyMs: Double, intervalMs: Double) {
        outputs += 1
        if latencyMs >= 3 * intervalMs { lateOutputs += 1 }
    }

    /// Once a second. behind: it fell behind two seconds in a row.
    mutating func secondElapsed() -> (behind: Bool, last: Second) {
        var s = Second()
        s.calls = calls
        s.avgCallMs = calls > 0 ? callMsSum / Double(calls) : 0
        s.slowShare = calls > 0 ? Double(slowCalls) / Double(calls) : 0
        s.lateShare = outputs > 0 ? Double(lateOutputs) / Double(outputs) : 0
        calls = 0; slowCalls = 0; callMsSum = 0; outputs = 0; lateOutputs = 0
        if s.calls >= Self.minimumCalls {
            behindInARow = s.slowShare > Self.behindShare || s.lateShare > Self.behindShare ? behindInARow + 1 : 0
        }
        return (behindInARow >= Self.secondsBehind, s)
    }

    /// A new frame rate or session: start counting again
    mutating func reset() { self = EncoderLoad() }
}
