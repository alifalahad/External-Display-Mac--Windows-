// =============================================================================
// Log.cpp — Structured, leveled logging (Phase 19)
// =============================================================================

#include "App/Log.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

namespace Log {
namespace detail {
std::atomic<int> gLevel{static_cast<int>(LogLevel::Info)};
}

namespace {

std::mutex gMutex;
std::ofstream gFile;
std::string gFilePath;
bool gConsole = true;

std::filesystem::path PathFromUtf8(const std::string& utf8) {
#if defined(_WIN32)
    // std::filesystem::path(std::string) uses the ANSI code page on Windows
    return std::filesystem::u8path(utf8);
#else
    return std::filesystem::path(utf8);
#endif
}

std::string Timestamp() {
    using namespace std::chrono;
    auto now = system_clock::now();
    std::time_t t = system_clock::to_time_t(now);
    int ms = static_cast<int>(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char buf[40];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &local);
    char out[48];
    std::snprintf(out, sizeof(out), "%s.%03d", buf, ms);
    return out;
}

int64_t SteadyMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

bool Init(const std::string& filePath, LogLevel level, uint64_t maxBytes) {
    SetLevel(level);
    std::lock_guard<std::mutex> lock(gMutex);
    if (gFile.is_open()) gFile.close();
    gFilePath.clear();
    if (filePath.empty()) return true;

    std::error_code ec;
    auto path = PathFromUtf8(filePath);
    if (std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) > maxBytes) {
        auto previous = path;
        previous += ".1";
        std::filesystem::remove(previous, ec);
        std::filesystem::rename(path, previous, ec);
    }
    gFile.open(path, std::ios::app);
    if (!gFile.is_open()) return false;
    gFilePath = filePath;
    gFile << "\n";
    return true;
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(gMutex);
    if (gFile.is_open()) gFile.close();
    gFilePath.clear();
}

void SetLevel(LogLevel level) { detail::gLevel.store(static_cast<int>(level)); }
LogLevel GetLevel() { return static_cast<LogLevel>(detail::gLevel.load()); }

void SetConsoleEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(gMutex);
    gConsole = enabled;
}

std::string FilePath() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gFilePath;
}

const char* LevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Trace:    return "TRACE";
        case LogLevel::Debug:    return "DEBUG";
        case LogLevel::Info:     return "INFO";
        case LogLevel::Warning:  return "WARN";
        case LogLevel::Error:    return "ERROR";
        case LogLevel::Critical: return "CRIT";
        default:                 return "OFF";
    }
}

bool ParseLevel(const std::string& text, LogLevel& out) {
    static const struct { const char* name; LogLevel level; } names[] = {
        {"trace", LogLevel::Trace}, {"debug", LogLevel::Debug}, {"info", LogLevel::Info},
        {"warning", LogLevel::Warning}, {"warn", LogLevel::Warning}, {"error", LogLevel::Error},
        {"critical", LogLevel::Critical}, {"off", LogLevel::Off},
    };
    for (const auto& n : names) {
        if (text == n.name) { out = n.level; return true; }
    }
    return false;
}

void Write(LogLevel level, const char* component, const std::string& message) {
    if (!Enabled(level)) return;
    char prefix[96];
    std::snprintf(prefix, sizeof(prefix), "%s %-5s [%s] ", Timestamp().c_str(), LevelName(level),
                  component ? component : "?");

    std::lock_guard<std::mutex> lock(gMutex);
    if (gConsole) {
        // Console: without the date (the console window is short-lived)
        std::FILE* stream = level >= LogLevel::Error ? stderr : stdout;
        std::fprintf(stream, "%s%s\n", prefix + 11, message.c_str());
        std::fflush(stream);
    }
    if (gFile.is_open()) {
        gFile << prefix << message << '\n';
        gFile.flush();   // the line must survive a crash right after it
    }
}

void Writef(LogLevel level, const char* component, const char* format, ...) {
    if (!Enabled(level)) return;
    char stackBuf[512];
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    int n = std::vsnprintf(stackBuf, sizeof(stackBuf), format, args);
    va_end(args);
    if (n < 0) {
        va_end(copy);
        return;
    }
    if (static_cast<size_t>(n) < sizeof(stackBuf)) {
        va_end(copy);
        Write(level, component, std::string(stackBuf, static_cast<size_t>(n)));
        return;
    }
    std::vector<char> big(static_cast<size_t>(n) + 1);
    std::vsnprintf(big.data(), big.size(), format, copy);
    va_end(copy);
    Write(level, component, std::string(big.data(), static_cast<size_t>(n)));
}

bool ShouldLogNow(std::atomic<int64_t>& lastMs, int64_t intervalMs) {
    int64_t now = SteadyMs();
    int64_t last = lastMs.load(std::memory_order_relaxed);
    if (last != 0 && now - last < intervalMs) return false;
    return lastMs.compare_exchange_strong(last, now);
}

} // namespace Log
