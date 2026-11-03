// =============================================================================
// Protocol.h — External Display streaming protocol definitions (C++)
// =============================================================================
// C++ mirror of Protocol.swift. Same wire format, same constants.
//
// Wire format (28 bytes header):
//   Magic(4) | Version(1) | Type(1) | Flags(2) |
//   Sequence(4) | Timestamp(8) | PayloadLen(4) | CRC32(4)
//
// All multi-byte integers are little-endian on the wire.
//
// Version 2: every message after the handshake is AES-256-GCM encrypted
// (FLAG_ENCRYPTED, header = associated data, CRC field 0):
//   TCP: header | ciphertext | tag(16)                 nonce counter implicit
//   UDP: header | counter(8, big-endian) | ciphertext | tag
// See Security/SecureChannel.h and docs/PROTOCOL.md.
// =============================================================================

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace exdp {

// ── Constants ───────────────────────────────────────────────────────────────

constexpr uint32_t PROTOCOL_MAGIC   = 0x50445845;  // "EXDP" little-endian
constexpr uint8_t  PROTOCOL_VERSION = 0x02;
constexpr int      HEADER_SIZE      = 28;
constexpr uint16_t TCP_PORT         = 9876;
constexpr uint16_t UDP_PORT         = 9877;
constexpr int      MAX_FRAGMENT_PAYLOAD = 1340;  // 1400 - 28 header - 8 counter - 16 tag - 8 frag
constexpr int      UDP_COUNTER_SIZE = 8;
constexpr int      AEAD_TAG_SIZE    = 16;
constexpr int      PUBLIC_KEY_SIZE  = 65;        // P-256, X9.63 uncompressed (04 | X | Y)

// ── Message Types ───────────────────────────────────────────────────────────

enum class MessageType : uint8_t {
    // Control (TCP)
    Hello         = 0x01,
    Welcome       = 0x02,
    StartStream   = 0x10,
    StopStream    = 0x11,
    Ping          = 0x20,
    Pong          = 0x21,
    KeyframeReq   = 0x30,
    QualityReport = 0x50,
    SenderStats   = 0x51,   // Mac → Win: sender performance, once per second
    RateFeedback  = 0x52,   // Win → Mac: queuing delay and received rate, every 100 ms (step 6)
    InputEvent    = 0x60,
    ClipboardData = 0x70,
    KeyExchange   = 0x80,   // Mac → Win: handshake mode + Mac's ephemeral key
    PairConfirm   = 0x81,   // Win → Mac: HMAC over the handshake transcript
    PairAccept    = 0x82,   // Mac → Win: Mac's HMAC; encryption starts after it
    PairReject    = 0x83,   // Mac → Win: handshake refused (reason)
    KeyReveal     = 0x84,   // Win → Mac: our ephemeral key (committed in HELLO)
    ReceiverCaps  = 0x90,   // Win → Mac: what this receiver can do (ReceiverCapsPayload)
    CursorShape   = 0x91,   // Mac → Win: pointer image, when it changes
    CursorPosition = 0x92,  // Mac → Win: pointer moved on the Mac
    Disconnect    = 0xFF,

    // Data (UDP)
    VideoFrame    = 0x40,
    VideoFragment = 0x41,
    VideoParity   = 0x42,   // XOR of a group of fragments: rebuilds a lost one (step 5)
};

// ── Flags ───────────────────────────────────────────────────────────────────

enum MessageFlags : uint16_t {
    FLAG_NONE     = 0,
    FLAG_KEYFRAME = 1 << 0,
    FLAG_TIMING   = 1 << 1,   // video: reassembled frame starts with FrameTimingPrefix
    FLAG_ENCRYPTED = 1 << 2,  // payload is AES-GCM sealed (protocol v2)
};

// ── Protocol Header (28 bytes) ─────────────────────────────────────────────

#pragma pack(push, 1)
struct ProtocolHeader {
    uint32_t magic       = PROTOCOL_MAGIC;
    uint8_t  version     = PROTOCOL_VERSION;
    uint8_t  type        = 0;
    uint16_t flags       = 0;
    uint32_t sequence    = 0;
    uint64_t timestamp   = 0;    // microseconds since stream start
    uint32_t payloadLength = 0;
    uint32_t crc32       = 0;
};
static_assert(sizeof(ProtocolHeader) == HEADER_SIZE, "Header must be 28 bytes");

// ── Fragment Header (8 bytes, inside VIDEO_FRAGMENT payload) ────────────────

struct FragmentHeader {
    uint32_t frameSequence = 0;   // Which frame this fragment belongs to
    uint16_t fragmentIndex = 0;   // 0-based index
    uint16_t fragmentTotal = 0;   // Total fragments
};
static_assert(sizeof(FragmentHeader) == 8, "Fragment header must be 8 bytes");

// ── Parity Header (16 bytes, inside VIDEO_PARITY payload; SMOOTHNESS.md step 5) ──
// Then the XOR of fragments groupIndex, groupIndex + groupCount, … each
// zero-padded to the longest. frameBytes gives the last fragment's length.

struct ParityHeader {
    uint32_t frameSequence = 0;
    uint16_t fragmentTotal = 0;
    uint16_t groupIndex = 0;
    uint16_t groupCount = 0;
    uint16_t reserved = 0;
    uint32_t frameBytes = 0;
};
static_assert(sizeof(ParityHeader) == 16, "Parity header must be 16 bytes");

// ── HELLO payload (162 bytes) ───────────────────────────────────────────────

struct HelloPayload {
    char     receiverName[64] = {};  // Null-terminated UTF-8
    uint16_t udpPort = UDP_PORT;
    char     deviceId[64] = {};      // Unique device identifier for pairing
    uint8_t  commitment[32] = {};    // SHA-256 of our ephemeral public key
};
static_assert(sizeof(HelloPayload) == 162, "HelloPayload must be 162 bytes");

// ── START_STREAM payload (20 bytes) ─────────────────────────────────────────

struct StartStreamPayload {
    uint32_t width   = 0;
    uint32_t height  = 0;
    uint32_t fps     = 60;
    uint32_t bitrate = 15000000;
    uint32_t codec   = 1;  // 1 = H.264
};

// ── QUALITY_REPORT payload (20 bytes, Windows → Mac) ───────────────────────────

struct QualityReportPayload {
    uint32_t packetLossPercent = 0;  // Loss × 100 (e.g. 350 = 3.50%)
    uint32_t rttMs = 0;              // Round-trip time in milliseconds (PING/PONG)
    uint32_t framesDropped = 0;      // Frames dropped since last report
    uint32_t queueDepth = 0;         // Current decode queue depth
    uint32_t latencyUs = 0;          // Avg capture→display latency (0 = unknown)
};
static_assert(sizeof(QualityReportPayload) == 20, "QualityReportPayload must be 20 bytes");

// ── Performance payloads (Phase 16) ─────────────────────────────────────────

/// Mac-side timestamps of one frame (Mac HostClock µs). Prefixes the frame
/// data of every packet flagged FLAG_TIMING; stripped before decoding.
struct FrameTimingPrefix {
    uint32_t frameSequence = 0;
    uint32_t flags = 0;              // TIMING_CAPTURE_VALID
    uint64_t captureUs = 0;          // frame was on screen (SCK displayTime)
    uint64_t encodeStartUs = 0;
    uint64_t encodeEndUs = 0;
    uint64_t sendUs = 0;             // packetization started
};
static_assert(sizeof(FrameTimingPrefix) == 40, "FrameTimingPrefix must be 40 bytes");
constexpr uint32_t TIMING_CAPTURE_VALID = 1 << 0;

/// SENDER_STATS payload (Mac → Windows, 32 bytes)
struct SenderStatsPayload {
    uint32_t captureFps100 = 0;      // × 100
    uint32_t encodeFps100 = 0;       // × 100
    uint32_t cpuPercent100 = 0;      // Mac process, % of all cores × 100
    uint32_t memoryMB = 0;
    uint32_t encoderKbps = 0;        // measured encoder output, last second
    uint32_t targetKbps = 0;
    uint32_t flags = 0;              // SENDER_HW_ENCODER
    uint32_t pacingSpreadUs = 0;     // a paced frame, first to last datagram (step 5; 0: not paced)
};
static_assert(sizeof(SenderStatsPayload) == 32, "SenderStatsPayload must be 32 bytes");
constexpr uint32_t SENDER_HW_ENCODER = 1 << 0;

// ── RATE_FEEDBACK payload (16 bytes, Windows → Mac, every 100 ms) ───────────
// docs/SMOOTHNESS.md, step 6 (Network/DelayMonitor.h)

struct RateFeedbackPayload {
    uint32_t queuingDelayUs = 0;     // median over 100 ms of (one-way delay − its 10 s minimum)
    uint32_t receivedKbps = 0;       // everything that arrived (fragments, parity), last 500 ms
    uint16_t lossPercent100 = 0;     // packets, last 500 ms
    uint8_t  state = 0;              // 0 normal (may rise), 1 hold, 2 overuse (lower now)
    uint8_t  samples = 0;            // frames behind the figure (0: no information)
    uint32_t linkKbps = 0;           // the last 100 ms if no frame got through without waiting
                                     // (the link was full: its capacity), else 0
};
static_assert(sizeof(RateFeedbackPayload) == 16, "RateFeedbackPayload must be 16 bytes");

// ── INPUT_EVENT payload (24 bytes, Windows → Mac) ─────────────────────────────

enum class InputEventType : uint8_t {
    MouseMove  = 1,
    MouseDown  = 2,
    MouseUp    = 3,
    Scroll     = 4,
    KeyDown    = 5,
    KeyUp      = 6,
};

enum InputModifiers : uint32_t {
    INPUT_MOD_NONE    = 0,
    INPUT_MOD_SHIFT   = 1 << 0,
    INPUT_MOD_CTRL    = 1 << 1,   // Maps to Cmd on Mac
    INPUT_MOD_ALT     = 1 << 2,   // Maps to Option on Mac
    INPUT_MOD_WIN     = 1 << 3,   // Maps to Ctrl on Mac
};

struct InputEventPayload {
    uint8_t  eventType  = 0;   // InputEventType
    uint8_t  button     = 0;   // 0=Left, 1=Right, 2=Middle
    uint16_t keyCode    = 0;   // Windows virtual key code
    uint32_t modifiers  = 0;   // InputModifiers bitmask
    float    x          = 0;   // Normalized X (0.0–1.0)
    float    y          = 0;   // Normalized Y (0.0–1.0)
    float    scrollDeltaX = 0; // Horizontal scroll
    float    scrollDeltaY = 0; // Vertical scroll
};
static_assert(sizeof(InputEventPayload) == 24, "InputEventPayload must be 24 bytes");

// ── Handshake Payloads (protocol v2) ────────────────────────────────────────

enum HandshakeMode : uint8_t {
    HANDSHAKE_NEW_PAIRING = 0,   // verification code + approval on the Mac
    HANDSHAKE_PAIRED      = 1,   // stored pairing key mixed into the keys
};

/// KEY_EXCHANGE payload (Mac → Windows, 73 bytes)
struct KeyExchangePayload {
    uint8_t mode = 0;                      // HandshakeMode
    uint8_t reserved[7] = {};
    uint8_t publicKey[PUBLIC_KEY_SIZE] = {};
};
static_assert(sizeof(KeyExchangePayload) == 73, "KeyExchangePayload must be 73 bytes");

/// KEY_REVEAL payload (Windows → Mac, 65 bytes)
struct KeyRevealPayload {
    uint8_t publicKey[PUBLIC_KEY_SIZE] = {};
};

/// PAIR_CONFIRM payload (Windows → Mac, 32 bytes): HMAC(confirm-win, transcript)
struct PairConfirmPayload {
    uint8_t mac[32] = {};
};

/// PAIR_ACCEPT payload (Mac → Windows, 40 bytes): HMAC(confirm-mac, transcript)
struct PairAcceptPayload {
    uint8_t newlyPaired = 0;
    uint8_t reserved[7] = {};
    uint8_t mac[32] = {};
};
static_assert(sizeof(PairAcceptPayload) == 40, "PairAcceptPayload must be 40 bytes");

enum PairRejectReason : uint8_t {
    REJECT_GENERIC          = 0,
    REJECT_USER_REJECTED    = 1,   // the Mac user clicked Reject
    REJECT_UNKNOWN_DEVICE   = 2,   // stored keys differ: pair again
    REJECT_VERSION_MISMATCH = 3,   // update both apps
    REJECT_BAD_HANDSHAKE    = 4,
};

/// PAIR_REJECT payload (Mac → Windows, 4 bytes)
struct PairRejectPayload {
    uint8_t reason = 0;
    uint8_t reserved[3] = {};
};

// ── Pointer (docs/SMOOTHNESS.md, step 1) ────────────────────────────────────

/// RECEIVER_CAPS payload (Windows → Mac, 8 bytes). Sent once the session is
/// encrypted, and again whenever it changes.
struct ReceiverCapsPayload {
    uint32_t flags = 0;
    uint32_t reserved = 0;
};
static_assert(sizeof(ReceiverCapsPayload) == 8, "ReceiverCapsPayload must be 8 bytes");
constexpr uint32_t CAP_LOCAL_CURSOR = 1 << 0;   // we draw the pointer: capture without it
constexpr uint32_t CAP_PARITY       = 1 << 1;   // send VIDEO_PARITY: we rebuild lost fragments (step 5)
constexpr uint32_t CAP_PACING       = 1 << 2;   // spread each frame's datagrams over ~1 ms (step 5)

/// CURSOR_SHAPE (Mac → Windows): this header, then width × height × 4 bytes
/// BGRA, premultiplied alpha, top row first
struct CursorShapeHeader {
    uint64_t shapeId  = 0;        // same image, same id
    uint16_t width    = 0;        // bitmap pixels (≤ MAX_CURSOR_SIDE)
    uint16_t height   = 0;
    uint16_t hotspotX = 0;        // from the top-left, bitmap pixels
    uint16_t hotspotY = 0;
    float    scale    = 1;        // stream pixels per bitmap pixel
    uint32_t reserved = 0;
};
static_assert(sizeof(CursorShapeHeader) == 24, "CursorShapeHeader must be 24 bytes");
constexpr int MAX_CURSOR_SIDE = 256;

/// CURSOR_POSITION payload (Mac → Windows, 12 bytes)
struct CursorPositionPayload {
    float    x = 0;               // 0…1 across the stream, as in INPUT_EVENT
    float    y = 0;
    uint32_t flags = 0;           // CURSOR_VISIBLE (else: on another Mac display)
};
static_assert(sizeof(CursorPositionPayload) == 12, "CursorPositionPayload must be 12 bytes");
constexpr uint32_t CURSOR_VISIBLE = 1 << 0;

#pragma pack(pop)

// ── CRC32 ───────────────────────────────────────────────────────────────────

inline uint32_t computeCRC32(const void* data, size_t length) {
    // Generate CRC32 table at first call (IEEE 802.3 polynomial)
    static uint32_t table[256] = {};
    static bool tableReady = false;
    if (!tableReady) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t crc = i;
            for (int j = 0; j < 8; j++) {
                crc = (crc & 1) ? (0xEDB88320 ^ (crc >> 1)) : (crc >> 1);
            }
            table[i] = crc;
        }
        tableReady = true;
    }

    auto bytes = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        crc = (crc >> 8) ^ table[(crc ^ bytes[i]) & 0xFF];
    }
    return crc ^ 0xFFFFFFFF;
}

