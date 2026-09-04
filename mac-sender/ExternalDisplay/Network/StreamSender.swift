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
    case connected(String)    // peer name
    case streaming(String)    // peer name
    case error(String)

    var displayText: String {
        switch self {
        case .idle:             return "Idle"
        case .listening:        return "Listening on port \(ProtocolConstants.tcpPort)…"
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

    // ── Stream state ────────────────────────────────────────────────────────

    private var remoteHost: NWEndpoint.Host?
    private var remoteUDPPort: UInt16 = ProtocolConstants.udpPort
    private var sequenceNumber: UInt32 = 0
    private var frameSequenceNumber: UInt32 = 0
    private var streamStartTime: Double = 0

    // Stream config (set before startStreaming)
    var streamWidth: UInt32 = 0
    var streamHeight: UInt32 = 0
    var streamFPS: UInt32 = 60
    var streamBitrate: UInt32 = 15_000_000

    // ── Lifecycle ───────────────────────────────────────────────────────────

    /// Start listening for incoming connections
    func startListening() {
        guard case .idle = state else { return }

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
                default:
                    break
                }
            }

            listener.newConnectionHandler = { [weak self] connection in
                // Only accept one connection at a time
                if self?.tcpConnection != nil {
                    connection.cancel()
                    return
                }
                self?.handleNewTCPConnection(connection)
            }

            listener.start(queue: networkQueue)
            tcpListener = listener

        } catch {
            DispatchQueue.main.async { [weak self] in
                self?.state = .error("Listen failed: \(error.localizedDescription)")
            }
        }
    }

    /// Stop everything
    func stop() {
        stopStreaming()
        tcpConnection?.cancel()
        tcpConnection = nil
        udpConnection?.cancel()
        udpConnection = nil
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

        // Extract remote host for UDP
        if case .hostPort(let host, _) = connection.endpoint {
            remoteHost = host
        }

        connection.stateUpdateHandler = { [weak self] state in
            switch state {
            case .ready:
                print("[Net] TCP connection ready")
                self?.receiveTCPMessages()
            case .failed(let error):
                print("[Net] TCP connection failed: \(error)")
                self?.handleDisconnect()
            case .cancelled:
                print("[Net] TCP connection cancelled")
                self?.handleDisconnect()
            default:
                break
            }
        }

        connection.start(queue: networkQueue)
    }

    private func receiveTCPMessages() {
        guard let connection = tcpConnection else { return }

        // Read header first (28 bytes)
        connection.receive(
            minimumIncompleteLength: ProtocolConstants.headerSize,
            maximumLength: 65536
        ) { [weak self] data, _, isComplete, error in
            guard let self = self else { return }

            if let error = error {
                print("[Net] TCP receive error: \(error)")
                self.handleDisconnect()
                return
            }

            if isComplete {
                print("[Net] TCP connection closed by peer")
                self.handleDisconnect()
                return
            }

            if let data = data, !data.isEmpty {
                self.handleTCPData(data)
            }

            // Continue receiving
            self.receiveTCPMessages()
        }
    }

    private func handleTCPData(_ data: Data) {
        guard let header = ProtocolHeader.deserialize(from: data) else {
            print("[Net] Invalid header received")
            return
        }

        guard let msgType = MessageType(rawValue: header.type) else {
            print("[Net] Unknown message type: \(header.type)")
            return
        }

        let payload = data.count > ProtocolConstants.headerSize
            ? data.subdata(in: ProtocolConstants.headerSize..<data.count)
            : Data()

        switch msgType {
        case .hello:
            handleHello(payload)
        case .ping:
            sendPong(sequence: header.sequence)
        case .keyframeReq:
            print("[Net] Keyframe requested")
            // TODO: Signal encoder to force keyframe
        case .disconnect:
            handleDisconnect()
        default:
            print("[Net] Unhandled message type: \(msgType)")
        }
    }

    private func handleHello(_ payload: Data) {
        guard let hello = HelloPayload.deserialize(from: payload) else {
            print("[Net] Invalid HELLO payload")
            return
        }

        print("[Net] HELLO from: \(hello.receiverName), UDP port: \(hello.udpPort)")
        remoteUDPPort = hello.udpPort

        // Send WELCOME back
        let welcomePayload = Data()  // Empty for now — display info sent in START_STREAM
        sendControlMessage(type: .welcome, payload: welcomePayload)

        DispatchQueue.main.async { [weak self] in
            self?.state = .connected(hello.receiverName)
        }
    }

    private func handleDisconnect() {
        stopStreaming()
        tcpConnection?.cancel()
        tcpConnection = nil
        udpConnection?.cancel()
        udpConnection = nil
        remoteHost = nil
        DispatchQueue.main.async { [weak self] in
            self?.state = .listening
        }
    }

    // ── Streaming Control ───────────────────────────────────────────────────

    /// Begin streaming: open UDP channel and send START_STREAM
    func startStreaming() {
        guard tcpConnection != nil, let host = remoteHost else {
            print("[Net] Cannot start streaming — not connected")
            return
        }

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

        let peerName: String
        if case .connected(let name) = state {
            peerName = name
        } else {
            peerName = "Unknown"
        }

        DispatchQueue.main.async { [weak self] in
            self?.state = .streaming(peerName)
        }

        print("[Net] Streaming started: \(streamWidth)×\(streamHeight) @ \(streamFPS) FPS")
    }

    func stopStreaming() {
        guard state.isStreaming else { return }
        sendControlMessage(type: .stopStream, payload: Data())
        udpConnection?.cancel()
        udpConnection = nil

        if let host = remoteHost {
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
            header.sequence = nextSequence()
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
                header.sequence = nextSequence()
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
}
