// Pointer drawn by the receiver (docs/SMOOTHNESS.md, step 1): wire layout
// shared with Windows, bitmaps from real macOS cursors, which moves are reported

import XCTest
import AppKit
@testable import ExternalDisplay

final class PointerTests: XCTestCase {

    private func hex(_ data: Data) -> String { data.map { String(format: "%02x", $0) }.joined() }

    override func setUp() {
        super.setUp()
        _ = NSApplication.shared   // NSCursor needs AppKit set up
    }

    // Same values and bytes as TestWireLayout in windows-receiver/tests/PointerTests.cpp
    func testPointerPayloadsMatchTheWindowsLayout() {
        var shape = CursorShapePayload()
        shape.id = 0x0102030405060708
        shape.width = 3
        shape.height = 2
        shape.hotspotX = 1
        shape.hotspotY = 1
        shape.scale = 0.5
        shape.pixels = Data((1...24).map { UInt8($0) })
        let bytes = shape.serialize()
        XCTAssertEqual(bytes.count, CursorShapePayload.headerSize + 24)
        XCTAssertEqual(hex(bytes.prefix(CursorShapePayload.headerSize)),
                       "0807060504030201" + "0300" + "0200" + "0100" + "0100" + "0000003f" + "00000000")
        XCTAssertEqual(Array(bytes.suffix(24)), (1...24).map { UInt8($0) })

        let position = CursorPositionPayload(x: 0.25, y: 0.75, flags: CursorPositionPayload.visible)
        XCTAssertEqual(hex(position.serialize()), "0000803e" + "0000403f" + "01000000")

        let caps = ReceiverCapsPayload.deserialize(from: Data([1, 0, 0, 0, 0, 0, 0, 0]))
        XCTAssertEqual(caps?.flags, ReceiverCapsPayload.localCursor)
        XCTAssertNil(ReceiverCapsPayload.deserialize(from: Data([1, 0, 0])))

        XCTAssertEqual(MessageType.receiverCaps.rawValue, 0x90)
        XCTAssertEqual(MessageType.cursorShape.rawValue, 0x91)
        XCTAssertEqual(MessageType.cursorPosition.rawValue, 0x92)
    }

    func testRendersTheRealArrow() throws {
        let cursor = NSCursor.arrow
        let shape = try XCTUnwrap(PointerImage.render(cursor, streamPixelsPerPoint: 1))
        let size = cursor.image.size
        XCTAssertEqual(Int(shape.width), Int((size.width * 2).rounded(.up)), "2 pixels per point")
        XCTAssertEqual(Int(shape.height), Int((size.height * 2).rounded(.up)))
        XCTAssertEqual(Int(shape.hotspotX), Int((cursor.hotSpot.x * 2).rounded()))
        XCTAssertEqual(Int(shape.hotspotY), Int((cursor.hotSpot.y * 2).rounded()))
        XCTAssertEqual(shape.scale, 0.5, "stream pixels per bitmap pixel")
        XCTAssertEqual(shape.pixels.count, Int(shape.width) * Int(shape.height) * 4)

        let w = Int(shape.width), h = Int(shape.height)
        let px = [UInt8](shape.pixels)
        func alpha(_ x: Int, _ y: Int) -> UInt8 { px[(y * w + x) * 4 + 3] }
        func maxAlpha(around cx: Int, _ cy: Int) -> UInt8 {
            var m: UInt8 = 0
            for y in max(cy - 3, 0)...min(cy + 3, h - 1) {
                for x in max(cx - 3, 0)...min(cx + 3, w - 1) { m = max(m, alpha(x, y)) }
            }
            return m
        }
        // Premultiplied BGRA: no color above its alpha
        var premultiplied = true
        for i in stride(from: 0, to: px.count, by: 4) where px[i] > px[i + 3] || px[i + 1] > px[i + 3] || px[i + 2] > px[i + 3] {
            premultiplied = false
        }
        XCTAssertTrue(premultiplied)
        XCTAssertEqual(px.enumerated().filter { $0.offset % 4 == 3 }.map(\.element).max(), 255, "opaque body")
        // Top row first: the arrow's tip is at the hot spot, nothing at the mirrored point
        let hx = Int(shape.hotspotX), hy = Int(shape.hotspotY)
        XCTAssertGreaterThan(maxAlpha(around: hx, hy), 128)
        XCTAssertEqual(maxAlpha(around: hx, h - 1 - hy), 0)
        XCTAssertEqual(alpha(w - 1, 0), 0, "top-right corner is empty")

        XCTAssertEqual(PointerImage.render(cursor, streamPixelsPerPoint: 2)?.scale, 1, "Retina-size stream")
    }

    func testShapeIdsIdentifyImages() throws {
        let arrow1 = try XCTUnwrap(PointerImage.render(.arrow, streamPixelsPerPoint: 1))
        let arrow2 = try XCTUnwrap(PointerImage.render(.arrow, streamPixelsPerPoint: 1))
        let beam = try XCTUnwrap(PointerImage.render(.iBeam, streamPixelsPerPoint: 1))
        XCTAssertEqual(arrow1.id, arrow2.id, "same image, same id: not sent twice")
        XCTAssertNotEqual(arrow1.id, beam.id)
        XCTAssertEqual(arrow1.id, arrow1.computeId())
        let bigger = try XCTUnwrap(PointerImage.render(.arrow, streamPixelsPerPoint: 2))
        XCTAssertNotEqual(arrow1.id, bigger.id, "a new size on screen is a new shape")
    }

