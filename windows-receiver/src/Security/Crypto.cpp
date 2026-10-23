// =============================================================================
// Crypto.cpp — Windows CNG (bcrypt) and DPAPI wrappers
// =============================================================================

#include "Security/Crypto.h"

#include <Windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <algorithm>
#include <cstring>
#include <mutex>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace crypto {
namespace {

/// Algorithm providers are thread-safe and expensive to open: open once
struct Providers {
    BCRYPT_ALG_HANDLE sha256 = nullptr;
    BCRYPT_ALG_HANDLE hmac = nullptr;
    BCRYPT_ALG_HANDLE ecdh = nullptr;
    BCRYPT_ALG_HANDLE aesGcm = nullptr;

    Providers() {
        BCryptOpenAlgorithmProvider(&sha256, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        BCryptOpenAlgorithmProvider(&hmac, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
        BCryptOpenAlgorithmProvider(&ecdh, BCRYPT_ECDH_P256_ALGORITHM, nullptr, 0);
        if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&aesGcm, BCRYPT_AES_ALGORITHM, nullptr, 0))) {
            BCryptSetProperty(aesGcm, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                              sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
        }
    }
};

Providers& P() {
    static Providers providers;
    return providers;
}

Bytes HashWith(BCRYPT_ALG_HANDLE alg, const uint8_t* secret, size_t secretLen,
               const uint8_t* data, size_t length) {
    Bytes out(32);
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (!alg || !BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, (PUCHAR)secret,
                                                 (ULONG)secretLen, 0))) {
        return {};
    }
    bool ok = BCRYPT_SUCCESS(BCryptHashData(hash, (PUCHAR)data, (ULONG)length, 0)) &&
              BCRYPT_SUCCESS(BCryptFinishHash(hash, out.data(), (ULONG)out.size(), 0));
    BCryptDestroyHash(hash);
    return ok ? out : Bytes{};
}

void Nonce(uint64_t counter, uint8_t nonce[12]) {
    std::memset(nonce, 0, 4);
    for (int i = 0; i < 8; i++) nonce[4 + i] = static_cast<uint8_t>(counter >> (56 - 8 * i));
}

// CNG returns BCRYPT_KDF_RAW_SECRET little-endian on current Windows; check it
// once against a known answer instead of assuming (computed with CryptoKit).
const uint8_t kKatPrivate[32] = {
    0x51,0x9b,0x42,0x3d,0x71,0x5f,0x8b,0x58,0x1f,0x4f,0xa8,0xee,0x59,0xf4,0x77,0x1a,
    0x5b,0x44,0xc8,0x13,0x0b,0x4e,0x3e,0xac,0xca,0x54,0xa5,0x6d,0xda,0x72,0xb4,0x64 };
const uint8_t kKatPublic[65] = { 0x04,
    0x1c,0xcb,0xe9,0x1c,0x07,0x5f,0xc7,0xf4,0xf0,0x33,0xbf,0xa2,0x48,0xdb,0x8f,0xcc,
    0xd3,0x56,0x5d,0xe9,0x4b,0xbf,0xb1,0x2f,0x3c,0x59,0xff,0x46,0xc2,0x71,0xbf,0x83,
    0xce,0x40,0x14,0xc6,0x88,0x11,0xf9,0xa2,0x1a,0x1f,0xdb,0x2c,0x0e,0x61,0x13,0xe0,
    0x6d,0xb7,0xca,0x93,0xb7,0x40,0x4e,0x78,0xdc,0x7c,0xcd,0x5c,0xa8,0x9a,0x4c,0xa9 };
const uint8_t kKatPeer[65] = { 0x04,
    0x60,0xfe,0xd4,0xba,0x25,0x5a,0x9d,0x31,0xc9,0x61,0xeb,0x74,0xc6,0x35,0x6d,0x68,
    0xc0,0x49,0xb8,0x92,0x3b,0x61,0xfa,0x6c,0xe6,0x69,0x62,0x2e,0x60,0xf2,0x9f,0xb6,
    0x79,0x03,0xfe,0x10,0x08,0xb8,0xbc,0x99,0xa4,0x1a,0xe9,0xe9,0x56,0x28,0xbc,0x64,
    0xf2,0xf1,0xb2,0x0c,0x2d,0x7e,0x9f,0x51,0x77,0xa3,0xc2,0x94,0xd4,0x46,0x22,0x99 };
