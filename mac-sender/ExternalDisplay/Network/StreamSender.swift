// =============================================================================
// StreamSender.swift — Network sender for Mac → Windows streaming
// =============================================================================
// Manages TCP control channel, UDP data channel, and Bonjour service.
//
// Connection flow:
//   1. Start listening (TCP + Bonjour)
//   2. Windows connects via TCP → HELLO exchange
//   3. Mac sends START_STREAM
//   4. Encoded H.264 frames sent over UDP (fragmented if needed)
//
// Uses Apple's Network.framework (NWConnection/NWListener).
// =============================================================================

import Foundation
import Network
import Combine
import QuartzCore

/// Connection state machine
enum SenderState: Equatable {
    case idle
    case listening
    case pairing(String)      // peer connected, waiting for PIN / HMAC verification
    case connected(String)    // peer name (authenticated)
    case streaming(String)    // peer name
    case error(String)

    var displayText: String {
        switch self {
        case .idle:             return "Idle"
        case .listening:        return "Listening on port \(ProtocolConstants.tcpPort)…"
        case .pairing(let p):   return "Authenticating \(p)…"
        case .connected(let p): return "Connected: \(p)"
        case .streaming(let p): return "Streaming to \(p)"
        case .error(let e):     return "Error: \(e)"
        }
    }

    var isStreaming: Bool {
        if case .streaming = self { return true }
        return false
    }
}

/// Network statistics snapshot
struct NetworkStats {
    var bytesSent: UInt64 = 0
    var packetsSent: UInt64 = 0
    var framesSent: UInt64 = 0
    var droppedFrames: UInt64 = 0
}

final class StreamSender: ObservableObject {

    // ── Published state ─────────────────────────────────────────────────────

    @Published var state: SenderState = .idle
    @Published var networkStats = NetworkStats()

    // ── Network objects ─────────────────────────────────────────────────────

    private var tcpListener: NWListener?
    private var tcpConnection: NWConnection?
    private var udpConnection: NWConnection?

    private let networkQueue = DispatchQueue(
        label: "com.externaldisplay.network",
        qos: .userInteractive
    )

    // ── Heartbeat (Phase 15) ────────────────────────────────────────────────

    private var heartbeatTimer: DispatchSourceTimer?
    private var lastPongReceived: Double = 0
    private let heartbeatInterval: TimeInterval = 5.0   // Send Ping every 5s
    private let heartbeatTimeout: TimeInterval = 15.0    // Dead if no Pong in 15s

    /// Called when the receiver needs a keyframe (stream start, packet loss,
    /// decoder reset). Invoked on the network queue.
    var onKeyframeRequest: (() -> Void)?

    /// Largest control payload accepted (clipboard text is the biggest, ≤ 1 MB).
    /// Anything larger means a corrupted or hostile stream.
    private let maxControlPayload = 4 * 1024 * 1024

    // ── Stream state ────────────────────────────────────────────────────────

    private var remoteHost: NWEndpoint.Host?
    private var remoteUDPPort: UInt16 = ProtocolConstants.udpPort
    private var sequenceNumber: UInt32 = 0          // TCP control messages
    private var udpPacketSequence: UInt32 = 0       // UDP video packets — gaps = loss
    private var frameSequenceNumber: UInt32 = 0
    private var streamStartTime: Double = 0

    // Stream config (set before startStreaming)
    var streamWidth: UInt32 = 0
    var streamHeight: UInt32 = 0
    var streamFPS: UInt32 = 60
    var streamBitrate: UInt32 = 15_000_000

    /// Called when a quality report is received from the Windows receiver
    var onQualityReport: ((QualityReportPayload) -> Void)?

    /// Called when an input event is received from the Windows receiver
    var onInputEvent: ((InputEventPayload) -> Void)?

    /// Called when clipboard data is received from the Windows receiver
    var onClipboardData: ((Data) -> Void)?

    /// Pairing manager for secure authentication
    var pairingManager: PairingManager?

