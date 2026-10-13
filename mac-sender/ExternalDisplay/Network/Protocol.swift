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
    case qualityReport  = 0x50   // Win → Mac: network health report
    case senderStats    = 0x51   // Mac → Win: sender-side performance (1 Hz)
    case inputEvent     = 0x60   // Win → Mac: mouse/keyboard input
    case clipboardData  = 0x70   // Both: clipboard text sync
    case pairChallenge  = 0x80   // Mac → Win: pairing challenge
    case pairResponse   = 0x81   // Win → Mac: pairing response (PIN or HMAC)
    case pairAccept     = 0x82   // Mac → Win: pairing accepted
    case pairReject     = 0x83   // Mac → Win: pairing rejected
    case disconnect     = 0xFF   // Both: clean shutdown

    // Data (UDP)
    case videoFrame     = 0x40   // Complete frame in one packet
    case videoFragment  = 0x41   // Fragment of a larger frame
}

// ── Flags ───────────────────────────────────────────────────────────────────

struct MessageFlags: OptionSet {
    let rawValue: UInt16
    static let keyframe = MessageFlags(rawValue: 1 << 0)
    /// Video: the reassembled frame starts with a FrameTimingPrefix
    static let timing   = MessageFlags(rawValue: 1 << 1)
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

/// HELLO payload (Windows → Mac, 130 bytes)
struct HelloPayload {
    var receiverName: String = "Windows Receiver"
    var udpPort: UInt16 = ProtocolConstants.udpPort
    var deviceId: String = ""   // Unique device identifier for pairing

    func serialize() -> Data {
        var data = Data()
        // Name as null-terminated UTF-8, max 64 bytes
        let nameData = receiverName.utf8.prefix(63)
        data.append(contentsOf: nameData)
        data.append(0) // null terminator
        // Pad to 64 bytes
        data.append(contentsOf: [UInt8](repeating: 0, count: 64 - data.count))
        data.appendLE(udpPort)
        // Device ID as null-terminated UTF-8, 64 bytes
        let idData = deviceId.utf8.prefix(63)
        data.append(contentsOf: idData)
        data.append(0)
        let idPadding = 64 - (idData.count + 1)
        if idPadding > 0 {
            data.append(contentsOf: [UInt8](repeating: 0, count: idPadding))
        }
        return data
    }

    static func deserialize(from data: Data) -> HelloPayload? {
        guard data.count >= 66 else { return nil }
        var h = HelloPayload()
        let base = data.startIndex
        h.receiverName = data.subdata(in: base..<base + 64).nullTerminatedUTF8String()
        var offset = 64
        h.udpPort = data.readLE(at: &offset)
        // Device ID (added in Phase 14) — empty if the receiver is too old to send it
        if data.count >= 130 {
            h.deviceId = data.subdata(in: base + 66..<base + 130).nullTerminatedUTF8String()
        }
        return h
    }
}

// ── Pairing Payloads ────────────────────────────────────────────────────────

/// Challenge types for PAIR_CHALLENGE
enum PairChallengeType: UInt8 {
    case pin  = 0   // First-time pairing: user must enter PIN
    case hmac = 1   // Already paired: HMAC challenge-response
}

/// PAIR_CHALLENGE payload (Mac → Windows, 40 bytes)
struct PairChallengePayload {
    var type: UInt8 = 0          // PairChallengeType
    var nonce: Data = Data(repeating: 0, count: 32)  // 32-byte random nonce

    func serialize() -> Data {
        var data = Data(capacity: 40)
        data.append(type)
        data.append(contentsOf: [UInt8](repeating: 0, count: 7))  // reserved
        data.append(nonce.prefix(32))
        // Pad nonce if needed
        if nonce.count < 32 {
            data.append(contentsOf: [UInt8](repeating: 0, count: 32 - nonce.count))
        }
        return data
    }

