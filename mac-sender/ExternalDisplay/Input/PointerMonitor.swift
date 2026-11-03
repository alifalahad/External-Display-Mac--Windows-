// =============================================================================
// PointerMonitor.swift — The Mac's pointer, for receivers that draw it
// =============================================================================
// docs/SMOOTHNESS.md, step 1. When the receiver announces localCursor
// (RECEIVER_CAPS), capture leaves the pointer out of the video and this
// monitor sends it separately:
//   CURSOR_SHAPE     when the pointer image changes (NSCursor.currentSystem)
//   CURSOR_POSITION  when the pointer moves on the Mac (trackpad, Mac mouse),
//                    polled 120 times per second
// Moves that InputInjector made for the PC's own mouse are not echoed back:
// the receiver already shows those under Windows' own pointer.
// =============================================================================

import AppKit
import CoreGraphics
import QuartzCore

/// Where InputInjector last put the pointer (global points). Written on the
/// network queue, read on the monitor's queue. Keeps two points: a poll
/// between recording a warp and the warp itself still sees the previous one.
final class PointerEcho {
    private let lock = NSLock()
    private var points: [CGPoint] = []

    /// Call before warping
    func record(_ p: CGPoint) {
        lock.lock()
        points = [points.last, p].compactMap { $0 }
        lock.unlock()
    }

    var recent: [CGPoint] {
        lock.lock()
        defer { lock.unlock() }
        return points
    }
}

/// Which pointer positions to report (no AppKit; tested)
struct PointerTracker {
    /// A warp lands on the pixel grid: closer than this is the injected point
    static let echoTolerance: CGFloat = 1.0

    private var lastLocation: CGPoint?
    private var lastSent: CursorPositionPayload?

    /// Whether the last update saw the pointer move
    private(set) var moved = false

    /// Report everything again (new connection)
    mutating func reset() {
        lastLocation = nil
        lastSent = nil
    }

    /// location: global points, y down (CGEvent). bounds: the streamed
    /// display. injected: InputInjector's recent warps. Returns what to send.
    mutating func update(location: CGPoint, bounds: CGRect, injected: [CGPoint]) -> CursorPositionPayload? {
        let first = lastLocation == nil
        moved = first || hypot(location.x - lastLocation!.x, location.y - lastLocation!.y) > 0.01
        lastLocation = location
        guard moved else { return nil }

        if !first, injected.contains(where: {
            hypot(location.x - $0.x, location.y - $0.y) <= Self.echoTolerance
        }) {
            // The PC's mouse put it there. The receiver now knows a newer
            // position than our last report, so the next real move must go out.
            lastSent = nil
            return nil
        }

        var payload = CursorPositionPayload()
        if !bounds.isEmpty && bounds.contains(location) {
            // Inverse of InputInjector.normalizedToDisplay (last pixel = 1.0)
            func unit(_ v: CGFloat, _ origin: CGFloat, _ size: CGFloat) -> Float {
                Float(min(max((v - origin) / max(size - 1, 1), 0), 1))
            }
            payload.x = unit(location.x, bounds.minX, bounds.width)
            payload.y = unit(location.y, bounds.minY, bounds.height)
            payload.flags = CursorPositionPayload.visible
        }
        guard payload != lastSent else { return nil }   // e.g. moving around on another display
        lastSent = payload
        return payload
    }
}

/// NSCursor → CURSOR_SHAPE bitmap
enum PointerImage {
    /// streamPixelsPerPoint: stream width / display width in points
    static func render(_ cursor: NSCursor, streamPixelsPerPoint: CGFloat) -> CursorShapePayload? {
        let size = cursor.image.size
        guard size.width >= 1, size.height >= 1, streamPixelsPerPoint > 0 else { return nil }

        // 2 pixels per point (the Retina image); less for very large pointers
        let maxSide = CGFloat(CursorShapePayload.maxSide)
        let bitmapScale = min(2, maxSide / max(size.width, size.height))
        let w = max(1, min(CursorShapePayload.maxSide, Int((size.width * bitmapScale).rounded(.up))))
        let h = max(1, min(CursorShapePayload.maxSide, Int((size.height * bitmapScale).rounded(.up))))

        // BGRA in memory, premultiplied: what the receiver uploads
        guard let space = CGColorSpace(name: CGColorSpace.sRGB),
              let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                                  space: space,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue
                                            | CGBitmapInfo.byteOrder32Little.rawValue) else { return nil }
        ctx.clear(CGRect(x: 0, y: 0, width: w, height: h))
        let previous = NSGraphicsContext.current
        NSGraphicsContext.current = NSGraphicsContext(cgContext: ctx, flipped: false)
        cursor.image.draw(in: CGRect(x: 0, y: 0, width: w, height: h))
        NSGraphicsContext.current = previous

