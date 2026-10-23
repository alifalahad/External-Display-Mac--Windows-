#pragma once
// =============================================================================
// Log.h — Structured, leveled logging (Phase 19)
// =============================================================================
// Every line: "2026-10-23 14:03:22.123 INFO  [Net] message"
// Goes to the console and to receiver.log next to the .exe (rotated at 5 MB:
// the previous file is kept as receiver.log.1).
//
//   LOG_INFO("Net") << "Connected to " << ip;          // stream style
//   LOGF_WARN("Decoder", "ProcessInput failed 0x%08lX", (unsigned long)hr);
//   LOG_EVERY_MS(1000, LogLevel::Warning, "Net") << "dropping packets";
//
// The level check happens before the message is formatted, so disabled TRACE
// and DEBUG lines cost one comparison. Hot paths (per frame, per packet) must
// only use LOG_EVERY_MS or TRACE — never plain INFO.
//
// Portable C++ (no Windows headers): unit-tested on any OS.
// =============================================================================

#include <atomic>
#include <cstdint>
#include <sstream>
#include <string>

enum class LogLevel : int { Trace = 0, Debug, Info, Warning, Error, Critical, Off };

namespace Log {

/// Start logging to `filePath` (UTF-8; empty = console only). Rotates an
/// existing file larger than maxBytes to "<file>.1" first.
bool Init(const std::string& filePath, LogLevel level, uint64_t maxBytes = 5 * 1024 * 1024);
void Shutdown();

void SetLevel(LogLevel level);
LogLevel GetLevel();
inline bool Enabled(LogLevel level);

/// Console output on/off (tests turn it off)
void SetConsoleEnabled(bool enabled);

/// Parse "trace" / "debug" / "info" / "warning" / "error" / "critical" / "off"
bool ParseLevel(const std::string& text, LogLevel& out);
const char* LevelName(LogLevel level);

/// Format and write one line (thread-safe)
void Write(LogLevel level, const char* component, const std::string& message);
void Writef(LogLevel level, const char* component, const char* format, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

/// Path of the log file ("" if none)
std::string FilePath();

/// For LOG_EVERY_MS: true at most once per interval for one call site
bool ShouldLogNow(std::atomic<int64_t>& lastMs, int64_t intervalMs);

namespace detail {
extern std::atomic<int> gLevel;
}

inline bool Enabled(LogLevel level) {
    return static_cast<int>(level) >= detail::gLevel.load(std::memory_order_relaxed);
}

} // namespace Log

/// Collects a stream-style message and writes it when the statement ends
class LogLine {
public:
    LogLine(LogLevel level, const char* component) : level_(level), component_(component) {}
    ~LogLine() { Log::Write(level_, component_, stream_.str()); }
    LogLine(const LogLine&) = delete;
    LogLine& operator=(const LogLine&) = delete;

    template <typename T>
    LogLine& operator<<(const T& value) {
        stream_ << value;
        return *this;
    }

private:
    LogLevel level_;
    const char* component_;
    std::ostringstream stream_;
};

// A one-shot for loop: safe inside if/else without braces (no dangling else),
// and the message is only built when the level is enabled
#define EXDP_LOG(level, component) \
    for (bool exdpLogOn = Log::Enabled(level); exdpLogOn; exdpLogOn = false) LogLine(level, component)

#define LOG_TRACE(component)    EXDP_LOG(LogLevel::Trace, component)
#define LOG_DEBUG(component)    EXDP_LOG(LogLevel::Debug, component)
#define LOG_INFO(component)     EXDP_LOG(LogLevel::Info, component)
#define LOG_WARN(component)     EXDP_LOG(LogLevel::Warning, component)
#define LOG_ERROR(component)    EXDP_LOG(LogLevel::Error, component)
#define LOG_CRITICAL(component) EXDP_LOG(LogLevel::Critical, component)

#define EXDP_LOGF(level, component, ...) \
    do { if (Log::Enabled(level)) Log::Writef(level, component, __VA_ARGS__); } while (0)

#define LOGF_TRACE(component, ...)    EXDP_LOGF(LogLevel::Trace, component, __VA_ARGS__)
#define LOGF_DEBUG(component, ...)    EXDP_LOGF(LogLevel::Debug, component, __VA_ARGS__)
#define LOGF_INFO(component, ...)     EXDP_LOGF(LogLevel::Info, component, __VA_ARGS__)
#define LOGF_WARN(component, ...)     EXDP_LOGF(LogLevel::Warning, component, __VA_ARGS__)
#define LOGF_ERROR(component, ...)    EXDP_LOGF(LogLevel::Error, component, __VA_ARGS__)
#define LOGF_CRITICAL(component, ...) EXDP_LOGF(LogLevel::Critical, component, __VA_ARGS__)

/// At most one line per `ms` milliseconds from this call site (hot paths)
#define LOG_EVERY_MS(ms, level, component)                                          \
    for (static std::atomic<int64_t> exdpLastLogMs{0};                              \
         Log::Enabled(level) && Log::ShouldLogNow(exdpLastLogMs, (ms));)            \
        for (bool exdpLogOnce = true; exdpLogOnce; exdpLogOnce = false) LogLine(level, component)
