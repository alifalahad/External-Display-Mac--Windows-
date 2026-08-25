// =============================================================================
// main.cpp — External Display Receiver Entry Point (Phase 1)
// =============================================================================
// Creates a fullscreen borderless D3D11 window and renders an animated test
// pattern at 60 FPS with VSync. Reports frame statistics via on-screen overlay
// and console output.
//
// Controls:
//   ESC  — Exit
//   F11  — Toggle fullscreen / windowed
// =============================================================================

#include "App/Window.h"
#include "Renderer/D3D11Renderer.h"
#include "Performance/FrameStats.h"

#include <Windows.h>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>

// Console helpers for debug output
static FILE* gConsoleFile = nullptr;

static void InitConsole() {
    if (AllocConsole()) {
        freopen_s(&gConsoleFile, "CONOUT$", "w", stdout);
        freopen_s(&gConsoleFile, "CONOUT$", "w", stderr);

        // Set console title
        SetConsoleTitleW(L"External Display Receiver - Stats");

        // Reasonable console size
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        SMALL_RECT rect = { 0, 0, 89, 24 };
        SetConsoleWindowInfo(hOut, TRUE, &rect);
    }
}

static void CleanupConsole() {
    if (gConsoleFile) {
        fclose(gConsoleFile);
        gConsoleFile = nullptr;
    }
    FreeConsole();
}

// =============================================================================
// WinMain — Application entry point
// =============================================================================

int WINAPI WinMain(
    _In_ HINSTANCE /*hInstance*/,
    _In_opt_ HINSTANCE /*hPrevInstance*/,
    _In_ LPSTR /*lpCmdLine*/,
    _In_ int /*nShowCmd*/)
{
    // ── Initialize console for stats output ─────────────────────────────────
    InitConsole();

    try {
        // ── Print startup banner ────────────────────────────────────────────
        wprintf(L"╔══════════════════════════════════════════════════╗\n");
        wprintf(L"║   External Display Receiver — Phase 1            ║\n");
        wprintf(L"║   D3D11 Test Pattern Renderer                    ║\n");
        wprintf(L"╠══════════════════════════════════════════════════╣\n");
        wprintf(L"║   ESC   = Exit                                   ║\n");
        wprintf(L"║   F11   = Toggle Fullscreen / Windowed           ║\n");
        wprintf(L"╚══════════════════════════════════════════════════╝\n\n");

        // ── Create window (fullscreen borderless) ───────────────────────────
        Window window(L"External Display Receiver", true);
        wprintf(L"Window: %d x %d (%s)\n",
            window.GetWidth(), window.GetHeight(),
            window.IsFullscreen() ? L"Fullscreen" : L"Windowed");

        // ── Create D3D11 renderer ───────────────────────────────────────────
        D3D11Renderer renderer(
            window.GetHandle(),
            window.GetWidth(),
            window.GetHeight()
        );
        wprintf(L"GPU:    %s\n", renderer.GetAdapterName().c_str());
        wprintf(L"VSync:  Enabled (60 Hz target)\n\n");

        // ── Create frame stats tracker ──────────────────────────────────────
        FrameStats stats(60.0f);

        // ── Main loop ───────────────────────────────────────────────────────
        auto startTime = std::chrono::high_resolution_clock::now();
        auto lastStatsPrint = startTime;
        uint64_t frameCounter = 0;

        while (window.ProcessMessages()) {
            // Handle resize
            if (window.WasResized()) {
                renderer.Resize(window.GetWidth(), window.GetHeight());
                window.ClearResizedFlag();
                wprintf(L"\n[Resize] %d x %d\n\n",
                    window.GetWidth(), window.GetHeight());
            }

            // Calculate elapsed time
            auto now = std::chrono::high_resolution_clock::now();
            float elapsed = std::chrono::duration<float>(now - startTime).count();

            // Render frame
            stats.BeginFrame();
            renderer.Render(elapsed, static_cast<float>(frameCounter), &stats);
            stats.EndFrame();
            frameCounter++;

            // Print stats to console every 2 seconds
            float sincePrint = std::chrono::duration<float>(now - lastStatsPrint).count();
            if (sincePrint >= 2.0f) {
                wprintf(L"%s\n", stats.FormatStats().c_str());
                lastStatsPrint = now;
            }
        }

        // ── Print final stats ───────────────────────────────────────────────
        wprintf(L"\n════════════════════════════════════════════════════\n");
        wprintf(L"Final: %s\n", stats.FormatStats().c_str());
        wprintf(L"════════════════════════════════════════════════════\n");

    } catch (const std::exception& e) {
        // Show error as both console output and message box
        fprintf(stderr, "FATAL ERROR: %s\n", e.what());

        int len = MultiByteToWideChar(CP_UTF8, 0, e.what(), -1, nullptr, 0);
        std::wstring wideMsg(static_cast<size_t>(len), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, e.what(), -1, wideMsg.data(), len);

        MessageBoxW(nullptr, wideMsg.c_str(),
            L"External Display Receiver - Error", MB_ICONERROR | MB_OK);
    }

    CleanupConsole();
    return 0;
}
