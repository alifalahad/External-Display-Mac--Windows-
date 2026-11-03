// Loss repair and pacing (docs/SMOOTHNESS.md, step 5): parity packets,
// their wire layout (shared with windows-receiver/tests/NetworkTests.cpp) and
// the pacing schedule

import XCTest
@testable import ExternalDisplay

final class LossRepairTests: XCTestCase {

    private func frame(_ count: Int) -> Data { Data((0..<count).map { UInt8(truncatingIfNeeded: $0 * 7 + 3) }) }
    private func hex(_ data: Data) -> String { data.map { String(format: "%02x", $0) }.joined() }

    func testGroupCounts() {
        XCTAssertEqual(FrameParity.groupCount(fragments: 1), 1)     // a small frame gets a copy
        XCTAssertEqual(FrameParity.groupCount(fragments: 10), 1)
        XCTAssertEqual(FrameParity.groupCount(fragments: 11), 2)
        XCTAssertEqual(FrameParity.groupCount(fragments: 24), 3)    // a typical P-frame
        XCTAssertEqual(FrameParity.groupCount(fragments: 112), 12)  // a keyframe: bursts of 12 repairable
        XCTAssertEqual(FrameParity.groupCount(fragments: 5000), 32)
    }

    // Same vectors as the Windows test; computed independently (Python, zlib)
    func testParityMatchesReferenceVectors() {
        let cases: [(bytes: Int, group: Int, groups: Int, length: Int, crc: UInt32)] = [
            (4000, 0, 1, 1340, 0x2f7e00a7),
            (30000, 0, 3, 1340, 0x2c3f2043),
            (30000, 1, 3, 1340, 0x9b1dd341),
            (30000, 2, 3, 1340, 0x36fafe97),
            (900, 0, 1, 900, 0x041117fd),      // one short fragment: the parity is its copy
        ]
        for c in cases {
            let p = FrameParity.parity(of: frame(c.bytes), fragmentSize: 1340, group: c.group, groups: c.groups)
            XCTAssertEqual(p.count, c.length, "\(c.bytes) bytes, group \(c.group)")
            XCTAssertEqual(CRC32.compute(p), c.crc, "\(c.bytes) bytes, group \(c.group)")
        }
        XCTAssertEqual(FrameParity.parity(of: frame(900), fragmentSize: 1340, group: 0, groups: 1), frame(900))
    }

    func testAnyOneLossPerGroupIsRebuilt() {
        let data = frame(30_001)                   // 23 fragments, last one 521 bytes
        let size = 1340, total = 23, groups = FrameParity.groupCount(fragments: total)
        func fragment(_ i: Int) -> Data { data.subdata(in: i * size..<min((i + 1) * size, data.count)) }
        for lost in 0..<total {
            let group = lost % groups
            var rebuilt = [UInt8](FrameParity.parity(of: data, fragmentSize: size, group: group, groups: groups))
            for i in stride(from: group, to: total, by: groups) where i != lost {
                for (k, b) in fragment(i).enumerated() { rebuilt[k] ^= b }
            }
            let length = lost == total - 1 ? data.count - (total - 1) * size : size
            XCTAssertEqual(Data(rebuilt.prefix(length)), fragment(lost), "fragment \(lost)")
        }
    }

    func testParityHeaderLayout() {
        var h = ParityHeader()
        h.frameSequence = 0x01020304
        h.fragmentTotal = 23
        h.groupIndex = 2
        h.groupCount = 3
        h.frameBytes = 30000
        XCTAssertEqual(hex(h.serialize()), "04030201" + "1700" + "0200" + "0300" + "0000" + "30750000")
        XCTAssertEqual(MessageType.videoParity.rawValue, 0x42)
        XCTAssertEqual(ReceiverCapsPayload.parity, 2)
        XCTAssertEqual(ReceiverCapsPayload.pacing, 4)
        // 28 header + 8 counter + 16 parity header + 1,340 data + 16 tag
        XCTAssertEqual(ProtocolConstants.headerSize + ProtocolConstants.udpCounterSize + ParityHeader.size
                       + ProtocolConstants.maxFragmentPayload + PacketCipher.tagSize, 1408)
    }

    func testPacingSchedule() {
        XCTAssertEqual(PacingPlan.gaps(packets: 1), [0])
        XCTAssertEqual(PacingPlan.gaps(packets: 8), [0], "small frames leave at once")
        XCTAssertEqual(PacingPlan.gaps(packets: 24), [0, 250, 250])
        let keyframe = PacingPlan.gaps(packets: 112)          // 14 bursts
        XCTAssertEqual(keyframe.count, 14)
        XCTAssertEqual(keyframe.reduce(0, +), 13 * 76, "spread within 1 ms")
        for n in [9, 50, 200, 1000] {
            XCTAssertLessThanOrEqual(PacingPlan.gaps(packets: n).reduce(0, +), PacingPlan.maxSpreadUs)
        }
    }
}