// ── Header Helpers ──────────────────────────────────────────────────────────

/// Build a complete message (header + payload) ready for sending
inline std::vector<uint8_t> buildMessage(
    MessageType type,
    uint16_t flags,
    uint32_t sequence,
    uint64_t timestamp,
    const void* payload,
    uint32_t payloadLen
) {
    std::vector<uint8_t> msg(HEADER_SIZE + payloadLen);

    ProtocolHeader hdr;
    hdr.type = static_cast<uint8_t>(type);
    hdr.flags = flags;
    hdr.sequence = sequence;
    hdr.timestamp = timestamp;
    hdr.payloadLength = payloadLen;
    hdr.crc32 = 0;

    // Copy header
    std::memcpy(msg.data(), &hdr, HEADER_SIZE);

    // Copy payload
    if (payload && payloadLen > 0) {
        std::memcpy(msg.data() + HEADER_SIZE, payload, payloadLen);
    }

    // Compute CRC over entire message (with crc field = 0)
    uint32_t crc = computeCRC32(msg.data(), msg.size());
    std::memcpy(msg.data() + 24, &crc, 4);  // Offset 24 = CRC32 field

    return msg;
}

/// Parse a protocol header from raw bytes. Returns true if valid.
inline bool parseHeader(const void* data, size_t length, ProtocolHeader& out) {
    if (length < HEADER_SIZE) return false;
    std::memcpy(&out, data, HEADER_SIZE);
    return out.magic == PROTOCOL_MAGIC;
}

} // namespace exdp