    func testLargePointersAreCapped() throws {
        let image = NSImage(size: NSSize(width: 400, height: 300))
        image.lockFocus()
        NSColor.red.setFill()
        NSRect(x: 0, y: 0, width: 400, height: 300).fill()
        image.unlockFocus()
        let cursor = NSCursor(image: image, hotSpot: NSPoint(x: 399, y: 299))
        let shape = try XCTUnwrap(PointerImage.render(cursor, streamPixelsPerPoint: 1))
        XCTAssertEqual(shape.width, 256)
        XCTAssertEqual(shape.height, 192)
        XCTAssertEqual(shape.hotspotX, 255)
        XCTAssertEqual(shape.hotspotY, 191)
        XCTAssertEqual(Double(shape.scale), 1 / 0.64, accuracy: 0.0001)
        XCTAssertEqual(shape.serialize().count, CursorShapePayload.headerSize + 256 * 192 * 4)
    }

    func testOnlyMacMovesAreReported() {
        let display = CGRect(x: 1440, y: 0, width: 1920, height: 1080)   // right of the MacBook screen
        var t = PointerTracker()

        // First look: always reported, 0…1 like INPUT_EVENT (last pixel = 1)
        XCTAssertEqual(t.update(location: CGPoint(x: 1440 + 959.5, y: 539.5), bounds: display, injected: []),
                       CursorPositionPayload(x: 0.5, y: 0.5, flags: CursorPositionPayload.visible))
        XCTAssertNil(t.update(location: CGPoint(x: 1440 + 959.5, y: 539.5), bounds: display, injected: []))
        XCTAssertFalse(t.moved)
        XCTAssertEqual(t.update(location: CGPoint(x: 1440 + 1919, y: 1079), bounds: display, injected: []),
                       CursorPositionPayload(x: 1, y: 1, flags: CursorPositionPayload.visible))

        // The PC's own mouse put it there (within a pixel): not echoed
        XCTAssertNil(t.update(location: CGPoint(x: 1500, y: 500), bounds: display,
                              injected: [CGPoint(x: 1500.4, y: 500.3)]))
        XCTAssertTrue(t.moved)
        // ...also when the poll came between recording a warp and the warp
        XCTAssertNil(t.update(location: CGPoint(x: 1600, y: 600), bounds: display,
                              injected: [CGPoint(x: 1600, y: 600), CGPoint(x: 1700, y: 700)]))
        // After an echo the PC knows a newer position: even the last reported one goes out again
        XCTAssertEqual(t.update(location: CGPoint(x: 1440 + 1919, y: 1079), bounds: display, injected: []),
                       CursorPositionPayload(x: 1, y: 1, flags: CursorPositionPayload.visible))

        // Onto the MacBook's own screen: hidden on the PC, said once
        XCTAssertEqual(t.update(location: CGPoint(x: 100, y: 100), bounds: display, injected: []),
                       CursorPositionPayload(x: 0, y: 0, flags: 0))
        XCTAssertNil(t.update(location: CGPoint(x: 200, y: 100), bounds: display, injected: []))
        XCTAssertEqual(t.update(location: CGPoint(x: 1440, y: 0), bounds: display, injected: [])?.flags,
                       CursorPositionPayload.visible)

        // New connection: report again even without a move
        t.reset()
        XCTAssertNotNil(t.update(location: CGPoint(x: 1440, y: 0), bounds: display, injected: [CGPoint(x: 1440, y: 0)]))
        // No display (capture not running): hidden
        var none = PointerTracker()
        XCTAssertEqual(none.update(location: CGPoint(x: 5, y: 5), bounds: .zero, injected: [])?.flags, 0)
    }

    func testEchoKeepsTheLastTwoWarps() {
        let echo = PointerEcho()
        XCTAssertEqual(echo.recent, [])
        echo.record(CGPoint(x: 1, y: 1))
        echo.record(CGPoint(x: 2, y: 2))
        echo.record(CGPoint(x: 3, y: 3))
        XCTAssertEqual(echo.recent, [CGPoint(x: 2, y: 2), CGPoint(x: 3, y: 3)])
    }

    func testInjectorFollowsTheDisplaysCurrentPosition() throws {
        let display = CGMainDisplayID()
        let bounds = CGDisplayBounds(display)
        try XCTSkipIf(bounds.isEmpty, "no display")
        let injector = InputInjector()
        injector.displayWidth = 10          // stale values lose to the live bounds
        injector.displayHeight = 10
        injector.displayOriginX = 5000
        injector.displayID = display
        XCTAssertEqual(injector.normalizedToDisplay(x: 0, y: 0), bounds.origin)
        XCTAssertEqual(injector.normalizedToDisplay(x: 1, y: 1), CGPoint(x: bounds.maxX - 1, y: bounds.maxY - 1))
    }
}
