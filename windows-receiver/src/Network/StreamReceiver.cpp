// =============================================================================
// StreamReceiver.cpp — Network receiver implementation
// =============================================================================

#include "StreamReceiver.h"
#include "App/Log.h"
#include "../Performance/PipelineClock.h"
#include <iostream>
#include <chrono>
#include <algorithm>
#include <exception>

using Clock = std::chrono::steady_clock;

namespace {

/// Wait up to timeoutMs for the socket to become readable.
/// Returns >0 readable, 0 timeout, SOCKET_ERROR on error.
int WaitReadable(SOCKET s, int timeoutMs) {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(s, &readSet);
    timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
    return select(0, &readSet, nullptr, nullptr, &tv);
}

/// This PC's name as UTF-8 — shown on the Mac so multiple receivers are distinguishable
std::string ComputerNameUTF8() {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(name, &size)) return "Windows Receiver";
    int len = WideCharToMultiByte(CP_UTF8, 0, name, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return "Windows Receiver";
    std::string out((size_t)len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, name, -1, out.data(), len, nullptr, nullptr);
    return out;
}

} // namespace

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
    m_pinWantedFor.store(0);

    closeTcpSocket();
    closeUdpSocket();

    if (m_networkThread.joinable()) m_networkThread.join();
    if (m_udpThread.joinable()) m_udpThread.join();
}

void StreamReceiver::forceReconnect() {
    m_skipBackoff.store(true);
    std::lock_guard<std::mutex> lock(m_sendMutex);
    if (m_tcpSocket != INVALID_SOCKET) {
        // The network thread sees EOF, cleans up and reconnects immediately
        shutdown(m_tcpSocket, SD_BOTH);
    }
}

// ── Network Thread ──────────────────────────────────────────────────────────

void StreamReceiver::networkThread(std::string hostIP) {
    // An exception escaping a std::thread calls std::terminate — log it instead
    try {
        runConnectionLoop(std::move(hostIP));
    } catch (const std::exception& e) {
        LOG_CRITICAL("Net") << "Fatal error in network thread: " << e.what();
        m_state.store(State::Error);
    }
}

void StreamReceiver::runConnectionLoop(std::string host) {
    while (m_running.load()) {
        LOG_INFO("Net") << "Connecting to " << host << ":" << exdp::TCP_PORT;
        m_state.store(State::Connecting);

        // Step 1: Connect TCP (bounded by CONNECT_TIMEOUT_MS)
        if (!tcpConnect(host)) {
            if (!m_running.load()) break;
            if (!m_autoReconnect.load()) {
                m_state.store(State::Error);
                return;
            }
            // Phase 15: the Mac may have a new IP (DHCP renew, other network)
            m_failedConnects++;
            if (m_hostResolver && m_failedConnects % REDISCOVER_AFTER == 0) {
                LOG_WARN("Net") << "Mac unreachable at " << host
                          << " — searching the network (mDNS)...";
                std::string found = m_hostResolver();
                if (!found.empty() && found != host) {
                    LOG_INFO("Net") << "Mac found at new address " << found;
                    host = found;
                    continue;
                }
            }
            waitBeforeReconnect();
            continue;
        }
        m_failedConnects = 0;
        LOG_INFO("Net") << "TCP connected";
        m_connectionId.fetch_add(1);
        m_currentHost = host;
        {
            std::lock_guard<std::mutex> lock(m_sendMutex);
            std::atomic_store(&m_session, std::shared_ptr<SecureSession>());
        }
        m_clockSync.reset();   // the Mac's clock may differ after a reconnect (sleep, restart)
        m_pinWantedFor.store(0);
        m_lastRxTime = Clock::now();

        // Step 2: Bind UDP socket
        if (!udpBind()) {
            closeTcpSocket();
            if (!m_running.load()) break;
            waitBeforeReconnect();
            continue;
        }
        LOG_INFO("Net") << "UDP bound on port " << exdp::UDP_PORT;

        // Step 3: HELLO starts the handshake (protocol v2)
        sendHello();

        m_state.store(State::Connected);

        // Step 4: Start UDP receive thread
        m_udpThread = std::thread(&StreamReceiver::udpReceiveLoop, this);

        // Step 5: TCP receive loop (control messages)
        bool keepConnection = true;
        while (m_running.load() && keepConnection) {
            exdp::ProtocolHeader header;
            uint8_t rawHeader[exdp::HEADER_SIZE];
            std::vector<uint8_t> payload;

            if (!tcpReceiveMessage(header, rawHeader, payload)) {
                LOG_WARN("Net") << "TCP connection lost";
                break;
            }

            if (header.version != exdp::PROTOCOL_VERSION) {
                LOG_ERROR("Net") << "The Mac app speaks protocol v" << int(header.version)
                                 << ", this receiver v" << int(exdp::PROTOCOL_VERSION)
                                 << " — update both apps";
                setUserMessage("The Mac app and this receiver are different versions.\nUpdate both apps.");
                m_versionMismatch = true;
                break;
            }

            auto type = static_cast<exdp::MessageType>(header.type);
            auto session = std::atomic_load(&m_session);
            if (header.flags & exdp::FLAG_ENCRYPTED) {
                std::vector<uint8_t> plaintext;
                if (!session || !session->openControl(rawHeader, exdp::HEADER_SIZE, payload.data(),
                                                      payload.size(), plaintext)) {
                    LOG_ERROR("Net") << "Control message failed authentication — dropping connection";
                    break;
                }
                payload.swap(plaintext);
            } else if (session) {
                LOG_WARN("Net") << "Unencrypted message (type " << int(header.type)
                                << ") after authentication — dropping connection";
                break;
            } else if (type != exdp::MessageType::KeyExchange && type != exdp::MessageType::PairAccept &&
                       type != exdp::MessageType::PairReject && type != exdp::MessageType::Ping &&
                       type != exdp::MessageType::Pong && type != exdp::MessageType::Disconnect) {
                LOG_WARN("Net") << "Ignoring message type " << int(header.type) << " before authentication";
                continue;
            }

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
                case exdp::MessageType::Ping:
                    // Heartbeat — the Mac drops us after 15 s without a PONG
                    tcpSendMessage(exdp::MessageType::Pong, nullptr, 0);
                    break;
                case exdp::MessageType::Pong:
                    handlePong(header, payload);
                    break;
                case exdp::MessageType::SenderStats:
                    handleSenderStats(payload);
                    break;
                case exdp::MessageType::Disconnect:
                    LOG_INFO("Net") << "Disconnect received";
                    m_running.store(false);
                    break;
                case exdp::MessageType::ClipboardData:
                    if (m_clipboardCallback && !payload.empty()) {
                        std::string text(payload.begin(), payload.end());
                        m_clipboardCallback(text);
                    }
                    break;
                case exdp::MessageType::KeyExchange:
                    keepConnection = handleKeyExchange(payload);
                    break;
                case exdp::MessageType::PairAccept:
                    keepConnection = handlePairAccept(payload);
                    break;
                case exdp::MessageType::PairReject:
                    keepConnection = handlePairReject(payload);
                    break;
                default:
                    break;
            }
        }

        // Cleanup sockets for potential reconnect
        m_pinWantedFor.store(0);
        closeTcpSocket();
        closeUdpSocket();
        if (m_udpThread.joinable()) m_udpThread.join();
        {
            std::lock_guard<std::mutex> lock(m_handshakeMutex);
            m_ephemeral.reset();
            m_handshakeKeys.reset();
        }
        {
            std::lock_guard<std::mutex> lock(m_sendMutex);
            std::atomic_store(&m_session, std::shared_ptr<SecureSession>());
        }
        if (m_versionMismatch) {
            m_versionMismatch = false;
            m_reconnectAttempts = 10;   // longest backoff: retrying won't help until updated
        }

        if (!m_running.load() || !m_autoReconnect.load()) break;
        waitBeforeReconnect();
    }

    m_state.store(State::Disconnected);
    LOG_DEBUG("Net") << "Network thread exiting";
}

