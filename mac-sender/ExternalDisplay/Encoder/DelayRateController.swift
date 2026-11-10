// =============================================================================
// DelayRateController.swift — Bitrate that follows the delay (docs/SMOOTHNESS.md, step 6)
// =============================================================================
// Before a Wi-Fi link loses packets, its queues fill and frames take longer to
// arrive. The receiver measures that queuing delay and reports it every 100 ms
// (RATE_FEEDBACK). This controller:
//   overuse on a full link (delay high,   → 85 % of what got through while it was
//   not falling, no frame without wait,    full (its video share: parity and
//   two reports in a row)                  headers travel too), at most every 0.3 s
//   … while the encoder sends under half  → 85 % of the target: what arrived then
//   its target (a still screen)            says nothing about the link
//   overuse in one report only, or that   → nothing: a burst (a keyframe) or a
//   drained again                          radio pause (AWDL on the Mac stops
//                                          the Wi-Fi for tens of ms about once a
//                                          second, and almost nothing arrives)
//   loss above 10 % otherwise             → lower in proportion to the loss, at
//                                          most once a second
//   normal (delay low, loss < 2 %)        → +25 % per second while far (< 70 %)
//                                          below where the link was last full,
//                                          then +8 %; only while the encoder
//                                          uses the bitrate it has
// Under congestion the bitrate may go below the preset's minimum, down to a
// quarter of it: a picture that keeps up is smoother than a sharp one that lags.
// Older receivers send no RATE_FEEDBACK: the loss/RTT tiers of
// AdaptiveQualityController stay in charge then (BitrateGovernor).
// =============================================================================

import Foundation
import QuartzCore

final class DelayRateController {
    static let decreaseFactor = 0.85
    static let increasePerSecond = 0.08
    static let fastIncreasePerSecond = 0.25
    static let fastBelow = 0.7                // of the last capacity found
    static let minDecreaseInterval = 0.3      // s: let the queue drain before cutting again
    static let minLossDecreaseInterval = 1.0  // s: the loss figure covers 500 ms
    static let increaseHoldAfterDecrease = 1.0
    static let minChange = 0.03               // smaller changes are not passed to the encoder
    static let overuseReportsToCut = 2        // in a row, ~200 ms: a radio pause drains sooner
    static let appLimitedBelow = 0.5          // encoder output under half the target

    private(set) var targetBps: Int
    private(set) var floorBps: Int
    private(set) var maxBps: Int
    private var appliedBps: Int
    private var lastDecrease = -Double.infinity
    private var lastIncrease: Double?
    private(set) var capacityBps = 0          // what arrived when the link was last full
    private var overuseStreak = 0             // overuse reports in a row

    init(preset: QualityPreset) {
        targetBps = preset.startBitrateBps
        appliedBps = preset.startBitrateBps
        floorBps = preset.minBitrateBps / 4
        maxBps = preset.maxBitrateBps
    }

    func apply(_ preset: QualityPreset) -> Int? {
        floorBps = preset.minBitrateBps / 4
        maxBps = preset.maxBitrateBps
        targetBps = min(max(targetBps, floorBps), maxBps)
        return changed(force: true)
    }

    /// One report from the receiver. encoderBps: what the encoder produced
    /// lately (a static screen uses little: then there is nothing to probe).
    /// videoShare: encoder output / everything sent (parity, headers).
    /// Returns the new bitrate when the encoder should change.
    func onFeedback(_ f: RateFeedbackPayload, encoderBps: Int, videoShare: Double = 1, now: Double) -> Int? {
        guard f.samples > 0 else { return nil }   // no frames: no information
        let loss = Double(f.lossPercent100) / 100
        overuseStreak = f.state == .overuse ? overuseStreak + 1 : 0

        if f.state == .overuse && f.linkKbps > 0 && overuseStreak >= Self.overuseReportsToCut {
            guard now - lastDecrease >= Self.minDecreaseInterval else { return nil }
            if Double(encoderBps) < Self.appLimitedBelow * Double(targetBps) {
                // We send little: the queue is someone else's, and what arrived
                // is our own small output, not what the link can carry
                targetBps = max(floorBps, Int(Double(targetBps) * Self.decreaseFactor))
            } else {
                // What a full link carried is its capacity; what arrived over 500 ms
                // would be the encoder's own (lower) output whenever the link had room
                let link = Double(f.linkKbps) * 1000 * min(max(videoShare, 0.6), 1)
                capacityBps = Int(link)
                targetBps = max(floorBps, Int(min(Double(targetBps), link) * Self.decreaseFactor))
            }
            lastDecrease = now
            lastIncrease = nil
        } else if loss > 10 {
            guard now - lastDecrease >= Self.minLossDecreaseInterval else { return nil }
            targetBps = max(floorBps, Int(Double(targetBps) * (1 - 0.5 * loss / 100)))
            lastDecrease = now
            lastIncrease = nil
        } else if f.state == .normal && loss < 2 {
            let since = lastIncrease ?? now
            lastIncrease = now
            guard now - lastDecrease >= Self.increaseHoldAfterDecrease,
                  Double(encoderBps) >= 0.6 * Double(targetBps) else { return nil }
            let dt = min(now - since, 0.5)
            let far = capacityBps > 0 && Double(targetBps) < Self.fastBelow * Double(capacityBps)
            let rate = far ? Self.fastIncreasePerSecond : Self.increasePerSecond
            targetBps = min(maxBps, Int(Double(targetBps) * (1 + rate * dt)))
        } else {
            lastIncrease = now   // hold, or a queue that emptied again
        }
        return changed(force: false)
    }

