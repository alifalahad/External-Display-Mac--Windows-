// =============================================================================
// main.cpp — External Display Receiver entry point
// =============================================================================
//   ExternalDisplayReceiver.exe              find the Mac on the network
//   ExternalDisplayReceiver.exe 192.168.1.5  connect to that Mac directly
//   Options: see App/Options.h (--gpu, --renderer, --bench, --log, --verbose,
//            --console, --test-pattern)
//
// The window is the whole UI: a status card until the picture arrives
// (connecting, pairing, errors), then the Mac's display. Everything is also
// logged to receiver.log next to the .exe.
//
// Controls: Esc quit · F11 full screen · F2 diagnostics · F3 mouse & keyboard
// =============================================================================

#include "App/CrashHandler.h"
#include "App/Log.h"
#include "App/Options.h"
#include "App/ReceiverApp.h"
#include "App/Utf8.h"
#include "App/Window.h"
#include "Security/Crypto.h"

#include <Windows.h>
#include <cstdio>

namespace {

FILE* gConsoleOut = nullptr;
FILE* gConsoleErr = nullptr;

/// Optional console (--console): the log lines live, as before Phase 18
void OpenConsole() {
    if (!AllocConsole()) return;
    freopen_s(&gConsoleOut, "CONOUT$", "w", stdout);
    freopen_s(&gConsoleErr, "CONOUT$", "w", stderr);
    SetConsoleTitleW(L"External Display Receiver — log");
    SetConsoleOutputCP(CP_UTF8);   // log lines contain UTF-8 (names, arrows)

    // QuickEdit pauses ALL console output while text is selected — the
    // render loop would then block inside a log call and the display freeze
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(input, &mode)) {
        SetConsoleMode(input, (mode & ~ENABLE_QUICK_EDIT_MODE) | ENABLE_EXTENDED_FLAGS);
    }
}

void CloseConsole() {
    if (gConsoleOut) fclose(gConsoleOut);
    if (gConsoleErr) fclose(gConsoleErr);
    FreeConsole();
}

/// Directory of the .exe (logs, benchmarks and pairing data live next to it)
std::string ExeDirectoryUtf8() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path);
    size_t slash = dir.find_last_of(L"\\/");
    return WideToUtf8(slash == std::wstring::npos ? std::wstring() : dir.substr(0, slash + 1));
}

/// A fatal error must be readable. When the window has been destroyed it
/// posted WM_QUIT — and a pending WM_QUIT closes a MessageBox the moment it
/// opens (this hid the real error on some PCs). Remove it first.
void ShowFatalError(const std::string& message) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {}
    std::string text = message;
    std::string logPath = Log::FilePath();
    if (!logPath.empty()) text += "\n\nDetails: " + logPath;
    MessageBoxW(nullptr, Utf8ToWide(text).c_str(), L"External Display Receiver",
                MB_ICONERROR | MB_OK | MB_TOPMOST);
}

} // namespace

int WINAPI WinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPSTR lpCmdLine, _In_ int) {
    Options options = ParseOptions(lpCmdLine ? lpCmdLine : "");
    if (options.showConsole) OpenConsole();
    InstallCrashHandler();

    // Phase 19: everything is written to receiver.log next to the .exe
    Log::Init(ExeDirectoryUtf8() + "receiver.log", options.logLevel);
    Log::SetConsoleEnabled(options.showConsole);
    LOG_INFO("Main") << "External Display Receiver starting (protocol v2, "
                     << (sizeof(void*) == 8 ? "64-bit" : "32-bit") << " build, log level "
                     << Log::LevelName(options.logLevel) << ")";
    for (const auto& warning : options.warnings) LOG_WARN("Main") << warning;

    int exitCode = 0;
    try {
        // Pairing must not run on a platform whose crypto disagrees with the Mac
        std::string cryptoError;
        if (!options.testPattern && !crypto::SelfTest(cryptoError)) {
            throw std::runtime_error("Windows cryptography self-test failed: " + cryptoError);
        }

        Window window(L"External Display Receiver", true);
        LOG_INFO("Main") << "Window " << window.GetWidth() << "x" << window.GetHeight()
                         << (window.IsFullscreen() ? " (full screen)" : " (windowed)");
        ReceiverApp app(window, options);
        app.run();
    } catch (const std::exception& e) {
        LOG_CRITICAL("Main") << "Fatal: " << e.what();
        ShowFatalError(e.what());
        exitCode = 1;
    }

    LOG_INFO("Main") << "Exiting";
    Log::Shutdown();
    if (options.showConsole) CloseConsole();
    return exitCode;
}