void StreamReceiver::waitBeforeReconnect() {
    m_state.store(State::Reconnecting);
    if (m_skipBackoff.exchange(false)) {
        LOG_INFO("Net") << "Reconnecting now";
        return;
    }

    // Phase 15: exponential backoff 1 s, 2 s, 4 s, 8 s (cap)
    m_reconnectAttempts++;
    int delayMs = (std::min)(1000 << (std::min)(m_reconnectAttempts - 1, 3), MAX_BACKOFF_MS);
    LOG_INFO("Net") << "Reconnecting in " << delayMs / 1000 << "s (attempt "
              << m_reconnectAttempts << ")";
    for (int waited = 0; waited < delayMs && m_running.load(); waited += 100) {
        if (m_skipBackoff.exchange(false)) break;
        Sleep(100);
    }
}

// ── TCP Operations ──────────────────────────────────────────────────────────

bool StreamReceiver::tcpConnect(const std::string& hostIP) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        LOG_ERROR("Net") << "TCP socket creation failed";
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(exdp::TCP_PORT);
    if (inet_pton(AF_INET, hostIP.c_str(), &addr.sin_addr) != 1) {
        LOG_ERROR("Net") << "Invalid IPv4 address: " << hostIP;
        closesocket(s);
        return false;
    }

    // Non-blocking connect so an unreachable Mac fails in seconds, not ~21 s
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);

    bool connected = false;
    if (::connect(s, (sockaddr*)&addr, sizeof(addr)) == 0) {
        connected = true;
    } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set writeSet, errorSet;
        FD_ZERO(&writeSet);
        FD_ZERO(&errorSet);
        FD_SET(s, &writeSet);
        FD_SET(s, &errorSet);
        timeval tv{ CONNECT_TIMEOUT_MS / 1000, (CONNECT_TIMEOUT_MS % 1000) * 1000 };
        int ready = select(0, nullptr, &writeSet, &errorSet, &tv);
        connected = ready > 0 && FD_ISSET(s, &writeSet) && !FD_ISSET(s, &errorSet);
        if (ready == 0) LOG_WARN("Net") << "TCP connect timed out";
    }

    if (!connected) {
        LOG_WARN("Net") << "TCP connect failed: " << WSAGetLastError();
        closesocket(s);
        return false;
    }

    nonBlocking = 0;
    ioctlsocket(s, FIONBIO, &nonBlocking);

    // Set TCP_NODELAY for low latency control messages
    BOOL nodelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char*)&nodelay, sizeof(nodelay));

    std::lock_guard<std::mutex> lock(m_sendMutex);
    m_tcpSocket = s;
    return true;
}

