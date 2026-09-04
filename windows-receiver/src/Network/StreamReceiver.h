// =============================================================================
// StreamReceiver.h — Network receiver for Windows (Winsock2)
// =============================================================================
// Connects to the Mac sender via TCP (control) and receives H.264 video
// data over UDP. Reassembles fragmented frames.
//
// Usage:
//   StreamReceiver receiver;
//   receiver.setFrameCallback([](const uint8_t* data, size_t len, bool key) { ... });
//   receiver.connect("192.168.1.x");
//   // ... in main loop: receiver.poll();
//   receiver.disconnect();
// =============================================================================

#pragma once

#include "Protocol.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <functional>
#include <vector>
#include <map>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>

#pragma comment(lib, "ws2_32.lib")

class StreamReceiver {
public:
    // ── Types ───────────────────────────────────────────────────────────────

    enum class State {
        Disconnected,
        Connecting,
        Connected,       // TCP connected, waiting for START_STREAM
        Streaming,       // Receiving video data
        Error
    };

    /// Called on the receive thread when a complete H.264 frame is ready.
    /// Parameters: (h264Data, dataLength, isKeyframe)
    using FrameCallback = std::function<void(const uint8_t*, size_t, bool)>;

    /// Called when stream parameters are received from sender
    using StreamInfoCallback = std::function<void(uint32_t width, uint32_t height, uint32_t fps)>;

    // ── Lifecycle ───────────────────────────────────────────────────────────

    StreamReceiver();
    ~StreamReceiver();

    /// Connect to the Mac sender at the given IP address.
    /// Starts a background thread for networking.
    bool connect(const std::string& hostIP);

    /// Disconnect and clean up
    void disconnect();

    /// Get current connection state
    State getState() const { return m_state.load(); }

    /// Set callback for decoded frames
    void setFrameCallback(FrameCallback cb) { m_frameCallback = std::move(cb); }

    /// Set callback for stream info
    void setStreamInfoCallback(StreamInfoCallback cb) { m_streamInfoCallback = std::move(cb); }

    // ── Stats ───────────────────────────────────────────────────────────────

    struct Stats {
        uint64_t bytesReceived = 0;
        uint64_t packetsReceived = 0;
        uint64_t framesReceived = 0;
        uint64_t framesDropped = 0;    // incomplete fragments
        uint64_t fragmentsReceived = 0;
    };

    Stats getStats() const {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        return m_stats;
    }

private:
    // ── Network Thread ──────────────────────────────────────────────────────

    void networkThread(const std::string& hostIP);

    /// TCP control channel operations
    bool tcpConnect(const std::string& hostIP);
    void tcpSendMessage(exdp::MessageType type, const void* payload, uint32_t payloadLen);
    bool tcpReceiveMessage(exdp::ProtocolHeader& header, std::vector<uint8_t>& payload);

    /// UDP data channel operations
    bool udpBind();
    void udpReceiveLoop();

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

    // ── State ───────────────────────────────────────────────────────────────

    std::atomic<State> m_state{State::Disconnected};
    std::atomic<bool> m_running{false};

    SOCKET m_tcpSocket = INVALID_SOCKET;
    SOCKET m_udpSocket = INVALID_SOCKET;

    std::thread m_networkThread;
    std::thread m_udpThread;

    FrameCallback m_frameCallback;
    StreamInfoCallback m_streamInfoCallback;

    uint32_t m_sequence = 0;

    // ── Fragment Reassembly ─────────────────────────────────────────────────

    struct PendingFrame {
        uint16_t totalFragments = 0;
        uint16_t receivedCount = 0;
        bool isKeyframe = false;
        std::map<uint16_t, std::vector<uint8_t>> fragments;
    };

    std::mutex m_fragmentMutex;
    std::map<uint32_t, PendingFrame> m_pendingFrames;

    /// Clean up old incomplete frames (keep only last N)
    void cleanupOldFrames(uint32_t currentFrameSeq);

    // ── Stats ───────────────────────────────────────────────────────────────

    mutable std::mutex m_statsMutex;
    Stats m_stats;
};
