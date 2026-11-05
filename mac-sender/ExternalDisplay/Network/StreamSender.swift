// =============================================================================
// StreamSender.swift — Network sender for Mac → Windows streaming
// =============================================================================
// Manages TCP control channel, UDP data channel, and Bonjour service.
//
// Connection flow (protocol v2):
//   1. Start listening (TCP + Bonjour)
//   2. Windows connects via TCP → HELLO / KEY_EXCHANGE / KEY_REVEAL
//   3. Authentication: PAIR_CONFIRM / PAIR_ACCEPT (new devices: the user
//      approves after Windows entered the verification code)
//   4. Everything after PAIR_ACCEPT is AES-GCM encrypted: START_STREAM,
//      video over UDP (fragmented if needed), input, clipboard, statistics
//
// Uses Apple's Network.framework (NWConnection/NWListener).
// =============================================================================

import Foundation
import Network
import Combine
import QuartzCore
import CryptoKit

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
    var parityPacketsSent: UInt64 = 0        // included in packetsSent (step 5)
    var pacingSpreadUs: Double = 0           // a paced frame, first to last datagram (0: not paced)
    var framesSent: UInt64 = 0
    var sendMbps: Double = 0                 // last second
    var sendLatency = LatencySummary()       // encoder output → packetization start

    // Reported by the receiver (QUALITY_REPORT, every 2 s)
    var receiverRttMs: Double? = nil
    var receiverLatencyMs: Double? = nil     // capture → on screen, clock-synced
    var receiverLossPercent: Double = 0
}

final class StreamSender: ObservableObject {

    // ── Published state ─────────────────────────────────────────────────────

    @Published var state: SenderState = .idle

    /// Counters are updated per packet on the network queue and read by the UI
    /// through statsSnapshot() — never published per packet (that was ~3,600
    /// main-thread dispatches per second at 40 Mbps).
    private var counters = NetworkStats()
    private var sendRate = RateMeter()
    private var sendLatencies = RollingWindow(capacity: 120)
    private let countersLock = NSLock()

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

    /// Called when the receiver announces what it can do (RECEIVER_CAPS: flags
    /// and its screen's refresh rate, after authentication). Invoked on the
    /// network queue.
    var onReceiverCaps: ((ReceiverCapsPayload) -> Void)?

    /// Queuing delay and received rate, every 100 ms (step 6). Network queue.
    var onRateFeedback: ((RateFeedbackPayload) -> Void)?

    /// Pairing manager for secure authentication
    var pairingManager: PairingManager?

    // ── Security (protocol v2, networkQueue only) ───────────────────────────

    /// Handshake of the current connection, until it is accepted
    private struct Handshake {
        var hello: HelloPayload
        var helloBytes: Data              // exactly as received: part of the transcript
        var mode: HandshakeMode
        var ephemeral: P256.KeyAgreement.PrivateKey
        var keyExchangeBytes: Data
        var keys: SessionKeys? = nil      // after KEY_REVEAL
        var windowsConfirmed = false      // new pairing: Windows entered the code
    }
    private var handshake: Handshake?

    /// Encryption keys and counters; set when PAIR_ACCEPT is sent.
    /// Nothing except the handshake is accepted or sent before this.
    private var session: SecureSession?
    private var isAuthenticated: Bool { session != nil }
    private var peerName = "Receiver"

    // ── Loss repair and pacing (docs/SMOOTHNESS.md, step 5; networkQueue) ──
    /// Asked for by the receiver in RECEIVER_CAPS, per connection
    private var sendParity = false
    private var pacePackets = false
    private let pacer = PacketPacer()

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
        Log.warning(.net, "Restarting listener in 2s…")
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
            // TCP_NODELAY: control messages are tiny (PONG, clipboard, START);
            // Nagle would hold them back behind unacknowledged data.
            let tcpOptions = NWProtocolTCP.Options()
            tcpOptions.noDelay = true
            let params = NWParameters(tls: nil, tcp: tcpOptions)
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
                    Log.info(.net, "TCP listener ready on port \(ProtocolConstants.tcpPort)")
                    DispatchQueue.main.async {
                        self?.state = .listening
                    }
                case .failed(let error):
                    Log.error(.net, "Listener failed: \(error)")
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
                    Log.warning(.net, "New connection while already connected — dropping old connection for reconnect")
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