void StreamReceiver::closeTcpSocket() {
    std::lock_guard<std::mutex> lock(m_sendMutex);
    if (m_tcpSocket != INVALID_SOCKET) {
        closesocket(m_tcpSocket);
        m_tcpSocket = INVALID_SOCKET;
    }
}

void StreamReceiver::closeUdpSocket() {
    std::lock_guard<std::mutex> lock(m_sendMutex);
    if (m_udpSocket != INVALID_SOCKET) {
        closesocket(m_udpSocket);
        m_udpSocket = INVALID_SOCKET;
    }
}

void StreamReceiver::tcpSendMessage(exdp::MessageType type, const void* payload, uint32_t payloadLen,
                                    uint64_t timestamp) {
    // Serialize whole messages so concurrent senders can't interleave bytes —
    // and so the encryption counter advances in send order
    std::lock_guard<std::mutex> lock(m_sendMutex);
    if (m_tcpSocket == INVALID_SOCKET) return;

    auto session = std::atomic_load(&m_session);
    if (!session) {
        auto msg = exdp::buildMessage(type, 0, m_sequence++, timestamp, payload, payloadLen);
        send(m_tcpSocket, (const char*)msg.data(), (int)msg.size(), 0);
        return;
    }

    // header (CRC 0, authenticated as associated data) | ciphertext | tag
    exdp::ProtocolHeader hdr;
    hdr.type = static_cast<uint8_t>(type);
    hdr.flags = exdp::FLAG_ENCRYPTED;
    hdr.sequence = m_sequence++;
    hdr.timestamp = timestamp;
    hdr.payloadLength = payloadLen + exdp::AEAD_TAG_SIZE;
    std::vector<uint8_t> msg(exdp::HEADER_SIZE);
    memcpy(msg.data(), &hdr, exdp::HEADER_SIZE);
    std::vector<uint8_t> sealed;
    if (!session->sealControl(msg.data(), exdp::HEADER_SIZE, static_cast<const uint8_t*>(payload),
                              payloadLen, sealed)) {
        LOG_ERROR("Net") << "Encrypting a control message failed";
        return;
    }
    msg.insert(msg.end(), sealed.begin(), sealed.end());
    send(m_tcpSocket, (const char*)msg.data(), (int)msg.size(), 0);
}

bool StreamReceiver::recvExact(uint8_t* dst, size_t len) {
    size_t got = 0;
    while (got < len) {
        if (!m_running.load()) return false;

        // select() instead of SO_RCVTIMEO: a timed-out blocking recv leaves a
        // Winsock TCP socket in an indeterminate state.
        int ready = WaitReadable(m_tcpSocket, 500);
        if (ready == SOCKET_ERROR) return false;
        if (ready == 0) {
            auto silentMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - m_lastRxTime).count();
            if (silentMs > PEER_TIMEOUT_MS) {
                LOG_WARN("Net") << "No data from Mac for " << silentMs / 1000
                          << "s — connection presumed dead";
                return false;
            }
            continue;
        }

        int n = recv(m_tcpSocket, (char*)dst + got, (int)(len - got), 0);
        if (n <= 0) return false;  // closed or error
        got += (size_t)n;
        m_lastRxTime = Clock::now();
    }
    return true;
}

bool StreamReceiver::tcpReceiveMessage(exdp::ProtocolHeader& header, uint8_t headerBuf[exdp::HEADER_SIZE],
                                       std::vector<uint8_t>& payload) {
    if (!recvExact(headerBuf, exdp::HEADER_SIZE)) return false;

    if (!exdp::parseHeader(headerBuf, exdp::HEADER_SIZE, header)) {
        LOG_ERROR("Net") << "Bad message header (stream out of sync)";
        return false;
    }
    // Never trust the length field: a corrupt value would allocate gigabytes
    if (header.payloadLength > MAX_CONTROL_PAYLOAD + exdp::AEAD_TAG_SIZE) {
        LOG_ERROR("Net") << "Control payload too large (" << header.payloadLength
                  << " bytes) — dropping connection";
        return false;
    }

    payload.resize(header.payloadLength);
    if (header.payloadLength > 0 && !recvExact(payload.data(), header.payloadLength)) {
        return false;
    }
    return true;
}

// ── UDP Operations ──────────────────────────────────────────────────────────

bool StreamReceiver::udpBind() {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        LOG_ERROR("Net") << "UDP socket creation failed";
        return false;
    }

    // Allow reuse
    BOOL reuseAddr = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char*)&reuseAddr, sizeof(reuseAddr));

    // Increase receive buffer size for burst traffic
    int rcvBufSize = 4 * 1024 * 1024; // 4 MB
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (char*)&rcvBufSize, sizeof(rcvBufSize));

    // Timeout so the loop can check m_running and send quality reports
    DWORD timeout = 100;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(exdp::UDP_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        LOG_ERROR("Net") << "UDP bind failed: " << WSAGetLastError();
        closesocket(s);
        return false;
    }

    std::lock_guard<std::mutex> lock(m_sendMutex);
    m_udpSocket = s;
    return true;
}

