// =============================================================================
// main.cpp — External Display Receiver Entry Point
// =============================================================================
// Creates a fullscreen borderless D3D11 window and either:
//   - Auto-discovers Mac sender via mDNS (no args)
//   - Connects to Mac sender at given IP (with IP arg)
//
// Usage:
//   ExternalDisplayReceiver.exe              → auto-discover Mac sender
//   ExternalDisplayReceiver.exe 192.168.1.5  → connect to Mac sender directly
//   --gpu=high | --gpu=low | --gpu=default   → which GPU renders (hybrid laptops)
//   --bench=SECONDS                          → record a benchmark, write
//                                              bench-*.csv / -summary.txt, exit
//
// Controls:
//   ESC  — Exit
//   F11  — Toggle fullscreen / windowed
//   F2   — Toggle diagnostics overlay
// =============================================================================

#include "App/Window.h"
#include "Renderer/D3D11Renderer.h"
#include "Performance/PipelineMetrics.h"
#include "Performance/PipelineClock.h"
#include "Performance/SystemMonitor.h"
#include "Network/StreamReceiver.h"
#include "Decoder/H264Decoder.h"
#include "Input/InputCapture.h"
#include "Clipboard/ClipboardSync.h"
#include "Discovery/ServiceDiscovery.h"
#include "Security/PairingManager.h"
#include "UI/PinPrompt.h"
#include "App/CrashHandler.h"

#include <Windows.h>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <mutex>
#include <deque>
#include <sstream>
#include <ctime>

// Console helpers
static FILE* gConsoleFile = nullptr;
static FILE* gConsoleInput = nullptr;
static void InitConsole() {
    if (AllocConsole()) {
        freopen_s(&gConsoleFile, "CONOUT$", "w", stdout);
        freopen_s(&gConsoleFile, "CONOUT$", "w", stderr);
        freopen_s(&gConsoleInput, "CONIN$", "r", stdin);  // Console input for sender selection
        SetConsoleTitleW(L"External Display Receiver - Stats");
        SetConsoleOutputCP(CP_UTF8);  // log lines contain UTF-8 (names, arrows)

        // QuickEdit pauses ALL console output while text is selected — the
        // render loop then blocks inside printf and the display freezes.
        HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode = 0;
        if (GetConsoleMode(input, &mode)) {
            SetConsoleMode(input, (mode & ~ENABLE_QUICK_EDIT_MODE) | ENABLE_EXTENDED_FLAGS);
        }
    }
}

/// Phase 15: find the Mac again via mDNS (its IP may have changed).
/// Prefers the service we connected to before; returns "" if nothing answers.
static std::string RediscoverMac(const std::string& serviceName) {
    ServiceDiscovery discovery;
    std::mutex foundMutex;
    std::string foundIP;
    discovery.setDiscoveryCallback(
        [&](const DiscoveredService& svc) {
            if (svc.ipAddress.empty()) return;
            if (!serviceName.empty() && svc.name != serviceName) return;
            std::lock_guard<std::mutex> lock(foundMutex);
            foundIP = svc.ipAddress;
        });
    discovery.startBrowsing();
    for (int i = 0; i < 50; i++) {  // up to 5 s
        Sleep(100);
        std::lock_guard<std::mutex> lock(foundMutex);
        if (!foundIP.empty()) break;
    }
    discovery.stopBrowsing();
    std::lock_guard<std::mutex> lock(foundMutex);
    return foundIP;
}
static void CleanupConsole() {
    if (gConsoleFile) { fclose(gConsoleFile); gConsoleFile = nullptr; }
    if (gConsoleInput) { fclose(gConsoleInput); gConsoleInput = nullptr; }
    FreeConsole();
}

// ── Newest decoded picture (main thread only) ───────────────────────────────
// Decoder callbacks run on the main thread and hand each picture straight to
// the renderer (a GPU copy for hardware decoding, an upload for software).
// This remembers which frame that was until it has been presented.

struct PictureState {
    bool fresh = false;            // in the renderer, not presented yet
    FrameTimeline timeline;
    uint64_t overwritten = 0;      // replaced by a newer picture before presenting
};
static PictureState gPicture;

static void OnPictureDecoded(const FrameTimeline& timeline) {
    if (gPicture.fresh) gPicture.overwritten++;   // latest picture wins
    gPicture.timeline = timeline;
    gPicture.fresh = true;
}

// Wakes the main loop: a frame arrived or the stream changed (auto-reset)
static HANDLE gWakeEvent = nullptr;

