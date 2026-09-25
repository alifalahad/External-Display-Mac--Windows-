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
// =============================================================================

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace exdp {

// ── Constants ───────────────────────────────────────────────────────────────

constexpr uint32_t PROTOCOL_MAGIC   = 0x50445845;  // "EXDP" little-endian
constexpr uint8_t  PROTOCOL_VERSION = 0x01;
constexpr int      HEADER_SIZE      = 28;
constexpr uint16_t TCP_PORT         = 9876;
constexpr uint16_t UDP_PORT         = 9877;
constexpr int      MAX_FRAGMENT_PAYLOAD = 1364;  // 1400 - 28 header - 8 frag

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
    InputEvent    = 0x60,
    ClipboardData = 0x70,
    Disconnect    = 0xFF,

    // Data (UDP)
    VideoFrame    = 0x40,
    VideoFragment = 0x41,
};

// ── Flags ───────────────────────────────────────────────────────────────────

enum MessageFlags : uint16_t {
    FLAG_NONE     = 0,
    FLAG_KEYFRAME = 1 << 0,
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

// ── HELLO payload (66 bytes) ────────────────────────────────────────────────

struct HelloPayload {
    char     receiverName[64] = {};  // Null-terminated UTF-8
    uint16_t udpPort = UDP_PORT;
};

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
    uint32_t rttMs = 0;              // Round-trip time in milliseconds
    uint32_t framesDropped = 0;      // Frames dropped since last report
    uint32_t queueDepth = 0;         // Current decode queue depth
    uint32_t reserved = 0;
};

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
