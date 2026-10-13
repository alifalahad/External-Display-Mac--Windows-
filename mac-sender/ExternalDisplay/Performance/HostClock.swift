// =============================================================================
// HostClock.swift — Monotonic microsecond clock for pipeline timestamps
// =============================================================================
// Every Mac-side timestamp that crosses the wire (frame timing prefix, PONG)
// uses this clock so the receiver can map them all with one clock offset.
//
// Based on mach_absolute_time — the same clock as CACurrentMediaTime(),
// ScreenCaptureKit's SCStreamFrameInfo.displayTime and the CoreMedia host clock.
// =============================================================================

import Darwin

enum HostClock {
    private static let timebase: mach_timebase_info_data_t = {
        var info = mach_timebase_info_data_t()
        mach_timebase_info(&info)
        return info
    }()

    /// Convert mach_absolute_time ticks (e.g. SCK displayTime) to microseconds
    static func microseconds(fromMachTicks ticks: UInt64) -> UInt64 {
        // 128-bit intermediate: ticks × numer can exceed 64 bits on long uptimes
        let product = ticks.multipliedFullWidth(by: UInt64(timebase.numer))
        let nanoseconds = UInt64(timebase.denom).dividingFullWidth(product).quotient
        return nanoseconds / 1000
    }

    /// Current time in microseconds
    static func nowUs() -> UInt64 {
        microseconds(fromMachTicks: mach_absolute_time())
    }
}
