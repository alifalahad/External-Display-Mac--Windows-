// Protocol v2 crypto: the same vectors tests/SecureChannelTests.cpp checks on
// Windows, so a change on either side that breaks interoperability fails here.

import XCTest
import CryptoKit
@testable import ExternalDisplay

final class SecureChannelTests: XCTestCase {

    private func hex(_ data: Data) -> String { data.map { String(format: "%02x", $0) }.joined() }
    private func hex(_ key: SymmetricKey) -> String { key.withUnsafeBytes { hex(Data($0)) } }

    private let macPriv = try! P256.KeyAgreement.PrivateKey(rawRepresentation:
        Data(hexString: "c9afa9d845ba75166b5c215767b1d6934e50c3db36e89b127b8a622b120f6721"))
    private let winPriv = try! P256.KeyAgreement.PrivateKey(rawRepresentation:
        Data(hexString: "519b423d715f8b581f4fa8ee59f4771a5b44c8130b4e3eacca54a56dda72b464"))

    func testEcdhSymmetricAndVector() throws {
        XCTAssertEqual(hex(winPriv.publicKey.x963Representation), "041ccbe91c075fc7f4f033bfa248db8fccd3565de94bbfb12f3c59ff46c271bf83ce4014c68811f9a21a1fdb2c0e6113e06db7ca93b7404e78dc7ccd5ca89a4ca9")
        XCTAssertEqual(hex(macPriv.publicKey.x963Representation), "0460fed4ba255a9d31c961eb74c6356d68c049b8923b61fa6ce669622e60f29fb67903fe1008b8bc99a41ae9e95628bc64f2f1b20c2d7e9f5177a3c294d4462299")
        let a = try KeySchedule.sharedSecret(privateKey: macPriv, peerPublicKey: winPriv.publicKey.x963Representation)
        let b = try KeySchedule.sharedSecret(privateKey: winPriv, peerPublicKey: macPriv.publicKey.x963Representation)
        XCTAssertEqual(a, b)
        XCTAssertEqual(hex(a), "874cc88baf56772eba61b4ccb240ed93d96f2345b346d0e7f9d58a3a555535d0")
    }

    func testInvalidPublicKeysRejected() {
        XCTAssertThrowsError(try KeySchedule.sharedSecret(privateKey: macPriv, peerPublicKey: Data(count: 65)))
        XCTAssertThrowsError(try KeySchedule.sharedSecret(privateKey: macPriv, peerPublicKey: Data(count: 10)))
        var offCurve = winPriv.publicKey.x963Representation
        offCurve[64] ^= 1
        XCTAssertThrowsError(try KeySchedule.sharedSecret(privateKey: macPriv, peerPublicKey: offCurve))
    }

    private func transcript() -> (Data, Data) {
        var hello = HelloPayload()
        hello.receiverName = "TEST-PC"
        hello.udpPort = 9877
        hello.deviceId = "0123456789abcdef"
        hello.commitment = KeySchedule.commitment(publicKey: winPriv.publicKey.x963Representation)
        var kx = KeyExchangePayload()
        kx.mode = .paired
        kx.publicKey = macPriv.publicKey.x963Representation
        XCTAssertEqual(hex(hello.serialize()), "544553542d5043000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000952630313233343536373839616263646566000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000899fbe8efaf514712bcaf48c01dbe8e465af5a531595ce7c388a7d55c4b44f4c")
        XCTAssertEqual(hex(kx.serialize()), "01000000000000000460fed4ba255a9d31c961eb74c6356d68c049b8923b61fa6ce669622e60f29fb67903fe1008b8bc99a41ae9e95628bc64f2f1b20c2d7e9f5177a3c294d4462299")
        let th = KeySchedule.transcriptHash(hello: hello.serialize(), keyExchange: kx.serialize(),
                                            keyReveal: winPriv.publicKey.x963Representation)
        let z = try! KeySchedule.sharedSecret(privateKey: macPriv, peerPublicKey: winPriv.publicKey.x963Representation)
        return (th, z)
    }

    func testKeyScheduleNewPairing() {
        let (th, z) = transcript()
        XCTAssertEqual(hex(th), "3d5650db94127644f4406f13604c1ac3eb1b8ef58514e3cb2dd8837a01034c32")
        let k = KeySchedule.derive(sharedSecret: z, pairingKey: nil, transcriptHash: th)
        XCTAssertEqual(k.verificationCode, "188800")
        XCTAssertEqual(hex(k.confirmWin), "962ef65060d4e889063330e8f167a11362de3df02dff5c721c99a7a3b7c4d706")
        XCTAssertEqual(hex(k.confirmMac), "7752dcf8e5f82d5abd0fdceea727446890d512829e3c10d0cbc336049494a53c")
        XCTAssertEqual(hex(k.controlMacToWin), "4f4c49978dfa427e91c18dda95f17db862fef82f74fb4283c9fb771f56c0cfb5")
        XCTAssertEqual(hex(k.controlWinToMac), "ba7d58c37b5d2612dc7cf57593310a9ccf7f427c94a79b0888d523643f3c2671")
        XCTAssertEqual(hex(k.videoMacToWin), "dc43337b6126afe9b4341c139bd9e05fbbf8bf5b3e1498f275f64fe2f0d4793e")
        XCTAssertEqual(hex(k.pairingKey), "5bff0878890e156a7b02ea0ae1f61ae32d9780e7c1625e3861154acea9c9c807")
        XCTAssertEqual(hex(KeySchedule.confirmation(k.confirmWin, transcriptHash: th)), "addca7ace2f6c90f726d2c6bef42769011b5c5113f88e2d2f416021b26b70612")
    }

