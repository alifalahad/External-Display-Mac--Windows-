// Pairing storage: owner-only file, survives restarts, forget, v1 cleanup (Phase 20)

import XCTest
@testable import ExternalDisplay

final class PairingManagerTests: XCTestCase {
    private var dir: URL!

    override func setUp() {
        dir = FileManager.default.temporaryDirectory.appendingPathComponent("pairing-tests-\(UUID().uuidString)")
    }

    override func tearDown() {
        try? FileManager.default.removeItem(at: dir)
    }

    func testSaveReloadForget() throws {
        let key = Data(repeating: 9, count: 32)
        let first = PairingManager(directory: dir)
        XCTAssertNil(first.pairingKey(for: "dev-1"))
        first.savePairing(deviceId: "dev-1", deviceName: "PC", pairingKey: key)
        XCTAssertEqual(first.pairingKey(for: "dev-1"), key)
        XCTAssertNil(first.pairingKey(for: ""), "empty device ID never matches")

        let reloaded = PairingManager(directory: dir)
        XCTAssertEqual(reloaded.pairingKey(for: "dev-1"), key, "survives a restart")

        let attributes = try FileManager.default.attributesOfItem(
            atPath: dir.appendingPathComponent("pairings-v2.json").path)
        XCTAssertEqual((attributes[.posixPermissions] as? NSNumber)?.intValue, 0o600, "owner-only")

        reloaded.unpair(deviceId: "dev-1")
        XCTAssertNil(PairingManager(directory: dir).pairingKey(for: "dev-1"))
    }

    func testLegacyV1PairingsRemoved() throws {
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let legacy = dir.appendingPathComponent("paired_devices.json")
        try Data("{}".utf8).write(to: legacy)
        _ = PairingManager(directory: dir)
        XCTAssertFalse(FileManager.default.fileExists(atPath: legacy.path),
                       "v1 keys were sent in plaintext: never reused")
    }

    func testApproveOnlyAfterWindowsConfirmed() {
        let pm = PairingManager(directory: dir)
        var decisions: [(String, Bool)] = []
        pm.onDecision = { decisions.append(($0, $1)) }

        pm.beginRequest(deviceId: "dev-2", deviceName: "PC", verificationCode: "123456")
        drainMain()
        pm.approve()   // not confirmed yet: ignored
        XCTAssertTrue(decisions.isEmpty)
        XCTAssertNotNil(pm.request)

        pm.markWindowsConfirmed(deviceId: "dev-2")
        drainMain()
        XCTAssertEqual(pm.request?.windowsConfirmed, true)
        pm.approve()
        XCTAssertEqual(decisions.count, 1)
        XCTAssertEqual(decisions.first?.0, "dev-2")
        XCTAssertEqual(decisions.first?.1, true)
        XCTAssertNil(pm.request)

        pm.beginRequest(deviceId: "dev-3", deviceName: "Other", verificationCode: "654321")
        drainMain()
        pm.markWindowsConfirmed(deviceId: "dev-999")   // confirmation for another device
        drainMain()
        XCTAssertEqual(pm.request?.windowsConfirmed, false)
        pm.reject()
        XCTAssertEqual(decisions.last?.1, false)
    }

    private func drainMain() {
        RunLoop.main.run(until: Date().addingTimeInterval(0.05))
    }
}