    static func deserialize(from data: Data) -> PairChallengePayload? {
        guard data.count >= 40 else { return nil }
        var p = PairChallengePayload()
        p.type = data[0]
        p.nonce = data.subdata(in: 8..<40)
        return p
    }
}

/// PAIR_RESPONSE payload (Windows → Mac, 40 bytes)
struct PairResponsePayload {
    var type: UInt8 = 0          // 0 = PIN response, 1 = HMAC response
    var data_: Data = Data(repeating: 0, count: 32)  // PIN (null-padded) or HMAC-SHA256

    func serialize() -> Data {
        var data = Data(capacity: 40)
        data.append(type)
        data.append(contentsOf: [UInt8](repeating: 0, count: 7))  // reserved
        data.append(data_.prefix(32))
        if data_.count < 32 {
            data.append(contentsOf: [UInt8](repeating: 0, count: 32 - data_.count))
        }
        return data
    }

    static func deserialize(from data: Data) -> PairResponsePayload? {
        guard data.count >= 40 else { return nil }
        var p = PairResponsePayload()
        p.type = data[0]
        p.data_ = data.subdata(in: 8..<40)
        return p
    }

    /// Extract PIN string from data_ field
    var pinString: String {
        data_.nullTerminatedUTF8String()
    }
}

/// PAIR_ACCEPT payload (Mac → Windows, 40 bytes)
struct PairAcceptPayload {
    var newlyPaired: UInt8 = 0   // 1 = new pairing (key included), 0 = existing
    var pairingKey: Data = Data(repeating: 0, count: 32)  // 256-bit pairing key

    func serialize() -> Data {
        var data = Data(capacity: 40)
        data.append(newlyPaired)
        data.append(contentsOf: [UInt8](repeating: 0, count: 7))  // reserved
        data.append(pairingKey.prefix(32))
        if pairingKey.count < 32 {
            data.append(contentsOf: [UInt8](repeating: 0, count: 32 - pairingKey.count))
        }
        return data
    }