const uint8_t kKatShared[32] = {
    0x87,0x4c,0xc8,0x8b,0xaf,0x56,0x77,0x2e,0xba,0x61,0xb4,0xcc,0xb2,0x40,0xed,0x93,
    0xd9,0x6f,0x23,0x45,0xb3,0x46,0xd0,0xe7,0xf9,0xd5,0x8a,0x3a,0x55,0x55,0x35,0xd0 };

enum class SecretOrder { Unknown, BigEndian, LittleEndian, Broken };
std::once_flag gOrderOnce;
SecretOrder gOrder = SecretOrder::Unknown;

/// Raw CNG secret (whatever byte order this Windows version uses)
bool RawAgree(BCRYPT_KEY_HANDLE key, const uint8_t* peer, Bytes& raw) {
    // BCRYPT_ECCPUBLIC_BLOB: header + X + Y (big-endian)
    uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
    auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob);
    header->dwMagic = BCRYPT_ECDH_PUBLIC_P256_MAGIC;
    header->cbKey = 32;
    std::memcpy(blob + sizeof(BCRYPT_ECCKEY_BLOB), peer + 1, 64);

    BCRYPT_KEY_HANDLE peerKey = nullptr;
    if (!BCRYPT_SUCCESS(BCryptImportKeyPair(P().ecdh, nullptr, BCRYPT_ECCPUBLIC_BLOB, &peerKey,
                                            blob, sizeof(blob), 0))) {
        return false;   // not a point on the curve
    }
    BCRYPT_SECRET_HANDLE secret = nullptr;
    bool ok = BCRYPT_SUCCESS(BCryptSecretAgreement(key, peerKey, &secret, 0));
    if (ok) {
        raw.assign(32, 0);
        ULONG got = 0;
        ok = BCRYPT_SUCCESS(BCryptDeriveKey(secret, BCRYPT_KDF_RAW_SECRET, nullptr, raw.data(),
                                            (ULONG)raw.size(), &got, 0)) && got == 32;
        BCryptDestroySecret(secret);
    }
    BCryptDestroyKey(peerKey);
    return ok;
}

void DetermineSecretOrder() {
    gOrder = SecretOrder::Broken;
    auto pair = EcdhKeyPair::FromPrivate(kKatPrivate, kKatPublic);
    Bytes raw;
    if (!pair || !pair->agreeRaw(kKatPeer, raw)) return;
    if (std::equal(raw.begin(), raw.end(), kKatShared)) {
        gOrder = SecretOrder::BigEndian;
    } else if (std::equal(raw.rbegin(), raw.rend(), kKatShared)) {
        gOrder = SecretOrder::LittleEndian;
    }
}

} // namespace

// ── Basic primitives ────────────────────────────────────────────────────────