// ── H.264 Input Queue (main-thread decode with frame dropping) ──────────────

struct H264InputFrame {
    std::vector<uint8_t> data;
    FrameTimeline timeline;
};

static std::mutex gInputMutex;
static std::deque<H264InputFrame> gInputQueue;
static uint64_t gInputDropped = 0;   // trimmed from the queue (guarded by gInputMutex)
static const size_t MAX_INPUT_QUEUE = 4;

static void QueueH264Frame(const uint8_t* data, size_t len, const FrameTimeline& timeline) {
    std::lock_guard<std::mutex> lock(gInputMutex);
    if (gInputQueue.size() >= MAX_INPUT_QUEUE) {
        int lastKey = -1;
        for (int i = (int)gInputQueue.size() - 1; i >= 0; i--) {
            if (gInputQueue[i].timeline.keyframe) { lastKey = i; break; }
        }
        if (lastKey > 0) {
            gInputQueue.erase(gInputQueue.begin(), gInputQueue.begin() + lastKey);
            gInputDropped += (uint64_t)lastKey;
        } else if (gInputQueue.size() >= MAX_INPUT_QUEUE * 2) {
            gInputDropped += gInputQueue.size() - MAX_INPUT_QUEUE;
            gInputQueue.erase(gInputQueue.begin(), gInputQueue.end() - MAX_INPUT_QUEUE);
        }
    }
    gInputQueue.push_back({std::vector<uint8_t>(data, data + len), timeline});
    if (gWakeEvent) SetEvent(gWakeEvent);
}

// ── Decode bookkeeping (main thread only) ───────────────────────────────────
// The decoder hands back the sample time we gave it; it identifies which input
// frame a decoded picture belongs to.

static constexpr int64_t kSampleTimeStep = 166667;   // 100 ns units, 1/60 s
static std::deque<std::pair<int64_t, FrameTimeline>> gDecoding;   // sample time → timeline
static int64_t gNextSampleTime = 0;

static FrameTimeline TakeDecodingTimeline(int64_t sampleTime) {
    // Exact match; older inputs that produced no picture are discarded
    for (size_t i = 0; i < gDecoding.size(); i++) {
        if (gDecoding[i].first == sampleTime) {
            FrameTimeline t = gDecoding[i].second;
            gDecoding.erase(gDecoding.begin(), gDecoding.begin() + (std::ptrdiff_t)i + 1);
            return t;
        }
    }
    // Time not passed through: no B-frames, so pictures come out in input order
    if (gDecoding.empty()) return FrameTimeline{};
    FrameTimeline t = gDecoding.front().second;
    gDecoding.pop_front();
    return t;
}

// ── Presented frames waiting for scan-out (main thread only) ────────────────

struct PendingDisplay {
    uint32_t presentCount;
    FrameTimeline timeline;
};

static std::string BenchmarkBasePath() {
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char name[64];
    std::strftime(name, sizeof(name), "bench-%Y%m%d-%H%M%S", &local);
    return name;
}

// ── Pending stream restart (set by network thread, handled by main thread) ──

static std::mutex gStreamInfoMutex;
static bool gPendingStreamRestart = false;
static uint32_t gPendingWidth = 0, gPendingHeight = 0, gPendingFPS = 0;

// =============================================================================

