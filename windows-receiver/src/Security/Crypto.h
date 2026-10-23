#pragma once
// =============================================================================
// Crypto.h — Thin wrappers over Windows CNG (bcrypt) and DPAPI
// =============================================================================
// Established primitives only: SHA-256, HMAC-SHA256, HKDF-SHA256 (RFC 5869,
// built from HMAC), ECDH P-256, AES-256-GCM, the system RNG, DPAPI.
// Byte formats match CryptoKit on the Mac (see SecureChannel.swift):
//   public keys  — X9.63 uncompressed, 65 bytes (04 | X | Y)
//   ECDH secret  — 32-byte big-endian x-coordinate
//   GCM nonce    — 4 zero bytes | 64-bit big-endian counter
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace crypto {

using Bytes = std::vector<uint8_t>;

constexpr size_t kPublicKeySize = 65;
constexpr size_t kTagSize = 16;

bool Random(uint8_t* out, size_t length);

Bytes Sha256(const uint8_t* data, size_t length);
inline Bytes Sha256(const Bytes& data) { return Sha256(data.data(), data.size()); }

Bytes HmacSha256(const Bytes& key, const uint8_t* data, size_t length);
inline Bytes HmacSha256(const Bytes& key, const Bytes& data) { return HmacSha256(key, data.data(), data.size()); }

/// RFC 5869 HKDF-SHA256 (extract + expand); empty result on failure
Bytes Hkdf(const Bytes& ikm, const Bytes& salt, const std::string& info, size_t length);

/// Comparison that takes the same time wherever the first difference is
bool ConstantTimeEqual(const uint8_t* a, const uint8_t* b, size_t length);

/// Ephemeral ECDH P-256 key pair
class EcdhKeyPair {
public:
    static std::unique_ptr<EcdhKeyPair> Generate();
    /// Known key (tests and the byte-order self-check): d = 32-byte scalar
    static std::unique_ptr<EcdhKeyPair> FromPrivate(const uint8_t d[32], const uint8_t publicKey[kPublicKeySize]);
    ~EcdhKeyPair();
    EcdhKeyPair(const EcdhKeyPair&) = delete;
    EcdhKeyPair& operator=(const EcdhKeyPair&) = delete;

    const Bytes& publicKey() const { return publicKey_; }

    /// 32-byte shared x-coordinate; false if the peer key is not a valid P-256 point
    bool agree(const uint8_t* peerPublicKey, size_t length, Bytes& shared) const;

    /// The secret in whatever byte order CNG returns it (self-test only)
    bool agreeRaw(const uint8_t* peerPublicKey, Bytes& raw) const;

private:
    EcdhKeyPair() = default;
    void* key_ = nullptr;          // BCRYPT_KEY_HANDLE
    Bytes publicKey_;
};

/// AES-256-GCM with counter nonces. One instance per direction and thread.
class AesGcm {
public:
    explicit AesGcm(const Bytes& key);
    ~AesGcm();
    AesGcm(const AesGcm&) = delete;
    AesGcm& operator=(const AesGcm&) = delete;

    bool valid() const { return key_ != nullptr; }

    /// out receives ciphertext | tag (plaintextLen + 16 bytes)
    bool seal(uint64_t counter, const uint8_t* aad, size_t aadLen,
              const uint8_t* plaintext, size_t plaintextLen, uint8_t* out) const;

    /// sealed = ciphertext | tag; out receives sealedLen - 16 bytes.
    /// False if the tag does not verify (tampered, wrong key or counter).
    bool open(uint64_t counter, const uint8_t* aad, size_t aadLen,
              const uint8_t* sealed, size_t sealedLen, uint8_t* out) const;

private:
    void* key_ = nullptr;          // BCRYPT_KEY_HANDLE
};

/// Encrypt for the current Windows user (pairing keys at rest)
bool DpapiProtect(const Bytes& plaintext, Bytes& protectedOut);
bool DpapiUnprotect(const Bytes& protectedData, Bytes& plaintextOut);

/// True when CNG produced the reference ECDH secret (run once at startup);
/// on false the platform's crypto cannot interoperate and pairing must not run
bool SelfTest(std::string& error);

} // namespace crypto
