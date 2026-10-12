// =============================================================================
// StreamReceiver.h — Network receiver for Windows (Winsock2)
// =============================================================================
// Connects to the Mac sender via TCP (control) and receives H.264 video
// data over UDP. Reassembles fragmented frames.
//
// Fault tolerance (Phase 15):
//   - connect timeout, exponential backoff (1→8 s), immediate retry on resume
//   - dead-peer detection: no TCP data for 15 s (Mac pings every 5 s)
//   - mDNS re-discovery when the Mac's IP changes
//   - UDP packet-loss detection → rate-limited keyframe requests
//
// Usage:
//   StreamReceiver receiver;
//   receiver.setFrameCallback([](const uint8_t* data, size_t len, bool key) { ... });
//   receiver.connect("192.168.1.x");
//   receiver.disconnect();
// =============================================================================

#pragma once

#include "Protocol.h"
#include "../Security/PairingManager.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <functional>
#include <vector>
#include <map>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>

#pragma comment(lib, "ws2_32.lib")

class StreamReceiver {
public:
    // ── Types ───────────────────────────────────────────────────────────────

    enum class State {
        Disconnected,
        Connecting,
        Connected,       // TCP connected, waiting for START_STREAM
        Streaming,       // Receiving video data
        Reconnecting,    // Connection lost, retrying
        Error
    };

    /// Called on the receive thread when a complete H.264 frame is ready.
    /// Parameters: (h264Data, dataLength, isKeyframe)
    using FrameCallback = std::function<void(const uint8_t*, size_t, bool)>;

    /// Called when stream parameters are received from sender
    using StreamInfoCallback = std::function<void(uint32_t width, uint32_t height, uint32_t fps)>;

    /// Called on the network thread after repeated connect failures (the Mac's
    /// IP may have changed). Returns the Mac's current IP, or "" if not found.
    using HostResolver = std::function<std::string()>;

    // ── Lifecycle ───────────────────────────────────────────────────────────

    StreamReceiver();
    ~StreamReceiver();

    /// Connect to the Mac sender at the given IP address.
    /// Starts a background thread for networking.
    bool connect(const std::string& hostIP);

    /// Disconnect and clean up
    void disconnect();

    /// Drop the current connection and reconnect without backoff
    /// (e.g. after the laptop resumes from sleep). Safe from any thread.
    void forceReconnect();

    /// Get current connection state
    State getState() const { return m_state.load(); }

    /// Set callback for decoded frames
    void setFrameCallback(FrameCallback cb) { m_frameCallback = std::move(cb); }

    /// Set callback for stream info
    void setStreamInfoCallback(StreamInfoCallback cb) { m_streamInfoCallback = std::move(cb); }

    /// Enable/disable auto-reconnect on connection loss (default: enabled)
    void enableAutoReconnect(bool enable) { m_autoReconnect.store(enable); }

    /// Set pairing manager for secure authentication
    void setPairingManager(PairingManager* pm) { m_pairingManager = pm; }

    /// Set the re-discovery hook used when the Mac is unreachable
    void setHostResolver(HostResolver resolver) { m_hostResolver = std::move(resolver); }

    /// Request a keyframe from the sender (decoder reset / recovery)
    void requestKeyframe() { tcpSendMessage(exdp::MessageType::KeyframeReq, nullptr, 0); }

    // ── Pairing (PIN is entered on the UI thread, never on the network thread) ──

    /// Returns true once per PIN challenge. outRequestId identifies the
    /// connection the PIN belongs to.
    bool takePinRequest(uint32_t& outRequestId);

    /// True while the Mac is still waiting for the PIN of that request
    bool isPinRequestCurrent(uint32_t requestId) const {
        return requestId != 0 && m_pinWantedFor.load() == requestId;
    }

    /// Send the PIN typed by the user. Ignored if the request went stale.
    void submitPIN(uint32_t requestId, const std::string& pin);

    // ── Stats ───────────────────────────────────────────────────────────────

    struct Stats {
        uint64_t bytesReceived = 0;
        uint64_t packetsReceived = 0;
        uint64_t packetsLost = 0;      // gaps in the UDP packet sequence
        uint64_t framesReceived = 0;
        uint64_t framesDropped = 0;    // incomplete fragments
        uint64_t fragmentsReceived = 0;
        uint64_t keyframeRequests = 0;
    };

    Stats getStats() const {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        return m_stats;
    }

    // ── Input Forwarding ────────────────────────────────────────────────────

    /// Send an input event to the Mac sender
    void sendInputEvent(const exdp::InputEventPayload& evt) {
        if (m_state.load() == State::Streaming) {
            tcpSendMessage(exdp::MessageType::InputEvent, &evt, sizeof(evt));
        }
    }

    // ── Clipboard Sync ──────────────────────────────────────────────────────

    using ClipboardCallback = std::function<void(const std::string& utf8Text)>;

    /// Set callback for clipboard data received from Mac
    void setClipboardCallback(ClipboardCallback cb) { m_clipboardCallback = std::move(cb); }

    /// Send clipboard text to the Mac sender
    void sendClipboardData(const std::string& utf8Text) {
        if (m_state.load() == State::Streaming && !utf8Text.empty()) {
            tcpSendMessage(exdp::MessageType::ClipboardData,
                           utf8Text.data(), static_cast<uint32_t>(utf8Text.size()));
        }
    }

private:
    // ── Network Thread ──────────────────────────────────────────────────────

