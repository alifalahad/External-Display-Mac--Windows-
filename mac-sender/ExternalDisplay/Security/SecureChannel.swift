// =============================================================================
// SecureChannel.swift — Protocol v2 key agreement and session encryption
// =============================================================================
// Standard primitives only (CryptoKit): ECDH P-256, SHA-256, HKDF-SHA256,
// HMAC-SHA256, AES-256-GCM. The construction follows ZRTP (RFC 6189) and
// Bluetooth "numeric comparison" pairing; see docs/SECURITY.md.
//
// Handshake (every connection):
//   Win → Mac  HELLO        name, UDP port, device ID, commit = SHA256(eW)
//   Mac → Win  KEY_EXCHANGE mode (new pairing / paired), eM
//   Win → Mac  KEY_REVEAL   eW            (Mac checks SHA256(eW) == commit)
//   both:      Z  = ECDH(e, e')                       32-byte x-coordinate
//              TH = SHA256("EXDP/2 handshake" ‖ HELLO ‖ KEY_EXCHANGE ‖ KEY_REVEAL)
//              keys = HKDF-SHA256(ikm = Z [‖ pairing key], salt = TH, info = label)
//   Win → Mac  PAIR_CONFIRM HMAC(confirm-win, TH)
//   Mac → Win  PAIR_ACCEPT  HMAC(confirm-mac, TH)  → everything after is encrypted
//
// New pairing: both sides derive the same 6-digit code from TH. The Mac shows
// it, the user types it on Windows (Windows refuses a mismatch = someone in
// the middle), and the user approves on the Mac. The commitment stops a man in
// the middle from searching for keys that give both sides the same code.
// The pairing key is derived, never transmitted.
//
// Paired: the stored pairing key is mixed into the key derivation, so only the
// two paired devices can produce the confirm MACs (mutual authentication), and
// fresh ECDH keys give every session forward secrecy.
// =============================================================================

import Foundation
import CryptoKit

enum SecureChannelError: Error {
    case badPublicKey
    case commitmentMismatch
    case authenticationFailed
    case decryptFailed
}

/// Keys of one protocol v2 session
struct SessionKeys {
    let transcriptHash: Data
    let verificationCode: String          // 6 digits, new pairing only
    let confirmWin: SymmetricKey          // Windows proves the transcript
    let confirmMac: SymmetricKey          // the Mac proves the transcript
    let controlMacToWin: SymmetricKey     // TCP, Mac → Windows
    let controlWinToMac: SymmetricKey     // TCP, Windows → Mac
    let videoMacToWin: SymmetricKey       // UDP video
    let pairingKey: Data                  // stored after a new pairing
}

enum KeySchedule {
    static let publicKeySize = 65          // X9.63 uncompressed: 04 ‖ X ‖ Y

    static func commitment(publicKey: Data) -> Data {
        Data(SHA256.hash(data: publicKey))
    }

    static func transcriptHash(hello: Data, keyExchange: Data, keyReveal: Data) -> Data {
        var hasher = SHA256()
        hasher.update(data: Data("EXDP/2 handshake".utf8))
        hasher.update(data: hello)
        hasher.update(data: keyExchange)
        hasher.update(data: keyReveal)
        return Data(hasher.finalize())
    }

    /// ECDH P-256: the 32-byte x-coordinate of the shared point
    static func sharedSecret(privateKey: P256.KeyAgreement.PrivateKey,
                             peerPublicKey: Data) throws -> Data {
        guard peerPublicKey.count == publicKeySize,
              let peer = try? P256.KeyAgreement.PublicKey(x963Representation: peerPublicKey) else {
            throw SecureChannelError.badPublicKey
        }
        let secret = try privateKey.sharedSecretFromKeyAgreement(with: peer)
        return secret.withUnsafeBytes { Data($0) }
    }