void StreamReceiver::udpReceiveLoop() {
    try {
        std::vector<uint8_t> buffer(65536);
        std::vector<uint8_t> plaintext;
        plaintext.reserve(2048);
        SOCKET s = m_udpSocket;

        m_haveUdpSequence = false;
        m_haveFrameSeq = false;
        m_jitter.reset();
        m_lastQualityReport = Clock::now();
        m_nextPing = Clock::now();
        {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            m_prevFramesReceived = m_stats.framesReceived;
            m_prevFramesDropped = m_stats.framesDropped;
            m_prevPacketsReceived = m_stats.packetsReceived;
            m_prevPacketsLost = m_stats.packetsLost;
        }

        while (m_running.load()) {
            sockaddr_in from{};
            int fromLen = sizeof(from);
            int n = recvfrom(s, (char*)buffer.data(), (int)buffer.size(), 0,
                             (sockaddr*)&from, &fromLen);
            const int64_t rxUs = PipelineClock::NowUs();

            if (n == SOCKET_ERROR) {
                int err = WSAGetLastError();
                if (err != WSAETIMEDOUT && err != WSAECONNRESET) break;  // socket closed
            } else if (n >= exdp::HEADER_SIZE) {
                exdp::ProtocolHeader header;
                auto session = std::atomic_load(&m_session);
                if (!exdp::parseHeader(buffer.data(), n, header)) {
                    // not ours
                } else if (!session || !(header.flags & exdp::FLAG_ENCRYPTED) ||
                           header.version != exdp::PROTOCOL_VERSION) {
                    LOG_EVERY_MS(5000, LogLevel::Warning, "Net") << "Dropping unauthenticated UDP packet";
                } else if (!session->openVideo(buffer.data(), (size_t)n, exdp::HEADER_SIZE, plaintext)) {
                    // Forged, corrupted or replayed: never reaches the decoder
                    LOG_EVERY_MS(5000, LogLevel::Warning, "Net")
                        << "Video packet failed authentication or was replayed ("
                        << session->videoRejected() << " so far)";
                } else {
                    {
                        std::lock_guard<std::mutex> lock(m_statsMutex);
                        m_stats.bytesReceived += n;
                        m_stats.packetsReceived++;
                    }
                    trackUdpSequence(header.sequence);

                    auto type = static_cast<exdp::MessageType>(header.type);
                    const uint8_t* payload = plaintext.data();
                    size_t payloadLen = plaintext.size();

                    if (type == exdp::MessageType::VideoFrame) {
                        handleVideoFrame(header, payload, payloadLen, rxUs);
                    } else if (type == exdp::MessageType::VideoFragment) {
                        handleVideoFragment(header, payload, payloadLen, rxUs);
                    }
                }
            }

            // Periodic quality report and clock sync (also while the screen is static)
            if (m_state.load() == State::Streaming) {
                auto now = Clock::now();
                float elapsed = std::chrono::duration<float>(now - m_lastQualityReport).count();
                if (elapsed >= QUALITY_REPORT_INTERVAL) {
                    sendQualityReport();
                    m_lastQualityReport = now;
                }
                if (m_pingBurst.exchange(false)) {
                    m_fastPingsLeft = CLOCK_PING_BURST;
                    m_nextPing = now;
                }
                if (now >= m_nextPing) {
                    sendClockPing();
                    int intervalMs = m_fastPingsLeft > 0 ? CLOCK_PING_BURST_MS : CLOCK_PING_INTERVAL_MS;
                    if (m_fastPingsLeft > 0) m_fastPingsLeft--;
                    m_nextPing = now + std::chrono::milliseconds(intervalMs);
                }
            }
        }
    } catch (const std::exception& e) {
        LOG_CRITICAL("Net") << "Fatal error in UDP thread: " << e.what();
    }

    LOG_DEBUG("Net") << "UDP receive loop exiting";
}

void StreamReceiver::trackUdpSequence(uint32_t sequence) {
    // The Mac numbers every UDP packet consecutively; a gap means packets were lost
    if (m_udpSequenceReset.exchange(false)) m_haveUdpSequence = false;
    if (!m_haveUdpSequence) {
        m_haveUdpSequence = true;
        m_expectedUdpSequence = sequence + 1;
        return;
    }

    int32_t gap = (int32_t)(sequence - m_expectedUdpSequence);
    if (gap == 0) {
        m_expectedUdpSequence = sequence + 1;
    } else if (gap > 0 && gap < 10000) {
        {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            m_stats.packetsLost += (uint64_t)gap;
        }
        m_expectedUdpSequence = sequence + 1;
        // Later P-frames reference the lost data — refresh with a keyframe
        requestKeyframeAfterLoss();
    } else if (gap < 0 && gap > -1000) {
        // Late / reordered packet — already counted as lost, nothing to do
    } else {
        m_expectedUdpSequence = sequence + 1;  // sender restarted numbering
    }
}

void StreamReceiver::requestKeyframeAfterLoss() {
    if (m_state.load() != State::Streaming) return;
    auto now = Clock::now();
    if (now - m_lastLossKeyframeRequest <
        std::chrono::milliseconds(KEYFRAME_REQUEST_INTERVAL_MS)) return;
    m_lastLossKeyframeRequest = now;

    requestKeyframe();
    std::lock_guard<std::mutex> lock(m_statsMutex);
    m_stats.keyframeRequests++;
}

// ── Control Message Handlers ────────────────────────────────────────────────

void StreamReceiver::handleWelcome(const std::vector<uint8_t>&) {
    // The keyframe is requested by the UI thread once the decoder is ready
    // (requesting it here races with the decoder reset on START_STREAM).
    LOG_INFO("Net") << "WELCOME received";
}

