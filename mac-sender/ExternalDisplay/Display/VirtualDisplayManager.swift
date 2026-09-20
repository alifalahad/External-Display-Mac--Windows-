// =============================================================================
// VirtualDisplayManager.swift — Creates a virtual macOS display
// =============================================================================
// Uses the private CGVirtualDisplay API (same as BetterDisplay) to create
// a virtual second monitor that macOS treats as a real display.
//
// This display appears in System Settings → Displays, and users can:
//   - Drag windows onto it
//   - Arrange it relative to the built-in display
//   - Use it as a true extended desktop
//
// ScreenCaptureKit then captures ONLY this virtual display for streaming.
//
// Technical approach: Runtime loading via Objective-C runtime to avoid
// bridging header complications with SwiftPM.
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

    // ── Private State ───────────────────────────────────────────────────────

    /// Hold a strong reference to the CGVirtualDisplay object to keep it alive
    private var virtualDisplay: AnyObject?

    /// Remember which displays existed BEFORE we created the virtual one
    private var preExistingDisplayIDs = Set<CGDirectDisplayID>()

    /// Display serial number (used for identification)
    private let displaySerial: UInt32 = 0xED5F_0001

    // ── Create Virtual Display ──────────────────────────────────────────────

    /// Create a virtual display with the specified resolution.
    /// Returns true on success.
    @discardableResult
    func createDisplay(width: Int, height: Int) -> Bool {
        guard !isActive else {
            print("[VDisplay] Already active")
            return false
        }

        self.width = width
        self.height = height

        // Snapshot existing displays before creating the virtual one
        preExistingDisplayIDs = getCurrentDisplayIDs()

        // Access private classes via Objective-C runtime
        guard let descriptorClass = NSClassFromString("CGVirtualDisplayDescriptor"),
              let modeClass = NSClassFromString("CGVirtualDisplayMode"),
              let settingsClass = NSClassFromString("CGVirtualDisplaySettings"),
              let virtualDisplayClass = NSClassFromString("CGVirtualDisplay") else {
            let err = "CGVirtualDisplay API not available on this macOS version"
            print("[VDisplay] \(err)")
            DispatchQueue.main.async { self.errorMessage = err }
            return false
        }

        // ── Create CGVirtualDisplayDescriptor ───────────────────────────────
        guard let desc = objcAlloc(descriptorClass) else {
            setError("Failed to create CGVirtualDisplayDescriptor")
            return false
        }

        // Set descriptor properties via KVC
        desc.setValue(width as NSNumber, forKey: "maxPixelsWide")
        desc.setValue(height as NSNumber, forKey: "maxPixelsHigh")
        desc.setValue("External Display (Windows)" as NSString, forKey: "name")
        desc.setValue(displaySerial as NSNumber, forKey: "serialNum")
        desc.setValue(0xED5F as NSNumber, forKey: "productID")
        desc.setValue(0x0001 as NSNumber, forKey: "vendorID")

        // Physical size in mm (approximate 24" display)
        let aspectRatio = Double(width) / Double(height)
        let diagMM = 24.0 * 25.4
        let heightMM = diagMM / sqrt(1.0 + aspectRatio * aspectRatio)
        let widthMM = heightMM * aspectRatio
        desc.setValue(NSValue(size: NSSize(width: widthMM, height: heightMM)),
                      forKey: "sizeInMillimeters")

        // ── Create CGVirtualDisplayMode ─────────────────────────────────────
        if let mode = objcAlloc(modeClass) {
            mode.setValue(width as NSNumber, forKey: "width")
            mode.setValue(height as NSNumber, forKey: "height")
            mode.setValue(60.0 as NSNumber, forKey: "refreshRate")
            desc.setValue([mode], forKey: "queue")
        }

        // ── Create CGVirtualDisplaySettings ─────────────────────────────────
        if let settings = objcAlloc(settingsClass) {
            // Apply HiDPI if resolution is 2x or higher of a standard res
            let isHiDPI = width >= 2560 || height >= 1440
            settings.setValue(NSNumber(value: isHiDPI), forKey: "hiDPI")
            desc.setValue(settings, forKey: "settings")
        }

        // ── Create CGVirtualDisplay ─────────────────────────────────────────
        let initSel = NSSelectorFromString("initWithDescriptor:")
        guard virtualDisplayClass.instancesRespond(to: initSel) else {
            setError("CGVirtualDisplay missing initWithDescriptor: selector")
            return false
        }

        guard let allocated = (virtualDisplayClass as AnyObject)
                .perform(NSSelectorFromString("alloc"))?.takeUnretainedValue() else {
            setError("Failed to alloc CGVirtualDisplay")
            return false
        }

        guard let display = allocated.perform(initSel, with: desc)?.takeUnretainedValue() else {
            setError("initWithDescriptor: returned nil — check macOS version")
            return false
        }

        // Keep the display alive
        virtualDisplay = display

        // ── Find the display ID ─────────────────────────────────────────────
        // Try property first
        let displayIDSel = NSSelectorFromString("displayID")
        var foundID: CGDirectDisplayID = 0
        if display.responds(to: displayIDSel),
           let result = display.perform(displayIDSel) {
            let rawID = UInt32(bitPattern: Int32(Int(bitPattern: result.toOpaque()) & 0xFFFFFFFF))
            if rawID != 0 {
                foundID = rawID
            }
        }

        // Fallback: compare display lists before/after
        if foundID == 0 {
            // Give WindowServer a moment to register the display
            Thread.sleep(forTimeInterval: 0.5)
            let currentIDs = getCurrentDisplayIDs()
            let newIDs = currentIDs.subtracting(preExistingDisplayIDs)
            if let newID = newIDs.first {
                foundID = newID
            }
        }

        if foundID != 0 {
            DispatchQueue.main.async {
                self.virtualDisplayID = foundID
                self.isActive = true
                self.errorMessage = nil
            }
            print("[VDisplay] ✅ Created: \(width)×\(height) @ 60Hz, ID=\(foundID)")
            return true
        }

        setError("Display created but could not determine display ID")
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
        print("[VDisplay] Destroyed virtual display")
    }

    // ── Helpers ─────────────────────────────────────────────────────────────

    /// Get the display origin (for coordinate remapping)
    func getDisplayBounds() -> CGRect {
        guard isActive, virtualDisplayID != 0 else { return .zero }
        return CGDisplayBounds(virtualDisplayID)
    }

    /// Allocate + init an Objective-C class
    private func objcAlloc(_ cls: AnyClass) -> AnyObject? {
        guard let allocResult = (cls as AnyObject).perform(NSSelectorFromString("alloc")),
              let initResult = allocResult.takeUnretainedValue().perform(NSSelectorFromString("init")) else {
            return nil
        }
        return initResult.takeUnretainedValue()
    }

    /// Get all currently active display IDs
    private func getCurrentDisplayIDs() -> Set<CGDirectDisplayID> {
        var ids = [CGDirectDisplayID](repeating: 0, count: 16)
        var count: UInt32 = 0
        guard CGGetActiveDisplayList(16, &ids, &count) == .success else { return [] }
        return Set(ids.prefix(Int(count)))
    }

    private func setError(_ msg: String) {
        print("[VDisplay] ❌ \(msg)")
        DispatchQueue.main.async { self.errorMessage = msg }
    }

    deinit {
        destroyDisplay()
    }
}