    // Pairing state for the current connection (networkQueue only)
    private var pendingDeviceId: String?
    private var pendingDeviceName: String?
    /// True once the current peer passed PIN or HMAC verification.
    /// Nothing except the pairing handshake is accepted or sent before this.
    private var isAuthenticated = false

    // ── Lifecycle ───────────────────────────────────────────────────────────

    /// Start listening for incoming connections
    func startListening() {
        guard case .idle = state else { return }
        networkQueue.async { [weak self] in
            self?.createListener()
        }
    }

    /// Phase 15: a listener fails when the network interface goes away
    /// (Wi-Fi switch, sleep). Recreate it so the receiver can find us again.
    private func scheduleListenerRestart() {
        tcpListener?.cancel()
        tcpListener = nil
        print("[Net] Restarting listener in 2s…")
        networkQueue.asyncAfter(deadline: .now() + 2.0) { [weak self] in
            guard let self = self, self.tcpListener == nil, self.listenerWanted else { return }
            self.createListener()
        }
    }

    /// False after shutdown() — prevents a pending restart from resurrecting the listener
    private var listenerWanted = false

    private func createListener() {
        listenerWanted = true
        do {
            let params = NWParameters.tcp
            params.acceptLocalOnly = false
            params.allowLocalEndpointReuse = true

            let listener = try NWListener(using: params, on: NWEndpoint.Port(rawValue: ProtocolConstants.tcpPort)!)

            // Publish via Bonjour
            listener.service = NWListener.Service(
                name: "ExternalDisplay-\(Host.current().localizedName ?? "Mac")",
                type: ProtocolConstants.bonjourType
            )

            listener.stateUpdateHandler = { [weak self] state in
                switch state {
                case .ready:
                    print("[Net] TCP listener ready on port \(ProtocolConstants.tcpPort)")
                    DispatchQueue.main.async {
                        self?.state = .listening
                    }
                case .failed(let error):
                    print("[Net] Listener failed: \(error)")
                    DispatchQueue.main.async {
                        self?.state = .error("Listener: \(error.localizedDescription)")
                    }
                    self?.scheduleListenerRestart()
                default:
                    break
                }
            }

            listener.newConnectionHandler = { [weak self] connection in
                guard let self = self else { connection.cancel(); return }

                // Phase 15: If a client is already connected, check if it's still alive.
                // If the old connection is stale (e.g. Wi-Fi dropout), drop it and accept the new one.
                if self.tcpConnection != nil {
                    print("[Net] New connection while already connected — dropping old connection for reconnect")
                    self.stopHeartbeat()
                    self.stopStreaming()
                    self.tcpConnection?.cancel()
                    self.tcpConnection = nil
                    self.udpConnection?.cancel()
                    self.udpConnection = nil
                    self.remoteHost = nil
                }
                self.handleNewTCPConnection(connection)
            }

            listener.start(queue: networkQueue)
            tcpListener = listener

        } catch {
            DispatchQueue.main.async { [weak self] in
                self?.state = .error("Listen failed: \(error.localizedDescription)")
            }
            scheduleListenerRestart()
        }
    }

