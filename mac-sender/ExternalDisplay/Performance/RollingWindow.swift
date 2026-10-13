// =============================================================================
// RollingWindow.swift — Fixed-size sample window (avg / p95 / max) and rate meter
// =============================================================================
// Not thread-safe: each owner guards its own windows with its own lock.
// =============================================================================

import Foundation

/// Avg / p95 / max of a timing in milliseconds
struct LatencySummary {
    var avgMs: Double = 0
    var p95Ms: Double = 0
    var maxMs: Double = 0
    var samples = 0
}

/// Last `capacity` samples in a ring buffer
struct RollingWindow {
    private var values: [Double]
    private var next = 0
    private(set) var count = 0

    init(capacity: Int) {
        values = [Double](repeating: 0, count: max(capacity, 1))
    }

    mutating func add(_ value: Double) {
        values[next] = value
        next = (next + 1) % values.count
        count = min(count + 1, values.count)
    }

    mutating func reset() {
        next = 0
        count = 0
    }

    func summary() -> LatencySummary {
        guard count > 0 else { return LatencySummary() }
        let sorted = values[0..<count].sorted()
        let p95Index = min(count - 1, Int((Double(count) * 0.95).rounded(.up)) - 1)
        return LatencySummary(
            avgMs: sorted.reduce(0, +) / Double(count),
            p95Ms: sorted[max(p95Index, 0)],
            maxMs: sorted[count - 1],
            samples: count
        )
    }
}

/// Events (and bytes) per second over a sliding one-second window
struct RateMeter {
    private var events: [(time: Double, bytes: Int)] = []
    private let window: Double = 1.0

    mutating func record(at time: Double, bytes: Int = 0) {
        events.append((time, bytes))
        trim(now: time)
    }

    /// Events per second
    mutating func rate(now: Double) -> Double {
        trim(now: now)
        return Double(events.count) / window
    }

    /// Megabits per second of the recorded byte counts
    mutating func megabitsPerSecond(now: Double) -> Double {
        trim(now: now)
        let bytes = events.reduce(0) { $0 + $1.bytes }
        return Double(bytes) * 8 / window / 1_000_000
    }

    mutating func reset() {
        events.removeAll(keepingCapacity: true)
    }

    private mutating func trim(now: Double) {
        let cutoff = now - window
        if let firstKept = events.firstIndex(where: { $0.time > cutoff }) {
            if firstKept > 0 { events.removeFirst(firstKept) }
        } else {
            events.removeAll(keepingCapacity: true)
        }
    }
}