void StreamReceiver::handleStartStream(const std::vector<uint8_t>& payload) {
    if (payload.size() < sizeof(exdp::StartStreamPayload)) return;

    exdp::StartStreamPayload info;
    memcpy(&info, payload.data(), sizeof(info));

    LOG_INFO("Net") << "START_STREAM: " << info.width << "x" << info.height
              << " @ " << info.fps << " FPS, " << (info.bitrate / 1000000) << " Mbps";

    m_udpSequenceReset.store(true);
    m_frameSeqReset.store(true);     // the Mac restarts its frame numbering
    m_pingBurst.store(true);         // a few quick pings so latency is known within ~1 s
    m_reconnectAttempts = 0;  // fully recovered — reset backoff
    m_state.store(State::Streaming);

    if (m_streamInfoCallback) {
        m_streamInfoCallback(info.width, info.height, info.fps);
    }
}

void StreamReceiver::handleStopStream() {
    LOG_INFO("Net") << "STOP_STREAM received";
    m_state.store(State::Connected);
}

// ── Video Frame Handlers ────────────────────────────────────────────────────

void StreamReceiver::handleVideoFrame(const exdp::ProtocolHeader& header,
                                       const uint8_t* data, size_t dataLen, int64_t rxUs) {
    // Complete frame in a single packet
    bool isKeyframe = (header.flags & exdp::FLAG_KEYFRAME) != 0;
    bool hasTiming = (header.flags & exdp::FLAG_TIMING) != 0;
    std::vector<uint8_t> frameData(data, data + dataLen);
    deliverFrame(frameData, isKeyframe, hasTiming, rxUs, rxUs);
}

void StreamReceiver::handleVideoFragment(const exdp::ProtocolHeader& header,
                                          const uint8_t* data, size_t dataLen, int64_t rxUs) {
    if (dataLen < sizeof(exdp::FragmentHeader)) return;

    exdp::FragmentHeader fragHeader;
    memcpy(&fragHeader, data, sizeof(fragHeader));

    // Reject nonsensical fragment headers instead of building huge frames
    if (fragHeader.fragmentTotal == 0 || fragHeader.fragmentTotal > MAX_FRAGMENTS ||
        fragHeader.fragmentIndex >= fragHeader.fragmentTotal) {
        return;
    }

    const uint8_t* fragData = data + sizeof(exdp::FragmentHeader);
    size_t fragDataLen = dataLen - sizeof(exdp::FragmentHeader);
    bool isKeyframe = (header.flags & exdp::FLAG_KEYFRAME) != 0;

    std::vector<uint8_t> fullFrame;
    bool complete = false;
    bool keyframe = false;
    bool hasTiming = false;
    int64_t firstPacketUs = rxUs;
    {
        std::lock_guard<std::mutex> lock(m_fragmentMutex);

        auto& frame = m_pendingFrames[fragHeader.frameSequence];
        if (frame.totalFragments == 0) {
            frame.totalFragments = fragHeader.fragmentTotal;
            frame.isKeyframe = isKeyframe;
            frame.hasTiming = (header.flags & exdp::FLAG_TIMING) != 0;
            frame.firstPacketUs = rxUs;
        } else if (frame.totalFragments != fragHeader.fragmentTotal) {
            return;  // inconsistent with earlier fragments
        }

        bool inserted = frame.fragments.emplace(
            fragHeader.fragmentIndex,
            std::vector<uint8_t>(fragData, fragData + fragDataLen)).second;
        if (!inserted) return;  // duplicate datagram
        frame.receivedCount++;

        {
            std::lock_guard<std::mutex> slock(m_statsMutex);
            m_stats.fragmentsReceived++;
        }

        if (frame.receivedCount == frame.totalFragments) {
            // Every index 0..total-1 is present (indices are unique and < total)
            size_t totalSize = 0;
            for (auto& [idx, bytes] : frame.fragments) totalSize += bytes.size();
            fullFrame.reserve(totalSize);
            for (auto& [idx, bytes] : frame.fragments) {
                fullFrame.insert(fullFrame.end(), bytes.begin(), bytes.end());
            }
            keyframe = frame.isKeyframe;
            hasTiming = frame.hasTiming;
            firstPacketUs = frame.firstPacketUs;
            complete = true;
            m_pendingFrames.erase(fragHeader.frameSequence);
        }

        cleanupOldFrames(fragHeader.frameSequence);
    }

    // Deliver outside the fragment lock
    if (complete) deliverFrame(fullFrame, keyframe, hasTiming, firstPacketUs, rxUs);
}

