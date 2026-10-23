// =============================================================================
// Log.swift — Structured, leveled logging (Phase 19)
// =============================================================================
// Two sinks:
//   - Unified logging (os.Logger), subsystem "com.externaldisplay.sender":
//       log stream --predicate 'subsystem == "com.externaldisplay.sender"' --level debug
//   - ~/Library/Logs/ExternalDisplay/sender.log (rotated at 5 MB → sender.log.1)
//
//   Log.info(.net, "Connected to \(peer)")
//   Log.every(1, key: "udp-drop", .warning, .net, "dropping packets")   // hot paths
//
// Messages are built only when their level is enabled (@autoclosure).
// Never log secrets: no pairing keys, no verification codes, no clipboard text.
// =============================================================================

import Foundation
import os

enum LogLevel: Int, Comparable, CaseIterable {
    case trace, debug, info, warning, error, critical

    static func < (a: LogLevel, b: LogLevel) -> Bool { a.rawValue < b.rawValue }

    var name: String {
        switch self {
        case .trace: return "TRACE"
        case .debug: return "DEBUG"
        case .info: return "INFO"
        case .warning: return "WARN"
        case .error: return "ERROR"
        case .critical: return "CRIT"
        }
    }

    var osLogType: OSLogType {
        switch self {
        case .trace, .debug: return .debug
        case .info: return .info
        case .warning: return .default
        case .error: return .error
        case .critical: return .fault
        }
    }
}

enum LogCategory: String, CaseIterable {
    case app = "App", capture = "Capture", encoder = "Encoder", net = "Net"
    case pairing = "Pairing", input = "Input", clipboard = "Clipboard"
    case display = "Display", quality = "Quality", perf = "Perf"
}

enum Log {
    static let subsystem = "com.externaldisplay.sender"

    /// Diagnostics mode (persisted): DEBUG detail instead of INFO
    static var diagnosticsEnabled: Bool {
        get { UserDefaults.standard.bool(forKey: "diagnosticsLogging") }
        set {
            UserDefaults.standard.set(newValue, forKey: "diagnosticsLogging")
            minimumLevel = newValue ? .debug : .info
        }
    }

    private static let state = LogState()
    private(set) static var minimumLevel: LogLevel {
        get { state.level }
        set { state.level = newValue }
    }

    static var filePath: String { state.fileURL.path }

    static func isEnabled(_ level: LogLevel) -> Bool { level >= minimumLevel }

    static func trace(_ c: LogCategory, _ m: @autoclosure () -> String) { write(.trace, c, m) }
    static func debug(_ c: LogCategory, _ m: @autoclosure () -> String) { write(.debug, c, m) }
    static func info(_ c: LogCategory, _ m: @autoclosure () -> String) { write(.info, c, m) }
    static func warning(_ c: LogCategory, _ m: @autoclosure () -> String) { write(.warning, c, m) }
    static func error(_ c: LogCategory, _ m: @autoclosure () -> String) { write(.error, c, m) }
    static func critical(_ c: LogCategory, _ m: @autoclosure () -> String) { write(.critical, c, m) }

    /// At most one line per `seconds` for `key` (per-frame / per-packet paths)
    static func every(_ seconds: Double, key: String, _ level: LogLevel, _ c: LogCategory,
                      _ m: @autoclosure () -> String) {
        guard isEnabled(level), state.shouldLog(key: key, interval: seconds) else { return }
        write(level, c, m)
    }

    private static func write(_ level: LogLevel, _ category: LogCategory, _ message: () -> String) {
        guard isEnabled(level) else { return }
        let text = message()
        state.logger(for: category).log(level: level.osLogType, "\(text, privacy: .public)")
        state.appendToFile(level: level, category: category, text: text)
    }
}

/// Mutable logging state behind a lock (loggers, file, rate limits)
private final class LogState: @unchecked Sendable {
    private let lock = NSLock()
    private var loggers: [LogCategory: Logger] = [:]
    private var lastLogged: [String: TimeInterval] = [:]
    private var _level: LogLevel
    private let fileQueue = DispatchQueue(label: "com.externaldisplay.log", qos: .utility)
    private var fileHandle: FileHandle?
    let fileURL: URL
    private let formatter: DateFormatter

    init() {
        _level = UserDefaults.standard.bool(forKey: "diagnosticsLogging") ? .debug : .info
        let logs = FileManager.default.urls(for: .libraryDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Logs/ExternalDisplay", isDirectory: true)
        fileURL = logs.appendingPathComponent("sender.log")
        formatter = DateFormatter()
        formatter.dateFormat = "yyyy-MM-dd HH:mm:ss.SSS"
        openFile(directory: logs)
    }

    var level: LogLevel {
        get { lock.lock(); defer { lock.unlock() }; return _level }
        set { lock.lock(); _level = newValue; lock.unlock() }
    }

    func logger(for category: LogCategory) -> Logger {
        lock.lock()
        defer { lock.unlock() }
        if let existing = loggers[category] { return existing }
        let created = Logger(subsystem: Log.subsystem, category: category.rawValue)
        loggers[category] = created
        return created
    }

    func shouldLog(key: String, interval: TimeInterval) -> Bool {
        let now = ProcessInfo.processInfo.systemUptime
        lock.lock()
        defer { lock.unlock() }
        if let last = lastLogged[key], now - last < interval { return false }
        lastLogged[key] = now
        return true
    }

    func appendToFile(level: LogLevel, category: LogCategory, text: String) {
        let line = "\(formatter.string(from: Date())) \(level.name.padding(toLength: 5, withPad: " ", startingAt: 0)) [\(category.rawValue)] \(text)\n"
        fileQueue.async { [weak self] in
            self?.fileHandle?.write(Data(line.utf8))
        }
    }

    private func openFile(directory: URL) {
        let fm = FileManager.default
        try? fm.createDirectory(at: directory, withIntermediateDirectories: true)
        if let size = (try? fm.attributesOfItem(atPath: fileURL.path))?[.size] as? UInt64,
           size > 5 * 1024 * 1024 {
            let previous = fileURL.appendingPathExtension("1")
            try? fm.removeItem(at: previous)
            try? fm.moveItem(at: fileURL, to: previous)
        }
        if !fm.fileExists(atPath: fileURL.path) {
            fm.createFile(atPath: fileURL.path, contents: nil)
        }
        fileHandle = try? FileHandle(forWritingTo: fileURL)
        fileHandle?.seekToEndOfFile()
    }
}
