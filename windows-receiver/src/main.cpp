// =============================================================================
// main.cpp — External Display Receiver Entry Point
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
//   F2   — Toggle stats overlay
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
#include <deque>

// Console helpers
static FILE* gConsoleFile = nullptr;
static void InitConsole() {
    if (AllocConsole()) {
        freopen_s(&gConsoleFile, "CONOUT$", "w", stdout);
        freopen_s(&gConsoleFile, "CONOUT$", "w", stderr);
        SetConsoleTitleW(L"External Display Receiver - Stats");
    }
}
static void CleanupConsole() {
    if (gConsoleFile) { fclose(gConsoleFile); gConsoleFile = nullptr; }
    FreeConsole();
}

// ── NV12 Frame (thread-safe, latest-frame-wins) ─────────────────────────────

struct NV12Frame {
    std::vector<uint8_t> yPlane;
    std::vector<uint8_t> uvPlane;
    int stride;
    uint32_t width;
    uint32_t height;
};

static std::mutex gNV12Mutex;
static NV12Frame gLatestNV12;
static bool gHasNewNV12 = false;

static void QueueNV12Frame(const uint8_t* yData, const uint8_t* uvData,
                            int stride, uint32_t width, uint32_t height) {
    uint32_t evenH = (height + 1) & ~1;
    size_t ySize  = (size_t)stride * evenH;
    size_t uvSize = (size_t)stride * (evenH / 2);

    std::lock_guard<std::mutex> lock(gNV12Mutex);
    if (gLatestNV12.yPlane.size() != ySize)   gLatestNV12.yPlane.resize(ySize);
    if (gLatestNV12.uvPlane.size() != uvSize) gLatestNV12.uvPlane.resize(uvSize);

    memcpy(gLatestNV12.yPlane.data(), yData, ySize);
    memcpy(gLatestNV12.uvPlane.data(), uvData, uvSize);
    gLatestNV12.stride = stride;
    gLatestNV12.width = width;
    gLatestNV12.height = height;
    gHasNewNV12 = true;
}

// ── H.264 Input Queue (main-thread decode with frame dropping) ──────────────

struct H264InputFrame {
    std::vector<uint8_t> data;
    bool isKeyframe;
};

static std::mutex gInputMutex;
static std::deque<H264InputFrame> gInputQueue;
static const size_t MAX_INPUT_QUEUE = 4;

static void QueueH264Frame(const uint8_t* data, size_t len, bool keyframe) {
    std::lock_guard<std::mutex> lock(gInputMutex);
    if (gInputQueue.size() >= MAX_INPUT_QUEUE) {
        int lastKey = -1;
        for (int i = (int)gInputQueue.size() - 1; i >= 0; i--) {
            if (gInputQueue[i].isKeyframe) { lastKey = i; break; }
        }
        if (lastKey > 0) {
            gInputQueue.erase(gInputQueue.begin(), gInputQueue.begin() + lastKey);
        } else if (gInputQueue.size() >= MAX_INPUT_QUEUE * 2) {
            gInputQueue.erase(gInputQueue.begin(), gInputQueue.end() - MAX_INPUT_QUEUE);
        }
    }
    gInputQueue.push_back({std::vector<uint8_t>(data, data + len), keyframe});
}

// =============================================================================

