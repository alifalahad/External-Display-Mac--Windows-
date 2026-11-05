// =============================================================================
// VirtualDisplayManager.swift — Creates a virtual macOS display
// =============================================================================
// Uses the private CGVirtualDisplay API (same as BetterDisplay / hidpi-mirror)
// to create a virtual second monitor that macOS treats as a real display.
//
// API structure (reverse-engineered from class-dump):
//
//   CGVirtualDisplayDescriptor:
//     - name, maxPixelsWide, maxPixelsHigh, sizeInMillimeters
//     - vendorID, productID, serialNum
//     - queue (DispatchQueue for events), terminationHandler
//
//   CGVirtualDisplayMode:
//     - initWithWidth:height:refreshRate:
//     (step 7: a 120 Hz and a 60 Hz mode; setRefreshRate switches between
//     them with the public CGConfigureDisplayWithDisplayMode)
//
//   CGVirtualDisplaySettings:
//     - modes (NSArray of CGVirtualDisplayMode)
//     - hiDPI (BOOL)
//
//   CGVirtualDisplay:
//     - initWithDescriptor:
//     - applySettings:
//     - displayID (readonly)
//
// Technical approach: Objective-C runtime (NSClassFromString + perform/KVC)
// to avoid bridging header complications with SwiftPM.
// =============================================================================

import Foundation
import CoreGraphics
import Combine

final class VirtualDisplayManager: ObservableObject {

    // ── Published State ─────────────────────────────────────────────────────

    @Published var isActive = false
    @Published var virtualDisplayID: CGDirectDisplayID = 0
    @Published var errorMessage: String?

    /// Resolution of the virtual display
    @Published var width: Int = 1920
    @Published var height: Int = 1080
    /// Refresh rate in effect (docs/SMOOTHNESS.md, step 7: 120 Hz for 120 FPS)
    @Published private(set) var refreshHz: Double = 60

    // ── Private State ───────────────────────────────────────────────────────

    /// Hold a strong reference to the CGVirtualDisplay object to keep it alive
    private var virtualDisplay: AnyObject?

    /// Remember which displays existed BEFORE we created the virtual one
    private var preExistingDisplayIDs = Set<CGDirectDisplayID>()

    /// Dispatch queue for virtual display events
    private let displayQueue = DispatchQueue(label: "com.externaldisplay.virtualdisplay")

    // ── Create Virtual Display ──────────────────────────────────────────────

