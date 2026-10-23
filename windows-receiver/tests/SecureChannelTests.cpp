// =============================================================================
// SecureChannelTests.cpp — Protocol v2 crypto against Mac (CryptoKit) vectors
// =============================================================================
// Windows only (CNG). Every expected value below was produced by the Mac
// implementation (SecureChannel.swift) or comes from a published standard, so
// a pass means both platforms derive byte-identical keys.
//
//   cmake -B build -DEXDP_BUILD_TESTS=ON && cmake --build build
//   .\build\SecureChannelTests.exe
// =============================================================================

#include "Security/Crypto.h"
#include "Security/SecureChannel.h"
#include "Security/ReplayWindow.h"
#include "Network/Protocol.h"

#include <cstdio>
#include <cstring>
#include <string>

using crypto::Bytes;

static int gChecks = 0, gFailures = 0;
#define CHECK(cond) do { gChecks++; if (!(cond)) { gFailures++; \
    std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static Bytes Hex(const char* hex) {
    Bytes out;
    for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) {
        unsigned v = 0;
        std::sscanf(hex + i, "%2x", &v);
        out.push_back(static_cast<uint8_t>(v));
    }
    return out;
}

static std::string ToHex(const Bytes& b) {
    std::string s;
    char buf[3];
    for (uint8_t x : b) { std::snprintf(buf, sizeof(buf), "%02x", x); s += buf; }
    return s;
}

