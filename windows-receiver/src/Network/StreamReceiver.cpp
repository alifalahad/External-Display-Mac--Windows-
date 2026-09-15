// =============================================================================
// StreamReceiver.cpp — Network receiver implementation
// =============================================================================

#include "StreamReceiver.h"
#include <iostream>
#include <chrono>
#include <algorithm>

// ── Lifecycle ───────────────────────────────────────────────────────────────

StreamReceiver::StreamReceiver() {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
}

StreamReceiver::~StreamReceiver() {
    disconnect();
    WSACleanup();
}

bool StreamReceiver::connect(const std::string& hostIP) {
    if (m_running.load()) {
        disconnect();
    }

    m_running.store(true);
    m_state.store(State::Connecting);

    m_networkThread = std::thread(&StreamReceiver::networkThread, this, hostIP);
    return true;
}

void StreamReceiver::disconnect() {
    m_running.store(false);
    m_state.store(State::Disconnected);

    if (m_tcpSocket != INVALID_SOCKET) {
        closesocket(m_tcpSocket);
        m_tcpSocket = INVALID_SOCKET;
    }
    if (m_udpSocket != INVALID_SOCKET) {
        closesocket(m_udpSocket);
        m_udpSocket = INVALID_SOCKET;
    }

    if (m_networkThread.joinable()) m_networkThread.join();
    if (m_udpThread.joinable()) m_udpThread.join();
}

void StreamReceiver::networkThread(const std::string& hostIP) {
    while (m_running.load()) {
        std::cout << "[Net] Connecting to " << hostIP << ":" << exdp::TCP_PORT << std::endl;
        m_state.store(State::Connecting);

        // Step 1: Connect TCP
        if (!tcpConnect(hostIP)) {
            if (!m_running.load() || !m_autoReconnect.load()) {
                m_state.store(State::Error);
                return;
            }
            std::cout << "[Net] Connection failed, retrying in 3s..." << std::endl;
            m_state.store(State::Reconnecting);
            for (int i = 0; i < 30 && m_running.load(); i++)
                Sleep(100);  // 3s in 100ms chunks (interruptible)
            continue;
        }
        std::cout << "[Net] TCP connected" << std::endl;

        // Step 2: Bind UDP socket
        if (!udpBind()) {
            m_state.store(State::Error);
            return;
        }
        std::cout << "[Net] UDP bound on port " << exdp::UDP_PORT << std::endl;

        // Step 3: Send HELLO
        exdp::HelloPayload hello{};
        strncpy_s(hello.receiverName, "Windows Receiver", sizeof(hello.receiverName) - 1);
        hello.udpPort = exdp::UDP_PORT;
        tcpSendMessage(exdp::MessageType::Hello, &hello, sizeof(hello));
        std::cout << "[Net] HELLO sent" << std::endl;

        m_state.store(State::Connected);

        // Step 4: Start UDP receive thread
        m_udpThread = std::thread(&StreamReceiver::udpReceiveLoop, this);

        // Step 5: TCP receive loop (control messages)
        while (m_running.load()) {
            exdp::ProtocolHeader header;
            std::vector<uint8_t> payload;

            if (!tcpReceiveMessage(header, payload)) {
                std::cout << "[Net] TCP connection lost" << std::endl;
                break;
            }

            auto type = static_cast<exdp::MessageType>(header.type);
            switch (type) {
                case exdp::MessageType::Welcome:
                    handleWelcome(payload);
                    break;
                case exdp::MessageType::StartStream:
                    handleStartStream(payload);
                    break;
                case exdp::MessageType::StopStream:
                    handleStopStream();
                    break;
                case exdp::MessageType::Ping: {
                    tcpSendMessage(exdp::MessageType::Pong, nullptr, 0);
                    break;
                }
                case exdp::MessageType::Disconnect:
                    std::cout << "[Net] Disconnect received" << std::endl;
                    m_running.store(false);
                    break;
                default:
                    break;
            }
        }

        // Cleanup sockets for potential reconnect
        if (m_tcpSocket != INVALID_SOCKET) {
            closesocket(m_tcpSocket);
            m_tcpSocket = INVALID_SOCKET;
        }
        if (m_udpSocket != INVALID_SOCKET) {
            closesocket(m_udpSocket);
            m_udpSocket = INVALID_SOCKET;
        }
        if (m_udpThread.joinable()) m_udpThread.join();

        // Auto-reconnect if enabled
        if (m_running.load() && m_autoReconnect.load()) {
            std::cout << "[Net] Reconnecting in 3s..." << std::endl;
            m_state.store(State::Reconnecting);
            for (int i = 0; i < 30 && m_running.load(); i++)
                Sleep(100);
        }
    }

    m_state.store(State::Disconnected);
    std::cout << "[Net] Network thread exiting" << std::endl;
}