    void networkThread(std::string hostIP);
    void runConnectionLoop(std::string hostIP);

    /// Sleep before the next connection attempt (exponential backoff,
    /// interrupted by forceReconnect / disconnect)
    void waitBeforeReconnect();

    /// TCP control channel operations
    bool tcpConnect(const std::string& hostIP);
    void tcpSendMessage(exdp::MessageType type, const void* payload, uint32_t payloadLen);
    bool tcpReceiveMessage(exdp::ProtocolHeader& header, std::vector<uint8_t>& payload);
    bool recvExact(uint8_t* dst, size_t len);
    void closeTcpSocket();
    void closeUdpSocket();

    /// UDP data channel operations
    bool udpBind();
    void udpReceiveLoop();
    void trackUdpSequence(uint32_t sequence);

    /// Handle control messages
    void handleWelcome(const std::vector<uint8_t>& payload);
    void handleStartStream(const std::vector<uint8_t>& payload);
    void handleStopStream();

    /// Handle video data
    void handleVideoFrame(const exdp::ProtocolHeader& header,
                          const uint8_t* data, size_t dataLen);
    void handleVideoFragment(const exdp::ProtocolHeader& header,
                             const uint8_t* data, size_t dataLen);
    void deliverFrame(const std::vector<uint8_t>& frameData, bool isKeyframe);

    /// Ask the Mac for a keyframe after loss (at most once per interval)
    void requestKeyframeAfterLoss();

    /// Handle pairing messages. Return false when the connection must be dropped.
    bool handlePairChallenge(const std::vector<uint8_t>& payload);
    void handlePairAccept(const std::vector<uint8_t>& payload);
    bool handlePairReject();

    // ── State ───────────────────────────────────────────────────────────────

    std::atomic<State> m_state{State::Disconnected};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_autoReconnect{true};
    std::atomic<bool> m_skipBackoff{false};     // set by forceReconnect()
    int m_reconnectAttempts = 0;                // backoff counter (network thread)
    int m_failedConnects = 0;                   // consecutive TCP connect failures

    PairingManager* m_pairingManager = nullptr;  // Not owned — must outlive this receiver
    HostResolver m_hostResolver;

    // PIN handshake state, shared between network and UI threads
    std::atomic<uint32_t> m_connectionId{0};    // incremented per TCP connection
    std::atomic<uint32_t> m_pinWantedFor{0};    // connection awaiting a PIN (0 = none)
    std::atomic<bool> m_pinRequestNew{false};   // not yet picked up by the UI

    SOCKET m_tcpSocket = INVALID_SOCKET;
    SOCKET m_udpSocket = INVALID_SOCKET;

    std::thread m_networkThread;
    std::thread m_udpThread;

    FrameCallback m_frameCallback;
    StreamInfoCallback m_streamInfoCallback;
    ClipboardCallback m_clipboardCallback;

    std::mutex m_sendMutex;   // TCP sends come from network, UI and UDP threads
    uint32_t m_sequence = 0;  // guarded by m_sendMutex

    // Dead-peer detection (network thread only)
    std::chrono::steady_clock::time_point m_lastRxTime;

    static constexpr int      CONNECT_TIMEOUT_MS  = 3000;
    static constexpr int      PEER_TIMEOUT_MS     = 15000;   // Mac pings every 5 s
    static constexpr int      MAX_BACKOFF_MS      = 8000;
    static constexpr int      REDISCOVER_AFTER    = 3;       // failed connects
    static constexpr uint32_t MAX_CONTROL_PAYLOAD = 4 * 1024 * 1024;
    static constexpr uint16_t MAX_FRAGMENTS       = 8192;    // ~11 MB frame
    static constexpr int      KEYFRAME_REQUEST_INTERVAL_MS = 500;

    // ── UDP loss detection (UDP thread only) ────────────────────────────────

    std::atomic<bool> m_udpSequenceReset{false};  // set on START_STREAM (sender restarts numbering)
    bool m_haveUdpSequence = false;
    uint32_t m_expectedUdpSequence = 0;
    std::chrono::steady_clock::time_point m_lastLossKeyframeRequest{};

    // ── Fragment Reassembly ─────────────────────────────────────────────────

    struct PendingFrame {
        uint16_t totalFragments = 0;
        uint16_t receivedCount = 0;
        bool isKeyframe = false;
        std::map<uint16_t, std::vector<uint8_t>> fragments;
    };

    std::mutex m_fragmentMutex;
    std::map<uint32_t, PendingFrame> m_pendingFrames;

    /// Drop incomplete frames that can no longer complete
    void cleanupOldFrames(uint32_t currentFrameSeq);

    // ── Stats ───────────────────────────────────────────────────────────────

    mutable std::mutex m_statsMutex;
    Stats m_stats;

    // ── Quality Reporting ───────────────────────────────────────────────────

    /// Send quality report to Mac every N seconds
    void sendQualityReport();

    // Tracking for quality reports
    uint64_t m_prevFramesReceived = 0;
    uint64_t m_prevFramesDropped = 0;
    uint64_t m_prevPacketsReceived = 0;
    uint64_t m_prevPacketsLost = 0;
    std::chrono::steady_clock::time_point m_lastQualityReport;
    static constexpr float QUALITY_REPORT_INTERVAL = 2.0f;  // seconds
};
