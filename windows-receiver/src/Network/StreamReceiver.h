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
// Performance (Phase 16):
//   - strips the per-frame timing prefix into a FrameTimeline
//   - PING/PONG clock sync (1 Hz) → RTT, Mac↔PC clock offset
//   - interarrival jitter, frames expected from Mac frame numbers
//   - keeps the latest SENDER_STATS for the diagnostics overlay
//
// Usage:
//   StreamReceiver receiver;
//   receiver.setFrameCallback([](const uint8_t* data, size_t len, const FrameTimeline& t) { ... });
//   receiver.connect("192.168.1.x");
//   receiver.disconnect();
// =============================================================================

#pragma once

#include "Protocol.h"
#include "DelayMonitor.h"
#include "FrameAssembler.h"
#include "NetworkSimulator.h"
#include "../Security/PairingManager.h"
#include "../Security/SecureChannel.h"
#include "../Performance/ClockSync.h"
#include "../Performance/PipelineMetrics.h"
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
    /// The timeline has the Mac timestamps and the arrival times filled in.
    using FrameCallback = std::function<void(const uint8_t*, size_t, const FrameTimeline&)>;

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

    /// Request a keyframe from the sender (decoder reset / recovery). The Mac
    /// sends keyframes only when asked (docs/SMOOTHNESS.md, step 2). Any thread.
    void requestKeyframe();

    // ── Pairing (the code is typed on the UI thread, never on the network thread) ──

    /// Returns true once per new pairing. outRequestId identifies the
    /// connection the verification code belongs to.
    bool takeCodeRequest(uint32_t& outRequestId);

    /// True while that pairing is still waiting for the code
    bool isCodeRequestCurrent(uint32_t requestId) const {
        return requestId != 0 && m_pinWantedFor.load() == requestId;
    }

    /// A new pairing is waiting for the user to type the code
    bool isAwaitingCode() const { return m_pinWantedFor.load() != 0; }

    enum class CodeResult { Sent, Mismatch, Expired };

    /// Check the code the user typed against the one this side derived from
    /// the handshake. A match is confirmed to the Mac; a mismatch means a
    /// typo or someone in the middle — the connection restarts with a new code.
    CodeResult submitVerificationCode(uint32_t requestId, const std::string& code);

    /// A pairing problem the user should see (empty if none); cleared on read
    std::string takeUserMessage();

    /// The session is authenticated and encrypted
    bool isSecure() const { return std::atomic_load(&m_session) != nullptr; }

    // ── Stats ───────────────────────────────────────────────────────────────

    struct Stats {
        uint64_t bytesReceived = 0;
        uint64_t packetsReceived = 0;
        uint64_t packetsLost = 0;      // gaps in the UDP packet sequence
        uint64_t framesReceived = 0;
        uint64_t framesDropped = 0;    // incomplete fragments
        uint64_t fragmentsReceived = 0;
        uint64_t keyframeRequests = 0;   // every KEYFRAME_REQ sent
        uint64_t keyframesReceived = 0;  // complete keyframes
        uint64_t parityPackets = 0;      // docs/SMOOTHNESS.md, step 5
        uint64_t fragmentsRebuilt = 0;   // lost fragments rebuilt from parity
        uint64_t framesRebuilt = 0;      // frames that needed it
        uint32_t queuingDelayUs = 0;     // step 6: the latest RATE_FEEDBACK figure
        RateState rateState = RateState::Normal;
        uint64_t framesExpected = 0;   // from Mac frame numbers (gaps = whole frames lost)
        double   jitterMs = 0;         // RFC 3550 interarrival jitter
    };

    Stats getStats() const {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        return m_stats;
    }

    // ── Performance (Phase 16) ──────────────────────────────────────────────

    /// Mac↔PC clock offset and RTT from PING/PONG
    ClockSync::Estimate clockEstimate() const { return m_clockSync.estimate(); }

    /// Latest SENDER_STATS from the Mac (valid = false if none in the last 3 s)
    SenderSample senderStats() const;

    /// Average capture→display latency to report back to the Mac (0 = unknown)
    void setReportedLatencyUs(uint32_t us) { m_reportedLatencyUs.store(us); }

    // ── Input Forwarding ────────────────────────────────────────────────────

    /// Send an input event to the Mac sender
    void sendInputEvent(const exdp::InputEventPayload& evt) {
        if (m_state.load() == State::Streaming) {
            tcpSendMessage(exdp::MessageType::InputEvent, &evt, sizeof(evt));
        }
    }

    // ── Pointer (docs/SMOOTHNESS.md, step 1) ────────────────────────────────

    /// What we tell the Mac once the session is encrypted (RECEIVER_CAPS,
    /// exdp::CAP_*). A change during a session is sent at once. Any thread.
    void setReceiverCaps(uint32_t flags);

    /// Drop incoming video packets on purpose (--simulate-loss: tests loss
    /// repair on a good network). Call before connect().
    void simulateLoss(double percent, int burst) { m_lossSimulator.configure(percent, burst); }
    /// A narrow link in front of the receiver (--simulate-link): tests the
    /// bitrate that follows the delay. Call before connect().
    void simulateLink(double kbps, double bufferMs) { m_linkSimulator.configure(kbps, bufferMs); }

    /// Report the queuing delay to the Mac every 100 ms (RATE_FEEDBACK,
    /// docs/SMOOTHNESS.md step 6). Off: the Mac keeps its loss/RTT tiers.
    void enableRateFeedback(bool on) { m_rateFeedback.store(on); }

    /// CURSOR_SHAPE / CURSOR_POSITION payloads (decrypted), network thread
    using PointerCallback = std::function<void(const uint8_t* data, size_t len)>;
    void setPointerCallbacks(PointerCallback shape, PointerCallback position) {
        m_cursorShapeCallback = std::move(shape);
        m_cursorPositionCallback = std::move(position);
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
    void tcpSendMessage(exdp::MessageType type, const void* payload, uint32_t payloadLen,
                        uint64_t timestamp = 0);
    bool tcpReceiveMessage(exdp::ProtocolHeader& header, uint8_t rawHeader[exdp::HEADER_SIZE],
                           std::vector<uint8_t>& payload);
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

    /// Handle video data (rxUs = arrival time of this packet, PipelineClock)
    /// One datagram as it arrives (or leaves the simulated link): authenticate,
    /// count, measure the delay, then hand the video on
    void handleDatagram(const uint8_t* data, size_t n, int64_t rxUs, std::vector<uint8_t>& plaintext);
    void handleVideoFrame(const exdp::ProtocolHeader& header,
                          const uint8_t* data, size_t dataLen, int64_t rxUs);
    void handleVideoFragment(const exdp::ProtocolHeader& header,
                             const uint8_t* data, size_t dataLen, int64_t rxUs);
    void handleVideoParity(const exdp::ProtocolHeader& header,
                           const uint8_t* data, size_t dataLen, int64_t rxUs);
    /// After the assembler took a packet: deliver, count, recover
    void afterAssembly(bool complete, AssembledFrame& frame, size_t framesLost,
                       uint64_t parityPackets, uint64_t fragmentsRebuilt, uint64_t framesRebuilt, int64_t rxUs);
    void deliverFrame(const std::vector<uint8_t>& frameData, bool isKeyframe, bool hasTiming,
                      int64_t firstPacketUs, int64_t completeUs);

    /// Phase 16: clock sync and sender statistics
    void sendClockPing();
    void handlePong(const exdp::ProtocolHeader& header, const std::vector<uint8_t>& payload);
    void handleSenderStats(const std::vector<uint8_t>& payload);

    /// Ask the Mac for a keyframe after loss (at most once per interval)
    void requestKeyframeAfterLoss();

    /// Handshake (protocol v2). Return false when the connection must be dropped.
    void sendHello();
    bool handleKeyExchange(const std::vector<uint8_t>& payload);
    bool handlePairAccept(const std::vector<uint8_t>& payload);
    bool handlePairReject(const std::vector<uint8_t>& payload);
    void setUserMessage(const std::string& text);

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
    std::string m_currentHost;                  // network thread

    // Handshake of the current connection (network thread; keys also read by
    // the UI thread when the user submits the code → m_handshakeMutex)
    std::mutex m_handshakeMutex;
    std::unique_ptr<crypto::EcdhKeyPair> m_ephemeral;
    crypto::Bytes m_helloBytes;
    std::unique_ptr<SessionKeys> m_handshakeKeys;

    // Set (under m_sendMutex) when PAIR_ACCEPT verified; read lock-free by the
    // UDP thread via std::atomic_load
    std::shared_ptr<SecureSession> m_session;

    std::mutex m_messageMutex;
    std::string m_userMessage;
    bool m_versionMismatch = false;             // network thread
    std::atomic<uint32_t> m_pinWantedFor{0};    // connection awaiting a PIN (0 = none)
    std::atomic<bool> m_pinRequestNew{false};   // not yet picked up by the UI

    SOCKET m_tcpSocket = INVALID_SOCKET;
    SOCKET m_udpSocket = INVALID_SOCKET;

    std::thread m_networkThread;
    std::thread m_udpThread;

    FrameCallback m_frameCallback;
    StreamInfoCallback m_streamInfoCallback;
    ClipboardCallback m_clipboardCallback;
    PointerCallback m_cursorShapeCallback;
    PointerCallback m_cursorPositionCallback;
    std::atomic<uint32_t> m_receiverCaps{0};
    void sendReceiverCaps();

    std::mutex m_sendMutex;   // TCP sends come from network, UI and UDP threads
    uint32_t m_sequence = 0;  // guarded by m_sendMutex

    // Dead-peer detection (network thread only)
    std::chrono::steady_clock::time_point m_lastRxTime;

    static constexpr int      CONNECT_TIMEOUT_MS  = 3000;
    static constexpr int      PEER_TIMEOUT_MS     = 15000;   // Mac pings every 5 s
    static constexpr int      MAX_BACKOFF_MS      = 8000;
    static constexpr int      REDISCOVER_AFTER    = 3;       // failed connects
    static constexpr uint32_t MAX_CONTROL_PAYLOAD = 4 * 1024 * 1024;
    static constexpr int      KEYFRAME_REQUEST_INTERVAL_MS = 500;

    // ── UDP loss detection (UDP thread only) ────────────────────────────────

    std::atomic<bool> m_udpSequenceReset{false};  // set on START_STREAM (sender restarts numbering)
    bool m_haveUdpSequence = false;
    uint32_t m_expectedUdpSequence = 0;
    std::chrono::steady_clock::time_point m_lastLossKeyframeRequest{};

    // ── Fragment reassembly and repair (docs/SMOOTHNESS.md, step 5) ─────────

    std::mutex m_fragmentMutex;
    FrameAssembler m_assembler;                    // m_fragmentMutex
    std::atomic<bool> m_assemblerReset{false};     // START_STREAM: numbering restarts
    // Parity packets arrive in this stream: a lost packet is usually rebuilt,
    // so the keyframe is asked for only when a frame really goes missing
    // (KeyframeGate), not on every gap in the packet numbers
    std::atomic<bool> m_parityActive{false};
    LossSimulator m_lossSimulator;                 // UDP thread (configured before connect)
    LinkSimulator m_linkSimulator;                 // UDP thread (configured before connect)

    // ── Bitrate that follows the delay (step 6; UDP thread) ─────────────────
    DelayMonitor m_delayMonitor;
    std::atomic<bool> m_delayReset{false};         // START_STREAM: the Mac's timestamps restart
    std::atomic<bool> m_rateFeedback{true};
    void sendRateFeedback();

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

    // ── Performance (Phase 16) ──────────────────────────────────────────────

    ClockSync m_clockSync;                          // thread-safe
    std::atomic<bool> m_pingBurst{false};           // START_STREAM: converge quickly
    std::chrono::steady_clock::time_point m_nextPing{};  // UDP thread
    int m_fastPingsLeft = 0;                        // UDP thread
    static constexpr int CLOCK_PING_INTERVAL_MS = 1000;
    static constexpr int CLOCK_PING_BURST_MS = 200;
    static constexpr int CLOCK_PING_BURST = 5;

    std::atomic<uint32_t> m_reportedLatencyUs{0};

    // Frame accounting (UDP thread)
    std::atomic<bool> m_frameSeqReset{false};
    bool m_haveFrameSeq = false;
    uint32_t m_lastFrameSeq = 0;
    JitterEstimator m_jitter;

    mutable std::mutex m_senderMutex;
    SenderSample m_senderSample;
    std::chrono::steady_clock::time_point m_senderSampleTime{};
};
