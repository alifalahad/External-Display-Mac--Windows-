// =============================================================================
// Protocol.swift — External Display streaming protocol definitions
// =============================================================================
// Shared protocol constants, message types, header format, and
// serialization used by both control (TCP) and data (UDP) channels.
//
// Wire format (28 bytes header):
//   Magic(4) | Version(1) | Type(1) | Flags(2) |
//   Sequence(4) | Timestamp(8) | PayloadLen(4) | CRC32(4)
//
// All multi-byte integers are little-endian on the wire.
// =============================================================================

import Foundation

// ── Constants ───────────────────────────────────────────────────────────────

enum ProtocolConstants {
    static let magic: UInt32 = 0x50445845    // "EXDP" in little-endian
    static let version: UInt8 = 0x01
    static let headerSize = 28
    static let tcpPort: UInt16 = 9876
    static let udpPort: UInt16 = 9877
    static let maxFragmentPayload = 1364     // 1400 - 28 header - 8 frag header
    static let bonjourType = "_externaldisplay._tcp"
}

// ── Message Types ───────────────────────────────────────────────────────────

enum MessageType: UInt8 {
    // Control (TCP)
    case hello          = 0x01   // Win → Mac: announce with UDP port
    case welcome        = 0x02   // Mac → Win: ack with display info
    case startStream    = 0x10   // Mac → Win: begin streaming
    case stopStream     = 0x11   // Mac → Win: stop streaming
    case ping           = 0x20   // Both: latency probe
    case pong           = 0x21   // Both: latency reply
    case keyframeReq    = 0x30   // Win → Mac: request I-frame
    case disconnect     = 0xFF   // Both: clean shutdown

    // Data (UDP)
    case videoFrame     = 0x40   // Complete frame in one packet
    case videoFragment  = 0x41   // Fragment of a larger frame
}

// ── Flags ───────────────────────────────────────────────────────────────────

struct MessageFlags: OptionSet {
    let rawValue: UInt16
    static let keyframe = MessageFlags(rawValue: 1 << 0)
}

// ── Protocol Header ────────────────────────────────────────────────────────

struct ProtocolHeader {
    var magic: UInt32 = ProtocolConstants.magic
    var version: UInt8 = ProtocolConstants.version
    var type: UInt8 = 0
    var flags: UInt16 = 0
    var sequence: UInt32 = 0
    var timestamp: UInt64 = 0       // microseconds since stream start
    var payloadLength: UInt32 = 0
    var crc32: UInt32 = 0

    /// Serialize header to 28-byte Data (little-endian)
    func serialize() -> Data {
        var data = Data(capacity: ProtocolConstants.headerSize)
        data.appendLE(magic)
        data.append(version)
        data.append(type)
        data.appendLE(flags)
        data.appendLE(sequence)
        data.appendLE(timestamp)
        data.appendLE(payloadLength)
        data.appendLE(crc32)
        return data
    }

    /// Deserialize from 28-byte Data
    static func deserialize(from data: Data) -> ProtocolHeader? {
        guard data.count >= ProtocolConstants.headerSize else { return nil }
        var h = ProtocolHeader()
        var offset = 0
        h.magic = data.readLE(at: &offset)
        h.version = data[offset]; offset += 1
        h.type = data[offset]; offset += 1
        h.flags = data.readLE(at: &offset)
        h.sequence = data.readLE(at: &offset)
        h.timestamp = data.readLE(at: &offset)
        h.payloadLength = data.readLE(at: &offset)
        h.crc32 = data.readLE(at: &offset)

        guard h.magic == ProtocolConstants.magic else { return nil }
        return h
    }
}

// ── Fragment Header (8 bytes, inside VIDEO_FRAGMENT payload) ────────────────

struct FragmentHeader {
    var frameSequence: UInt32 = 0   // Which frame this fragment belongs to
    var fragmentIndex: UInt16 = 0   // 0-based index of this fragment
    var fragmentTotal: UInt16 = 0   // Total fragments in this frame

