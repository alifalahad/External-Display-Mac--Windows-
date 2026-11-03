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
//
// Version 2 (Phase 14 rework): every message after the handshake is encrypted
// (MessageFlags.encrypted, AES-256-GCM, header = associated data):
//   TCP: header ‖ ciphertext ‖ tag(16)               nonce counter implicit
//   UDP: header ‖ counter(8, BE) ‖ ciphertext ‖ tag   counter explicit
// The CRC field is 0 on encrypted messages: the GCM tag protects them.
// See SecureChannel.swift and docs/PROTOCOL.md.
// =============================================================================

import Foundation

// ── Constants ───────────────────────────────────────────────────────────────

enum ProtocolConstants {
    static let magic: UInt32 = 0x50445845    // "EXDP" in little-endian
    static let version: UInt8 = 0x02
    static let headerSize = 28
    static let tcpPort: UInt16 = 9876
    static let udpPort: UInt16 = 9877
    /// 1400-byte datagrams: 28 header + 8 counter + 16 tag + 8 fragment header
    static let maxFragmentPayload = 1340
    static let udpCounterSize = 8
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
    case keyExchange    = 0x80   // Mac → Win: handshake mode + Mac's ephemeral key
    case pairConfirm    = 0x81   // Win → Mac: HMAC over the handshake transcript
    case pairAccept     = 0x82   // Mac → Win: Mac's HMAC; encryption starts after it
    case pairReject     = 0x83   // Mac → Win: handshake refused (reason)
    case keyReveal      = 0x84   // Win → Mac: Windows' ephemeral key (was committed in HELLO)
    case receiverCaps   = 0x90   // Win → Mac: what the receiver can do (ReceiverCapsPayload)
    case cursorShape    = 0x91   // Mac → Win: pointer image, when it changes
    case cursorPosition = 0x92   // Mac → Win: pointer moved on the Mac
    case disconnect     = 0xFF   // Both: clean shutdown

    // Data (UDP)
    case videoFrame     = 0x40   // Complete frame in one packet
    case videoFragment  = 0x41   // Fragment of a larger frame
}

// ── Flags ───────────────────────────────────────────────────────────────────

struct MessageFlags: OptionSet {
    let rawValue: UInt16
    static let keyframe  = MessageFlags(rawValue: 1 << 0)
    /// Video: the reassembled frame starts with a FrameTimingPrefix
    static let timing    = MessageFlags(rawValue: 1 << 1)
    /// Payload is AES-GCM sealed (protocol v2, after the handshake)
    static let encrypted = MessageFlags(rawValue: 1 << 2)
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

/// HELLO payload (Windows → Mac, 162 bytes)
struct HelloPayload {
    static let size = 162

    var receiverName: String = "Windows Receiver"
    var udpPort: UInt16 = ProtocolConstants.udpPort
    var deviceId: String = ""             // identifies a paired receiver
    var commitment = Data(count: 32)      // SHA256(Windows' ephemeral public key)

    func serialize() -> Data {
        var data = Data()
        data.append(Self.fixedField(receiverName, size: 64))
        data.appendLE(udpPort)
        data.append(Self.fixedField(deviceId, size: 64))
        data.append(commitment.prefix(32))
        data.append(Data(count: 32 - min(commitment.count, 32)))
        return data
    }

    static func deserialize(from data: Data) -> HelloPayload? {
        guard data.count >= size else { return nil }
        var h = HelloPayload()
        let base = data.startIndex
        h.receiverName = data.subdata(in: base..<base + 64).nullTerminatedUTF8String()
        var offset = base + 64
        h.udpPort = data.readLE(at: &offset)
        h.deviceId = data.subdata(in: base + 66..<base + 130).nullTerminatedUTF8String()
        h.commitment = data.subdata(in: base + 130..<base + 162)
        return h
    }

    /// Null-terminated, zero-padded UTF-8 (never splits a character)
    static func fixedField(_ text: String, size: Int) -> Data {
        var bytes = Data()
        for ch in text {
            let utf8 = Data(String(ch).utf8)
            if bytes.count + utf8.count > size - 1 { break }
            bytes.append(utf8)
        }
        bytes.append(Data(count: size - bytes.count))
        return bytes
    }
}

// ── Handshake Payloads (protocol v2) ────────────────────────────────────────

/// Handshake mode chosen by the Mac
enum HandshakeMode: UInt8 {
    case newPairing = 0   // unknown device: verification code + user approval
    case paired     = 1   // stored pairing key mixed into the keys
}

/// KEY_EXCHANGE payload (Mac → Windows, 73 bytes)
struct KeyExchangePayload {
    static let size = 8 + KeySchedule.publicKeySize

    var mode: HandshakeMode = .newPairing
    var publicKey = Data(count: KeySchedule.publicKeySize)