bool Random(uint8_t* out, size_t length) {
    return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, out, (ULONG)length, BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

Bytes Sha256(const uint8_t* data, size_t length) {
    return HashWith(P().sha256, nullptr, 0, data, length);
}

Bytes HmacSha256(const Bytes& key, const uint8_t* data, size_t length) {
    return HashWith(P().hmac, key.data(), key.size(), data, length);
}

Bytes Hkdf(const Bytes& ikm, const Bytes& salt, const std::string& info, size_t length) {
    if (length == 0 || length > 255 * 32) return {};
    // Extract: PRK = HMAC(salt, IKM) — an empty salt means 32 zero bytes
    Bytes prk = HmacSha256(salt.empty() ? Bytes(32, 0) : salt, ikm);
    if (prk.empty()) return {};
    // Expand: T(i) = HMAC(PRK, T(i-1) | info | i)
    Bytes okm, previous;
    for (uint8_t i = 1; okm.size() < length; i++) {
        Bytes block = previous;
        block.insert(block.end(), info.begin(), info.end());
        block.push_back(i);
        previous = HmacSha256(prk, block);
        if (previous.empty()) return {};
        okm.insert(okm.end(), previous.begin(), previous.end());
    }
    okm.resize(length);
    return okm;
}

bool ConstantTimeEqual(const uint8_t* a, const uint8_t* b, size_t length) {
    uint8_t diff = 0;
    for (size_t i = 0; i < length; i++) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    return diff == 0;
}

// ── ECDH P-256 ──────────────────────────────────────────────────────────────

std::unique_ptr<EcdhKeyPair> EcdhKeyPair::Generate() {
    std::unique_ptr<EcdhKeyPair> pair(new EcdhKeyPair());
    BCRYPT_KEY_HANDLE key = nullptr;
    if (!P().ecdh || !BCRYPT_SUCCESS(BCryptGenerateKeyPair(P().ecdh, &key, 256, 0)) ||
        !BCRYPT_SUCCESS(BCryptFinalizeKeyPair(key, 0))) {
        if (key) BCryptDestroyKey(key);
        return nullptr;
    }
    pair->key_ = key;

    uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
    ULONG got = 0;
    if (!BCRYPT_SUCCESS(BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob, sizeof(blob), &got, 0)) ||
        got != sizeof(blob)) {
        return nullptr;
    }
    pair->publicKey_.assign(1, 0x04);
    pair->publicKey_.insert(pair->publicKey_.end(), blob + sizeof(BCRYPT_ECCKEY_BLOB), blob + sizeof(blob));
    return pair;
}

std::unique_ptr<EcdhKeyPair> EcdhKeyPair::FromPrivate(const uint8_t d[32], const uint8_t publicKey[kPublicKeySize]) {
    if (!P().ecdh || publicKey[0] != 0x04) return nullptr;
    // BCRYPT_ECCPRIVATE_BLOB: header + X + Y + d (big-endian)
    uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 96];
    auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob);
    header->dwMagic = BCRYPT_ECDH_PRIVATE_P256_MAGIC;
    header->cbKey = 32;
    std::memcpy(blob + sizeof(BCRYPT_ECCKEY_BLOB), publicKey + 1, 64);
    std::memcpy(blob + sizeof(BCRYPT_ECCKEY_BLOB) + 64, d, 32);

    std::unique_ptr<EcdhKeyPair> pair(new EcdhKeyPair());
    BCRYPT_KEY_HANDLE key = nullptr;
    if (!BCRYPT_SUCCESS(BCryptImportKeyPair(P().ecdh, nullptr, BCRYPT_ECCPRIVATE_BLOB, &key,
                                            blob, sizeof(blob), 0))) {
        return nullptr;
    }
    SecureZeroMemory(blob, sizeof(blob));
    pair->key_ = key;
    pair->publicKey_.assign(publicKey, publicKey + kPublicKeySize);
    return pair;
}

EcdhKeyPair::~EcdhKeyPair() {
    if (key_) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(key_));
}

bool EcdhKeyPair::agree(const uint8_t* peerPublicKey, size_t length, Bytes& shared) const {
    if (!key_ || length != kPublicKeySize || peerPublicKey[0] != 0x04) return false;
    std::call_once(gOrderOnce, DetermineSecretOrder);
    if (gOrder != SecretOrder::BigEndian && gOrder != SecretOrder::LittleEndian) return false;
    if (!RawAgree(static_cast<BCRYPT_KEY_HANDLE>(key_), peerPublicKey, shared)) return false;
    if (gOrder == SecretOrder::LittleEndian) std::reverse(shared.begin(), shared.end());
    return true;
}

bool EcdhKeyPair::agreeRaw(const uint8_t* peerPublicKey, Bytes& raw) const {
    return key_ && RawAgree(static_cast<BCRYPT_KEY_HANDLE>(key_), peerPublicKey, raw);
}

// ── AES-256-GCM ─────────────────────────────────────────────────────────────

