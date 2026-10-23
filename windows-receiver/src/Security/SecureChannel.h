#pragma once
// =============================================================================
// SecureChannel.h — Protocol v2 key agreement and session encryption (Windows)
// =============================================================================
// Mirror of SecureChannel.swift — byte-for-byte identical results (checked by
// tests/SecureChannelTests.cpp against vectors produced by the Mac code).
//
//   Win → Mac  HELLO        name, UDP port, device ID, commit = SHA256(eW)
//   Mac → Win  KEY_EXCHANGE mode, eM
//   Win → Mac  KEY_REVEAL   eW
//   both:      Z  = ECDH(e, e')
//              TH = SHA256("EXDP/2 handshake" | HELLO | KEY_EXCHANGE | KEY_REVEAL)
//              keys = HKDF-SHA256(ikm = Z [| pairing key], salt = TH, info = "EXDP/2 <label>")
//   Win → Mac  PAIR_CONFIRM HMAC(confirm-win, TH)
//   Mac → Win  PAIR_ACCEPT  HMAC(confirm-mac, TH)   → encrypted from here on
// =============================================================================

#include "Security/Crypto.h"
#include "Security/ReplayWindow.h"

#include <memory>
#include <string>

struct SessionKeys {
    crypto::Bytes transcriptHash;
    std::string   verificationCode;   // 6 digits (new pairing)
    crypto::Bytes confirmWin;
    crypto::Bytes confirmMac;
    crypto::Bytes controlMacToWin;
    crypto::Bytes controlWinToMac;
    crypto::Bytes videoMacToWin;
    crypto::Bytes pairingKey;
};

namespace KeySchedule {

crypto::Bytes Commitment(const crypto::Bytes& publicKey);

crypto::Bytes TranscriptHash(const crypto::Bytes& hello, const crypto::Bytes& keyExchange,
                             const crypto::Bytes& keyReveal);

/// pairingKey: nullptr for a new pairing, the stored 32-byte key when paired
SessionKeys Derive(const crypto::Bytes& sharedSecret, const crypto::Bytes* pairingKey,
                   const crypto::Bytes& transcriptHash);

crypto::Bytes Confirmation(const crypto::Bytes& key, const crypto::Bytes& transcriptHash);
bool Verify(const uint8_t mac[32], const crypto::Bytes& key, const crypto::Bytes& transcriptHash);

} // namespace KeySchedule

/// Encryption state after PAIR_ACCEPT. Threading: sealControl under the TCP
/// send lock, openControl on the network thread, openVideo on the UDP thread —
/// each direction has its own key object.
class SecureSession {
public:
    explicit SecureSession(const SessionKeys& keys);
    bool valid() const;

    /// ciphertext | tag of a Windows → Mac control message
    bool sealControl(const uint8_t* header, size_t headerLen, const uint8_t* plaintext,
                     size_t plaintextLen, crypto::Bytes& out);

    /// Mac → Windows control message (TCP: in order, counter implicit)
    bool openControl(const uint8_t* header, size_t headerLen, const uint8_t* sealed,
                     size_t sealedLen, crypto::Bytes& out);

    /// Mac → Windows video packet: header | counter(8, BE) | sealed.
    /// Rejects forgeries and replays; out = plaintext payload.
    bool openVideo(const uint8_t* packet, size_t packetLen, size_t headerLen, crypto::Bytes& out);

    uint64_t videoRejected() const { return videoRejected_; }

private:
    crypto::AesGcm controlOut_;
    crypto::AesGcm controlIn_;
    crypto::AesGcm videoIn_;
    uint64_t controlSendCounter_ = 0;
    uint64_t controlReceiveCounter_ = 0;
    ReplayWindow videoWindow_;
    uint64_t videoRejected_ = 0;
};
