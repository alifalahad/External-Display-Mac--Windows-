// Protocol serialization, CRC, malformed input, version (Phase 20)

import XCTest
@testable import ExternalDisplay

final class ProtocolTests: XCTestCase {

    func testHeaderRoundTrip() {
        var h = ProtocolHeader()
        h.type = MessageType.startStream.rawValue
        h.flags = MessageFlags([.keyframe, .encrypted]).rawValue
        h.sequence = 0xDEADBEEF
        h.timestamp = 0x0102030405060708
        h.payloadLength = 1234
        h.crc32 = 0xCAFEBABE
        let bytes = h.serialize()
        XCTAssertEqual(bytes.count, ProtocolConstants.headerSize)
        XCTAssertEqual(Array(bytes.prefix(4)), [0x45, 0x58, 0x44, 0x50])   // "EXDP"
        XCTAssertEqual(bytes[4], 2, "protocol version 2")

        let back = ProtocolHeader.deserialize(from: bytes)
        XCTAssertEqual(back?.type, h.type)
        XCTAssertEqual(back?.flags, h.flags)
        XCTAssertEqual(back?.sequence, h.sequence)
        XCTAssertEqual(back?.timestamp, h.timestamp)
        XCTAssertEqual(back?.payloadLength, h.payloadLength)
        XCTAssertEqual(back?.crc32, h.crc32)
    }

    func testMalformedHeadersRejected() {
        XCTAssertNil(ProtocolHeader.deserialize(from: Data(count: 27)), "too short")
        var bytes = ProtocolHeader().serialize()
        bytes[0] = 0x00
        XCTAssertNil(ProtocolHeader.deserialize(from: bytes), "wrong magic")
        XCTAssertNil(HelloPayload.deserialize(from: Data(count: HelloPayload.size - 1)))
        XCTAssertNil(KeyRevealPayload.deserialize(from: Data(count: 64)))
        XCTAssertNil(PairConfirmPayload.deserialize(from: Data(count: 31)))
        XCTAssertNil(QualityReportPayload.deserialize(from: Data(count: 19)))
        XCTAssertNil(InputEventPayload.deserialize(from: Data(count: 23)))
        XCTAssertNil(FrameTimingPrefix.deserialize(from: Data(count: 39)))
    }

    func testCRC32KnownValueAndEquivalence() {
        XCTAssertEqual(CRC32.compute(Data("123456789".utf8)), 0xCBF43926)   // standard check value
        XCTAssertEqual(CRC32.compute(Data()), 0)
        // Slicing-by-8 must equal the bitwise definition for every length
        for length in 0..<40 {
            let data = Data((0..<length).map { UInt8(($0 * 37 + 11) & 0xFF) })
            var crc: UInt32 = 0xFFFFFFFF
            for byte in data {
                crc ^= UInt32(byte)
                for _ in 0..<8 { crc = (crc & 1) != 0 ? (crc >> 1) ^ 0xEDB88320 : crc >> 1 }
            }
            XCTAssertEqual(CRC32.compute(data), crc ^ 0xFFFFFFFF, "length \(length)")
        }
    }

    func testHelloLayoutMatchesWindows() {
        var hello = HelloPayload()
        hello.receiverName = "PC"
        hello.udpPort = 0x1234
        hello.deviceId = "id"
        hello.commitment = Data(repeating: 0xAB, count: 32)
        let bytes = hello.serialize()
        XCTAssertEqual(bytes.count, 162)                 // static_assert in Protocol.h
        XCTAssertEqual(bytes[64], 0x34)                  // UDP port, little-endian at 64
        XCTAssertEqual(bytes[65], 0x12)
        XCTAssertEqual(bytes[66], UInt8(ascii: "i"))     // device ID at 66
        XCTAssertEqual(bytes[130], 0xAB)                 // commitment at 130
        let back = HelloPayload.deserialize(from: bytes)
        XCTAssertEqual(back?.receiverName, "PC")
        XCTAssertEqual(back?.deviceId, "id")
        XCTAssertEqual(back?.commitment, hello.commitment)
    }

    func testFixedFieldsTruncateWithoutSplittingCharacters() {
        let name = String(repeating: "é", count: 40)       // 80 bytes of UTF-8
        let field = HelloPayload.fixedField(name, size: 64)
        XCTAssertEqual(field.count, 64)
        XCTAssertEqual(field.last, 0, "always terminated")
        let text = field.nullTerminatedUTF8String()
        XCTAssertEqual(text, String(repeating: "é", count: 31), "whole characters only")
    }

    func testHandshakePayloadSizes() {
        XCTAssertEqual(KeyExchangePayload().serialize().count, 73)
        XCTAssertEqual(PairAcceptPayload().serialize().count, 40)
        XCTAssertEqual(PairRejectPayload(reason: .versionMismatch).serialize(), Data([3, 0, 0, 0]))
        XCTAssertEqual(SenderStatsPayload().serialize().count, SenderStatsPayload.size)
    }

    func testFrameTimingPrefixRoundTrip() {
        var p = FrameTimingPrefix()
        p.frameSequence = 77
        p.flags = FrameTimingPrefix.flagCaptureTimeValid
        p.captureUs = 1; p.encodeStartUs = 2; p.encodeEndUs = 3; p.sendUs = 4
        let bytes = p.serialize()
        XCTAssertEqual(bytes.count, FrameTimingPrefix.size)
        let back = FrameTimingPrefix.deserialize(from: bytes)
        XCTAssertEqual(back?.frameSequence, 77)
        XCTAssertEqual(back?.sendUs, 4)
        // Works on a slice too (frames are prefixes of bigger buffers)
        let slice = (Data([9, 9]) + bytes).dropFirst(2)
        XCTAssertEqual(FrameTimingPrefix.deserialize(from: slice)?.encodeEndUs, 3)
    }

    func testFragmentSizeFitsDatagram() {
        // header + counter + tag + fragment header + data ≤ 1400 bytes
        let total = ProtocolConstants.headerSize + ProtocolConstants.udpCounterSize + PacketCipher.tagSize
            + 8 + ProtocolConstants.maxFragmentPayload
        XCTAssertEqual(total, 1400)
    }
}