void StreamReceiver::deliverFrame(const std::vector<uint8_t>& frameData, bool isKeyframe,
                                  bool hasTiming, int64_t firstPacketUs, int64_t completeUs) {
    FrameTimeline timeline;
    timeline.keyframe = isKeyframe;
    timeline.firstPacketUs = firstPacketUs;
    timeline.completeUs = completeUs;

    const uint8_t* bitstream = frameData.data();
    size_t bitstreamLen = frameData.size();
    uint64_t newlyExpected = 1;   // without frame numbers every frame counts once

    if (hasTiming) {
        // Phase 16: Mac timestamps precede the H.264 data
        if (bitstreamLen <= sizeof(exdp::FrameTimingPrefix)) return;  // malformed
        exdp::FrameTimingPrefix prefix;
        memcpy(&prefix, bitstream, sizeof(prefix));
        bitstream += sizeof(prefix);
        bitstreamLen -= sizeof(prefix);

        timeline.hasSenderTiming = true;
        timeline.captureTimeValid = (prefix.flags & exdp::TIMING_CAPTURE_VALID) != 0;
        timeline.frameSequence = prefix.frameSequence;
        timeline.captureUs = prefix.captureUs;
        timeline.encodeStartUs = prefix.encodeStartUs;
        timeline.encodeEndUs = prefix.encodeEndUs;
        timeline.sendUs = prefix.sendUs;

        // Frames lost completely (every fragment) show up as gaps in the numbering
        if (m_frameSeqReset.exchange(false)) m_haveFrameSeq = false;
        if (m_haveFrameSeq) {
            int32_t gap = (int32_t)(prefix.frameSequence - m_lastFrameSeq);
            if (gap > 0 && gap < 10000) {
                newlyExpected = (uint64_t)gap;
                m_lastFrameSeq = prefix.frameSequence;
            } else if (gap <= 0 && gap > -10000) {
                newlyExpected = 0;   // late or duplicate frame: already counted
            } else {
                m_lastFrameSeq = prefix.frameSequence;   // numbering restarted
            }
        } else {
            m_haveFrameSeq = true;
            m_lastFrameSeq = prefix.frameSequence;
        }

        if (timeline.captureTimeValid) m_jitter.addFrame(prefix.captureUs, completeUs);
    }
    timeline.bytes = (uint32_t)bitstreamLen;

    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats.framesReceived++;
        m_stats.framesExpected += newlyExpected;
        m_stats.jitterMs = m_jitter.jitterMs();
    }

    if (m_frameCallback) {
        m_frameCallback(bitstream, bitstreamLen, timeline);
    }
}

void StreamReceiver::cleanupOldFrames(uint32_t currentFrameSeq) {
    // Called with m_fragmentMutex held.
    bool dropped = false;
    for (auto it = m_pendingFrames.begin(); it != m_pendingFrames.end(); ) {
        int32_t age = (int32_t)(currentFrameSeq - it->first);
        if (age > 10) {
            // Too old and still missing fragments — it will never complete
            it = m_pendingFrames.erase(it);
            dropped = true;
            std::lock_guard<std::mutex> lock(m_statsMutex);
            m_stats.framesDropped++;
        } else if (age < -1000) {
            // Left over from before the sender restarted its frame numbering
            it = m_pendingFrames.erase(it);
        } else {
            ++it;
        }
    }
    if (dropped) requestKeyframeAfterLoss();
}

// ── Quality Reporting ───────────────────────────────────────────────────────

void StreamReceiver::sendQualityReport() {
    Stats currentStats = getStats();

    // Packet loss since last report, from UDP sequence gaps
    uint64_t newPackets = currentStats.packetsReceived - m_prevPacketsReceived;
    uint64_t newLost = currentStats.packetsLost - m_prevPacketsLost;
    uint64_t expected = newPackets + newLost;
    uint32_t lossPercent100 = expected > 0 ? (uint32_t)((newLost * 10000) / expected) : 0;

    uint64_t newDropped = currentStats.framesDropped - m_prevFramesDropped;

    // Queue depth from fragment reassembly
    uint32_t queueDepth = 0;
    {
        std::lock_guard<std::mutex> lock(m_fragmentMutex);
        queueDepth = (uint32_t)m_pendingFrames.size();
    }

    ClockSync::Estimate sync = m_clockSync.estimate();

    exdp::QualityReportPayload report;
    report.packetLossPercent = lossPercent100;
    report.rttMs = sync.valid ? (uint32_t)(sync.smoothedRttMs + 0.5) : 0;
    report.framesDropped = (uint32_t)newDropped;
    report.queueDepth = queueDepth;
    report.latencyUs = m_reportedLatencyUs.load();

    tcpSendMessage(exdp::MessageType::QualityReport, &report, sizeof(report));

    m_prevFramesReceived = currentStats.framesReceived;
    m_prevFramesDropped = currentStats.framesDropped;
    m_prevPacketsReceived = currentStats.packetsReceived;
    m_prevPacketsLost = currentStats.packetsLost;
}

// ── Clock Sync & Sender Stats (Phase 16) ────────────────────────────────────

void StreamReceiver::sendClockPing() {
    // Our clock goes in the header; the Mac's PONG echoes it back with its own
    tcpSendMessage(exdp::MessageType::Ping, nullptr, 0, (uint64_t)PipelineClock::NowUs());
}

void StreamReceiver::handlePong(const exdp::ProtocolHeader& header, const std::vector<uint8_t>& payload) {
    const int64_t recvUs = PipelineClock::NowUs();
    if (payload.size() < sizeof(uint64_t)) return;   // old Mac build: no echo, no sync
    uint64_t sentUs = 0;
    memcpy(&sentUs, payload.data(), sizeof(sentUs));
    if (sentUs == 0) return;
    m_clockSync.addSample((int64_t)sentUs, header.timestamp, recvUs);
}