        // Row 0 of a bitmap context is the top of the image
        guard let base = ctx.data else { return nil }
        var pixels = Data(capacity: w * h * 4)
        for row in 0..<h {
            pixels.append(base.advanced(by: row * ctx.bytesPerRow).assumingMemoryBound(to: UInt8.self),
                          count: w * 4)
        }

        // NSCursor's hot spot is measured from the top-left
        let hot = cursor.hotSpot
        var shape = CursorShapePayload()
        shape.width = UInt16(w)
        shape.height = UInt16(h)
        shape.hotspotX = UInt16(min(max(Int((hot.x * bitmapScale).rounded()), 0), w - 1))
        shape.hotspotY = UInt16(min(max(Int((hot.y * bitmapScale).rounded()), 0), h - 1))
        shape.scale = Float(streamPixelsPerPoint / bitmapScale)
        shape.pixels = pixels
        shape.id = shape.computeId()
        return shape
    }
}

/// Polls the pointer while a receiver draws it. start/stop on the main thread.
final class PointerMonitor {
    /// Main thread
    var onShape: ((CursorShapePayload) -> Void)?
    /// Monitor queue
    var onPosition: ((CursorPositionPayload) -> Void)?

    private let displayID: CGDirectDisplayID
    private let streamWidth: Int
    private let echo: PointerEcho
    private let queue = DispatchQueue(label: "com.externaldisplay.pointer", qos: .userInteractive)

    // Monitor queue
    private var timer: DispatchSourceTimer?
    private var tracker = PointerTracker()
    private var lastShapeCheck: CFTimeInterval = 0
    private var shapeCheckPending = false

    // Main thread
    private var lastShapeId: UInt64?

    /// streamWidth: stream pixels across the display (sets the pointer's size)
    init(displayID: CGDirectDisplayID, streamWidth: Int, echo: PointerEcho) {
        self.displayID = displayID
        self.streamWidth = streamWidth
        self.echo = echo
    }

    func start() {
        lastShapeId = nil
        queue.async { [weak self] in
            guard let self else { return }
            self.tracker.reset()
            self.lastShapeCheck = 0
            let timer = DispatchSource.makeTimerSource(queue: self.queue)
            timer.schedule(deadline: .now(), repeating: .microseconds(8333), leeway: .milliseconds(1))
            timer.setEventHandler { [weak self] in self?.poll() }
            timer.resume()
            self.timer = timer
        }
        Log.info(.input, "Pointer sent separately (display \(displayID), stream width \(streamWidth))")
    }

    func stop() {
        onShape = nil
        queue.sync {
            timer?.cancel()
            timer = nil
            onPosition = nil
        }
    }

    private func poll() {
        guard let location = CGEvent(source: nil)?.location else { return }
        if let position = tracker.update(location: location, bounds: CGDisplayBounds(displayID),
                                         injected: echo.recent) {
            onPosition?(position)
        }

        // The image changes over text, links, edges: check often while it moves
        let now = CACurrentMediaTime()
        guard !shapeCheckPending, now - lastShapeCheck >= (tracker.moved ? 1.0 / 30 : 0.25) else { return }
        lastShapeCheck = now
        shapeCheckPending = true
        DispatchQueue.main.async { [weak self] in
            self?.checkShape()
            self?.queue.async { self?.shapeCheckPending = false }
        }
    }

    /// Main thread: AppKit
    private func checkShape() {
        guard let onShape else { return }   // stopped
        // The only public API for the system-wide pointer image (Chrome Remote
        // Desktop uses it too); Apple marks it "to be deprecated"
        let cursor = NSCursor.currentSystem ?? NSCursor.arrow
        let pointsWide = CGDisplayBounds(displayID).width
        let pixelsPerPoint = pointsWide > 0 && streamWidth > 0 ? CGFloat(streamWidth) / pointsWide : 1
        guard let shape = PointerImage.render(cursor, streamPixelsPerPoint: pixelsPerPoint),
              shape.id != lastShapeId else { return }
        lastShapeId = shape.id
        onShape(shape)
    }
}
