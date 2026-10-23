// =============================================================================
// SecureChannel.cpp — Protocol v2 key schedule and session ciphers
// =============================================================================

#include "Security/SecureChannel.h"

#include <cstdio>

using crypto::Bytes;

namespace KeySchedule {

Bytes Commitment(const Bytes& publicKey) {
    return crypto::Sha256(publicKey);
}

Bytes TranscriptHash(const Bytes& hello, const Bytes& keyExchange, const Bytes& keyReveal) {
    static const char kLabel[] = "EXDP/2 handshake";
    Bytes all(kLabel, kLabel + sizeof(kLabel) - 1);
    all.insert(all.end(), hello.begin(), hello.end());
    all.insert(all.end(), keyExchange.begin(), keyExchange.end());
    all.insert(all.end(), keyReveal.begin(), keyReveal.end());
    return crypto::Sha256(all);
}

SessionKeys Derive(const Bytes& sharedSecret, const Bytes* pairingKey, const Bytes& transcriptHash) {
    Bytes ikm = sharedSecret;
    if (pairingKey) ikm.insert(ikm.end(), pairingKey->begin(), pairingKey->end());
    auto key = [&](const char* label) {
        return crypto::Hkdf(ikm, transcriptHash, std::string("EXDP/2 ") + label, 32);
    };

    SessionKeys k;
    k.transcriptHash = transcriptHash;
    Bytes sas = key("sas");
    uint32_t value = sas.size() >= 4
        ? (uint32_t(sas[0]) << 24) | (uint32_t(sas[1]) << 16) | (uint32_t(sas[2]) << 8) | sas[3] : 0;
    char code[8];
    std::snprintf(code, sizeof(code), "%06u", static_cast<unsigned>(value % 1000000u));
    k.verificationCode = code;
    k.confirmWin = key("confirm win");
    k.confirmMac = key("confirm mac");
    k.controlMacToWin = key("control m2w");
    k.controlWinToMac = key("control w2m");
    k.videoMacToWin = key("video m2w");
    k.pairingKey = key("pairing key");
    return k;
}

Bytes Confirmation(const Bytes& key, const Bytes& transcriptHash) {
    return crypto::HmacSha256(key, transcriptHash);
}

bool Verify(const uint8_t mac[32], const Bytes& key, const Bytes& transcriptHash) {
    Bytes expected = Confirmation(key, transcriptHash);
    return expected.size() == 32 && crypto::ConstantTimeEqual(mac, expected.data(), 32);
}

} // namespace KeySchedule

// ── Session ─────────────────────────────────────────────────────────────────

SecureSession::SecureSession(const SessionKeys& keys)
    : controlOut_(keys.controlWinToMac)
    , controlIn_(keys.controlMacToWin)
    , videoIn_(keys.videoMacToWin) {}

bool SecureSession::valid() const {
    return controlOut_.valid() && controlIn_.valid() && videoIn_.valid();
}

bool SecureSession::sealControl(const uint8_t* header, size_t headerLen, const uint8_t* plaintext,
                                size_t plaintextLen, Bytes& out) {
    out.resize(plaintextLen + crypto::kTagSize);
    if (!controlOut_.seal(controlSendCounter_, header, headerLen, plaintext, plaintextLen, out.data())) {
        return false;
    }
    controlSendCounter_++;
    return true;
}

bool SecureSession::openControl(const uint8_t* header, size_t headerLen, const uint8_t* sealed,
                                size_t sealedLen, Bytes& out) {
    if (sealedLen < crypto::kTagSize) return false;
    out.resize(sealedLen - crypto::kTagSize);
    if (!controlIn_.open(controlReceiveCounter_, header, headerLen, sealed, sealedLen, out.data())) {
        return false;
    }
    controlReceiveCounter_++;
    return true;
}

bool SecureSession::openVideo(const uint8_t* packet, size_t packetLen, size_t headerLen, Bytes& out) {
    const size_t aadLen = headerLen + 8;   // header | counter
    if (packetLen < aadLen + crypto::kTagSize) {
        videoRejected_++;
        return false;
    }
    uint64_t counter = 0;
    for (size_t i = 0; i < 8; i++) counter = (counter << 8) | packet[headerLen + i];
    if (!videoWindow_.isFresh(counter)) {   // replay or far too old: skip the crypto
        videoRejected_++;
        return false;
    }
    out.resize(packetLen - aadLen - crypto::kTagSize);
    if (!videoIn_.open(counter, packet, aadLen, packet + aadLen, packetLen - aadLen, out.data()) ||
        !videoWindow_.accept(counter)) {
        videoRejected_++;
        return false;
    }
    return true;
}