void StreamReceiver::handleSenderStats(const std::vector<uint8_t>& payload) {
    if (payload.size() < sizeof(exdp::SenderStatsPayload)) return;
    exdp::SenderStatsPayload p;
    memcpy(&p, payload.data(), sizeof(p));

    SenderSample s;
    s.valid = true;
    s.captureFps = p.captureFps100 / 100.0;
    s.encodeFps = p.encodeFps100 / 100.0;
    s.cpuPercent = p.cpuPercent100 / 100.0;
    s.memoryMB = p.memoryMB;
    s.encoderMbps = p.encoderKbps / 1000.0;
    s.targetMbps = p.targetKbps / 1000.0;
    s.hardwareEncoder = (p.flags & exdp::SENDER_HW_ENCODER) != 0;

    std::lock_guard<std::mutex> lock(m_senderMutex);
    m_senderSample = s;
    m_senderSampleTime = Clock::now();
}

SenderSample StreamReceiver::senderStats() const {
    std::lock_guard<std::mutex> lock(m_senderMutex);
    if (!m_senderSample.valid || Clock::now() - m_senderSampleTime > std::chrono::seconds(3)) {
        return SenderSample{};
    }
    return m_senderSample;
}

// ── Handshake (protocol v2, see Security/SecureChannel.h) ───────────────────

void StreamReceiver::sendHello() {
    exdp::HelloPayload hello{};
    strncpy_s(hello.receiverName, ComputerNameUTF8().c_str(), sizeof(hello.receiverName) - 1);
    hello.udpPort = exdp::UDP_PORT;
    std::string deviceId = m_pairingManager ? m_pairingManager->getDeviceId() : std::string();
    strncpy_s(hello.deviceId, deviceId.c_str(), sizeof(hello.deviceId) - 1);

    // A fresh key for every connection; HELLO commits to it so the Mac can't
    // be shown a key chosen after seeing its own
    auto ephemeral = crypto::EcdhKeyPair::Generate();
    if (!ephemeral) {
        LOG_CRITICAL("Pairing") << "Could not generate a P-256 key (Windows CNG) — cannot connect securely";
        setUserMessage("Windows could not create an encryption key.\nSee receiver.log.");
        return;
    }
    crypto::Bytes commitment = KeySchedule::Commitment(ephemeral->publicKey());
    memcpy(hello.commitment, commitment.data(), sizeof(hello.commitment));

    {
        std::lock_guard<std::mutex> lock(m_handshakeMutex);
        m_ephemeral = std::move(ephemeral);
        m_handshakeKeys.reset();
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&hello);
        m_helloBytes.assign(bytes, bytes + sizeof(hello));
    }
    tcpSendMessage(exdp::MessageType::Hello, &hello, sizeof(hello));
    LOG_INFO("Net") << "HELLO sent as '" << hello.receiverName << "' (device " << deviceId.substr(0, 8) << "…)";
}

bool StreamReceiver::handleKeyExchange(const std::vector<uint8_t>& payload) {
    if (payload.size() < sizeof(exdp::KeyExchangePayload)) return false;
    exdp::KeyExchangePayload kx;
    memcpy(&kx, payload.data(), sizeof(kx));
    crypto::Bytes kxBytes(payload.begin(), payload.begin() + sizeof(kx));

    crypto::Bytes ourKey, helloBytes, shared;
    {
        std::lock_guard<std::mutex> lock(m_handshakeMutex);
        if (!m_ephemeral || m_handshakeKeys) return false;   // unexpected
        ourKey = m_ephemeral->publicKey();
        helloBytes = m_helloBytes;
        if (!m_ephemeral->agree(kx.publicKey, sizeof(kx.publicKey), shared)) {
            LOG_ERROR("Pairing") << "The Mac sent an invalid public key";
            return false;
        }
    }

    // Reveal the key HELLO committed to
    tcpSendMessage(exdp::MessageType::KeyReveal, ourKey.data(), (uint32_t)ourKey.size());
    crypto::Bytes transcript = KeySchedule::TranscriptHash(helloBytes, kxBytes, ourKey);

    if (kx.mode == exdp::HANDSHAKE_PAIRED) {
        crypto::Bytes pairingKey = m_pairingManager ? m_pairingManager->getPairingKey() : crypto::Bytes();
        if (pairingKey.size() != 32) {
            // The Mac remembers this device ID but the key is gone (pairing.dat
            // deleted / another Windows user): start over with a new identity
            LOG_WARN("Pairing") << "The Mac expects a paired receiver but the pairing key is missing — pairing again";
            if (m_pairingManager) m_pairingManager->clearPairing();
            m_skipBackoff.store(true);
            return false;
        }
        auto keys = std::make_unique<SessionKeys>(KeySchedule::Derive(shared, &pairingKey, transcript));
        crypto::Bytes mac = KeySchedule::Confirmation(keys->confirmWin, transcript);
        {
            std::lock_guard<std::mutex> lock(m_handshakeMutex);
            m_handshakeKeys = std::move(keys);
        }
        tcpSendMessage(exdp::MessageType::PairConfirm, mac.data(), (uint32_t)mac.size());
        LOG_INFO("Pairing") << "Authenticating with the stored pairing";
        return true;
    }

    // New pairing: the user types the code the Mac shows; it must equal ours
    auto keys = std::make_unique<SessionKeys>(KeySchedule::Derive(shared, nullptr, transcript));
    {
        std::lock_guard<std::mutex> lock(m_handshakeMutex);
        m_handshakeKeys = std::move(keys);
    }
    if (m_pairingManager && m_pairingManager->isPaired()) {
        LOG_INFO("Pairing") << "The Mac does not know this receiver (any more) — pairing again";
    }
    LOG_INFO("Pairing") << "New pairing — enter the code shown in the Mac app";
    m_pinWantedFor.store(m_connectionId.load());
    m_pinRequestNew.store(true);
    return true;
}