    private func changed(force: Bool) -> Int? {
        let delta = abs(Double(targetBps - appliedBps)) / Double(max(appliedBps, 1))
        guard targetBps != appliedBps, force || delta >= Self.minChange else { return nil }
        appliedBps = targetBps
        return targetBps
    }
}

/// Chooses the encoder's bitrate from the receiver's reports: the delay
/// controller once RATE_FEEDBACK arrives, the loss/RTT tiers until then.
/// Thread-safe: reports come on the network queue, presets on the main thread.
final class BitrateGovernor {
    /// The encoder's new bitrate (called on the thread of the report)
    var onBitrateChange: ((Int) -> Void)?
    /// What the encoder produced lately, bits per second
    var encoderOutputBps: () -> Int = { 0 }
    /// Everything sent lately (video, parity, headers), bits per second
    var sentBps: () -> Int = { 0 }

    private let lock = NSLock()
    private var preset: QualityPreset
    private var tiers: AdaptiveQualityController
    private var delay: DelayRateController
    private(set) var followsDelay = false

    init(preset: QualityPreset) {
        self.preset = preset
        tiers = AdaptiveQualityController(preset: preset)
        delay = DelayRateController(preset: preset)
    }

    var mode: String { lock.lock(); defer { lock.unlock() }; return followsDelay ? "delay" : "tiers" }

    /// New connection: back to the preset's start bitrate and the tiers
    func reset() {
        lock.lock()
        tiers = AdaptiveQualityController(preset: preset)
        delay = DelayRateController(preset: preset)
        followsDelay = false
        let start = preset.startBitrateBps
        lock.unlock()
        onBitrateChange?(start)
    }

    func apply(_ preset: QualityPreset) {
        lock.lock()
        self.preset = preset
        var change: Int?
        tiers.onBitrateChange = { change = $0 }
        tiers.apply(preset)
        tiers.onBitrateChange = nil
        let delayChange = delay.apply(preset)
        let result = followsDelay ? delayChange : change
        lock.unlock()
        if let result { onBitrateChange?(result) }
    }

    func onQualityReport(_ r: QualityReportPayload) {
        lock.lock()
        guard !followsDelay else { lock.unlock(); return }   // the delay controller decides
        var change: Int?
        tiers.onBitrateChange = { change = $0 }
        tiers.processReport(packetLossPercent100: r.packetLossPercent, rttMs: r.rttMs,
                            framesDropped: r.framesDropped, queueDepth: r.queueDepth)
        tiers.onBitrateChange = nil
        lock.unlock()
        if let change { onBitrateChange?(change) }
    }

    func onRateFeedback(_ f: RateFeedbackPayload, now: Double = CACurrentMediaTime()) {
        let encoderBps = encoderOutputBps()
        let sent = sentBps()
        let share = sent > 0 && encoderBps > 0 ? Double(encoderBps) / Double(sent) : 1
        lock.lock()
        if !followsDelay {
            followsDelay = true
            Log.info(.quality, "Bitrate follows the receiver's delay feedback (RATE_FEEDBACK)")
        }
        let before = delay.targetBps
        let change = delay.onFeedback(f, encoderBps: encoderBps, videoShare: share, now: now)
        lock.unlock()
        if let change {
            // Every cut (at most ~3 a second), rises once a second
            let down = change < before
            let link = f.linkKbps > 0 ? ", link full at \(f.linkKbps) kbps" : ""
            Log.every(down ? 0.25 : 1, key: down ? "delay-rate-down" : "delay-rate-up", .info, .quality,
                      "Bitrate \(down ? "down" : "up") to \(change / 1000) kbps (\(f.state), queue "
                      + "\(f.queuingDelayUs / 1000) ms, loss \(f.lossPercent100 / 100) %, received "
                      + "\(f.receivedKbps) kbps\(link))")
            onBitrateChange?(change)
        }
    }
}