    func serialize() -> Data {
        var data = Data(capacity: 8)
        data.appendLE(frameSequence)
        data.appendLE(fragmentIndex)
        data.appendLE(fragmentTotal)
        return data
    }

    static func deserialize(from data: Data) -> FragmentHeader? {
        guard data.count >= 8 else { return nil }
        var h = FragmentHeader()
        var offset = 0
        h.frameSequence = data.readLE(at: &offset)
        h.fragmentIndex = data.readLE(at: &offset)
        h.fragmentTotal = data.readLE(at: &offset)
        return h
    }
}

// ── Control Message Payloads ────────────────────────────────────────────────

/// HELLO payload (Windows → Mac)
struct HelloPayload {
    var receiverName: String = "Windows Receiver"
    var udpPort: UInt16 = ProtocolConstants.udpPort

    func serialize() -> Data {
        var data = Data()
        // Name as null-terminated UTF-8, max 64 bytes
        let nameData = receiverName.utf8.prefix(63)
        data.append(contentsOf: nameData)
        data.append(0) // null terminator
        // Pad to 64 bytes
        data.append(contentsOf: [UInt8](repeating: 0, count: 64 - data.count))
        data.appendLE(udpPort)
        return data
    }

    static func deserialize(from data: Data) -> HelloPayload? {
        guard data.count >= 66 else { return nil }
        var h = HelloPayload()
        h.receiverName = data.prefix(64).withUnsafeBytes { buf in
            String(cString: buf.baseAddress!.assumingMemoryBound(to: CChar.self))
        }
        var offset = 64
        h.udpPort = data.readLE(at: &offset)
        return h
    }
}

/// START_STREAM payload (Mac → Windows)
struct StartStreamPayload {
    var width: UInt32 = 0
    var height: UInt32 = 0
    var fps: UInt32 = 60
    var bitrate: UInt32 = 15_000_000
    var codec: UInt32 = 1  // 1 = H.264

    func serialize() -> Data {
        var data = Data(capacity: 20)
        data.appendLE(width)
        data.appendLE(height)
        data.appendLE(fps)
        data.appendLE(bitrate)
        data.appendLE(codec)
        return data
    }

    static func deserialize(from data: Data) -> StartStreamPayload? {
        guard data.count >= 20 else { return nil }
        var s = StartStreamPayload()
        var offset = 0
        s.width = data.readLE(at: &offset)
        s.height = data.readLE(at: &offset)
        s.fps = data.readLE(at: &offset)
        s.bitrate = data.readLE(at: &offset)
        s.codec = data.readLE(at: &offset)
        return s
    }
}

// ── CRC32 ───────────────────────────────────────────────────────────────────

enum CRC32 {
    /// Standard CRC32 lookup table (IEEE 802.3 polynomial)
    private static let table: [UInt32] = {
        (0..<256).map { i -> UInt32 in
            var crc = UInt32(i)
            for _ in 0..<8 {
                crc = (crc & 1 != 0) ? (0xEDB88320 ^ (crc >> 1)) : (crc >> 1)
            }
            return crc
        }
    }()

    static func compute(_ data: Data) -> UInt32 {
        var crc: UInt32 = 0xFFFFFFFF
        for byte in data {
            let index = Int((crc ^ UInt32(byte)) & 0xFF)
            crc = (crc >> 8) ^ table[index]
        }
        return crc ^ 0xFFFFFFFF
    }
}

// ── Data Helpers ────────────────────────────────────────────────────────────

extension Data {
    mutating func appendLE<T: FixedWidthInteger>(_ value: T) {
        var le = value.littleEndian
        append(UnsafeBufferPointer(start: &le, count: 1))
    }

    func readLE<T: FixedWidthInteger>(at offset: inout Int) -> T {
        let size = MemoryLayout<T>.size
        let value = self.subdata(in: offset..<offset+size)
            .withUnsafeBytes { $0.load(as: T.self) }
        offset += size
        return T(littleEndian: value)
    }
}
