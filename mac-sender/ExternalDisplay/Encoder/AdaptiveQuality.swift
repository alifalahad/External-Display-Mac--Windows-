// =============================================================================
// AdaptiveQuality.swift — Adaptive bitrate controller
// =============================================================================
// Receives QualityReport from Windows receiver and dynamically adjusts
// encoder bitrate to match network conditions.
//
// Design principles:
// - Hysteresis: only change after 3 consecutive reports in same direction
// - Smooth transitions: max 5 Mbps change per step
// - Range: 8 Mbps (poor WiFi) to 40 Mbps (excellent WiFi)
// - Default: 25 Mbps (balanced)
// =============================================================================

import Foundation

/// Network quality tier
enum QualityTier: String {
    case excellent  // loss < 1%, RTT < 20ms  → target 40 Mbps
    case good       // loss < 3%, RTT < 50ms  → target 25 Mbps
    case fair       // loss < 5%, RTT < 100ms → target 15 Mbps
    case poor       // loss > 5% or RTT > 100ms → target 8 Mbps
}

final class AdaptiveQualityController {

    // ── Configuration ───────────────────────────────────────────────────────

    static let minBitrateBps  = 8_000_000    //  8 Mbps
    static let maxBitrateBps  = 40_000_000   // 40 Mbps
    static let defaultBitrate = 25_000_000   // 25 Mbps
    static let maxStepBps     = 5_000_000    //  5 Mbps per adjustment
    static let hysteresis     = 3            // consecutive reports needed

    // ── State ───────────────────────────────────────────────────────────────

    private(set) var currentBitrateBps: Int = defaultBitrate
    private(set) var currentTier: QualityTier = .good

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

        print("[Quality] Loss: \(String(format: "%.1f", lossPercent))%, "
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

    private func bitrateForTier(_ tier: QualityTier) -> Int {
        switch tier {
        case .excellent: return Self.maxBitrateBps
        case .good:      return Self.defaultBitrate
        case .fair:      return 15_000_000
        case .poor:      return Self.minBitrateBps
        }
    }

    private func adjustBitrate(toward target: Int) {
        let diff = target - currentBitrateBps
        let step = min(abs(diff), Self.maxStepBps)
        let newBitrate: Int

        if diff > 0 {
            newBitrate = min(currentBitrateBps + step, Self.maxBitrateBps)
        } else {
            newBitrate = max(currentBitrateBps - step, Self.minBitrateBps)
        }

        if newBitrate != currentBitrateBps {
            currentBitrateBps = newBitrate
            onBitrateChange?(newBitrate)
        }
    }
}
