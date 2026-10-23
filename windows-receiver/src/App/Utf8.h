#pragma once
// =============================================================================
// Utf8.h — UTF-16 (Windows APIs) ↔ UTF-8 (logs, protocol) conversion
// =============================================================================

#include <Windows.h>
#include <string>

inline std::string WideToUtf8(const wchar_t* wide) {
    if (!wide || !*wide) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string out(static_cast<size_t>(len) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), len, nullptr, nullptr);
    return out;
}

inline std::string WideToUtf8(const std::wstring& wide) { return WideToUtf8(wide.c_str()); }

inline std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (len <= 1) return {};
    std::wstring out(static_cast<size_t>(len) - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, out.data(), len);
    return out;
}