    func testKeySchedulePairedNeedsTheRightKey() {
        let (th, z) = transcript()
        let pk = Data(0..<32)
        let k = KeySchedule.derive(sharedSecret: z, pairingKey: pk, transcriptHash: th)
        XCTAssertEqual(k.verificationCode, "631407")
        XCTAssertEqual(hex(k.controlMacToWin), "65454ae785c79b49e40153c2f0604e16d70529fd9741c9eb25aaf9fa23885498")
        XCTAssertEqual(hex(k.videoMacToWin), "f56ea9142705cff802be4b4bfd4a7db8977c3f73380b56c73cd74567e5e77f1c")
        let windowsMac = Data(hexString: "7994b2769c09923021e64f36fcdd6240f110cb2d3698808693d84ed4628b9cf9")
        XCTAssertTrue(KeySchedule.verify(windowsMac, key: k.confirmWin, transcriptHash: th))

        let impostor = KeySchedule.derive(sharedSecret: z, pairingKey: Data(repeating: 0x42, count: 32), transcriptHash: th)
        XCTAssertFalse(KeySchedule.verify(windowsMac, key: impostor.confirmWin, transcriptHash: th))
        var otherTranscript = th
        otherTranscript[0] ^= 1                       // anything changed in the handshake
        XCTAssertFalse(KeySchedule.verify(windowsMac, key: k.confirmWin, transcriptHash: otherTranscript))
    }

    func testCodesAreSixDigitsAndVary() {
        var codes = Set<String>()
        for i in 0..<200 {
            let k = KeySchedule.derive(sharedSecret: Data(repeating: UInt8(i & 0xFF), count: 32),
                                       pairingKey: nil, transcriptHash: Data(repeating: UInt8(i >> 1), count: 32))
            XCTAssertEqual(k.verificationCode.count, 6)
            XCTAssertTrue(k.verificationCode.allSatisfy(\.isNumber))
            codes.insert(k.verificationCode)
        }
        XCTAssertGreaterThan(codes.count, 195)        // no obvious bias
    }

    func testGcmVectorAndTamperDetection() throws {
        let key = SymmetricKey(data: Data(hexString: "feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308"))
        let aad = Data("header-aad".utf8), plaintext = Data("encrypted control message".utf8)
        let sealed = PacketCipher.seal(plaintext, key: key, counter: 0x0102030405060708, aad: aad)
        XCTAssertEqual(hex(sealed), "0de8c97f5166352ee696f5d10ea368e492573c2f034f7f002b7063c4e7ffec337eee181868c035f70f")
        XCTAssertEqual(try PacketCipher.open(sealed, key: key, counter: 0x0102030405060708, aad: aad), plaintext)

        var tampered = sealed
        tampered[0] ^= 1
        XCTAssertThrowsError(try PacketCipher.open(tampered, key: key, counter: 0x0102030405060708, aad: aad))
        XCTAssertThrowsError(try PacketCipher.open(sealed, key: key, counter: 0x0102030405060709, aad: aad))
        XCTAssertThrowsError(try PacketCipher.open(sealed, key: key, counter: 0x0102030405060708, aad: Data("x".utf8)))
        XCTAssertThrowsError(try PacketCipher.open(Data(count: 10), key: key, counter: 0, aad: aad))
    }

    func testSessionCountersNeverRepeat() throws {
        let (th, z) = transcript()
        let keys = KeySchedule.derive(sharedSecret: z, pairingKey: nil, transcriptHash: th)
        let session = SecureSession(keys: keys)
        let header = Data(repeating: 7, count: 28)
        let first = session.sealControl(Data("a".utf8), aad: header)
        let second = session.sealControl(Data("a".utf8), aad: header)
        XCTAssertNotEqual(first, second, "same plaintext, different nonce")
        // The receiver (Windows) opens them with counters 0 and 1 in order
        XCTAssertNoThrow(try PacketCipher.open(first, key: keys.controlMacToWin, counter: 0, aad: header))
        XCTAssertNoThrow(try PacketCipher.open(second, key: keys.controlMacToWin, counter: 1, aad: header))
        XCTAssertEqual(session.nextVideoCounter(), 0)
        XCTAssertEqual(session.nextVideoCounter(), 1)
    }
}

extension Data {
    init(hexString: String) {
        self.init()
        var i = hexString.startIndex
        while i < hexString.endIndex {
            let j = hexString.index(i, offsetBy: 2)
            append(UInt8(hexString[i..<j], radix: 16)!)
            i = j
        }
    }
}