    /// Stop streaming and disconnect client, but keep listener alive for reconnects
    func stop() {
        stopStreaming()
        tcpConnection?.cancel()
        tcpConnection = nil
        udpConnection?.cancel()
        udpConnection = nil
        remoteHost = nil
        resetAuthentication()
        sequenceNumber = 0
        frameSequenceNumber = 0
        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }
            // Go back to listening if listener is still active
            if self.tcpListener != nil {
                self.state = .listening
            } else {
                self.state = .idle
            }
            self.networkStats = NetworkStats()
        }
    }

    /// Full shutdown — stop everything including the listener
    func shutdown() {
        listenerWanted = false
        stopStreaming()
        tcpConnection?.cancel()
        tcpConnection = nil
        udpConnection?.cancel()
        udpConnection = nil
        remoteHost = nil
        tcpListener?.cancel()
        tcpListener = nil
        sequenceNumber = 0
        frameSequenceNumber = 0
        DispatchQueue.main.async { [weak self] in
            self?.state = .idle
            self?.networkStats = NetworkStats()
        }
    }

    // ── TCP Connection Handling ─────────────────────────────────────────────

    private func handleNewTCPConnection(_ connection: NWConnection) {
        print("[Net] New TCP connection from \(connection.endpoint)")
        tcpConnection = connection
        resetAuthentication()

        // Extract remote host for UDP
        if case .hostPort(let host, _) = connection.endpoint {
            remoteHost = host
        }

        connection.stateUpdateHandler = { [weak self] state in
            switch state {
            case .ready:
                print("[Net] TCP connection ready")
                self?.lastPongReceived = CACurrentMediaTime()
                self?.startHeartbeat()
                self?.receiveTCPMessages()
            case .failed(let error):
                print("[Net] TCP connection failed: \(error)")
                self?.stopHeartbeat()
                self?.handleDisconnect(for: connection)
            case .cancelled:
                print("[Net] TCP connection cancelled")
                self?.stopHeartbeat()
                break
            default:
                break
            }
        }

        connection.start(queue: networkQueue)
    }

    private func receiveTCPMessages() {
        guard let connection = tcpConnection else { return }

        // Step 1: Read exactly the 28-byte header
        connection.receive(
            minimumIncompleteLength: ProtocolConstants.headerSize,
            maximumLength: ProtocolConstants.headerSize
        ) { [weak self] headerData, _, isComplete, error in
            guard let self = self else { return }

            if let error = error {
                print("[Net] TCP receive error: \(error)")
                self.handleDisconnect(for: connection)
                return
            }

            if isComplete {
                print("[Net] TCP connection closed by peer")
                self.handleDisconnect(for: connection)
                return
            }

            guard let headerData = headerData, headerData.count >= ProtocolConstants.headerSize,
                  let header = ProtocolHeader.deserialize(from: headerData) else {
                // Continue receiving if we didn't get a full header
                self.receiveTCPMessages()
                return
            }

            let payloadLen = Int(header.payloadLength)
            guard payloadLen <= self.maxControlPayload else {
                print("[Net] ❌ Control payload too large (\(payloadLen) bytes) — dropping connection")
                self.handleDisconnect(for: connection)
                return
            }

            if payloadLen == 0 {
                // No payload — dispatch immediately
                self.dispatchMessage(header: header, payload: Data(), connection: connection)
                self.receiveTCPMessages()
            } else {
                // Step 2: Read exactly payloadLength bytes
                self.readTCPPayload(connection: connection, header: header,
                                     remaining: payloadLen, accumulated: Data())
            }
        }
    }

    /// Read exactly `remaining` bytes of payload from TCP, accumulating chunks
    private func readTCPPayload(connection: NWConnection, header: ProtocolHeader,
                                 remaining: Int, accumulated: Data) {
        connection.receive(
            minimumIncompleteLength: remaining,
            maximumLength: remaining
        ) { [weak self] data, _, isComplete, error in
            guard let self = self else { return }

            if let error = error {
                print("[Net] TCP payload read error: \(error)")
                self.handleDisconnect(for: connection)
                return
            }

            if isComplete && (data == nil || data!.isEmpty) {
                print("[Net] TCP connection closed during payload read")
                self.handleDisconnect(for: connection)
                return
            }

            var acc = accumulated
            if let data = data {
                acc.append(data)
            }

            let left = remaining - (data?.count ?? 0)
            if left > 0 {
                // Need more data
                self.readTCPPayload(connection: connection, header: header,
                                     remaining: left, accumulated: acc)
            } else {
                // Full payload received
                self.dispatchMessage(header: header, payload: acc, connection: connection)
                self.receiveTCPMessages()
            }
        }
    }

    /// Dispatch a fully-received TCP message to the appropriate handler
    private func dispatchMessage(header: ProtocolHeader, payload: Data, connection: NWConnection) {
        guard let msgType = MessageType(rawValue: header.type) else {
            print("[Net] Unknown message type: \(header.type)")
            return
        }

        // Before authentication only the handshake and keepalives are allowed —
        // never inject input or clipboard data from an unverified peer.
        if !isAuthenticated {
            switch msgType {
            case .hello, .pairResponse, .ping, .pong, .disconnect:
                break
            default:
                print("[Net] Dropping \(msgType) from unauthenticated peer")
                return
            }
        }

        switch msgType {
        case .hello:
            handleHello(payload)
        case .ping:
            sendPong(sequence: header.sequence)
        case .pong:
            // Phase 15: Update heartbeat timestamp
            lastPongReceived = CACurrentMediaTime()
        case .keyframeReq:
            onKeyframeRequest?()
        case .qualityReport:
            handleQualityReport(payload)
        case .inputEvent:
            handleInputEvent(payload)
        case .clipboardData:
            handleClipboardData(payload)
        case .pairResponse:
            handlePairResponse(payload)
        case .disconnect:
            handleDisconnect(for: connection)
        default:
            print("[Net] Unhandled message type: \(msgType)")
        }
    }

    private func handleInputEvent(_ payload: Data) {
        guard let event = InputEventPayload.deserialize(from: payload) else {
            return  // Silent fail — input events are high frequency
        }
        onInputEvent?(event)
    }

    private func handleQualityReport(_ payload: Data) {
        guard let report = QualityReportPayload.deserialize(from: payload) else {
            print("[Net] Invalid QualityReport payload")
            return
        }
        onQualityReport?(report)
    }

    private func handleClipboardData(_ payload: Data) {
        guard !payload.isEmpty else { return }
        onClipboardData?(payload)
    }

    private func handleHello(_ payload: Data) {
        guard let hello = HelloPayload.deserialize(from: payload) else {
            print("[Net] Invalid HELLO payload")
            return
        }
        guard !isAuthenticated else {
            print("[Net] Ignoring repeated HELLO on an authenticated connection")
            return
        }

        print("[Net] HELLO from: \(hello.receiverName), UDP port: \(hello.udpPort), device: \(hello.deviceId.prefix(16))")
        remoteUDPPort = hello.udpPort

        // Store pending connection info for pairing
        pendingDeviceId = hello.deviceId
        pendingDeviceName = hello.receiverName

        // Every receiver must authenticate. A missing device ID means an old or
        // broken receiver build — reject instead of silently skipping pairing.
        guard let pm = pairingManager, !hello.deviceId.isEmpty else {
            print("[Pairing] ❌ HELLO without device ID from '\(hello.receiverName)' — rejecting")
            sendControlMessage(type: .pairReject, payload: Data())
            return
        }

        if pm.isPaired(deviceId: hello.deviceId) {
            // Already paired — send HMAC challenge
            let nonce = pm.generateNonce()
            var challenge = PairChallengePayload()
            challenge.type = PairChallengeType.hmac.rawValue
            challenge.nonce = nonce
            sendControlMessage(type: .pairChallenge, payload: challenge.serialize())
            print("[Pairing] Sent HMAC challenge to '\(hello.receiverName)'")
        } else {
            // Not paired — generate PIN and show in UI
            let pin = pm.generatePIN(forDevice: hello.receiverName)
            var challenge = PairChallengePayload()
            challenge.type = PairChallengeType.pin.rawValue
            challenge.nonce = Data(repeating: 0, count: 32)  // unused for PIN
            sendControlMessage(type: .pairChallenge, payload: challenge.serialize())
            print("[Pairing] Sent PIN challenge to '\(hello.receiverName)' — PIN: \(pin)")
        }

        DispatchQueue.main.async { [weak self] in
            self?.state = .pairing(hello.receiverName)
        }
    }

    /// Handle pairing response from Windows (PIN or HMAC)
    private func handlePairResponse(_ payload: Data) {
        guard let response = PairResponsePayload.deserialize(from: payload),
              let pm = pairingManager,
              let deviceId = pendingDeviceId else {
            print("[Pairing] Invalid pair response")
            sendControlMessage(type: .pairReject, payload: Data())
            return
        }

        let deviceName = pendingDeviceName ?? "Unknown"

        if response.type == PairChallengeType.pin.rawValue {
            // Validate PIN
            let enteredPIN = response.pinString
            if let pairingKey = pm.validatePIN(enteredPIN, deviceId: deviceId, deviceName: deviceName) {
                // Send PAIR_ACCEPT with the pairing key
                var accept = PairAcceptPayload()
                accept.newlyPaired = 1
                accept.pairingKey = pairingKey
                sendControlMessage(type: .pairAccept, payload: accept.serialize())

                // Now send WELCOME to proceed
                sendControlMessage(type: .welcome, payload: Data())
                markAuthenticated(deviceName)
                print("[Pairing] ✅ PIN accepted — paired with '\(deviceName)'")
            } else {
                // One attempt per PIN: the receiver reconnects and gets a fresh one
                pm.cancelPairing()
                sendControlMessage(type: .pairReject, payload: Data())
                print("[Pairing] ❌ PIN rejected from '\(deviceName)'")
            }

        } else if response.type == PairChallengeType.hmac.rawValue {
            // Validate HMAC
            let hmacData = response.data_
            if pm.validateHMAC(deviceId: deviceId, receivedHMAC: hmacData) {
                // Send PAIR_ACCEPT (no key since already paired)
                var accept = PairAcceptPayload()
                accept.newlyPaired = 0
                accept.pairingKey = Data(repeating: 0, count: 32)
                sendControlMessage(type: .pairAccept, payload: accept.serialize())

                // Now send WELCOME to proceed
                sendControlMessage(type: .welcome, payload: Data())
                markAuthenticated(deviceName)
                print("[Pairing] ✅ HMAC verified — auto-authenticated '\(deviceName)'")
            } else {
                sendControlMessage(type: .pairReject, payload: Data())
                print("[Pairing] ❌ HMAC failed from '\(deviceName)'")
            }
        }
    }

    /// Peer verified — switch to .connected, which the engine observes to start streaming
    private func markAuthenticated(_ peerName: String) {
        isAuthenticated = true
        DispatchQueue.main.async { [weak self] in
            self?.state = .connected(peerName)
        }
    }

    /// Forget the previous peer's authentication and hide any PIN still on screen
    private func resetAuthentication() {
        isAuthenticated = false
        pendingDeviceId = nil
        pendingDeviceName = nil
        pairingManager?.cancelPairing()
    }

    /// Handle client disconnect — only if the disconnected connection is still current
    private func handleDisconnect(for connection: NWConnection) {
        // Guard: only clean up if this is still the active connection
        // Prevents a stale callback from killing a new reconnection
        guard tcpConnection === connection else {
            print("[Net] Ignoring stale disconnect callback")
            return
        }
        stopHeartbeat()
        stopStreaming()
        tcpConnection?.cancel()
        tcpConnection = nil
        udpConnection?.cancel()
        udpConnection = nil
        remoteHost = nil
        resetAuthentication()
        print("[Net] Client disconnected — waiting for reconnection...")
        DispatchQueue.main.async { [weak self] in
            self?.state = .listening
        }
    }

    // ── Heartbeat (Phase 15) ─────────────────────────────────────────────────

    /// Start sending periodic Pings to detect dead connections
    private func startHeartbeat() {
        stopHeartbeat()
        let timer = DispatchSource.makeTimerSource(queue: networkQueue)
        timer.schedule(deadline: .now() + heartbeatInterval, repeating: heartbeatInterval)
        timer.setEventHandler { [weak self] in
            guard let self = self, self.tcpConnection != nil else { return }

            // Check if the peer has responded recently
            let now = CACurrentMediaTime()
            if now - self.lastPongReceived > self.heartbeatTimeout {
                print("[Net] ⚠️ Heartbeat timeout — peer not responding (\(Int(now - self.lastPongReceived))s)")
                if let conn = self.tcpConnection {
                    self.handleDisconnect(for: conn)
                }
                return
            }

            // Send Ping
            self.sendControlMessage(type: .ping, payload: Data())
        }
        timer.resume()
        heartbeatTimer = timer
    }

    /// Stop the heartbeat timer
    private func stopHeartbeat() {
        heartbeatTimer?.cancel()
        heartbeatTimer = nil
    }

    // ── Streaming Control ───────────────────────────────────────────────────

    /// Begin streaming: open UDP channel and send START_STREAM.
    /// Called from the main thread; connection state lives on networkQueue.
    func startStreaming() {
        networkQueue.async { [weak self] in
            self?.startStreamingOnNetworkQueue()
        }
    }

    private func startStreamingOnNetworkQueue() {
        guard tcpConnection != nil, let host = remoteHost else {
            print("[Net] Cannot start streaming — not connected")
            return
        }
        guard isAuthenticated else {
            print("[Net] Cannot start streaming — peer not authenticated")
            return
        }
        guard udpConnection == nil else { return }  // already streaming

        // Open UDP connection to receiver
        let udpParams = NWParameters.udp
        udpParams.allowLocalEndpointReuse = true
        let udp = NWConnection(
            host: host,
            port: NWEndpoint.Port(rawValue: remoteUDPPort)!,
            using: udpParams
        )
        udp.stateUpdateHandler = { state in
            if case .ready = state {
                print("[Net] UDP channel ready → \(host):\(self.remoteUDPPort)")
            }
        }
        udp.start(queue: networkQueue)
        udpConnection = udp

        // Send START_STREAM control message
        var startPayload = StartStreamPayload()
        startPayload.width = streamWidth
        startPayload.height = streamHeight
        startPayload.fps = streamFPS
        startPayload.bitrate = streamBitrate
        sendControlMessage(type: .startStream, payload: startPayload.serialize())

        streamStartTime = CACurrentMediaTime()
        frameSequenceNumber = 0
        udpPacketSequence = 0

        let peerName = pendingDeviceName ?? "Receiver"
        DispatchQueue.main.async { [weak self] in
            self?.state = .streaming(peerName)
        }

        print("[Net] Streaming started: \(streamWidth)×\(streamHeight) @ \(streamFPS) FPS")
    }

    /// Phase 15: capture restarted (possibly at a new resolution) — tell the
    /// receiver so it rebuilds its decoder, then it requests a keyframe.
    func restartStreamIfActive() {
        networkQueue.async { [weak self] in
            guard let self = self, self.isAuthenticated, self.udpConnection != nil else { return }
            var startPayload = StartStreamPayload()
            startPayload.width = self.streamWidth
            startPayload.height = self.streamHeight
            startPayload.fps = self.streamFPS
            startPayload.bitrate = self.streamBitrate
            self.sendControlMessage(type: .startStream, payload: startPayload.serialize())
            print("[Net] Stream parameters re-sent: \(self.streamWidth)×\(self.streamHeight)")
        }
    }

    func stopStreaming() {
        guard state.isStreaming else { return }
        sendControlMessage(type: .stopStream, payload: Data())
        udpConnection?.cancel()
        udpConnection = nil

        if remoteHost != nil {
            let peerName: String
            if case .streaming(let name) = state {
                peerName = name
            } else {
                peerName = "Receiver"
            }
            DispatchQueue.main.async { [weak self] in
                self?.state = .connected(peerName)
            }
        }
    }

    // ── Send Video Frame ────────────────────────────────────────────────────

    /// Send an H.264 Annex B frame over UDP (with fragmentation if needed)
    func sendVideoFrame(annexBData: Data, isKeyframe: Bool) {
        guard let udp = udpConnection else { return }

        let frameSeq = frameSequenceNumber
        frameSequenceNumber += 1

        let timestamp = UInt64((CACurrentMediaTime() - streamStartTime) * 1_000_000)
        let flags: UInt16 = isKeyframe ? MessageFlags.keyframe.rawValue : 0

        if annexBData.count <= ProtocolConstants.maxFragmentPayload {
            // ── Single packet ───────────────────────────────────────────
            var header = ProtocolHeader()
            header.type = MessageType.videoFrame.rawValue
            header.flags = flags
            header.sequence = nextUDPSequence()
            header.timestamp = timestamp
            header.payloadLength = UInt32(annexBData.count)

            var packet = header.serialize()
            packet.append(annexBData)

            // Compute and set CRC
            let crc = CRC32.compute(packet)
            packet.replaceSubrange(24..<28, with: withUnsafeBytes(of: crc.littleEndian) { Data($0) })

            udp.send(content: packet, completion: .contentProcessed { _ in })

            DispatchQueue.main.async { [weak self] in
                self?.networkStats.packetsSent += 1
                self?.networkStats.bytesSent += UInt64(packet.count)
                self?.networkStats.framesSent += 1
            }
        } else {
            // ── Fragmented ──────────────────────────────────────────────
            let maxDataPerFragment = ProtocolConstants.maxFragmentPayload
            let fragmentCount = (annexBData.count + maxDataPerFragment - 1) / maxDataPerFragment

            for i in 0..<fragmentCount {
                let offset = i * maxDataPerFragment
                let end = min(offset + maxDataPerFragment, annexBData.count)
                let fragmentData = annexBData.subdata(in: offset..<end)

                // Fragment header
                var fragHeader = FragmentHeader()
                fragHeader.frameSequence = frameSeq
                fragHeader.fragmentIndex = UInt16(i)
                fragHeader.fragmentTotal = UInt16(fragmentCount)

                // Protocol header
                var header = ProtocolHeader()
                header.type = MessageType.videoFragment.rawValue
                header.flags = flags
                header.sequence = nextUDPSequence()
                header.timestamp = timestamp
                header.payloadLength = UInt32(8 + fragmentData.count)

                var packet = header.serialize()
                packet.append(fragHeader.serialize())
                packet.append(fragmentData)

                // CRC
                let crc = CRC32.compute(packet)
                packet.replaceSubrange(24..<28, with: withUnsafeBytes(of: crc.littleEndian) { Data($0) })

                udp.send(content: packet, completion: .contentProcessed { _ in })

                DispatchQueue.main.async { [weak self] in
                    self?.networkStats.packetsSent += 1
                    self?.networkStats.bytesSent += UInt64(packet.count)
                }
            }

            DispatchQueue.main.async { [weak self] in
                self?.networkStats.framesSent += 1
            }
        }
    }

    // ── Control Messages ────────────────────────────────────────────────────

    private func sendControlMessage(type: MessageType, payload: Data) {
        guard let connection = tcpConnection else { return }

        var header = ProtocolHeader()
        header.type = type.rawValue
        header.sequence = nextSequence()
        header.timestamp = UInt64(CACurrentMediaTime() * 1_000_000)
        header.payloadLength = UInt32(payload.count)

        var data = header.serialize()
        data.append(payload)

        // CRC
        let crc = CRC32.compute(data)
        data.replaceSubrange(24..<28, with: withUnsafeBytes(of: crc.littleEndian) { Data($0) })

        connection.send(content: data, completion: .contentProcessed { error in
            if let error = error {
                print("[Net] TCP send error: \(error)")
            }
        })
    }

    private func sendPong(sequence: UInt32) {
        var header = ProtocolHeader()
        header.type = MessageType.pong.rawValue
        header.sequence = sequence
        header.timestamp = UInt64(CACurrentMediaTime() * 1_000_000)
        header.payloadLength = 0

        var data = header.serialize()
        let crc = CRC32.compute(data)
        data.replaceSubrange(24..<28, with: withUnsafeBytes(of: crc.littleEndian) { Data($0) })

        tcpConnection?.send(content: data, completion: .contentProcessed { _ in })
    }

    private func nextSequence() -> UInt32 {
        sequenceNumber += 1
        return sequenceNumber
    }

    /// Consecutive per-packet counter on the UDP channel (encoder callback thread only)
    private func nextUDPSequence() -> UInt32 {
        udpPacketSequence &+= 1
        return udpPacketSequence
    }

    // ── Clipboard ───────────────────────────────────────────────────────────

    /// Send clipboard text data to the Windows receiver
    func sendClipboardData(_ data: Data) {
        // Called from the main thread; connection state lives on networkQueue
        networkQueue.async { [weak self] in
            guard let self = self, self.isAuthenticated, self.udpConnection != nil else { return }
            self.sendControlMessage(type: .clipboardData, payload: data)
        }
    }
}
