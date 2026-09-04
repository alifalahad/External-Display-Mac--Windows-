// =============================================================================
// main.cpp — External Display Receiver Entry Point (Phase 4+5+6)
// =============================================================================
// Creates a fullscreen borderless D3D11 window and either:
//   - Shows test pattern (no args)
//   - Connects to Mac sender and displays live video (with IP arg)
//
// Usage:
//   ExternalDisplayReceiver.exe              → test pattern mode
//   ExternalDisplayReceiver.exe 192.168.1.5  → connect to Mac sender
//
// Controls:
//   ESC  — Exit
//   F11  — Toggle fullscreen / windowed
// =============================================================================

#include "App/Window.h"
#include "Renderer/D3D11Renderer.h"
#include "Performance/FrameStats.h"
#include "Network/StreamReceiver.h"
#include "Decoder/H264Decoder.h"

#include <Windows.h>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <mutex>
#include <queue>

// Console helpers for debug output
static FILE* gConsoleFile = nullptr;

static void InitConsole() {
    if (AllocConsole()) {
        freopen_s(&gConsoleFile, "CONOUT$", "w", stdout);
        freopen_s(&gConsoleFile, "CONOUT$", "w", stderr);

        SetConsoleTitleW(L"External Display Receiver - Stats");

        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        SMALL_RECT rect = { 0, 0, 99, 30 };
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

// ── Decoded Frame Queue (thread-safe) ───────────────────────────────────────

struct DecodedFrame {
    std::vector<uint8_t> bgraData;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
};

static std::mutex gFrameQueueMutex;
static std::queue<DecodedFrame> gFrameQueue;
static const size_t MAX_QUEUED_FRAMES = 3;  // Keep queue small for low latency

static void QueueDecodedFrame(const uint8_t* data, uint32_t w, uint32_t h, uint32_t stride) {
    std::lock_guard<std::mutex> lock(gFrameQueueMutex);

    // Drop oldest frames if queue is full (fresh > old)
    while (gFrameQueue.size() >= MAX_QUEUED_FRAMES) {
        gFrameQueue.pop();
    }

    DecodedFrame frame;
    frame.width = w;
    frame.height = h;
    frame.stride = stride;
    frame.bgraData.assign(data, data + h * stride);
    gFrameQueue.push(std::move(frame));
}

static bool DequeueFrame(DecodedFrame& frame) {
    std::lock_guard<std::mutex> lock(gFrameQueueMutex);
    if (gFrameQueue.empty()) return false;

    // Take the newest frame, drop all older ones (low latency)
    while (gFrameQueue.size() > 1) {
        gFrameQueue.pop();
    }
    frame = std::move(gFrameQueue.front());
    gFrameQueue.pop();
    return true;
}

// =============================================================================
// WinMain — Application entry point
// =============================================================================

int WINAPI WinMain(
    _In_ HINSTANCE /*hInstance*/,
    _In_opt_ HINSTANCE /*hPrevInstance*/,
    _In_ LPSTR lpCmdLine,
    _In_ int /*nShowCmd*/)
{
    // ── Initialize console for stats output ─────────────────────────────────
    InitConsole();

    // ── Parse command line for Mac IP address ───────────────────────────────
    std::string macIP;
    if (lpCmdLine && strlen(lpCmdLine) > 0) {
        macIP = lpCmdLine;
        // Trim whitespace
        while (!macIP.empty() && macIP.back() == ' ') macIP.pop_back();
        while (!macIP.empty() && macIP.front() == ' ') macIP.erase(macIP.begin());
    }

    bool networkMode = !macIP.empty();

    try {
        // ── Print startup banner ────────────────────────────────────────────
        wprintf(L"╔══════════════════════════════════════════════════╗\n");
        if (networkMode) {
            wprintf(L"║   External Display Receiver — Streaming Mode     ║\n");
        } else {
            wprintf(L"║   External Display Receiver — Test Pattern Mode   ║\n");
        }
        wprintf(L"╠══════════════════════════════════════════════════╣\n");
        wprintf(L"║   ESC   = Exit                                   ║\n");
        wprintf(L"║   F11   = Toggle Fullscreen / Windowed           ║\n");
        wprintf(L"╚══════════════════════════════════════════════════╝\n\n");

        if (networkMode) {
            printf("Connecting to Mac sender at: %s\n\n", macIP.c_str());
        } else {
            wprintf(L"Tip: Pass Mac IP as argument to connect:\n");
            wprintf(L"  ExternalDisplayReceiver.exe 192.168.1.x\n\n");
        }

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

        // ── Create decoder and network receiver (if streaming) ──────────────
        std::unique_ptr<StreamReceiver> receiver;
        std::unique_ptr<H264Decoder> decoder;

        if (networkMode) {
            // Initialize COM for Media Foundation
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);

            decoder = std::make_unique<H264Decoder>();
            receiver = std::make_unique<StreamReceiver>();

            // When stream info arrives, initialize the decoder
            receiver->setStreamInfoCallback([&decoder](uint32_t w, uint32_t h, uint32_t fps) {
                printf("[Main] Stream: %ux%u @ %u FPS — initializing decoder\n", w, h, fps);
                decoder->initialize(w, h);
            });

            // When H.264 frames arrive, feed to decoder
            receiver->setFrameCallback([&decoder](const uint8_t* data, size_t len, bool key) {
                if (decoder) {
                    decoder->decode(data, len);
                }
            });

            // When decoder produces BGRA frames, queue them for rendering
            decoder->setDecodedFrameCallback([](const uint8_t* data, uint32_t w, uint32_t h, uint32_t stride) {
                QueueDecodedFrame(data, w, h, stride);
            });

            // Connect to Mac sender
            if (!receiver->connect(macIP)) {
                fprintf(stderr, "Failed to connect to %s\n", macIP.c_str());
            }
        }

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

            // Dequeue decoded video frame (if any)
            if (networkMode) {
                DecodedFrame frame;
                if (DequeueFrame(frame)) {
                    renderer.UpdateFrame(
                        frame.bgraData.data(),
                        frame.width,
                        frame.height,
                        frame.stride
                    );
                }
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
                wprintf(L"%s", stats.FormatStats().c_str());

                // Print network + decoder stats
                if (networkMode && receiver) {
                    auto ns = receiver->getStats();
                    auto state = receiver->getState();

                    const char* stateStr = "Unknown";
                    switch (state) {
                        case StreamReceiver::State::Disconnected: stateStr = "Disconnected"; break;
                        case StreamReceiver::State::Connecting:   stateStr = "Connecting"; break;
                        case StreamReceiver::State::Connected:    stateStr = "Connected"; break;
                        case StreamReceiver::State::Streaming:    stateStr = "Streaming"; break;
                        case StreamReceiver::State::Error:        stateStr = "Error"; break;
                    }

                    printf("  Net: %s | Recv: %llu bytes, %llu pkts, %llu frames (%llu dropped)\n",
                        stateStr, ns.bytesReceived, ns.packetsReceived,
                        ns.framesReceived, ns.framesDropped);

                    if (decoder) {
                        auto ds = decoder->getStats();
                        printf("  Decoder: %llu frames | Latency: %.1f ms\n",
                            ds.framesDecoded, ds.avgDecodeLatencyMs);
                    }
                }

                wprintf(L"\n");
                lastStatsPrint = now;
            }
        }

        // ── Cleanup ────────────────────────────────────────────────────────
        if (receiver) receiver->disconnect();
        if (decoder) decoder->shutdown();

        // ── Print final stats ───────────────────────────────────────────────
        wprintf(L"\n════════════════════════════════════════════════════\n");
        wprintf(L"Final: %s\n", stats.FormatStats().c_str());
        wprintf(L"════════════════════════════════════════════════════\n");

        if (networkMode) {
            CoUninitialize();
        }

    } catch (const std::exception& e) {
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