    static func deserialize(from data: Data) -> PairAcceptPayload? {
        guard data.count >= 40 else { return nil }
        var p = PairAcceptPayload()
        p.newlyPaired = data[0]
        p.pairingKey = data.subdata(in: 8..<40)
        return p
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
/// QUALITY_REPORT payload (Windows → Mac, 20 bytes)
struct QualityReportPayload {
    var packetLossPercent: UInt32 = 0   // Loss × 100 (e.g. 350 = 3.50%)
    var rttMs: UInt32 = 0               // Round-trip time in ms (PING/PONG)
    var framesDropped: UInt32 = 0       // Frames dropped since last report
    var queueDepth: UInt32 = 0          // Current decode queue depth
    var latencyUs: UInt32 = 0           // Avg capture→display latency (0 = unknown)

    static func deserialize(from data: Data) -> QualityReportPayload? {
        guard data.count >= 20 else { return nil }
        var q = QualityReportPayload()
        var offset = 0
        q.packetLossPercent = data.readLE(at: &offset)
        q.rttMs = data.readLE(at: &offset)
        q.framesDropped = data.readLE(at: &offset)
        q.queueDepth = data.readLE(at: &offset)
        q.latencyUs = data.readLE(at: &offset)
        return q
    }
}

// ── Performance Payloads (Phase 16) ─────────────────────────────────────────

/// Mac-side timestamps of one video frame, all in HostClock microseconds.
/// Sent as a 40-byte prefix of the frame data when the packets carry
/// MessageFlags.timing; the receiver strips it before decoding.
///
///   frameSequence(4) | flags(4) | captureUs(8) | encodeStartUs(8) |
///   encodeEndUs(8) | sendUs(8)
struct FrameTimingPrefix {
    static let size = 40
    static let flagCaptureTimeValid: UInt32 = 1 << 0

    var frameSequence: UInt32 = 0
    var flags: UInt32 = 0
    var captureUs: UInt64 = 0       // when the frame was displayed (SCK displayTime)
    var encodeStartUs: UInt64 = 0   // handed to VideoToolbox
    var encodeEndUs: UInt64 = 0     // VideoToolbox output callback
    var sendUs: UInt64 = 0          // packetization started

    func serialize() -> Data {
        var data = Data(capacity: Self.size)
        data.appendLE(frameSequence)
        data.appendLE(flags)
        data.appendLE(captureUs)
        data.appendLE(encodeStartUs)
        data.appendLE(encodeEndUs)
        data.appendLE(sendUs)
        return data
    }

    static func deserialize(from data: Data) -> FrameTimingPrefix? {
        guard data.count >= size else { return nil }
        var p = FrameTimingPrefix()
        var offset = data.startIndex
        p.frameSequence = data.readLE(at: &offset)
        p.flags = data.readLE(at: &offset)
        p.captureUs = data.readLE(at: &offset)
        p.encodeStartUs = data.readLE(at: &offset)
        p.encodeEndUs = data.readLE(at: &offset)
        p.sendUs = data.readLE(at: &offset)
        return p
    }
}

/// SENDER_STATS payload (Mac → Windows, 32 bytes, once per second)
struct SenderStatsPayload {
    static let size = 32
    static let flagHardwareEncoder: UInt32 = 1 << 0

    var captureFps100: UInt32 = 0     // capture FPS × 100
    var encodeFps100: UInt32 = 0      // encoded FPS × 100
    var cpuPercent100: UInt32 = 0     // sender process CPU, % of all cores × 100
    var memoryMB: UInt32 = 0          // sender process physical footprint
    var encoderKbps: UInt32 = 0       // measured encoder output, last second
    var targetKbps: UInt32 = 0        // bitrate the encoder is asked for
    var flags: UInt32 = 0
    var reserved: UInt32 = 0

    func serialize() -> Data {
        var data = Data(capacity: Self.size)
        for value in [captureFps100, encodeFps100, cpuPercent100, memoryMB,
                      encoderKbps, targetKbps, flags, reserved] {
            data.appendLE(value)
        }
        return data
    }
}

/// INPUT_EVENT types
enum InputEventType: UInt8 {
    case mouseMove  = 1
    case mouseDown  = 2
    case mouseUp    = 3
    case scroll     = 4
    case keyDown    = 5
    case keyUp      = 6
}

/// INPUT_EVENT modifier flags
struct InputModifiers: OptionSet {
    let rawValue: UInt32
    static let shift = InputModifiers(rawValue: 1 << 0)
    static let ctrl  = InputModifiers(rawValue: 1 << 1)  // Maps to Cmd on Mac
    static let alt   = InputModifiers(rawValue: 1 << 2)  // Maps to Option on Mac
    static let win   = InputModifiers(rawValue: 1 << 3)  // Maps to Ctrl on Mac
}

/// INPUT_EVENT payload (Windows → Mac, 24 bytes)
struct InputEventPayload {
    var eventType: UInt8 = 0     // InputEventType
    var button: UInt8 = 0        // 0=Left, 1=Right, 2=Middle
    var keyCode: UInt16 = 0      // Windows virtual key code
    var modifiers: UInt32 = 0    // InputModifiers bitmask
    var x: Float = 0             // Normalized X (0.0–1.0)
    var y: Float = 0             // Normalized Y (0.0–1.0)
    var scrollDeltaX: Float = 0
    var scrollDeltaY: Float = 0

    static func deserialize(from data: Data) -> InputEventPayload? {
        guard data.count >= 24 else { return nil }
        var p = InputEventPayload()
        p.eventType = data[0]
        p.button = data[1]
        var offset = 2
        p.keyCode = data.readLE(at: &offset)
        p.modifiers = data.readLE(at: &offset)
        // Read floats as UInt32 bit patterns
        let xBits: UInt32 = data.readLE(at: &offset)
        let yBits: UInt32 = data.readLE(at: &offset)
        let sxBits: UInt32 = data.readLE(at: &offset)
        let syBits: UInt32 = data.readLE(at: &offset)
        p.x = Float(bitPattern: xBits)
        p.y = Float(bitPattern: yBits)
        p.scrollDeltaX = Float(bitPattern: sxBits)
        p.scrollDeltaY = Float(bitPattern: syBits)
        return p
    }
}

// ── CRC32 ───────────────────────────────────────────────────────────────────

enum CRC32 {
    /// Slicing-by-8 tables for the IEEE 802.3 polynomial (table k at k*256).
    /// Same result as the classic byte-at-a-time CRC-32, ~8× faster: computed
    /// for every UDP packet (~3,600 packets/s at 40 Mbps).
    private static let tables: [UInt32] = {
        var t = [UInt32](repeating: 0, count: 256 * 8)
        for i in 0..<256 {
            var crc = UInt32(i)
            for _ in 0..<8 {
                crc = (crc & 1 != 0) ? (0xEDB88320 ^ (crc >> 1)) : (crc >> 1)
            }
            t[i] = crc
        }
        for i in 0..<256 {
            for k in 1..<8 {
                let prev = t[(k - 1) * 256 + i]
                t[k * 256 + i] = (prev >> 8) ^ t[Int(prev & 0xFF)]
            }
        }
        return t
    }()

    static func compute(_ data: Data) -> UInt32 {
        data.withUnsafeBytes { raw -> UInt32 in
            tables.withUnsafeBufferPointer { t -> UInt32 in
                var crc: UInt32 = 0xFFFFFFFF
                guard var p = raw.bindMemory(to: UInt8.self).baseAddress else { return 0 }
                var remaining = raw.count
                while remaining >= 8 {
                    let lo = (UInt32(p[0]) | UInt32(p[1]) << 8 | UInt32(p[2]) << 16
                              | UInt32(p[3]) << 24) ^ crc
                    let hi = UInt32(p[4]) | UInt32(p[5]) << 8 | UInt32(p[6]) << 16
                             | UInt32(p[7]) << 24
                    crc = t[7 * 256 + Int(lo & 0xFF)] ^ t[6 * 256 + Int((lo >> 8) & 0xFF)]
                        ^ t[5 * 256 + Int((lo >> 16) & 0xFF)] ^ t[4 * 256 + Int(lo >> 24)]
                        ^ t[3 * 256 + Int(hi & 0xFF)] ^ t[2 * 256 + Int((hi >> 8) & 0xFF)]
                        ^ t[256 + Int((hi >> 16) & 0xFF)] ^ t[Int(hi >> 24)]
                    p += 8
                    remaining -= 8
                }
                while remaining > 0 {
                    crc = (crc >> 8) ^ t[Int((crc ^ UInt32(p[0])) & 0xFF)]
                    p += 1
                    remaining -= 1
                }
                return crc ^ 0xFFFFFFFF
            }
        }
    }
}

// ── Data Helpers ────────────────────────────────────────────────────────────

extension Data {
    mutating func appendLE<T: FixedWidthInteger>(_ value: T) {
        Swift.withUnsafeBytes(of: value.littleEndian) { append(contentsOf: $0) }
    }

    func readLE<T: FixedWidthInteger>(at offset: inout Int) -> T {
        let size = MemoryLayout<T>.size
        let value = self.subdata(in: offset..<offset+size)
            .withUnsafeBytes { $0.loadUnaligned(as: T.self) }
        offset += size
        return T(littleEndian: value)
    }

    /// Decode a fixed-size, null-padded UTF-8 field without reading past its end
    /// (String(cString:) would overrun if the peer omits the terminator).
    func nullTerminatedUTF8String() -> String {
        let bytes = prefix { $0 != 0 }
        return String(decoding: bytes, as: UTF8.self)
    }
}