int WINAPI WinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE,
                   _In_ LPSTR lpCmdLine, _In_ int) {
    InitConsole();
    InstallCrashHandler();

    // Arguments: [mac-ip] [--gpu=high|low|default] [--bench=SECONDS]
    std::string macIP;
    std::string macServiceName;  // mDNS name, used to find the Mac again if its IP changes
    GpuPreference gpuPreference = GpuPreference::Default;
    int benchSeconds = 0;
    {
        std::istringstream args(lpCmdLine ? lpCmdLine : "");
        std::string arg;
        while (args >> arg) {
            if (arg == "--gpu=high")         gpuPreference = GpuPreference::HighPerformance;
            else if (arg == "--gpu=low")     gpuPreference = GpuPreference::MinimumPower;
            else if (arg == "--gpu=default") gpuPreference = GpuPreference::Default;
            else if (arg.rfind("--bench=", 0) == 0) {
                benchSeconds = atoi(arg.c_str() + 8);
                if (benchSeconds <= 0) {
                    printf("--bench needs a duration in seconds, e.g. --bench=60\n");
                    benchSeconds = 0;
                }
            }
            else if (arg.rfind("--", 0) == 0) printf("Ignoring unknown option: %s\n", arg.c_str());
            else macIP = arg;
        }
    }

    // ── Auto-Discovery Mode ─────────────────────────────────────────────
    // If no IP is provided, discover Mac senders on the LAN via mDNS
    if (macIP.empty()) {
        printf("=== External Display Receiver — Auto-Discovery ===\n");
        printf("Searching for Mac senders on the local network...\n\n");

        // Initialize Winsock for discovery
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);

        ServiceDiscovery discovery;
        std::mutex discoveryMutex;
        std::vector<DiscoveredService> foundServices;

        discovery.setDiscoveryCallback(
            [&discoveryMutex, &foundServices](const DiscoveredService& svc) {
                std::lock_guard<std::mutex> lock(discoveryMutex);
                // Update or add
                bool found = false;
                for (auto& existing : foundServices) {
                    if (existing.name == svc.name) {
                        existing = svc;
                        found = true;
                        break;
                    }
                }
                if (!found) foundServices.push_back(svc);
            });

        discovery.startBrowsing();

        // Wait up to 10 seconds for services to appear, checking every second
        printf("Waiting for Mac senders to respond...\n");
        for (int i = 0; i < 10; i++) {
            Sleep(1000);
            {
                std::lock_guard<std::mutex> lock(discoveryMutex);
                if (!foundServices.empty()) {
                    printf("Found %zu sender(s), waiting for more...\n", foundServices.size());
                    fflush(stdout);
                    break;
                }
            }
            printf("  Searching... (%d/10s)\n", i + 1);
        }

        // Wait 2 more seconds for additional senders (outside mutex!)
        Sleep(2000);

        discovery.stopBrowsing();

        {
            std::lock_guard<std::mutex> lock(discoveryMutex);
            if (foundServices.empty()) {
                printf("\nNo Mac senders found on the network.\n");
                printf("Make sure:\n");
                printf("  1. The Mac sender app is running and capture is started\n");
                printf("  2. Both devices are on the same Wi-Fi network\n");
                printf("  3. Firewall is not blocking mDNS (port 5353)\n");
                printf("\nYou can also pass the Mac's IP address directly:\n");
                printf("  ExternalDisplayReceiver.exe 192.168.1.x\n\n");
                printf("Press Enter to exit...\n");
                getchar();
                WSACleanup();
                CleanupConsole();
                return 0;
            }

            if (foundServices.size() == 1) {
                // Auto-select the only sender
                macIP = foundServices[0].ipAddress;
                macServiceName = foundServices[0].name;
                printf("\nAuto-connecting to: %s (%s)\n\n",
                    foundServices[0].name.c_str(), macIP.c_str());
            } else {
                // Multiple senders — let user choose
                printf("\n═══════════════════════════════════════════════════\n");
                printf("  Discovered Mac Senders:\n");
                printf("═══════════════════════════════════════════════════\n");
                for (size_t i = 0; i < foundServices.size(); i++) {
                    printf("  [%zu] %s\n", i + 1, foundServices[i].name.c_str());
                    printf("      IP: %s  Port: %u\n",
                        foundServices[i].ipAddress.c_str(),
                        foundServices[i].port);
                }
                printf("═══════════════════════════════════════════════════\n");
                printf("\nSelect sender (1-%zu): ", foundServices.size());

                int choice = 0;
                if (scanf_s("%d", &choice) != 1 || choice < 1 || choice > (int)foundServices.size()) {
                    printf("Invalid selection. Using first sender.\n");
                    choice = 1;
                }
                macIP = foundServices[choice - 1].ipAddress;
                macServiceName = foundServices[choice - 1].name;
                printf("\nConnecting to: %s (%s)\n\n",
                    foundServices[choice - 1].name.c_str(), macIP.c_str());
            }
        }

        WSACleanup(); // StreamReceiver will re-init Winsock
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

        printf("GPUs:\n");
        D3D11Renderer::LogAdapters();
        auto renderer = std::make_unique<D3D11Renderer>(
            window.GetHandle(), window.GetWidth(), window.GetHeight(), gpuPreference);
        wprintf(L"GPU:    %ls (rendering)\n", renderer->GetAdapterName().c_str());
        printf("VSync:  on, renders only when something changed\n\n");
        gWakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        // Declared before the receiver so it outlives the receiver's network thread
        std::unique_ptr<PairingManager> pairingMgr;
        std::unique_ptr<StreamReceiver> receiver;
        std::unique_ptr<H264Decoder> decoder;
        std::unique_ptr<InputCapture> inputCapture;
        std::unique_ptr<ClipboardSync> clipboardSync;

        if (networkMode) {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            decoder = std::make_unique<H264Decoder>();
            receiver = std::make_unique<StreamReceiver>();

            // Queue stream info change — main thread will reinitialize decoder
            receiver->setStreamInfoCallback(
                [&window](uint32_t w, uint32_t h, uint32_t fps) {
                    std::lock_guard<std::mutex> lock(gStreamInfoMutex);
                    gPendingWidth = w;
                    gPendingHeight = h;
                    gPendingFPS = fps;
                    gPendingStreamRestart = true;
                    printf("[Net] Stream info queued: %ux%u @ %u FPS\n", w, h, fps);
                    if (gWakeEvent) SetEvent(gWakeEvent);
                });

            // Network thread: queue H.264 frames (don't decode here)
            receiver->setFrameCallback(
                [](const uint8_t* data, size_t len, const FrameTimeline& timeline) {
                    QueueH264Frame(data, len, timeline);
                });

            // Decoder output (main thread) goes straight to the renderer.
            // Hardware: GPU→GPU copy of the decoded surface.
            decoder->setTextureCallback(
                [&renderer](ID3D11Texture2D* texture, UINT subresource,
                            uint32_t w, uint32_t h, int64_t sampleTime) {
                    FrameTimeline timeline = TakeDecodingTimeline(sampleTime);
                    timeline.decodeEndUs = PipelineClock::NowUs();
                    if (renderer->UpdateFrameTexture(texture, subresource, w, h)) {
                        OnPictureDecoded(timeline);
                    }
                });
            // Software fallback: upload the NV12 planes from system memory
            decoder->setRawNV12Callback(
                [&renderer](const uint8_t* yData, const uint8_t* uvData,
                            int stride, uint32_t w, uint32_t h, int64_t sampleTime) {
                    FrameTimeline timeline = TakeDecodingTimeline(sampleTime);
                    timeline.decodeEndUs = PipelineClock::NowUs();
                    renderer->UpdateFrameNV12(yData, uvData, stride, w, h);
                    OnPictureDecoded(timeline);
                });

            // Set up pairing manager for secure authentication.
            // Must live as long as the receiver — the network thread uses it.
            pairingMgr = std::make_unique<PairingManager>();
            pairingMgr->initialize();
            receiver->setPairingManager(pairingMgr.get());
            receiver->setHostResolver([macServiceName] { return RediscoverMac(macServiceName); });

            if (!receiver->connect(macIP))
                fprintf(stderr, "Failed to connect to %s\n", macIP.c_str());

            // Set up input capture (must outlive the main loop — declared above)
            inputCapture = std::make_unique<InputCapture>();
            inputCapture->setSendCallback(
                [&receiver](const exdp::InputEventPayload& evt) {
                    if (receiver) receiver->sendInputEvent(evt);
                });
            window.SetInputCapture(inputCapture.get());

            // Set up clipboard sync
            clipboardSync = std::make_unique<ClipboardSync>();
            // Mac → Windows: clipboard data from network → set on Windows clipboard
            receiver->setClipboardCallback(
                [&clipboardSync](const std::string& text) {
                    if (clipboardSync) clipboardSync->receiveRemoteClipboard(text);
                });
            // Windows → Mac: local clipboard changes → send over network
            clipboardSync->setClipboardCallback(
                [&receiver](const std::string& text) {
                    if (receiver) receiver->sendClipboardData(text);
                });
            clipboardSync->start(GetModuleHandle(nullptr));

            printf("Controls: ESC=quit, F11=fullscreen, F2=stats, F3=toggle input\n\n");
        }

        // ── Performance instrumentation (Phase 16) ──────────────────────────
        SystemMonitor systemMonitor;
        PipelineMetrics metrics;
        BenchmarkRecorder bench;
        std::deque<PendingDisplay> pendingDisplay;
        bool frameStatsUsable = true;       // DXGI frame statistics match our presents
        bool frameStatsSeen = false;        // at least one present matched to its vblank
        int64_t lastFrameStatsOkUs = 0;
        uint64_t presentsTracked = 0;
        int64_t lastTickUs = PipelineClock::NowUs();
        int64_t lastOverlayUs = 0;
        int64_t lastConsoleUs = lastTickUs;
        uint64_t skippedReported = 0;
        std::wstring overlayText;
        uint32_t streamWidth = 0, streamHeight = 0;
        if (benchSeconds > 0) {
            printf("Benchmark: %d s, starts with the first video frame%s\n\n", benchSeconds,
                   networkMode ? "" : " (needs a Mac connection)");
        }

        auto finishFrame = [&](const FrameTimeline& t) {
            StageTimes stages = metrics.onFrameDisplayed(t, receiver ? receiver->clockEstimate()
                                                                     : ClockSync::Estimate{});
            if (benchSeconds > 0 && !bench.active() && bench.basePath().empty()) {
                std::string base = BenchmarkBasePath();
                if (bench.start(base, benchSeconds, PipelineClock::NowUs())) {
                    printf("[Bench] Recording %d s to %s-*.csv\n", benchSeconds, base.c_str());
                    bench.addNote(decoder && decoder->isHardwareAccelerated()
                        ? "Decoder: hardware (D3D11 video engine)" : "Decoder: software (CPU)");
                } else {
                    printf("[Bench] Cannot create %s-*.csv — benchmark disabled\n", base.c_str());
                    benchSeconds = 0;
                }
            }
            bench.recordFrame(t, stages);
        };

        // Decoder (re)start on the renderer's current device: hardware decoding
        // when the GPU allows it. The decoder can only start from a keyframe.
        auto restartDecoder = [&](uint32_t w, uint32_t h) {
            decoder->shutdown();
            gDecoding.clear();
            gPicture.fresh = false;
            decoder->setD3D11Device(renderer->GetVideoDevice());
            decoder->initialize(w, h);
            if (receiver) receiver->requestKeyframe();
        };

        // Decode everything that has arrived. Pictures go to the renderer as
        // they come out; only the newest is presented.
        int consecutiveDecodeFailures = 0;
        auto decodePending = [&]() {
            if (!decoder) return;
            std::deque<H264InputFrame> frames;
            {
                std::lock_guard<std::mutex> lock(gInputMutex);
                frames.swap(gInputQueue);   // don't hold the lock while decoding
            }
            for (auto& frame : frames) {
                const int64_t sampleTime = gNextSampleTime;
                gNextSampleTime += kSampleTimeStep;
                frame.timeline.decodeStartUs = PipelineClock::NowUs();
                gDecoding.emplace_back(sampleTime, frame.timeline);

                if (decoder->decode(frame.data.data(), frame.data.size(), sampleTime)) {
                    metrics.onFrameDecoded();
                    consecutiveDecodeFailures = 0;
                } else if (++consecutiveDecodeFailures >= 30) {
                    // Phase 15: a decoder that keeps failing is rebuilt
                    printf("[Decoder] %d consecutive failures — reinitializing decoder\n",
                           consecutiveDecodeFailures);
                    consecutiveDecodeFailures = 0;
                    restartDecoder(streamWidth ? streamWidth : 1920, streamHeight ? streamHeight : 1080);
                    break;
                }
            }
            // Pictures the decoder finished after the last input
            if (!gDecoding.empty()) decoder->drainOutput();

            // Inputs that never produced a picture (e.g. before the first
            // keyframe) must not keep the loop polling
            const int64_t staleUs = PipelineClock::NowUs() - 500'000;
            while (!gDecoding.empty() && gDecoding.front().second.decodeStartUs < staleUs) {
                gDecoding.pop_front();
            }
        };

        // ── Main loop ───────────────────────────────────────────────────────
        // Phase 17: event-driven. The loop sleeps until a frame arrives, a
        // window message comes in, or a timer (overlay, statistics) is due, and
        // renders only when the picture changed — a static screen costs almost
        // nothing. Before rendering it waits for the swap chain's frame slot, so
        // the newest picture goes out with at most one frame queued.
        auto startTime = std::chrono::high_resolution_clock::now();
        uint64_t frameCounter = 0;
        bool forceRender = true;
        bool lastShowStats = window.ShowStats();

        while (window.ProcessMessages()) {
            if (window.WasResized()) {
                renderer->Resize(window.GetWidth(), window.GetHeight());
                window.ClearResizedFlag();
                forceRender = true;
            }

            // Phase 15: GPU removed/reset (driver update, TDR, hybrid GPU switch)
            // or a failed resize — rebuild the renderer instead of crashing.
            if (renderer->IsDeviceLost()) {
                printf("[Renderer] Recreating renderer...\n");
                renderer.reset();  // release the old swap chain first
                for (int attempt = 1; !renderer; attempt++) {
                    try {
                        renderer = std::make_unique<D3D11Renderer>(
                            window.GetHandle(), window.GetWidth(), window.GetHeight(), gpuPreference);
                    } catch (const std::exception& e) {
                        if (attempt >= 10) throw;
                        printf("[Renderer] Recreate failed (%s), retrying...\n", e.what());
                        Sleep(500);
                    }
                }
                wprintf(L"[Renderer] Recovered on %ls\n", renderer->GetAdapterName().c_str());
                // The new swap chain numbers its presents from scratch
                for (const auto& p : pendingDisplay) finishFrame(p.timeline);
                pendingDisplay.clear();
                // A hardware decoder is bound to the old device
                if (decoder && streamWidth) restartDecoder(streamWidth, streamHeight);
                forceRender = true;
            }

            // Phase 15: laptop woke up — the old TCP connection is dead, don't
            // wait for the 15 s timeout
            if (window.TakeResumeEvent() && receiver) {
                printf("[Power] Resumed from sleep — reconnecting now\n");
                receiver->forceReconnect();
            }

            // Handle pending stream restart on main thread (thread-safe)
            if (networkMode && decoder) {
                std::lock_guard<std::mutex> lock(gStreamInfoMutex);
                if (gPendingStreamRestart) {
                    printf("[Main] Reinitializing decoder: %ux%u @ %u FPS\n",
                        gPendingWidth, gPendingHeight, gPendingFPS);

                    // Flush old input queue to prevent stale frames
                    {
                        std::lock_guard<std::mutex> iq(gInputMutex);
                        gInputQueue.clear();
                    }
                    pendingDisplay.clear();
                    streamWidth = gPendingWidth;
                    streamHeight = gPendingHeight;
                    restartDecoder(gPendingWidth, gPendingHeight);

                    float aspect = (float)gPendingWidth / (float)gPendingHeight;
                    window.SetVideoAspectRatio(aspect);
                    printf("[Main] Aspect ratio locked: %.4f\n", aspect);

                    gPendingStreamRestart = false;
                    forceRender = true;
                }
            }

            // Phase 14: first-time pairing — ask for the PIN in a dialog above the
            // display window. Modal, but nothing is streaming before pairing completes.
            uint32_t pinRequest = 0;
            if (receiver && receiver->takePinRequest(pinRequest)) {
                std::string pin;
                bool entered = PromptForPIN(window.GetHandle(), pin,
                    [&receiver, pinRequest] { return receiver->isPinRequestCurrent(pinRequest); });
                if (entered) {
                    receiver->submitPIN(pinRequest, pin);
                } else if (receiver->isPinRequestCurrent(pinRequest)) {
                    printf("[Pairing] Cancelled — disconnected. Restart the receiver to pair again.\n");
                    receiver->disconnect();
                }
            }

            // Poll clipboard sync (processes hidden window messages)
            if (clipboardSync) {
                clipboardSync->poll();
            }

            decodePending();

            // ── Overlay text, rebuilt 4× per second (DirectWrite layout is not free) ──
            int64_t nowUs = PipelineClock::NowUs();
            if (window.ShowStats() != lastShowStats) {
                lastShowStats = window.ShowStats();
                lastOverlayUs = 0;
                forceRender = true;
            }
            if (window.ShowStats() && nowUs - lastOverlayUs >= 250'000) {
                lastOverlayUs = nowUs;
                OverlayInputs in;
                in.metrics = metrics.snapshot();
                in.pc = systemMonitor.latest();
                in.gpuName = renderer->GetAdapterName();
                in.hardwareDecode = decoder && decoder->isHardwareAccelerated();
                in.benchmarking = bench.active();
                if (receiver) {
                    in.sync = receiver->clockEstimate();
                    in.mac = receiver->senderStats();
                    const wchar_t* state = L"?";
                    switch (receiver->getState()) {
                        case StreamReceiver::State::Disconnected: state = L"Disconnected"; break;
                        case StreamReceiver::State::Connecting:   state = L"Connecting"; break;
                        case StreamReceiver::State::Connected:    state = L"Connected"; break;
                        case StreamReceiver::State::Streaming:    state = L"Streaming"; break;
                        case StreamReceiver::State::Reconnecting: state = L"Reconnecting"; break;
                        case StreamReceiver::State::Error:        state = L"Error"; break;
                    }
                    wchar_t line[128];
                    swprintf(line, 128, L"%ls %ux%u -> window %dx%d", state, streamWidth, streamHeight,
                             window.GetWidth(), window.GetHeight());
                    in.connection = line;
                } else {
                    in.connection = L"Test pattern (no Mac connection)";
                }
                overlayText = FormatDiagnostics(in);
                forceRender = true;
            }

            // ── Render only when something changed ──────────────────────────
            const bool animating = !networkMode;   // test pattern moves every frame
            if (gPicture.fresh || forceRender || animating) {
                // Wait for a free frame slot, then take whatever arrived meanwhile:
                // the frame we present is the newest one, not the one that woke us
                renderer->WaitForFrameSlot(100);
                decodePending();

                auto now = std::chrono::high_resolution_clock::now();
                float elapsed = std::chrono::duration<float>(now - startTime).count();
                const int64_t renderStartUs = PipelineClock::NowUs();
                renderer->Render(elapsed, (float)frameCounter, window.ShowStats() ? &overlayText : nullptr);
                nowUs = PipelineClock::NowUs();
                metrics.onRender((nowUs - renderStartUs) / 1000.0);
                frameCounter++;
                forceRender = false;

                // ── Phase 16: follow each new video frame until it is on screen ──
                if (gPicture.fresh) {
                    gPicture.fresh = false;
                    FrameTimeline presented = gPicture.timeline;
                    presented.presentUs = nowUs;
                    uint32_t presentCount = 0;
                    if (frameStatsUsable && renderer->GetLastPresentCount(presentCount)) {
                        if (lastFrameStatsOkUs == 0) lastFrameStatsOkUs = nowUs;
                        presentsTracked++;
                        pendingDisplay.push_back({ presentCount, presented });
                        if (pendingDisplay.size() > 8) {   // statistics stalled: give up on the oldest
                            finishFrame(pendingDisplay.front().timeline);
                            pendingDisplay.pop_front();
                        }
                    } else {
                        finishFrame(presented);   // no scan-out time available
                    }
                }
            }

            if (!pendingDisplay.empty()) {
                uint32_t shownCount = 0;
                int64_t shownUs = 0;
                bool statsOk = renderer->GetLastDisplayedPresent(shownCount, shownUs);
                if (statsOk) lastFrameStatsOkUs = nowUs;

                // Give up on scan-out times when the driver does not deliver them:
                // failing for 2 s, or 120 presents without a single match
                const char* why = nullptr;
                if (!statsOk && nowUs - lastFrameStatsOkUs > 2'000'000) why = "not provided";
                else if (!frameStatsSeen && presentsTracked > 120) why = "never matched a present";
                if (why && frameStatsUsable) {
                    printf("[Perf] DXGI frame statistics %s — Display stage unavailable, "
                           "Total measured to Present()\n", why);
                    frameStatsUsable = false;
                }

                while (!pendingDisplay.empty()) {
                    PendingDisplay& p = pendingDisplay.front();
                    int32_t ahead = (int32_t)(shownCount - p.presentCount);
                    if (statsOk && frameStatsUsable) {
                        if (ahead < 0 && ahead > -1000) break;   // not on screen yet
                        int64_t delayUs = shownUs - p.timeline.presentUs;
                        if (ahead == 0 && delayUs >= -1000 && delayUs < 100'000) {
                            p.timeline.displayUs = (std::max)(shownUs, p.timeline.presentUs);
                            frameStatsSeen = true;
                        }
                    } else if (frameStatsUsable && nowUs - p.timeline.presentUs < 250'000) {
                        break;   // statistics briefly unavailable: wait a little
                    }
                    finishFrame(p.timeline);
                    pendingDisplay.pop_front();
                }
            }

            // ── Once per second: rates, benchmark row, latency for the Mac ──
            if (nowUs - lastTickUs >= 1'000'000) {
                lastTickUs = nowUs;
                NetworkCounters net;
                if (receiver) {
                    auto ns = receiver->getStats();
                    net.bytesReceived = ns.bytesReceived;
                    net.packetsReceived = ns.packetsReceived;
                    net.packetsLost = ns.packetsLost;
                    net.framesExpected = ns.framesExpected;
                    net.framesReceived = ns.framesReceived;
                    net.jitterMs = ns.jitterMs;
                }
                uint64_t skippedNow;
                {
                    std::lock_guard<std::mutex> iq(gInputMutex);
                    skippedNow = gInputDropped;
                }
                skippedNow += gPicture.overwritten;
                if (skippedNow > skippedReported) {
                    metrics.onFramesSkipped(skippedNow - skippedReported);
                    skippedReported = skippedNow;
                }
                metrics.tick(nowUs, net);

                MetricsSnapshot snap = metrics.snapshot();
                const StageSummary& total = snap.stages[static_cast<size_t>(Stage::Total)];
                if (receiver) {
                    receiver->setReportedLatencyUs(total.samples ? (uint32_t)(total.avgMs * 1000) : 0);
                }
                ClockSync::Estimate sync = receiver ? receiver->clockEstimate() : ClockSync::Estimate{};
                SenderSample mac = receiver ? receiver->senderStats() : SenderSample{};
                bench.recordSecond(nowUs, snap, systemMonitor.latest(), mac, sync);

                // Console summary every 5 s (the overlay has the full breakdown)
                if (nowUs - lastConsoleUs >= 5'000'000) {
                    lastConsoleUs = nowUs;
                    auto stageAvg = [&snap](Stage s) {
                        const StageSummary& x = snap.stages[static_cast<size_t>(s)];
                        return x.samples ? x.avgMs : -1.0;
                    };
                    printf("[Perf] shown %.1f FPS (recv %.1f, rendered %.1f) | %.1f Mbps | RTT %.1f ms | "
                           "jitter %.1f ms | drop %.2f%% | loss %.2f%%\n",
                           snap.presentedFps, snap.receivedFps, snap.renderedFps, snap.bitrateMbps,
                           sync.valid ? sync.smoothedRttMs : -1.0, snap.jitterMs,
                           snap.frameDropPercent, snap.packetLossPercent);
                    printf("[Perf] latency ms: capture %.1f  encode %.1f  send %.1f  network %.1f  "
                           "queue %.1f  decode %.1f  render %.1f  display %.1f  | total %.1f (p95 %.1f)\n",
                           stageAvg(Stage::Capture), stageAvg(Stage::Encode), stageAvg(Stage::Send),
                           stageAvg(Stage::Network), stageAvg(Stage::Queue), stageAvg(Stage::Decode),
                           stageAvg(Stage::Render), stageAvg(Stage::Display),
                           stageAvg(Stage::Total), total.samples ? total.p95Ms : -1.0);
                    if (receiver) {
                        auto ns = receiver->getStats();
                        size_t q;
                        { std::lock_guard<std::mutex> lk(gInputMutex); q = gInputQueue.size(); }
                        printf("[Perf] frames recv %llu / expected %llu, packets lost %llu, "
                               "keyframe req %llu, decode queue %zu\n",
                               ns.framesReceived, ns.framesExpected, ns.packetsLost,
                               ns.keyframeRequests, q);
                    }
                }
            }

            if (bench.due(nowUs)) {
                printf("\n%s\n[Bench] Files: %s-frames.csv, %s-seconds.csv, %s-summary.txt\n",
                       bench.finish(nowUs).c_str(), bench.basePath().c_str(),
                       bench.basePath().c_str(), bench.basePath().c_str());
                if (!frameStatsSeen) {
                    printf("[Bench] Note: no DXGI scan-out times — Total is measured to Present()\n");
                }
                break;   // benchmark complete: exit
            }

            // ── Sleep until there is something to do ────────────────────────
            if (!animating) {
                int64_t waitUs = 100'000;   // PIN requests, resume, state changes
                waitUs = (std::min)(waitUs, lastTickUs + 1'000'000 - nowUs);
                if (window.ShowStats()) waitUs = (std::min)(waitUs, lastOverlayUs + 250'000 - nowUs);
                // Decoder still holds frames: check soon. A scan-out time is due
                // within a refresh or two: half a refresh period is precise enough.
                if (!gDecoding.empty()) waitUs = (std::min)(waitUs, (int64_t)4'000);
                if (!pendingDisplay.empty()) waitUs = (std::min)(waitUs, (int64_t)8'000);
                if (waitUs > 0) {
                    MsgWaitForMultipleObjectsEx(1, &gWakeEvent, (DWORD)((waitUs + 999) / 1000),
                                                QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                }
            }
        }

        if (bench.active()) {   // ESC before the benchmark finished: keep what we have
            int64_t endUs = PipelineClock::NowUs();
            printf("\n[Bench] Stopped early\n%s\n", bench.finish(endUs).c_str());
        }

        if (clipboardSync) clipboardSync->stop();
        if (receiver) receiver->disconnect();
        if (decoder) decoder->shutdown();
        if (gWakeEvent) CloseHandle(gWakeEvent);
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