    func serialize() -> Data {
        var data = Data([mode.rawValue])
        data.append(Data(count: 7))
        data.append(publicKey)
        return data
    }
}

/// KEY_REVEAL payload (Windows → Mac, 65 bytes)
struct KeyRevealPayload {
    var publicKey = Data()

    static func deserialize(from data: Data) -> KeyRevealPayload? {
        guard data.count >= KeySchedule.publicKeySize else { return nil }
        return KeyRevealPayload(publicKey: Data(data.prefix(KeySchedule.publicKeySize)))
    }
}

/// PAIR_CONFIRM payload (Windows → Mac, 32 bytes): HMAC(confirm-win, TH)
struct PairConfirmPayload {
    var mac = Data()

    static func deserialize(from data: Data) -> PairConfirmPayload? {
        guard data.count >= 32 else { return nil }
        return PairConfirmPayload(mac: Data(data.prefix(32)))
    }
}

/// PAIR_ACCEPT payload (Mac → Windows, 40 bytes): HMAC(confirm-mac, TH)
struct PairAcceptPayload {
    var newlyPaired: Bool = false
    var mac = Data(count: 32)

    func serialize() -> Data {
        var data = Data([newlyPaired ? 1 : 0])
        data.append(Data(count: 7))
        data.append(mac.prefix(32))
        return data
    }
}

/// PAIR_REJECT reasons
enum PairRejectReason: UInt8 {
    case generic          = 0
    case userRejected     = 1   // Mac user clicked Reject / wrong code
    case unknownDevice    = 2   // stored keys don't match: pair again
    case versionMismatch  = 3   // update both apps
    case badHandshake     = 4   // malformed handshake / commitment mismatch
}

/// PAIR_REJECT payload (Mac → Windows, 4 bytes)
struct PairRejectPayload {
    var reason: PairRejectReason = .generic

    func serialize() -> Data {
        Data([reason.rawValue, 0, 0, 0])
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

// ── Pointer (docs/SMOOTHNESS.md, step 1) ────────────────────────────────────

/// RECEIVER_CAPS payload (Windows → Mac, 8 bytes): flags(4) | reserved(4).
/// Sent once the session is encrypted, and again whenever it changes.
struct ReceiverCapsPayload {
    /// The receiver draws the pointer: capture without it, send CURSOR_SHAPE
    /// and CURSOR_POSITION instead
    static let localCursor: UInt32 = 1 << 0

    var flags: UInt32 = 0

    static func deserialize(from data: Data) -> ReceiverCapsPayload? {
        guard data.count >= 4 else { return nil }
        var offset = data.startIndex
        return ReceiverCapsPayload(flags: data.readLE(at: &offset))
    }
}

/// CURSOR_SHAPE payload (Mac → Windows): 24-byte header, then the bitmap
///
///   shapeId(8) | width(2) | height(2) | hotspotX(2) | hotspotY(2) |
///   scale(4, float) | reserved(4) | pixels(width × height × 4)
///
/// Pixels are BGRA with premultiplied alpha, top row first. The hot spot is
/// in bitmap pixels from the top-left. scale = stream pixels per bitmap pixel
/// (the bitmap is drawn at 2× so the receiver can scale it down sharply).
struct CursorShapePayload: Equatable {
    static let headerSize = 24
    static let maxSide = 256

    var id: UInt64 = 0
    var width: UInt16 = 0
    var height: UInt16 = 0
    var hotspotX: UInt16 = 0
    var hotspotY: UInt16 = 0
    var scale: Float = 1
    var pixels = Data()

    func serialize() -> Data {
        var data = Data(capacity: Self.headerSize + pixels.count)
        data.appendLE(id)
        data.appendLE(width)
        data.appendLE(height)
        data.appendLE(hotspotX)
        data.appendLE(hotspotY)
        data.appendLE(scale.bitPattern)
        data.appendLE(UInt32(0))
        data.append(pixels)
        return data
    }

    /// FNV-1a over everything except the id: the same image gets the same id
    func computeId() -> UInt64 {
        var hash: UInt64 = 0xcbf29ce484222325
        func mix(_ bytes: Data) {
            for b in bytes { hash = (hash ^ UInt64(b)) &* 0x100000001b3 }
        }
        mix(Data(serialize().dropFirst(8)))
        return hash
    }
}

/// CURSOR_POSITION payload (Mac → Windows, 12 bytes): x(4) | y(4) | flags(4).
/// x, y are 0…1 across the streamed display, as in INPUT_EVENT. Not visible:
/// the pointer is on another display (x = y = 0).
struct CursorPositionPayload: Equatable {
    static let size = 12
    static let visible: UInt32 = 1 << 0

    var x: Float = 0
    var y: Float = 0
    var flags: UInt32 = 0

    func serialize() -> Data {
        var data = Data(capacity: Self.size)
        data.appendLE(x.bitPattern)
        data.appendLE(y.bitPattern)
        data.appendLE(flags)
        return data
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