int WINAPI WinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE,
                   _In_ LPSTR lpCmdLine, _In_ int) {
    InitConsole();

    std::string macIP;
    if (lpCmdLine && strlen(lpCmdLine) > 0) {
        macIP = lpCmdLine;
        while (!macIP.empty() && macIP.back() == ' ') macIP.pop_back();
        while (!macIP.empty() && macIP.front() == ' ') macIP.erase(macIP.begin());
    }
    bool networkMode = !macIP.empty();

    try {
        if (networkMode) {
            printf("=== External Display Receiver — Streaming Mode ===\n");
            printf("Connecting to Mac sender at: %s\n\n", macIP.c_str());
        } else {
            printf("=== External Display Receiver — Test Pattern Mode ===\n");
            printf("Tip: Pass Mac IP as argument to connect\n\n");
        }

        Window window(L"External Display Receiver", true);
        printf("Window: %d x %d (%s)\n",
            window.GetWidth(), window.GetHeight(),
            window.IsFullscreen() ? "Fullscreen" : "Windowed");

        D3D11Renderer renderer(
            window.GetHandle(), window.GetWidth(), window.GetHeight());
        wprintf(L"GPU:    %s\n", renderer.GetAdapterName().c_str());
        printf("VSync:  Enabled (60 Hz target)\n\n");

        std::unique_ptr<StreamReceiver> receiver;
        std::unique_ptr<H264Decoder> decoder;

        if (networkMode) {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            decoder = std::make_unique<H264Decoder>();
            receiver = std::make_unique<StreamReceiver>();

            receiver->setStreamInfoCallback(
                [&decoder, &window](uint32_t w, uint32_t h, uint32_t fps) {
                    printf("[Main] Stream: %ux%u @ %u FPS\n", w, h, fps);
                    decoder->initialize(w, h);
                    // Lock window aspect ratio to match video
                    float aspect = (float)w / (float)h;
                    window.SetVideoAspectRatio(aspect);
                    printf("[Main] Aspect ratio locked: %.4f\n", aspect);
                });

            // Network thread: queue H.264 frames (don't decode here)
            receiver->setFrameCallback(
                [](const uint8_t* data, size_t len, bool key) {
                    QueueH264Frame(data, len, key);
                });

            // Decoder: output raw NV12 Y+UV planes for GPU upload
            decoder->setRawNV12Callback(
                [](const uint8_t* yData, const uint8_t* uvData,
                   int stride, uint32_t w, uint32_t h) {
                    QueueNV12Frame(yData, uvData, stride, w, h);
                });

            if (!receiver->connect(macIP))
                fprintf(stderr, "Failed to connect to %s\n", macIP.c_str());
        }

        // ── Main loop ───────────────────────────────────────────────────────
        FrameStats stats(60.0f);
        auto startTime = std::chrono::high_resolution_clock::now();
        auto lastStatsPrint = startTime;
        uint64_t frameCounter = 0;

        while (window.ProcessMessages()) {
            if (window.WasResized()) {
                renderer.Resize(window.GetWidth(), window.GetHeight());
                window.ClearResizedFlag();
            }

            // Decode H.264 on main thread (up to 2 per vsync)
            if (networkMode && decoder) {
                std::lock_guard<std::mutex> lock(gInputMutex);
                int decoded = 0;
                while (!gInputQueue.empty() && decoded < 2) {
                    auto& frame = gInputQueue.front();
                    decoder->decode(frame.data.data(), frame.data.size());
                    gInputQueue.pop_front();
                    decoded++;
                }
            }

            // Upload latest NV12 frame to GPU
            if (networkMode) {
                std::lock_guard<std::mutex> lock(gNV12Mutex);
                if (gHasNewNV12) {
                    renderer.UpdateFrameNV12(
                        gLatestNV12.yPlane.data(),
                        gLatestNV12.uvPlane.data(),
                        gLatestNV12.stride,
                        gLatestNV12.width,
                        gLatestNV12.height);
                    gHasNewNV12 = false;
                }
            }

            // Render
            auto now = std::chrono::high_resolution_clock::now();
            float elapsed = std::chrono::duration<float>(now - startTime).count();
            stats.BeginFrame();
            renderer.Render(elapsed, (float)frameCounter, window.ShowStats() ? &stats : nullptr);
            stats.EndFrame();
            frameCounter++;

            // Stats every 2 seconds
            float sincePrint = std::chrono::duration<float>(now - lastStatsPrint).count();
            if (sincePrint >= 2.0f) {
                wprintf(L"%s", stats.FormatStats().c_str());
                if (networkMode && receiver) {
                    auto ns = receiver->getStats();
                    auto state = receiver->getState();
                    const char* stateStr = "?";
                    switch (state) {
                        case StreamReceiver::State::Disconnected: stateStr = "Disconnected"; break;
                        case StreamReceiver::State::Connecting:   stateStr = "Connecting"; break;
                        case StreamReceiver::State::Connected:    stateStr = "Connected"; break;
                        case StreamReceiver::State::Streaming:    stateStr = "Streaming"; break;
                        case StreamReceiver::State::Reconnecting: stateStr = "Reconnecting"; break;
                        case StreamReceiver::State::Error:        stateStr = "Error"; break;
                    }
                    printf("  Net: %s | Recv: %llu bytes, %llu pkts, %llu frames (%llu dropped)\n",
                        stateStr, ns.bytesReceived, ns.packetsReceived,
                        ns.framesReceived, ns.framesDropped);
                    if (decoder) {
                        auto ds = decoder->getStats();
                        size_t q;
                        { std::lock_guard<std::mutex> lk(gInputMutex); q = gInputQueue.size(); }
                        printf("  Decoder: %llu frames | Latency: %.1f ms | Queue: %zu",
                            ds.framesDecoded, ds.avgDecodeLatencyMs, q);
                    }
                }
                printf("\n\n");
                lastStatsPrint = now;
            }
        }

        if (receiver) receiver->disconnect();
        if (decoder) decoder->shutdown();
        wprintf(L"\nFinal: %s\n", stats.FormatStats().c_str());
        if (networkMode) CoUninitialize();

    } catch (const std::exception& e) {
        fprintf(stderr, "FATAL: %s\n", e.what());
        int len = MultiByteToWideChar(CP_UTF8, 0, e.what(), -1, nullptr, 0);
        std::wstring wMsg((size_t)len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, e.what(), -1, wMsg.data(), len);
        MessageBoxW(nullptr, wMsg.c_str(), L"Error", MB_ICONERROR | MB_OK);
    }

    CleanupConsole();
    return 0;
}