// ── TCP Operations ──────────────────────────────────────────────────────────

bool StreamReceiver::tcpConnect(const std::string& hostIP) {
    m_tcpSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_tcpSocket == INVALID_SOCKET) {
        std::cerr << "[Net] TCP socket creation failed" << std::endl;
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(exdp::TCP_PORT);
    inet_pton(AF_INET, hostIP.c_str(), &addr.sin_addr);

    if (::connect(m_tcpSocket, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << "[Net] TCP connect failed: " << WSAGetLastError() << std::endl;
        closesocket(m_tcpSocket);
        m_tcpSocket = INVALID_SOCKET;
        return false;
    }

    // Set TCP_NODELAY for low latency control messages
    BOOL nodelay = TRUE;
    setsockopt(m_tcpSocket, IPPROTO_TCP, TCP_NODELAY, (char*)&nodelay, sizeof(nodelay));

    return true;
}

void StreamReceiver::tcpSendMessage(exdp::MessageType type, const void* payload, uint32_t payloadLen) {
    auto msg = exdp::buildMessage(type, 0, m_sequence++, 0, payload, payloadLen);
    send(m_tcpSocket, (const char*)msg.data(), (int)msg.size(), 0);
}

bool StreamReceiver::tcpReceiveMessage(exdp::ProtocolHeader& header, std::vector<uint8_t>& payload) {
    // Set a timeout so we can check m_running periodically
    DWORD timeout = 500; // 500ms
    setsockopt(m_tcpSocket, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

    // Read header
    uint8_t headerBuf[exdp::HEADER_SIZE];
    int totalRead = 0;
    while (totalRead < exdp::HEADER_SIZE) {
        int n = recv(m_tcpSocket, (char*)headerBuf + totalRead,
                     exdp::HEADER_SIZE - totalRead, 0);
        if (n == 0) return false; // Connection closed
        if (n == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT) {
                if (!m_running.load()) return false;
                continue; // Retry
            }
            return false;
        }
        totalRead += n;
    }

    if (!exdp::parseHeader(headerBuf, exdp::HEADER_SIZE, header)) {
        return false;
    }

    // Read payload
    payload.resize(header.payloadLength);
    totalRead = 0;
    while (totalRead < (int)header.payloadLength) {
        int n = recv(m_tcpSocket, (char*)payload.data() + totalRead,
                     (int)header.payloadLength - totalRead, 0);
        if (n <= 0) return false;
        totalRead += n;
    }

    return true;
}

// ── UDP Operations ──────────────────────────────────────────────────────────

bool StreamReceiver::udpBind() {
    m_udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_udpSocket == INVALID_SOCKET) {
        std::cerr << "[Net] UDP socket creation failed" << std::endl;
        return false;
    }

    // Allow reuse
    BOOL reuseAddr = TRUE;
    setsockopt(m_udpSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&reuseAddr, sizeof(reuseAddr));

    // Increase receive buffer size for burst traffic
    int rcvBufSize = 4 * 1024 * 1024; // 4 MB
    setsockopt(m_udpSocket, SOL_SOCKET, SO_RCVBUF, (char*)&rcvBufSize, sizeof(rcvBufSize));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(exdp::UDP_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(m_udpSocket, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << "[Net] UDP bind failed: " << WSAGetLastError() << std::endl;
        closesocket(m_udpSocket);
        m_udpSocket = INVALID_SOCKET;
        return false;
    }

    return true;
}

void StreamReceiver::udpReceiveLoop() {
    std::vector<uint8_t> buffer(65536);

    // Set timeout for periodic m_running check
    DWORD timeout = 100; // 100ms
    setsockopt(m_udpSocket, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

    while (m_running.load()) {
        sockaddr_in from{};
        int fromLen = sizeof(from);
        int n = recvfrom(m_udpSocket, (char*)buffer.data(), (int)buffer.size(), 0,
                         (sockaddr*)&from, &fromLen);

        if (n <= 0) {
            if (WSAGetLastError() == WSAETIMEDOUT) continue;
            break;
        }

        if (n < exdp::HEADER_SIZE) continue;

        // Parse header
        exdp::ProtocolHeader header;
        if (!exdp::parseHeader(buffer.data(), n, header)) continue;

        {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            m_stats.bytesReceived += n;
            m_stats.packetsReceived++;
        }

        auto type = static_cast<exdp::MessageType>(header.type);
        const uint8_t* payload = buffer.data() + exdp::HEADER_SIZE;
        size_t payloadLen = n - exdp::HEADER_SIZE;

        if (type == exdp::MessageType::VideoFrame) {
            handleVideoFrame(header, payload, payloadLen);
        } else if (type == exdp::MessageType::VideoFragment) {
            handleVideoFragment(header, payload, payloadLen);
        }
    }

    std::cout << "[Net] UDP receive loop exiting" << std::endl;
}

// ── Control Message Handlers ────────────────────────────────────────────────

void StreamReceiver::handleWelcome(const std::vector<uint8_t>& payload) {
    std::cout << "[Net] WELCOME received" << std::endl;
}

void StreamReceiver::handleStartStream(const std::vector<uint8_t>& payload) {
    if (payload.size() < sizeof(exdp::StartStreamPayload)) return;

    exdp::StartStreamPayload info;
    memcpy(&info, payload.data(), sizeof(info));

    std::cout << "[Net] START_STREAM: " << info.width << "x" << info.height
              << " @ " << info.fps << " FPS, " << (info.bitrate / 1000000) << " Mbps"
              << std::endl;

    m_state.store(State::Streaming);

    if (m_streamInfoCallback) {
        m_streamInfoCallback(info.width, info.height, info.fps);
    }
}

void StreamReceiver::handleStopStream() {
    std::cout << "[Net] STOP_STREAM received" << std::endl;
    m_state.store(State::Connected);
}

// ── Video Frame Handlers ────────────────────────────────────────────────────

void StreamReceiver::handleVideoFrame(const exdp::ProtocolHeader& header,
                                       const uint8_t* data, size_t dataLen) {
    // Complete frame in a single packet
    bool isKeyframe = (header.flags & exdp::FLAG_KEYFRAME) != 0;
    std::vector<uint8_t> frameData(data, data + dataLen);
    deliverFrame(frameData, isKeyframe);
}

void StreamReceiver::handleVideoFragment(const exdp::ProtocolHeader& header,
                                          const uint8_t* data, size_t dataLen) {
    if (dataLen < sizeof(exdp::FragmentHeader)) return;

    exdp::FragmentHeader fragHeader;
    memcpy(&fragHeader, data, sizeof(fragHeader));

    const uint8_t* fragData = data + sizeof(exdp::FragmentHeader);
    size_t fragDataLen = dataLen - sizeof(exdp::FragmentHeader);

    bool isKeyframe = (header.flags & exdp::FLAG_KEYFRAME) != 0;

    std::lock_guard<std::mutex> lock(m_fragmentMutex);

    // Get or create pending frame
    auto& frame = m_pendingFrames[fragHeader.frameSequence];
    if (frame.totalFragments == 0) {
        frame.totalFragments = fragHeader.fragmentTotal;
        frame.isKeyframe = isKeyframe;
    }

    // Store fragment
    frame.fragments[fragHeader.fragmentIndex] =
        std::vector<uint8_t>(fragData, fragData + fragDataLen);
    frame.receivedCount++;

    {
        std::lock_guard<std::mutex> slock(m_statsMutex);
        m_stats.fragmentsReceived++;
    }

    // Check if frame is complete
    if (frame.receivedCount >= frame.totalFragments) {
        // Reassemble in order
        std::vector<uint8_t> fullFrame;
        for (uint16_t i = 0; i < frame.totalFragments; i++) {
            auto it = frame.fragments.find(i);
            if (it != frame.fragments.end()) {
                fullFrame.insert(fullFrame.end(), it->second.begin(), it->second.end());
            }
        }

        bool keyframe = frame.isKeyframe;
        m_pendingFrames.erase(fragHeader.frameSequence);

        // Unlock before callback
        // (Note: can't unlock since we're using lock_guard, but the callback
        //  should be fast — it just queues the frame for decoding)
        deliverFrame(fullFrame, keyframe);
    }

    // Cleanup old incomplete frames
    cleanupOldFrames(fragHeader.frameSequence);
}

void StreamReceiver::deliverFrame(const std::vector<uint8_t>& frameData, bool isKeyframe) {
    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats.framesReceived++;
    }

    if (m_frameCallback) {
        m_frameCallback(frameData.data(), frameData.size(), isKeyframe);
    }
}

void StreamReceiver::cleanupOldFrames(uint32_t currentFrameSeq) {
    // Remove frames that are more than 10 sequences behind
    // (they're too old and missing fragments — drop them)
    std::vector<uint32_t> toRemove;
    for (auto& [seq, frame] : m_pendingFrames) {
        if (currentFrameSeq > seq && (currentFrameSeq - seq) > 10) {
            toRemove.push_back(seq);
        }
    }
    for (auto seq : toRemove) {
        m_pendingFrames.erase(seq);
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats.framesDropped++;
    }
}