    /// Stop streaming and disconnect client, but keep listener alive for reconnects.
    /// Runs on the network queue: the session's nonce counters live there.
    func stop() {
        networkQueue.sync { self.stopOnNetworkQueue() }
    }

    private func stopOnNetworkQueue() {
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
        }
        resetCounters()
    }

    /// Full shutdown — stop everything including the listener
    func shutdown() {
        networkQueue.sync { self.shutdownOnNetworkQueue() }
    }

    private func shutdownOnNetworkQueue() {
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
        }
        resetCounters()
    }

    // ── TCP Connection Handling ─────────────────────────────────────────────

    private func handleNewTCPConnection(_ connection: NWConnection) {
        Log.info(.net, "New TCP connection from \(connection.endpoint)")
        tcpConnection = connection
        resetAuthentication()

        // Extract remote host for UDP
        if case .hostPort(let host, _) = connection.endpoint {
            remoteHost = host
        }

        connection.stateUpdateHandler = { [weak self] state in
            switch state {
            case .ready:
                Log.info(.net, "TCP connection ready")
                self?.lastPongReceived = CACurrentMediaTime()
                self?.startHeartbeat()
                self?.receiveTCPMessages()
            case .failed(let error):
                Log.error(.net, "TCP connection failed: \(error)")
                self?.stopHeartbeat()
                self?.handleDisconnect(for: connection)
            case .cancelled:
                Log.debug(.net, "TCP connection cancelled")
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
                Log.error(.net, "TCP receive error: \(error)")
                self.handleDisconnect(for: connection)
                return
            }

            if isComplete {
                Log.info(.net, "TCP connection closed by peer")
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
                Log.warning(.net, "Control payload too large (\(payloadLen) bytes) — dropping connection")
                self.handleDisconnect(for: connection)
                return
            }

            if payloadLen == 0 {
                // No payload — dispatch immediately
                self.processMessage(header: header, rawHeader: headerData, payload: Data(),
                                    connection: connection)
                self.receiveTCPMessages()
            } else {
                // Step 2: Read exactly payloadLength bytes
                self.readTCPPayload(connection: connection, header: header, rawHeader: headerData,
                                     remaining: payloadLen, accumulated: Data())
            }
        }
    }

    /// Read exactly `remaining` bytes of payload from TCP, accumulating chunks
    private func readTCPPayload(connection: NWConnection, header: ProtocolHeader, rawHeader: Data,
                                 remaining: Int, accumulated: Data) {
        connection.receive(
            minimumIncompleteLength: remaining,
            maximumLength: remaining
        ) { [weak self] data, _, isComplete, error in
            guard let self = self else { return }

            if let error = error {
                Log.error(.net, "TCP payload read error: \(error)")
                self.handleDisconnect(for: connection)
                return
            }

            if isComplete && (data == nil || data!.isEmpty) {
                Log.info(.net, "TCP connection closed during payload read")
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
                self.readTCPPayload(connection: connection, header: header, rawHeader: rawHeader,
                                     remaining: left, accumulated: acc)
            } else {
                // Full payload received
                self.processMessage(header: header, rawHeader: rawHeader, payload: acc,
                                    connection: connection)
                self.receiveTCPMessages()
            }
        }
    }

    /// Version check and decryption, then dispatch
    private func processMessage(header: ProtocolHeader, rawHeader: Data, payload: Data,
                                connection: NWConnection) {
        guard header.version == ProtocolConstants.version else {
            Log.error(.net, "Receiver speaks protocol v\(header.version), this Mac app v\(ProtocolConstants.version) — update both apps")
            reject(.versionMismatch)
            return
        }

        if MessageFlags(rawValue: header.flags).contains(.encrypted) {
            guard let session else {
                Log.warning(.net, "Encrypted message before the handshake finished — dropping connection")
                handleDisconnect(for: connection)
                return
            }
            do {
                let plaintext = try session.openControl(payload, aad: Data(rawHeader.prefix(ProtocolConstants.headerSize)))
                dispatchMessage(header: header, payload: plaintext, connection: connection)
            } catch {
                Log.error(.net, "Control message failed authentication — dropping connection")
                handleDisconnect(for: connection)
            }
            return
        }

        if isAuthenticated {
            // Only a PONG the receiver sent before it saw PAIR_ACCEPT may still be plaintext
            if header.type == MessageType.pong.rawValue {
                lastPongReceived = CACurrentMediaTime()
                return
            }
            Log.warning(.net, "Unencrypted message type \(header.type) after authentication — dropping connection")
            handleDisconnect(for: connection)
            return
        }
        dispatchMessage(header: header, payload: payload, connection: connection)
    }

    /// Dispatch a fully-received TCP message to the appropriate handler
    private func dispatchMessage(header: ProtocolHeader, payload: Data, connection: NWConnection) {
        guard let msgType = MessageType(rawValue: header.type) else {
            Log.warning(.net, "Unknown message type: \(header.type)")
            return
        }

        // Before authentication only the handshake and keepalives are allowed —
        // never inject input or clipboard data from an unverified peer.
        if !isAuthenticated {
            switch msgType {
            case .hello, .keyReveal, .pairConfirm, .ping, .pong, .disconnect:
                break
            default:
                Log.warning(.net, "Dropping \(msgType) from unauthenticated peer")
                return
            }
        }

        switch msgType {
        case .hello:
            handleHello(payload)
        case .ping:
            sendPong(sequence: header.sequence, echoTimestamp: header.timestamp)
        case .pong:
            // Phase 15: Update heartbeat timestamp
            lastPongReceived = CACurrentMediaTime()
        case .keyframeReq:
            onKeyframeRequest?()
        case .qualityReport:
            handleQualityReport(payload)
        case .rateFeedback:
            if let feedback = RateFeedbackPayload.deserialize(from: payload) { onRateFeedback?(feedback) }
        case .inputEvent:
            handleInputEvent(payload)
        case .clipboardData:
            handleClipboardData(payload)
        case .receiverCaps:
            if let caps = ReceiverCapsPayload.deserialize(from: payload) {
                sendParity = caps.flags & ReceiverCapsPayload.parity != 0
                pacePackets = caps.flags & ReceiverCapsPayload.pacing != 0
                Log.info(.net, "Receiver capabilities: 0x\(String(caps.flags, radix: 16)) — parity "
                         + (sendParity ? "on" : "off") + ", pacing " + (pacePackets ? "on" : "off")
                         + ", screen \(caps.refreshHz) Hz")
                onReceiverCaps?(caps)
            }
        case .keyReveal:
            handleKeyReveal(payload)
        case .pairConfirm:
            handlePairConfirm(payload)
        case .disconnect:
            handleDisconnect(for: connection)
        default:
            Log.warning(.net, "Unhandled message type: \(msgType)")
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
            Log.error(.net, "Invalid QualityReport payload")
            return
        }
        countersLock.lock()
        counters.receiverRttMs = report.rttMs > 0 ? Double(report.rttMs) : nil
        counters.receiverLatencyMs = report.latencyUs > 0 ? Double(report.latencyUs) / 1000 : nil
        counters.receiverLossPercent = Double(report.packetLossPercent) / 100
        countersLock.unlock()
        onQualityReport?(report)
    }

    private func handleClipboardData(_ payload: Data) {
        guard !payload.isEmpty else { return }
        onClipboardData?(payload)
    }

    // ── Handshake (protocol v2, see SecureChannel.swift) ────────────────────

    private func handleHello(_ payload: Data) {
        guard handshake == nil, !isAuthenticated else {
            Log.warning(.net, "Ignoring repeated HELLO")
            return
        }
        // Every receiver authenticates; a missing device ID is a broken receiver
        guard let hello = HelloPayload.deserialize(from: payload), !hello.deviceId.isEmpty else {
            Log.warning(.pairing, "Malformed HELLO — rejecting")
            reject(.badHandshake)
            return
        }
        remoteUDPPort = hello.udpPort
        peerName = hello.receiverName

        let mode: HandshakeMode = pairingManager?.pairingKey(for: hello.deviceId) != nil ? .paired : .newPairing
        let ephemeral = P256.KeyAgreement.PrivateKey()
        var keyExchange = KeyExchangePayload()
        keyExchange.mode = mode
        keyExchange.publicKey = ephemeral.publicKey.x963Representation
        let keyExchangeBytes = keyExchange.serialize()

        handshake = Handshake(hello: hello, helloBytes: Data(payload.prefix(HelloPayload.size)),
                              mode: mode, ephemeral: ephemeral, keyExchangeBytes: keyExchangeBytes)
        sendControlMessage(type: .keyExchange, payload: keyExchangeBytes)
        Log.info(.pairing, "HELLO from '\(hello.receiverName)' (UDP port \(hello.udpPort)) — "
                 + (mode == .paired ? "paired receiver, authenticating" : "new receiver, pairing"))

        DispatchQueue.main.async { [weak self] in
            self?.state = .pairing(hello.receiverName)
        }
    }

    private func handleKeyReveal(_ payload: Data) {
        guard var hs = handshake, hs.keys == nil,
              let reveal = KeyRevealPayload.deserialize(from: payload) else {
            reject(.badHandshake)
            return
        }
        // The key must be the one committed to in HELLO (prevents code searching)
        guard KeySchedule.commitment(publicKey: reveal.publicKey) == hs.hello.commitment else {
            Log.warning(.pairing, "Receiver's key does not match its commitment — rejecting")
            reject(.badHandshake)
            return
        }
        do {
            let shared = try KeySchedule.sharedSecret(privateKey: hs.ephemeral, peerPublicKey: reveal.publicKey)
            let transcript = KeySchedule.transcriptHash(hello: hs.helloBytes, keyExchange: hs.keyExchangeBytes,
                                                        keyReveal: reveal.publicKey)
            let pairingKey = hs.mode == .paired ? pairingManager?.pairingKey(for: hs.hello.deviceId) : nil
            let keys = KeySchedule.derive(sharedSecret: shared, pairingKey: pairingKey, transcriptHash: transcript)
            hs.keys = keys
            handshake = hs
            if hs.mode == .newPairing {
                pairingManager?.beginRequest(deviceId: hs.hello.deviceId, deviceName: hs.hello.receiverName,
                                             verificationCode: keys.verificationCode)
            }
        } catch {
            Log.warning(.pairing, "Invalid public key from receiver — rejecting")
            reject(.badHandshake)
        }
    }

    private func handlePairConfirm(_ payload: Data) {
        guard var hs = handshake, let keys = hs.keys,
              let confirm = PairConfirmPayload.deserialize(from: payload) else {
            reject(.badHandshake)
            return
        }
        guard KeySchedule.verify(confirm.mac, key: keys.confirmWin, transcriptHash: keys.transcriptHash) else {
            if hs.mode == .paired {
                // Its key differs from ours (it reset, or it is not who it claims): pair again
                Log.warning(.pairing, "'\(hs.hello.receiverName)' failed authentication — it has to pair again")
                reject(.unknownDevice)
            } else {
                Log.warning(.pairing, "Invalid pairing confirmation from '\(hs.hello.receiverName)'")
                reject(.badHandshake)
            }
            return
        }

        if hs.mode == .paired {
            accept(newlyPaired: false)
        } else {
            hs.windowsConfirmed = true
            handshake = hs
            pairingManager?.markWindowsConfirmed(deviceId: hs.hello.deviceId)
        }
    }

    /// The user approved or rejected a new pairing (called from the main thread)
    func resolvePairing(deviceId: String, approved: Bool) {
        networkQueue.async { [weak self] in
            guard let self, let hs = self.handshake, hs.hello.deviceId == deviceId,
                  hs.mode == .newPairing else { return }
            if approved, hs.windowsConfirmed, let keys = hs.keys {
                self.pairingManager?.savePairing(deviceId: deviceId, deviceName: hs.hello.receiverName,
                                                 pairingKey: keys.pairingKey)
                self.accept(newlyPaired: true)
            } else {
                Log.info(.pairing, "Pairing with '\(hs.hello.receiverName)' rejected on the Mac")
                self.reject(.userRejected)
            }
        }
    }

    /// Prove the transcript to the receiver and switch to encryption
    private func accept(newlyPaired: Bool) {
        guard let hs = handshake, let keys = hs.keys else { return }
        var acceptPayload = PairAcceptPayload()
        acceptPayload.newlyPaired = newlyPaired
        acceptPayload.mac = KeySchedule.confirmation(keys.confirmMac, transcriptHash: keys.transcriptHash)
        sendControlMessage(type: .pairAccept, payload: acceptPayload.serialize())   // last plaintext message

        session = SecureSession(keys: keys)
        handshake = nil
        sendControlMessage(type: .welcome, payload: Data())
        markAuthenticated(peerName)
        Log.info(.pairing, "'\(peerName)' authenticated — session encrypted")
    }

    /// Refuse the handshake and close the connection once the reason is out
    private func reject(_ reason: PairRejectReason) {
        sendControlMessage(type: .pairReject, payload: PairRejectPayload(reason: reason).serialize())
        pairingManager?.cancelRequest()
        guard let connection = tcpConnection else { return }
        networkQueue.asyncAfter(deadline: .now() + 0.2) { [weak self] in
            self?.handleDisconnect(for: connection)
        }
    }

    /// Peer verified — switch to .connected, which the engine observes to start streaming
    private func markAuthenticated(_ peerName: String) {
        DispatchQueue.main.async { [weak self] in
            self?.state = .connected(peerName)
        }
    }

    /// Forget the previous peer's keys and hide any pairing request still on screen
    private func resetAuthentication() {
        handshake = nil
        session = nil
        sendParity = false   // the next receiver says what it wants
        pacePackets = false
        pairingManager?.cancelRequest()
    }

    /// Handle client disconnect — only if the disconnected connection is still current
    private func handleDisconnect(for connection: NWConnection) {
        // Guard: only clean up if this is still the active connection
        // Prevents a stale callback from killing a new reconnection
        guard tcpConnection === connection else {
            Log.debug(.net, "Ignoring stale disconnect callback")
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
        Log.info(.net, "Client disconnected — waiting for reconnection...")
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
                Log.warning(.net, "Heartbeat timeout — peer not responding (\(Int(now - self.lastPongReceived))s)")
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
            Log.error(.net, "Cannot start streaming — not connected")
            return
        }
        guard isAuthenticated else {
            Log.error(.net, "Cannot start streaming — peer not authenticated")
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
                Log.info(.net, "UDP channel ready → \(host):\(self.remoteUDPPort)")
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

        let name = peerName
        DispatchQueue.main.async { [weak self] in
            self?.state = .streaming(name)
        }

        Log.info(.net, "Streaming started: \(streamWidth)×\(streamHeight) @ \(streamFPS) FPS")
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
            Log.info(.net, "Stream parameters re-sent: \(self.streamWidth)×\(self.streamHeight)")
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

    /// Send an H.264 Annex B frame over UDP (fragmented if needed).
    /// `frameData` must start with FrameTimingPrefix.size reserved bytes
    /// (H264Converter reservedPrefix) — they are filled in here.
    /// Called on VideoToolbox's callback thread; the work runs on the network
    /// queue, which owns the UDP connection and the sequence counters.
    func sendVideoFrame(frameData: Data, isKeyframe: Bool, timing: FrameTiming) {
        networkQueue.async { [weak self] in
            self?.sendVideoFrameOnNetworkQueue(frameData, isKeyframe: isKeyframe, timing: timing)
        }
    }

    private func sendVideoFrameOnNetworkQueue(_ frameData: Data, isKeyframe: Bool, timing: FrameTiming) {
        guard let udp = udpConnection, let session, frameData.count > FrameTimingPrefix.size else { return }

        let frameSeq = frameSequenceNumber
        frameSequenceNumber &+= 1

        // ── Phase 16: per-frame timestamps for the receiver's latency breakdown
        var prefix = FrameTimingPrefix()
        prefix.frameSequence = frameSeq
        prefix.flags = timing.captureIsReal ? FrameTimingPrefix.flagCaptureTimeValid : 0
        prefix.captureUs = timing.captureUs
        prefix.encodeStartUs = timing.encodeStartUs
        prefix.encodeEndUs = timing.encodeEndUs
        prefix.sendUs = HostClock.nowUs()

        var frame = frameData
        frame.replaceSubrange(frame.startIndex..<frame.startIndex + FrameTimingPrefix.size,
                              with: prefix.serialize())

        let timestamp = UInt64((CACurrentMediaTime() - streamStartTime) * 1_000_000)
        var flags = MessageFlags.timing
        if isKeyframe { flags.insert(.keyframe) }

        // Datagrams are built (numbered, encrypted) here in order, then sent
        // at once or paced
        var datagrams: [Data] = []
        var parityPackets = 0

        if frame.count <= ProtocolConstants.maxFragmentPayload && !sendParity {
            // ── Single packet ───────────────────────────────────────────
            datagrams.append(encryptedPacket(type: .videoFrame, flags: flags, timestamp: timestamp,
                                             payload: frame, session: session))
        } else {
            // ── Fragmented (also one fragment of one: parity refers to fragments)
            let maxDataPerFragment = ProtocolConstants.maxFragmentPayload
            let fragmentCount = (frame.count + maxDataPerFragment - 1) / maxDataPerFragment

            for i in 0..<fragmentCount {
                let offset = frame.startIndex + i * maxDataPerFragment
                let end = min(offset + maxDataPerFragment, frame.endIndex)

                var fragHeader = FragmentHeader()
                fragHeader.frameSequence = frameSeq
                fragHeader.fragmentIndex = UInt16(i)
                fragHeader.fragmentTotal = UInt16(fragmentCount)

                var payload = Data(capacity: 8 + end - offset)
                payload.append(fragHeader.serialize())
                payload.append(frame[offset..<end])
                datagrams.append(encryptedPacket(type: .videoFragment, flags: flags, timestamp: timestamp,
                                                 payload: payload, session: session))
            }

            // ── Parity after the data: rebuilds a lost fragment per group ──
            if sendParity {
                let groups = FrameParity.groupCount(fragments: fragmentCount)
                for group in 0..<groups {
                    var header = ParityHeader()
                    header.frameSequence = frameSeq
                    header.fragmentTotal = UInt16(fragmentCount)
                    header.groupIndex = UInt16(group)
                    header.groupCount = UInt16(groups)
                    header.frameBytes = UInt32(frame.count)
                    var payload = header.serialize()
                    payload.append(FrameParity.parity(of: frame, fragmentSize: maxDataPerFragment,
                                                      group: group, groups: groups))
                    datagrams.append(encryptedPacket(type: .videoParity, flags: flags, timestamp: timestamp,
                                                     payload: payload, session: session))
                }
                parityPackets = groups
            }
        }

        if pacePackets && datagrams.count > PacingPlan.burst {
            pacer.send(datagrams, over: udp)
        } else {
            for packet in datagrams { udp.send(content: packet, completion: .contentProcessed { _ in }) }
        }
        let packets = datagrams.count
        let bytes = datagrams.reduce(0) { $0 + $1.count }

        let sendLatencyMs = prefix.sendUs >= timing.encodeEndUs
            ? Double(prefix.sendUs - timing.encodeEndUs) / 1000 : 0
        countersLock.lock()
        counters.packetsSent += UInt64(packets)
        counters.parityPacketsSent += UInt64(parityPackets)
        counters.bytesSent += UInt64(bytes)
        counters.framesSent += 1
        sendRate.record(at: CACurrentMediaTime(), bytes: bytes)
        sendLatencies.add(sendLatencyMs)
        countersLock.unlock()
    }

    /// header ‖ counter(8, big-endian) ‖ AES-GCM(payload) ‖ tag.
    /// Header and counter are authenticated as associated data.
    private func encryptedPacket(type: MessageType, flags: MessageFlags, timestamp: UInt64,
                                 payload: Data, session: SecureSession) -> Data {
        var header = ProtocolHeader()
        header.type = type.rawValue
        header.flags = flags.union(.encrypted).rawValue
        header.sequence = nextUDPSequence()
        header.timestamp = timestamp
        header.payloadLength = UInt32(ProtocolConstants.udpCounterSize + payload.count + PacketCipher.tagSize)

        let counter = session.nextVideoCounter()
        var packet = header.serialize()
        withUnsafeBytes(of: counter.bigEndian) { packet.append(contentsOf: $0) }
        packet.append(session.sealVideo(payload, counter: counter, aad: packet))
        return packet
    }

    // ── Stats ───────────────────────────────────────────────────────────────

    /// Thread-safe copy of the counters, with rates over the last second
    func statsSnapshot() -> NetworkStats {
        countersLock.lock()
        defer { countersLock.unlock() }
        var snapshot = counters
        snapshot.sendMbps = sendRate.megabitsPerSecond(now: CACurrentMediaTime())
        snapshot.sendLatency = sendLatencies.summary()
        snapshot.pacingSpreadUs = pacer.averageSpreadUs()
        return snapshot
    }

    private func resetCounters() {
        countersLock.lock()
        counters = NetworkStats()
        sendRate.reset()
        sendLatencies.reset()
        countersLock.unlock()
    }

    /// Phase 16: sender-side performance for the receiver's diagnostics overlay
    func sendSenderStats(_ stats: SenderStatsPayload) {
        networkQueue.async { [weak self] in
            guard let self = self, self.isAuthenticated, self.udpConnection != nil else { return }
            self.sendControlMessage(type: .senderStats, payload: stats.serialize())
        }
    }

    /// Diagnostics: runs `probe` on the network queue (tests: is it responsive?)
    func probeNetworkQueue(_ probe: @escaping () -> Void) {
        networkQueue.async(execute: probe)
    }

    // ── Pointer (docs/SMOOTHNESS.md, step 1) ────────────────────────────────

    /// Pointer image for a receiver that draws the pointer. Any thread.
    func sendCursorShape(_ shape: CursorShapePayload) {
        let payload = shape.serialize()
        networkQueue.async { [weak self] in
            guard let self = self, self.isAuthenticated else { return }
            self.sendControlMessage(type: .cursorShape, payload: payload)
        }
    }

    /// Pointer moved on the Mac. Any thread.
    func sendCursorPosition(_ position: CursorPositionPayload) {
        networkQueue.async { [weak self] in
            guard let self = self, self.isAuthenticated else { return }
            self.sendControlMessage(type: .cursorPosition, payload: position.serialize())
        }
    }

    // ── Control Messages ────────────────────────────────────────────────────

    /// Send on the TCP control channel (network queue only: the encryption
    /// counter must advance in send order). Encrypted once authenticated;
    /// handshake messages are plaintext with a CRC.
    private func sendControlMessage(type: MessageType, payload: Data, sequence: UInt32? = nil) {
        guard let connection = tcpConnection else { return }

        var header = ProtocolHeader()
        header.type = type.rawValue
        header.sequence = sequence ?? nextSequence()
        header.timestamp = HostClock.nowUs()

        var data: Data
        if let session {
            header.flags = MessageFlags.encrypted.rawValue
            header.payloadLength = UInt32(payload.count + PacketCipher.tagSize)
            data = header.serialize()   // CRC 0: the tag authenticates header + payload
            data.append(session.sealControl(payload, aad: data))
        } else {
            header.payloadLength = UInt32(payload.count)
            data = header.serialize()
            data.append(payload)
            let crc = CRC32.compute(data)
            data.replaceSubrange(24..<28, with: withUnsafeBytes(of: crc.littleEndian) { Data($0) })
        }

        connection.send(content: data, completion: .contentProcessed { error in
            if let error = error {
                Log.every(5, key: "tcp-send-error", .warning, .net, "TCP send error: \(error)")
            }
        })
    }

    /// PONG carries our clock in the header and echoes the PING's timestamp
    /// in the payload — the receiver derives RTT and the Mac↔PC clock offset.
    private func sendPong(sequence: UInt32, echoTimestamp: UInt64) {
        var payload = Data()
        payload.appendLE(echoTimestamp)
        sendControlMessage(type: .pong, payload: payload, sequence: sequence)
    }

    private func nextSequence() -> UInt32 {
        sequenceNumber += 1
        return sequenceNumber
    }

    /// Consecutive per-packet counter on the UDP channel (network queue only)
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