AesGcm::AesGcm(const Bytes& key) {
    BCRYPT_KEY_HANDLE handle = nullptr;
    if (key.size() == 32 && P().aesGcm &&
        BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(P().aesGcm, &handle, nullptr, 0,
                                                  (PUCHAR)key.data(), (ULONG)key.size(), 0))) {
        key_ = handle;
    }
}

AesGcm::~AesGcm() {
    if (key_) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(key_));
}

bool AesGcm::seal(uint64_t counter, const uint8_t* aad, size_t aadLen,
                  const uint8_t* plaintext, size_t plaintextLen, uint8_t* out) const {
    if (!key_) return false;
    uint8_t nonce[12];
    Nonce(counter, nonce);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = nonce;
    info.cbNonce = sizeof(nonce);
    info.pbAuthData = const_cast<PUCHAR>(aad);
    info.cbAuthData = (ULONG)aadLen;
    info.pbTag = out + plaintextLen;
    info.cbTag = kTagSize;
    ULONG written = 0;
    return BCRYPT_SUCCESS(BCryptEncrypt(static_cast<BCRYPT_KEY_HANDLE>(key_),
                                        const_cast<PUCHAR>(plaintext), (ULONG)plaintextLen, &info,
                                        nullptr, 0, out, (ULONG)plaintextLen, &written, 0)) &&
           written == plaintextLen;
}

bool AesGcm::open(uint64_t counter, const uint8_t* aad, size_t aadLen,
                  const uint8_t* sealed, size_t sealedLen, uint8_t* out) const {
    if (!key_ || sealedLen < kTagSize) return false;
    size_t ciphertextLen = sealedLen - kTagSize;
    uint8_t nonce[12];
    Nonce(counter, nonce);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = nonce;
    info.cbNonce = sizeof(nonce);
    info.pbAuthData = const_cast<PUCHAR>(aad);
    info.cbAuthData = (ULONG)aadLen;
    info.pbTag = const_cast<PUCHAR>(sealed + ciphertextLen);
    info.cbTag = kTagSize;
    ULONG written = 0;
    return BCRYPT_SUCCESS(BCryptDecrypt(static_cast<BCRYPT_KEY_HANDLE>(key_),
                                        const_cast<PUCHAR>(sealed), (ULONG)ciphertextLen, &info,
                                        nullptr, 0, out, (ULONG)ciphertextLen, &written, 0)) &&
           written == ciphertextLen;
}

// ── DPAPI ───────────────────────────────────────────────────────────────────

bool DpapiProtect(const Bytes& plaintext, Bytes& protectedOut) {
    DATA_BLOB in{ (DWORD)plaintext.size(), const_cast<BYTE*>(plaintext.data()) };
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"External Display pairing", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return false;
    }
    protectedOut.assign(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return true;
}

bool DpapiUnprotect(const Bytes& protectedData, Bytes& plaintextOut) {
    DATA_BLOB in{ (DWORD)protectedData.size(), const_cast<BYTE*>(protectedData.data()) };
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return false;
    }
    plaintextOut.assign(out.pbData, out.pbData + out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}

// ── Self-test ───────────────────────────────────────────────────────────────

bool SelfTest(std::string& error) {
    std::call_once(gOrderOnce, DetermineSecretOrder);
    if (gOrder != SecretOrder::BigEndian && gOrder != SecretOrder::LittleEndian) {
        error = "ECDH P-256 known-answer test failed (CNG unavailable or incompatible)";
        return false;
    }
    if (Sha256(nullptr, 0).size() != 32 || Hkdf(Bytes(32, 1), {}, "x", 32).size() != 32) {
        error = "SHA-256/HMAC unavailable";
        return false;
    }
    AesGcm gcm(Bytes(32, 7));
    uint8_t pt[4] = {1, 2, 3, 4}, sealed[4 + kTagSize], back[4];
    if (!gcm.valid() || !gcm.seal(1, pt, 4, pt, 4, sealed) || !gcm.open(1, pt, 4, sealed, sizeof(sealed), back) ||
        std::memcmp(pt, back, 4) != 0) {
        error = "AES-GCM unavailable";
        return false;
    }
    return true;
}

} // namespace crypto