bool StreamReceiver::takeCodeRequest(uint32_t& outRequestId) {
    if (!m_pinRequestNew.exchange(false)) return false;
    outRequestId = m_pinWantedFor.load();
    return outRequestId != 0;
}

StreamReceiver::CodeResult StreamReceiver::submitVerificationCode(uint32_t requestId, const std::string& typed) {
    // Claim the request so a double submit or a stale dialog can't send twice
    uint32_t expected = requestId;
    if (requestId == 0 || !m_pinWantedFor.compare_exchange_strong(expected, 0)) {
        LOG_INFO("Pairing") << "Pairing request expired — waiting for a new one";
        return CodeResult::Expired;
    }

    std::string code;
    for (char c : typed) {
        if (c >= '0' && c <= '9') code += c;
    }

    std::string wanted;
    crypto::Bytes mac;
    {
        std::lock_guard<std::mutex> lock(m_handshakeMutex);
        if (!m_handshakeKeys) return CodeResult::Expired;
        wanted = m_handshakeKeys->verificationCode;
        mac = KeySchedule::Confirmation(m_handshakeKeys->confirmWin, m_handshakeKeys->transcriptHash);
    }

    if (code.size() != wanted.size() ||
        !crypto::ConstantTimeEqual(reinterpret_cast<const uint8_t*>(code.data()),
                                   reinterpret_cast<const uint8_t*>(wanted.data()), wanted.size())) {
        // A typo — or a man in the middle, who causes different codes on the two sides
        LOG_WARN("Pairing") << "The code does not match this connection — restarting pairing with a new code";
        forceReconnect();
        return CodeResult::Mismatch;
    }

    tcpSendMessage(exdp::MessageType::PairConfirm, mac.data(), (uint32_t)mac.size());
    LOG_INFO("Pairing") << "Code verified — approve the pairing on the Mac";
    return CodeResult::Sent;
}

bool StreamReceiver::handlePairAccept(const std::vector<uint8_t>& payload) {
    if (payload.size() < sizeof(exdp::PairAcceptPayload)) return false;
    exdp::PairAcceptPayload accept;
    memcpy(&accept, payload.data(), sizeof(accept));
    m_pinWantedFor.store(0);

    std::unique_ptr<SessionKeys> keys;
    {
        std::lock_guard<std::mutex> lock(m_handshakeMutex);
        keys = std::move(m_handshakeKeys);
        m_ephemeral.reset();
    }
    // The Mac proves it derived the same keys (mutual authentication)
    if (!keys || !KeySchedule::Verify(accept.mac, keys->confirmMac, keys->transcriptHash)) {
        LOG_ERROR("Pairing") << "The Mac could not prove the shared keys — not trusting this connection";
        return false;
    }

    auto session = std::make_shared<SecureSession>(*keys);
    if (!session->valid()) {
        LOG_CRITICAL("Pairing") << "Could not set up AES-GCM (Windows CNG)";
        return false;
    }
    if (accept.newlyPaired && m_pairingManager) {
        m_pairingManager->savePairing("Mac at " + m_currentHost, keys->pairingKey);
    }
    {
        // Under the send lock: every message after this point is encrypted
        std::lock_guard<std::mutex> lock(m_sendMutex);
        std::atomic_store(&m_session, session);
    }
    LOG_INFO("Pairing") << (accept.newlyPaired ? "Paired with the Mac" : "Authenticated")
                        << " — connection encrypted (AES-256-GCM)";
    return true;
}

bool StreamReceiver::handlePairReject(const std::vector<uint8_t>& payload) {
    m_pinWantedFor.store(0);
    uint8_t reason = payload.empty() ? static_cast<uint8_t>(exdp::REJECT_GENERIC) : payload[0];
    switch (reason) {
        case exdp::REJECT_USER_REJECTED:
            LOG_WARN("Pairing") << "Pairing was rejected on the Mac";
            setUserMessage("Pairing was rejected on the Mac.\nRestart the receiver to try again.");
            m_autoReconnect.store(false);   // don't pester the Mac with new requests
            break;
        case exdp::REJECT_UNKNOWN_DEVICE:
            LOG_WARN("Pairing") << "The Mac no longer accepts this receiver's pairing — pairing again";
            if (m_pairingManager) m_pairingManager->clearPairing();
            m_skipBackoff.store(true);
            break;
        case exdp::REJECT_VERSION_MISMATCH:
            LOG_ERROR("Pairing") << "The Mac app uses a different protocol version — update both apps";
            setUserMessage("The Mac app and this receiver are different versions.\nUpdate both apps.");
            m_versionMismatch = true;
            break;
        default:
            LOG_WARN("Pairing") << "The Mac refused the handshake (reason " << int(reason) << ")";
            break;
    }
    return false;
}

void StreamReceiver::setUserMessage(const std::string& text) {
    std::lock_guard<std::mutex> lock(m_messageMutex);
    m_userMessage = text;
}

std::string StreamReceiver::takeUserMessage() {
    std::lock_guard<std::mutex> lock(m_messageMutex);
    std::string text;
    text.swap(m_userMessage);
    return text;
}
