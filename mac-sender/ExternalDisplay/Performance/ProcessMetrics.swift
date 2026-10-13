// =============================================================================
// ProcessMetrics.swift — This process's CPU and memory usage
// =============================================================================
// CPU:    getrusage() user+system time delta / wall time delta, divided by the
//         number of cores → percent of the WHOLE machine (100% = all cores busy).
//         Activity Monitor shows percent of ONE core: multiply by core count.
// Memory: phys_footprint from task_info — the "Memory" column in Activity Monitor.
//
// GPU usage is not reported: macOS has no public per-process GPU utilization API.
// =============================================================================

import Foundation
import QuartzCore

struct ProcessUsage {
    var cpuPercent: Double = 0      // of all cores
    var memoryMB: Double = 0
}

final class ProcessMetrics {
    private var lastCPUTime: Double = 0
    private var lastWallTime: Double = 0
    private let coreCount = Double(max(ProcessInfo.processInfo.activeProcessorCount, 1))

    /// Usage since the previous call (first call primes the CPU baseline)
    func sample() -> ProcessUsage {
        let cpuTime = Self.processCPUSeconds()
        let wallTime = CACurrentMediaTime()
        var usage = ProcessUsage(memoryMB: Self.physicalFootprintMB())

        if lastWallTime > 0, wallTime > lastWallTime {
            usage.cpuPercent = (cpuTime - lastCPUTime) / (wallTime - lastWallTime) / coreCount * 100
        }
        lastCPUTime = cpuTime
        lastWallTime = wallTime
        return usage
    }

    private static func processCPUSeconds() -> Double {
        var usage = rusage()
        guard getrusage(RUSAGE_SELF, &usage) == 0 else { return 0 }
        func seconds(_ t: timeval) -> Double { Double(t.tv_sec) + Double(t.tv_usec) / 1_000_000 }
        return seconds(usage.ru_utime) + seconds(usage.ru_stime)
    }

    private static func physicalFootprintMB() -> Double {
        var info = task_vm_info_data_t()
        var count = mach_msg_type_number_t(
            MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<natural_t>.size)
        let result = withUnsafeMutablePointer(to: &info) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count)
            }
        }
        guard result == KERN_SUCCESS else { return 0 }
        return Double(info.phys_footprint) / (1024 * 1024)
    }
}