    static func derive(sharedSecret: Data, pairingKey: Data?, transcriptHash: Data) -> SessionKeys {
        var ikm = sharedSecret
        if let pairingKey { ikm.append(pairingKey) }
        let ikmKey = SymmetricKey(data: ikm)

        func key(_ label: String) -> SymmetricKey {
            HKDF<SHA256>.deriveKey(inputKeyMaterial: ikmKey, salt: transcriptHash,
                                   info: Data("EXDP/2 \(label)".utf8), outputByteCount: 32)
        }
        func bytes(_ k: SymmetricKey) -> Data { k.withUnsafeBytes { Data($0) } }

        let sas = bytes(key("sas"))
        let value = sas.prefix(4).reduce(UInt32(0)) { ($0 << 8) | UInt32($1) }
        return SessionKeys(
            transcriptHash: transcriptHash,
            verificationCode: String(format: "%06u", value % 1_000_000),
            confirmWin: key("confirm win"),
            confirmMac: key("confirm mac"),
            controlMacToWin: key("control m2w"),
            controlWinToMac: key("control w2m"),
            videoMacToWin: key("video m2w"),
            pairingKey: bytes(key("pairing key")))
    }

    static func confirmation(_ key: SymmetricKey, transcriptHash: Data) -> Data {
        Data(HMAC<SHA256>.authenticationCode(for: transcriptHash, using: key))
    }

    static func verify(_ mac: Data, key: SymmetricKey, transcriptHash: Data) -> Bool {
        HMAC<SHA256>.isValidAuthenticationCode(mac, authenticating: transcriptHash, using: key)
    }
}

/// AES-256-GCM with a 96-bit nonce = 4 zero bytes ‖ 64-bit big-endian counter.
/// A counter is never reused with the same key: every message advances it.
enum PacketCipher {
    static let tagSize = 16

    static func nonce(_ counter: UInt64) -> AES.GCM.Nonce {
        var bytes = Data(count: 4)
        withUnsafeBytes(of: counter.bigEndian) { bytes.append(contentsOf: $0) }
        return try! AES.GCM.Nonce(data: bytes)   // 12 bytes: always valid
    }

    /// ciphertext ‖ tag, indexed from 0. (CryptoKit's ciphertext is a slice
    /// starting at index 12; concatenating it directly kept that offset.)
    static func seal(_ plaintext: Data, key: SymmetricKey, counter: UInt64, aad: Data) -> Data {
        let box = try! AES.GCM.seal(plaintext, using: key, nonce: nonce(counter), authenticating: aad)
        return Data(box.ciphertext) + box.tag
    }

    static func open(_ sealed: Data, key: SymmetricKey, counter: UInt64, aad: Data) throws -> Data {
        guard sealed.count >= tagSize else { throw SecureChannelError.decryptFailed }
        let split = sealed.index(sealed.endIndex, offsetBy: -tagSize)
        do {
            let box = try AES.GCM.SealedBox(nonce: nonce(counter),
                                            ciphertext: sealed[sealed.startIndex..<split],
                                            tag: sealed[split...])
            // Rebased to index 0: payload parsers read from absolute offset 0
            return Data(try AES.GCM.open(box, using: key, authenticating: aad))
        } catch {
            throw SecureChannelError.decryptFailed
        }
    }
}

/// Session state after a successful handshake (owned by the network queue)
final class SecureSession {
    let keys: SessionKeys
    private var controlSendCounter: UInt64 = 0
    private var controlReceiveCounter: UInt64 = 0
    private var videoCounter: UInt64 = 0   // never reset within a session

    init(keys: SessionKeys) {
        self.keys = keys
    }

    func sealControl(_ plaintext: Data, aad: Data) -> Data {
        defer { controlSendCounter += 1 }
        return PacketCipher.seal(plaintext, key: keys.controlMacToWin, counter: controlSendCounter, aad: aad)
    }

    /// TCP is ordered and reliable: the counter is implicit
    func openControl(_ sealed: Data, aad: Data) throws -> Data {
        let plaintext = try PacketCipher.open(sealed, key: keys.controlWinToMac,
                                              counter: controlReceiveCounter, aad: aad)
        controlReceiveCounter += 1
        return plaintext
    }

    /// UDP can drop and reorder: the counter travels with the packet
    func nextVideoCounter() -> UInt64 {
        defer { videoCounter += 1 }
        return videoCounter
    }

    func sealVideo(_ plaintext: Data, counter: UInt64, aad: Data) -> Data {
        PacketCipher.seal(plaintext, key: keys.videoMacToWin, counter: counter, aad: aad)
    }
}
