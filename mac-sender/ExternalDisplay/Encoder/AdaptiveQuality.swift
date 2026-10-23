// =============================================================================
// AdaptiveQuality.swift — Adaptive bitrate controller
// =============================================================================
// Receives QualityReport from Windows receiver and dynamically adjusts
// encoder bitrate to match network conditions.
//
// Design principles:
// - Hysteresis: up after 3 consecutive reports, down after 2 (react faster
//   to trouble than to recovery)
// - Smooth transitions: max 5 Mbps change per step
// - Range: set by the user's QualityPreset (e.g. Balanced: 8 – 25 Mbps)
// =============================================================================

import Foundation

/// Network quality tier
enum QualityTier: String {
    case excellent  // loss < 1%, RTT < 20ms  → preset maximum
    case good       // loss < 3%, RTT < 50ms  → 70% of the maximum
    case fair       // loss < 5%, RTT < 100ms → 45% of the maximum
    case poor       // loss > 5% or RTT > 100ms → preset minimum
}

final class AdaptiveQualityController {

    // ── Configuration ───────────────────────────────────────────────────────

    static let maxStepBps     = 5_000_000    //  5 Mbps per adjustment
    static let hysteresis     = 3            // consecutive reports needed

    private(set) var minBitrateBps: Int
    private(set) var maxBitrateBps: Int

    // ── State ───────────────────────────────────────────────────────────────

    private(set) var currentBitrateBps: Int
    private(set) var currentTier: QualityTier = .good

    init(preset: QualityPreset = .balanced) {
        minBitrateBps = preset.minBitrateBps
        maxBitrateBps = preset.maxBitrateBps
        currentBitrateBps = preset.startBitrateBps
    }

    /// The user changed the preset: new bounds now, clamped bitrate reported
    func apply(_ preset: QualityPreset) {
        minBitrateBps = preset.minBitrateBps
        maxBitrateBps = preset.maxBitrateBps
        let clamped = min(max(currentBitrateBps, minBitrateBps), maxBitrateBps)
        consecutiveUp = 0
        consecutiveDown = 0
        if clamped != currentBitrateBps {
            currentBitrateBps = clamped
            onBitrateChange?(clamped)
        }
    }

    /// Called when bitrate should change. Parameter: new bitrate in bps.
    var onBitrateChange: ((Int) -> Void)?

    // Hysteresis tracking
    private var consecutiveUp = 0
    private var consecutiveDown = 0

    // ── Process Quality Report ──────────────────────────────────────────────

    /// Process a quality report from the receiver and potentially adjust bitrate.
    /// - Parameters:
    ///   - packetLossPercent100: Packet loss × 100 (e.g. 350 = 3.50%)
    ///   - rttMs: Round-trip time in milliseconds
    ///   - framesDropped: Frames dropped since last report
    ///   - queueDepth: Current decode queue depth
    func processReport(
        packetLossPercent100: UInt32,
        rttMs: UInt32,
        framesDropped: UInt32,
        queueDepth: UInt32
    ) {
        let lossPercent = Double(packetLossPercent100) / 100.0

        // Determine target tier based on metrics
        let tier = classifyQuality(
            lossPercent: lossPercent,
            rttMs: rttMs,
            framesDropped: framesDropped,
            queueDepth: queueDepth
        )

        // Determine direction
        let targetBitrate = bitrateForTier(tier)

        if targetBitrate > currentBitrateBps {
            // Want to go up
            consecutiveUp += 1
            consecutiveDown = 0

            if consecutiveUp >= Self.hysteresis {
                adjustBitrate(toward: targetBitrate)
                consecutiveUp = 0
            }
        } else if targetBitrate < currentBitrateBps {
            // Want to go down — react faster (2 reports) to prevent stutter
            consecutiveDown += 1
            consecutiveUp = 0

            if consecutiveDown >= max(Self.hysteresis - 1, 1) {
                adjustBitrate(toward: targetBitrate)
                consecutiveDown = 0
            }
        } else {
            // Stable
            consecutiveUp = 0
            consecutiveDown = 0
        }

        currentTier = tier

        Log.debug(.quality, "Loss: \(String(format: "%.1f", lossPercent))%, "
            + "Drops: \(framesDropped), Queue: \(queueDepth), "
            + "Tier: \(tier.rawValue), Bitrate: \(currentBitrateBps / 1_000_000) Mbps")
    }

    // ── Private ─────────────────────────────────────────────────────────────

    private func classifyQuality(
        lossPercent: Double,
        rttMs: UInt32,
        framesDropped: UInt32,
        queueDepth: UInt32
    ) -> QualityTier {
        // Poor: high loss or high RTT or many drops
        if lossPercent > 5.0 || rttMs > 100 || framesDropped > 10 {
            return .poor
        }
        // Fair: moderate issues
        if lossPercent > 3.0 || rttMs > 50 || framesDropped > 5 || queueDepth > 3 {
            return .fair
        }
        // Excellent: very clean network
        if lossPercent < 1.0 && rttMs < 20 && framesDropped == 0 && queueDepth <= 1 {
            return .excellent
        }
        // Good: default
        return .good
    }

    func bitrateForTier(_ tier: QualityTier) -> Int {
        switch tier {
        case .excellent: return maxBitrateBps
        case .good:      return max(minBitrateBps, maxBitrateBps * 7 / 10)
        case .fair:      return max(minBitrateBps, maxBitrateBps * 45 / 100)
        case .poor:      return minBitrateBps
        }
    }

    private func adjustBitrate(toward target: Int) {
        let diff = target - currentBitrateBps
        let step = min(abs(diff), Self.maxStepBps)
        let newBitrate: Int

        if diff > 0 {
            newBitrate = min(currentBitrateBps + step, maxBitrateBps)
        } else {
            newBitrate = max(currentBitrateBps - step, minBitrateBps)
        }

        if newBitrate != currentBitrateBps {
            currentBitrateBps = newBitrate
            onBitrateChange?(newBitrate)
        }
    }
}