    /// refreshHz: 120 for a stream at up to 120 FPS (step 7), else 60. Both
    /// modes are offered, so setRefreshRate() can switch later.
    @discardableResult
    func createDisplay(width: Int, height: Int, refreshHz: Double = 60) -> Bool {
        guard !isActive else {
            Log.debug(.display, "Already active")
            return false
        }

        self.width = width
        self.height = height

        // Snapshot existing displays before creating the virtual one
        preExistingDisplayIDs = getCurrentDisplayIDs()
        Log.debug(.display, "Pre-existing displays: \(preExistingDisplayIDs)")

        // ── 1. Load private classes ─────────────────────────────────────────
        guard let descriptorClass = NSClassFromString("CGVirtualDisplayDescriptor"),
              let modeClass = NSClassFromString("CGVirtualDisplayMode"),
              let settingsClass = NSClassFromString("CGVirtualDisplaySettings"),
              let virtualDisplayClass = NSClassFromString("CGVirtualDisplay") else {
            setError("CGVirtualDisplay API not available on this macOS version")
            return false
        }

        // ── 2. Create CGVirtualDisplayDescriptor ────────────────────────────
        guard let desc = objcAllocInit(descriptorClass) else {
            setError("Failed to create CGVirtualDisplayDescriptor")
            return false
        }

        // Set descriptor properties via KVC
        desc.setValue(width as NSNumber, forKey: "maxPixelsWide")
        desc.setValue(height as NSNumber, forKey: "maxPixelsHigh")
        desc.setValue("External Display (Windows)" as NSString, forKey: "name")

        // Use width-based product ID so mode preferences don't stick
        // (macOS caches display mode per vendor+product, ignoring serial)
        let productID = UInt32(0xED00 | (width & 0xFF))
        desc.setValue(productID as NSNumber, forKey: "productID")
        desc.setValue(0xED5F as NSNumber, forKey: "vendorID")
        desc.setValue(0 as NSNumber, forKey: "serialNum")

        // Physical size in mm (approximate 24" display for realistic PPI)
        let aspectRatio = Double(width) / Double(height)
        let diagMM = 24.0 * 25.4
        let heightMM = diagMM / sqrt(1.0 + aspectRatio * aspectRatio)
        let widthMM = heightMM * aspectRatio
        desc.setValue(NSValue(size: NSSize(width: widthMM, height: heightMM)),
                      forKey: "sizeInMillimeters")

        // Set the dispatch queue (required — it's a DispatchQueue, NOT an array)
        desc.setValue(displayQueue, forKey: "queue")

        Log.debug(.display, "Descriptor: \(width)×\(height), vendor=0xED5F, product=0x\(String(productID, radix: 16))")

        // ── 3. Create CGVirtualDisplay ──────────────────────────────────────
        let initSel = NSSelectorFromString("initWithDescriptor:")
        guard virtualDisplayClass.instancesRespond(to: initSel) else {
            setError("CGVirtualDisplay missing initWithDescriptor:")
            return false
        }

        guard let allocated = (virtualDisplayClass as AnyObject)
                .perform(NSSelectorFromString("alloc"))?.takeUnretainedValue() else {
            setError("Failed to alloc CGVirtualDisplay")
            return false
        }

        guard let display = allocated.perform(initSel, with: desc)?.takeUnretainedValue() else {
            setError("initWithDescriptor: returned nil — check macOS version/entitlements")
            return false
        }

        Log.debug(.display, "CGVirtualDisplay created, applying settings...")

        // ── 4. Create CGVirtualDisplaySettings with modes ───────────────────
        if let settings = objcAllocInit(settingsClass) {
            // Create display mode using initWithWidth:height:refreshRate:
            // Since perform() can't pass primitive args, we use NSInvocation
            // The wanted refresh rate first: the display starts in it
            let rates = refreshHz > 60 ? [refreshHz, 60.0] : [60.0, 120.0]
            let modes = rates.compactMap { createMode(modeClass: modeClass, w: width, h: height, hz: $0) }
            if !modes.isEmpty {
                settings.setValue(modes, forKey: "modes")
                Log.debug(.display, "Modes: \(width)×\(height) @ " + rates.map { "\(Int($0)) Hz" }.joined(separator: ", "))
            } else {
                Log.warning(.display, "Could not create display mode, continuing without explicit modes")
            }

            // Apply settings to the display
            let applySel = NSSelectorFromString("applySettings:")
            if display.responds(to: applySel) {
                _ = display.perform(applySel, with: settings)
                Log.debug(.display, "Settings applied")
            } else {
                Log.warning(.display, "applySettings: not available")
            }
        }

        // Keep the display alive
        virtualDisplay = display

        // ── 5. Find the display ID ──────────────────────────────────────────
        var foundID: CGDirectDisplayID = 0

        // Try the displayID property
        let displayIDSel = NSSelectorFromString("displayID")
        if display.responds(to: displayIDSel),
           let result = display.perform(displayIDSel) {
            let rawPtr = result.toOpaque()
            let rawID = UInt32(UInt(bitPattern: rawPtr) & 0xFFFFFFFF)
            if rawID != 0 {
                foundID = rawID
                Log.debug(.display, "displayID property returned: \(foundID)")
            }
        }

        // Fallback: compare display lists before/after
        if foundID == 0 {
            // Give WindowServer time to register the display
            Thread.sleep(forTimeInterval: 0.5)
            let currentIDs = getCurrentDisplayIDs()
            let newIDs = currentIDs.subtracting(preExistingDisplayIDs)
            Log.debug(.display, "Displays after creation: \(currentIDs), new: \(newIDs)")
            if let newID = newIDs.first {
                foundID = newID
            }
        }

        if foundID != 0 {
            let hz = Self.switchRefresh(of: foundID, to: refreshHz)
            DispatchQueue.main.async {
                self.virtualDisplayID = foundID
                self.isActive = true
                self.errorMessage = nil
                self.refreshHz = hz
            }
            Log.info(.display, "Created: \(width)×\(height) @ \(Int(hz.rounded())) Hz, ID=\(foundID)")
            return true
        }

        // Even if we can't find the ID, the display may still be active
        // Try one more time after a longer delay
        Thread.sleep(forTimeInterval: 1.0)
        let finalIDs = getCurrentDisplayIDs()
        let finalNewIDs = finalIDs.subtracting(preExistingDisplayIDs)
        if let newID = finalNewIDs.first {
            let hz = Self.switchRefresh(of: newID, to: refreshHz)
            DispatchQueue.main.async {
                self.virtualDisplayID = newID
                self.isActive = true
                self.errorMessage = nil
                self.refreshHz = hz
            }
            Log.info(.display, "Created (delayed detection): \(width)×\(height) @ \(Int(hz.rounded())) Hz, ID=\(newID)")
            return true
        }

        setError("Display object created but could not detect display ID. Check System Settings → Displays.")
        return false
    }

    // ── Destroy Virtual Display ─────────────────────────────────────────────

    func destroyDisplay() {
        guard isActive else { return }
        virtualDisplay = nil
        DispatchQueue.main.async {
            self.isActive = false
            self.virtualDisplayID = 0
            self.errorMessage = nil
        }
        Log.info(.display, "Destroyed virtual display")
    }

