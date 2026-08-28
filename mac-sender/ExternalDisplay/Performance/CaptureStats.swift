// =============================================================================
// CaptureStats.swift — Capture performance metrics
// =============================================================================
// Thread-safe frame statistics tracker using a rolling window.
// Called from the capture queue, read from the main thread.
// =============================================================================

import Foundation
import CoreGraphics

/// Snapshot of capture statistics at a point in time
struct CaptureStats {
    var fps: Double = 0
    var avgFrameTimeMs: Double = 0
    var minFrameTimeMs: Double = 999
    var maxFrameTimeMs: Double = 0
    var resolution: CGSize = .zero
    var totalFrames: UInt64 = 0
    var droppedFrames: UInt64 = 0
    var uptime: Double = 0
}

/// Thread-safe tracker that accumulates frame timing data
/// and produces `CaptureStats` snapshots on demand.
final class CaptureStatsTracker: @unchecked Sendable {

    // ── Rolling window ──────────────────────────────────────────────────────
    private let windowSize = 120  // ~2 seconds at 60 FPS
    private var frameTimes: [Double] = []

    // ── Counters ────────────────────────────────────────────────────────────
    private(set) var totalFrames: UInt64 = 0
    private var droppedFrames: UInt64 = 0
    private var lastFrameTime: Double = 0
    private var firstFrameTime: Double = 0
    private var currentWidth: Int = 0
    private var currentHeight: Int = 0

    // Thread safety
    private let lock = NSLock()

    // ── API ─────────────────────────────────────────────────────────────────

    /// Record a captured frame's timing. Called from the capture queue.
    func recordFrame(time: Double, width: Int, height: Int) {
        lock.lock()
        defer { lock.unlock() }

        if firstFrameTime == 0 { firstFrameTime = time }

        totalFrames += 1
        currentWidth = width
        currentHeight = height

        if lastFrameTime > 0 {
            let dt = time - lastFrameTime
            frameTimes.append(dt)
            if frameTimes.count > windowSize {
                frameTimes.removeFirst()
            }
        }
        lastFrameTime = time
    }

    /// Record a dropped/skipped frame.
    func recordDrop() {
        lock.lock()
        defer { lock.unlock() }
        droppedFrames += 1
    }

    /// Produce a snapshot of current statistics. Safe to call from any thread.
    func currentStats() -> CaptureStats {
        lock.lock()
        defer { lock.unlock() }

        var stats = CaptureStats()
        stats.totalFrames = totalFrames
        stats.droppedFrames = droppedFrames
        stats.resolution = CGSize(width: currentWidth, height: currentHeight)

        if lastFrameTime > 0 && firstFrameTime > 0 {
            stats.uptime = lastFrameTime - firstFrameTime
        }

        if !frameTimes.isEmpty {
            let sum = frameTimes.reduce(0, +)
            let avg = sum / Double(frameTimes.count)
            stats.fps = avg > 0 ? 1.0 / avg : 0
            stats.avgFrameTimeMs = avg * 1000
            stats.minFrameTimeMs = (frameTimes.min() ?? 0) * 1000
            stats.maxFrameTimeMs = (frameTimes.max() ?? 0) * 1000
        }

        return stats
    }

    /// Reset all counters and history.
    func reset() {
        lock.lock()
        defer { lock.unlock() }
        frameTimes.removeAll()
        totalFrames = 0
        droppedFrames = 0
        lastFrameTime = 0
        firstFrameTime = 0
    }
}
