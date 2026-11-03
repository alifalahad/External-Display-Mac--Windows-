// =============================================================================
// FrameParity.swift — Repair packets and paced sending (docs/SMOOTHNESS.md, step 5)
// =============================================================================
// Parity: a frame is cut into fragments of up to 1,340 bytes. With P groups,
// parity packet j is the XOR of fragments j, j+P, j+2P… (each zero-padded to
// the longest one in its group). The receiver rebuilds any group that lost
// exactly one fragment, so isolated losses and bursts of up to P consecutive
// packets cost nothing — no keyframe, no freeze. About one parity per 10
// fragments (≈10 % more data), at least one per frame.
//
// Pacing: a frame's datagrams leave in bursts of 8, spread over at most about
// a millisecond, instead of 100 at once into the Wi-Fi queue.
// =============================================================================

import Foundation
import Network

enum FrameParity {
    /// Parity groups for a frame of `fragments` fragments
    static func groupCount(fragments: Int) -> Int {
        min(32, max(1, (fragments + 9) / 10))
    }

    /// XOR of group `group`'s fragments, as long as its longest member
    static func parity(of frame: Data, fragmentSize: Int, group: Int, groups: Int) -> Data {
        let total = (frame.count + fragmentSize - 1) / fragmentSize
        var length = 0
        for i in stride(from: group, to: total, by: groups) {
            length = max(length, min(fragmentSize, frame.count - i * fragmentSize))
        }
        var out = Data(count: length)
        frame.withUnsafeBytes { (src: UnsafeRawBufferPointer) in
            out.withUnsafeMutableBytes { (dst: UnsafeMutableRawBufferPointer) in
                guard let s = src.baseAddress, let d = dst.baseAddress else { return }
                for i in stride(from: group, to: total, by: groups) {
                    let start = i * fragmentSize
                    let count = min(fragmentSize, frame.count - start)
                    // 8 bytes at a time (fast in Debug builds too), then the tail
                    var k = 0
                    while k + 8 <= count {
                        let a = d.loadUnaligned(fromByteOffset: k, as: UInt64.self)
                        let b = s.loadUnaligned(fromByteOffset: start + k, as: UInt64.self)
                        d.storeBytes(of: a ^ b, toByteOffset: k, as: UInt64.self)
                        k += 8
                    }
                    while k < count {
                        let a = d.load(fromByteOffset: k, as: UInt8.self)
                        let b = s.load(fromByteOffset: start + k, as: UInt8.self)
                        d.storeBytes(of: a ^ b, toByteOffset: k, as: UInt8.self)
                        k += 1
                    }
                }
            }
        }
        return out
    }
}

enum PacingPlan {
    static let burst = 8                  // datagrams back to back
    static let maxGapUs = 250             // between bursts
    static let maxSpreadUs = 1000         // a whole frame leaves within about 1 ms

    /// Microseconds to wait before each burst (the first is 0)
    static func gaps(packets: Int) -> [Int] {
        let bursts = max(1, (packets + burst - 1) / burst)
        guard bursts > 1 else { return [0] }
        let gap = min(maxGapUs, maxSpreadUs / (bursts - 1))
        return [0] + Array(repeating: gap, count: bursts - 1)
    }
}

/// Sends one frame's datagrams per PacingPlan. Frames stay in order: one
/// serial queue, which only this sending ever blocks.
final class PacketPacer {
    private let queue = DispatchQueue(label: "com.externaldisplay.pacer", qos: .userInteractive)

    // First to last datagram of the recent paced frames (sleeps overshoot)
    private let statsLock = NSLock()
    private var spreads = RollingWindow(capacity: 120)
    private var lastPacedUs: UInt64 = 0

    func send(_ packets: [Data], over udp: NWConnection) {
        queue.async { [weak self] in
            let startUs = HostClock.nowUs()
            let gaps = PacingPlan.gaps(packets: packets.count)
            for (index, gapUs) in gaps.enumerated() {
                if gapUs > 0 { usleep(useconds_t(gapUs)) }
                let start = index * PacingPlan.burst
                for packet in packets[start..<min(start + PacingPlan.burst, packets.count)] {
                    udp.send(content: packet, completion: .contentProcessed { _ in })
                }
            }
            let endUs = HostClock.nowUs()
            guard let self else { return }
            self.statsLock.lock()
            self.spreads.add(Double(endUs - startUs))
            self.lastPacedUs = endUs
            self.statsLock.unlock()
        }
    }

    /// Average microseconds from a frame's first datagram to its last, over
    /// the last ~2 s of paced frames (0: none paced recently). Reported to
    /// the receiver in SENDER_STATS.
    func averageSpreadUs() -> Double {
        statsLock.lock()
        defer { statsLock.unlock() }
        guard lastPacedUs > 0, HostClock.nowUs() - lastPacedUs < 2_000_000 else { return 0 }
        return spreads.summary().avgMs   // the window holds µs here
    }
}