    // ── Refresh rate (docs/SMOOTHNESS.md, step 7) ───────────────────────────

    /// Main thread. Switch the virtual display to `hz` (60 or 120); returns the
    /// refresh rate in effect afterwards.
    @discardableResult
    func setRefreshRate(_ hz: Double) -> Double {
        guard isActive, virtualDisplayID != 0 else { return refreshHz }
        refreshHz = Self.switchRefresh(of: virtualDisplayID, to: hz)
        return refreshHz
    }

    /// The refresh rate a display runs at (0: unknown)
    static func refreshRate(of id: CGDirectDisplayID) -> Double {
        CGDisplayCopyDisplayMode(id)?.refreshRate ?? 0
    }

    /// Switch `id` to the mode of the same size at `hz`, if it has one
    private static func switchRefresh(of id: CGDirectDisplayID, to hz: Double) -> Double {
        guard let current = CGDisplayCopyDisplayMode(id) else { return 0 }
        if abs(current.refreshRate - hz) < 1 { return current.refreshRate }
        let modes = (CGDisplayCopyAllDisplayModes(id, nil) as? [CGDisplayMode]) ?? []
        guard let mode = modes.first(where: {
            $0.width == current.width && $0.height == current.height && abs($0.refreshRate - hz) < 1
        }) else {
            Log.warning(.display, "No \(Int(hz)) Hz mode (this display offers "
                        + modes.map { "\($0.width)×\($0.height) @ \(Int($0.refreshRate)) Hz" }.joined(separator: ", ")
                        + "): staying at \(Int(current.refreshRate)) Hz")
            return current.refreshRate
        }
        var config: CGDisplayConfigRef?
        guard CGBeginDisplayConfiguration(&config) == .success, let config else { return current.refreshRate }
        CGConfigureDisplayWithDisplayMode(config, id, mode, nil)
        guard CGCompleteDisplayConfiguration(config, .forSession) == .success else {
            Log.warning(.display, "Could not switch to \(Int(hz)) Hz: staying at \(Int(current.refreshRate)) Hz")
            return current.refreshRate
        }
        let now = refreshRate(of: id)
        Log.info(.display, "Refresh rate \(Int(current.refreshRate)) → \(Int(now.rounded())) Hz")
        return now
    }

    // ── Helpers ─────────────────────────────────────────────────────────────

    /// Get the display origin (for coordinate remapping)
    func getDisplayBounds() -> CGRect {
        guard isActive, virtualDisplayID != 0 else { return .zero }
        return CGDisplayBounds(virtualDisplayID)
    }

    /// Allocate + init an Objective-C class via runtime
    private func objcAllocInit(_ cls: AnyClass) -> AnyObject? {
        guard let allocResult = (cls as AnyObject).perform(NSSelectorFromString("alloc")),
              let initResult = allocResult.takeUnretainedValue().perform(NSSelectorFromString("init")) else {
            return nil
        }
        return initResult.takeUnretainedValue()
    }

    /// Create a CGVirtualDisplayMode
    private func createMode(modeClass: AnyClass, w: Int, h: Int, hz: Double) -> AnyObject? {
        // First try the designated initializer via objc_msgSend (handles primitive args)
        let sel = NSSelectorFromString("initWithWidth:height:refreshRate:")
        if modeClass.instancesRespond(to: sel) {
            typealias InitFunc = @convention(c) (AnyObject, Selector, UInt32, UInt32, Double) -> AnyObject?
            guard let allocResult = (modeClass as AnyObject).perform(NSSelectorFromString("alloc")) else {
                return nil
            }
            let allocated = allocResult.takeUnretainedValue()
            let imp = allocated.method(for: sel)
            let initCall = unsafeBitCast(imp, to: InitFunc.self)
            if let mode = initCall(allocated, sel, UInt32(w), UInt32(h), hz) {
                return mode
            }
        }

        // Fallback: alloc/init then set properties via KVC
        guard let mode = objcAllocInit(modeClass) else { return nil }
        mode.setValue(w as NSNumber, forKey: "width")
        mode.setValue(h as NSNumber, forKey: "height")
        mode.setValue(hz as NSNumber, forKey: "refreshRate")
        return mode
    }

    /// Get all currently active display IDs
    private func getCurrentDisplayIDs() -> Set<CGDirectDisplayID> {
        var ids = [CGDirectDisplayID](repeating: 0, count: 16)
        var count: UInt32 = 0
        guard CGGetActiveDisplayList(16, &ids, &count) == .success else { return [] }
        return Set(ids.prefix(Int(count)))
    }

    private func setError(_ msg: String) {
        Log.error(.display, msg)
        DispatchQueue.main.async { self.errorMessage = msg }
    }

    deinit {
        destroyDisplay()
    }
}