#define CHECK_HEX(bytes, expected) do { gChecks++; std::string got_ = ToHex(bytes); \
    if (got_ != (expected)) { gFailures++; std::printf("  FAIL %s:%d  %s\n    got      %s\n    expected %s\n", \
    __FILE__, __LINE__, #bytes, got_.c_str(), (expected)); } } while (0)

static void TestSelfTest() {
    std::printf("Crypto self-test (ECDH byte order, SHA/HMAC, AES-GCM)\n");
    std::string error;
    CHECK(crypto::SelfTest(error));
    if (!error.empty()) std::printf("    %s\n", error.c_str());
}

static void TestHkdfRfc5869() {
    std::printf("HKDF-SHA256: RFC 5869 test case 1\n");
    Bytes ikm(22, 0x0b);
    Bytes salt = Hex("000102030405060708090a0b0c");
    Bytes infoBytes = Hex("f0f1f2f3f4f5f6f7f8f9");
    std::string info(infoBytes.begin(), infoBytes.end());
    CHECK_HEX(crypto::Hkdf(ikm, salt, info, 42),
              "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");
}

static void TestHmacRfc4231() {
    std::printf("HMAC-SHA256: RFC 4231 test case 2\n");
    Bytes key(reinterpret_cast<const uint8_t*>("Jefe"), reinterpret_cast<const uint8_t*>("Jefe") + 4);
    const char* msg = "what do ya want for nothing?";
    CHECK_HEX(crypto::HmacSha256(key, reinterpret_cast<const uint8_t*>(msg), std::strlen(msg)),
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

static void TestEcdhAgainstMac() {
    std::printf("ECDH P-256: same secret as CryptoKit\n");
    Bytes d = Hex("519b423d715f8b581f4fa8ee59f4771a5b44c8130b4e3eacca54a56dda72b464");
    Bytes pub = Hex("041ccbe91c075fc7f4f033bfa248db8fccd3565de94bbfb12f3c59ff46c271bf83ce4014c68811f9a21a1fdb2c0e6113e06db7ca93b7404e78dc7ccd5ca89a4ca9");
    Bytes macPub = Hex("0460fed4ba255a9d31c961eb74c6356d68c049b8923b61fa6ce669622e60f29fb67903fe1008b8bc99a41ae9e95628bc64f2f1b20c2d7e9f5177a3c294d4462299");
    auto pair = crypto::EcdhKeyPair::FromPrivate(d.data(), pub.data());
    CHECK(pair != nullptr);
    if (!pair) return;
    Bytes shared;
    CHECK(pair->agree(macPub.data(), macPub.size(), shared));
    CHECK_HEX(shared, "874cc88baf56772eba61b4ccb240ed93d96f2345b346d0e7f9d58a3a555535d0");

    // Generated keys agree with each other and export valid X9.63 points
    auto a = crypto::EcdhKeyPair::Generate();
    auto b = crypto::EcdhKeyPair::Generate();
    CHECK(a && b);
    if (!a || !b) return;
    CHECK(a->publicKey().size() == 65 && a->publicKey()[0] == 0x04);
    Bytes ab, ba;
    CHECK(a->agree(b->publicKey().data(), 65, ab));
    CHECK(b->agree(a->publicKey().data(), 65, ba));
    CHECK(ab == ba && ab.size() == 32);

    // A point that is not on the curve is refused
    Bytes bogus = macPub;
    bogus[64] ^= 0x01;
    Bytes out;
    CHECK(!pair->agree(bogus.data(), bogus.size(), out));
}

static void TestKeyScheduleAgainstMac() {
    std::printf("Key schedule: transcript, code, keys, MACs identical to the Mac\n");
    Bytes hello = Hex("544553542d5043000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000952630313233343536373839616263646566000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000899fbe8efaf514712bcaf48c01dbe8e465af5a531595ce7c388a7d55c4b44f4c");
    Bytes kx = Hex("01000000000000000460fed4ba255a9d31c961eb74c6356d68c049b8923b61fa6ce669622e60f29fb67903fe1008b8bc99a41ae9e95628bc64f2f1b20c2d7e9f5177a3c294d4462299");
    Bytes winPub = Hex("041ccbe91c075fc7f4f033bfa248db8fccd3565de94bbfb12f3c59ff46c271bf83ce4014c68811f9a21a1fdb2c0e6113e06db7ca93b7404e78dc7ccd5ca89a4ca9");
    Bytes shared = Hex("874cc88baf56772eba61b4ccb240ed93d96f2345b346d0e7f9d58a3a555535d0");
    CHECK(hello.size() == sizeof(exdp::HelloPayload));
    CHECK(kx.size() == sizeof(exdp::KeyExchangePayload));

    // The HELLO commitment is SHA-256 of the revealed key
    CHECK(Bytes(hello.end() - 32, hello.end()) == KeySchedule::Commitment(winPub));

    Bytes th = KeySchedule::TranscriptHash(hello, kx, winPub);
    CHECK_HEX(th, "3d5650db94127644f4406f13604c1ac3eb1b8ef58514e3cb2dd8837a01034c32");

    SessionKeys fresh = KeySchedule::Derive(shared, nullptr, th);
    CHECK(fresh.verificationCode == "188800");
    CHECK_HEX(fresh.confirmWin, "962ef65060d4e889063330e8f167a11362de3df02dff5c721c99a7a3b7c4d706");
    CHECK_HEX(fresh.confirmMac, "7752dcf8e5f82d5abd0fdceea727446890d512829e3c10d0cbc336049494a53c");
    CHECK_HEX(fresh.controlMacToWin, "4f4c49978dfa427e91c18dda95f17db862fef82f74fb4283c9fb771f56c0cfb5");
    CHECK_HEX(fresh.controlWinToMac, "ba7d58c37b5d2612dc7cf57593310a9ccf7f427c94a79b0888d523643f3c2671");
    CHECK_HEX(fresh.videoMacToWin, "dc43337b6126afe9b4341c139bd9e05fbbf8bf5b3e1498f275f64fe2f0d4793e");
    CHECK_HEX(fresh.pairingKey, "5bff0878890e156a7b02ea0ae1f61ae32d9780e7c1625e3861154acea9c9c807");
    CHECK_HEX(KeySchedule::Confirmation(fresh.confirmWin, th), "addca7ace2f6c90f726d2c6bef42769011b5c5113f88e2d2f416021b26b70612");

    Bytes pk = Hex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    SessionKeys paired = KeySchedule::Derive(shared, &pk, th);
    CHECK(paired.verificationCode == "631407");
    CHECK_HEX(paired.confirmWin, "f8ccf8ad3a3fc7465ab9d34fe2bc99a4abfd574bcff679939efca4e3467d2542");
    CHECK_HEX(paired.confirmMac, "2bed01989f8091826c46d9dcb592283452335b8087788fdf9b021b8dfd8eb395");
    CHECK_HEX(paired.controlMacToWin, "65454ae785c79b49e40153c2f0604e16d70529fd9741c9eb25aaf9fa23885498");
    CHECK_HEX(paired.controlWinToMac, "bad0c587a63aafd388d340443a6783de4a1ac9ac286bba221389c2b354a7b6b7");
    CHECK_HEX(paired.videoMacToWin, "f56ea9142705cff802be4b4bfd4a7db8977c3f73380b56c73cd74567e5e77f1c");
    Bytes winMac = Hex("7994b2769c09923021e64f36fcdd6240f110cb2d3698808693d84ed4628b9cf9");
    CHECK(KeySchedule::Verify(winMac.data(), paired.confirmWin, th));

    // Without the right pairing key the confirmation does not verify
    Bytes wrongPk(32, 0x42);
    SessionKeys impostor = KeySchedule::Derive(shared, &wrongPk, th);
    CHECK(!KeySchedule::Verify(winMac.data(), impostor.confirmWin, th));
}

static void TestAesGcmAgainstMac() {
    std::printf("AES-256-GCM: same ciphertext as CryptoKit, tampering rejected\n");
    Bytes key = Hex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308");
    const char* aad = "header-aad";
    const char* pt = "encrypted control message";
    crypto::AesGcm gcm(key);
    CHECK(gcm.valid());
    Bytes sealed(std::strlen(pt) + 16);
    CHECK(gcm.seal(0x0102030405060708ULL, reinterpret_cast<const uint8_t*>(aad), std::strlen(aad),
                   reinterpret_cast<const uint8_t*>(pt), std::strlen(pt), sealed.data()));
    CHECK_HEX(sealed, "0de8c97f5166352ee696f5d10ea368e492573c2f034f7f002b7063c4e7ffec337eee181868c035f70f");

    Bytes back(std::strlen(pt));
    CHECK(gcm.open(0x0102030405060708ULL, reinterpret_cast<const uint8_t*>(aad), std::strlen(aad),
                   sealed.data(), sealed.size(), back.data()));
    CHECK(std::memcmp(back.data(), pt, back.size()) == 0);

    Bytes tampered = sealed;
    tampered[3] ^= 0x80;
    CHECK(!gcm.open(0x0102030405060708ULL, reinterpret_cast<const uint8_t*>(aad), std::strlen(aad),
                    tampered.data(), tampered.size(), back.data()));
    CHECK(!gcm.open(0x0102030405060709ULL, reinterpret_cast<const uint8_t*>(aad), std::strlen(aad),
                    sealed.data(), sealed.size(), back.data()));   // wrong counter
    CHECK(!gcm.open(0x0102030405060708ULL, reinterpret_cast<const uint8_t*>("header-aaD"), 10,
                    sealed.data(), sealed.size(), back.data()));   // header changed
}

static void TestReplayWindow() {
    std::printf("Replay window: reorder accepted, duplicates and stale counters refused\n");
    ReplayWindow w;
    CHECK(w.accept(10));
    CHECK(!w.accept(10));          // duplicate
    CHECK(w.accept(12));
    CHECK(w.accept(11));           // late but inside the window
    CHECK(!w.accept(11));
    CHECK(w.accept(100));
    CHECK(w.accept(37));           // 63 behind: still inside
    CHECK(!w.accept(36));          // 64 behind: too old
    CHECK(w.accept(1000));         // big jump clears the window
    CHECK(!w.accept(100));
    CHECK(w.isFresh(999) && !w.isFresh(1000));
}

static void TestVideoSessionRoundTrip() {
    std::printf("Session: Mac-format video packet decrypts once, replay and forgery refused\n");
    Bytes shared = Hex("874cc88baf56772eba61b4ccb240ed93d96f2345b346d0e7f9d58a3a555535d0");
    Bytes th = Hex("3d5650db94127644f4406f13604c1ac3eb1b8ef58514e3cb2dd8837a01034c32");
    SessionKeys keys = KeySchedule::Derive(shared, nullptr, th);
    SecureSession session(keys);
    CHECK(session.valid());

    // Build a packet exactly like the Mac: header | counter | AES-GCM(payload)
    uint8_t header[exdp::HEADER_SIZE] = {};
    header[5] = static_cast<uint8_t>(exdp::MessageType::VideoFrame);
    const uint64_t counter = 5;
    Bytes packet(header, header + sizeof(header));
    for (int i = 7; i >= 0; i--) packet.push_back(static_cast<uint8_t>(counter >> (8 * i)));
    const char* payload = "frame bytes";
    crypto::AesGcm macVideo(keys.videoMacToWin);
    Bytes sealed(std::strlen(payload) + 16);
    macVideo.seal(counter, packet.data(), packet.size(), reinterpret_cast<const uint8_t*>(payload),
                  std::strlen(payload), sealed.data());
    packet.insert(packet.end(), sealed.begin(), sealed.end());

    Bytes out;
    CHECK(session.openVideo(packet.data(), packet.size(), exdp::HEADER_SIZE, out));
    CHECK(std::string(out.begin(), out.end()) == payload);
    CHECK(!session.openVideo(packet.data(), packet.size(), exdp::HEADER_SIZE, out));   // replay
    Bytes forged = packet;
    forged[2] ^= 1;   // header byte: authenticated as associated data
    forged[exdp::HEADER_SIZE + 7] = 6;
    CHECK(!session.openVideo(forged.data(), forged.size(), exdp::HEADER_SIZE, out));
    CHECK(session.videoRejected() == 2);

    // Control channel: Windows seals with control w2m, the Mac opens with the same key
    Bytes ctl;
    CHECK(session.sealControl(header, sizeof(header), reinterpret_cast<const uint8_t*>("ping"), 4, ctl));
    crypto::AesGcm macIn(keys.controlWinToMac);
    uint8_t plain[4];
    CHECK(macIn.open(0, header, sizeof(header), ctl.data(), ctl.size(), plain));
    CHECK(std::memcmp(plain, "ping", 4) == 0);
}

static void TestDpapi() {
    std::printf("DPAPI: pairing key at rest round-trips\n");
    Bytes secret(32, 0x5a), sealed, back;
    CHECK(crypto::DpapiProtect(secret, sealed));
    CHECK(sealed != secret && sealed.size() > secret.size());
    CHECK(crypto::DpapiUnprotect(sealed, back));
    CHECK(back == secret);
    // (Tamper detection is DPAPI's own job — Windows signs the blob; Wine's
    // implementation does not, so it is not asserted here.)
}

int main() {
    TestSelfTest();
    TestHkdfRfc5869();
    TestHmacRfc4231();
    TestEcdhAgainstMac();
    TestKeyScheduleAgainstMac();
    TestAesGcmAgainstMac();
    TestReplayWindow();
    TestVideoSessionRoundTrip();
    TestDpapi();
    std::printf("\n%d checks, %d failed\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
