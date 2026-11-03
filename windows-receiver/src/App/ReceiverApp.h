#pragma once
// =============================================================================
// ReceiverApp.h — The receiver: find the Mac, connect, pair, show the stream
// =============================================================================
// Runs on the UI thread. Event-driven (Phase 17): sleeps until a frame
// arrives, a window message comes in or a timer is due, and renders only
// when something changed. Phases (Phase 18):
//
//   FindingMac ─(found / picked / IP given)→ Connected session:
//     Connecting → Pairing / Securing → WaitingForPicture → Streaming
//
// Without a picture the window shows a status card instead of a console.
// =============================================================================

#include "App/MacFinder.h"
#include "App/Options.h"
#include "App/StatusText.h"
#include "App/Window.h"
#include "Clipboard/ClipboardSync.h"
#include "Decoder/H264Decoder.h"
#include "Decoder/KeyframeGate.h"
#include "Input/InputCapture.h"
#include "Input/Pointer.h"
#include "Network/StreamReceiver.h"
#include "Performance/PipelineMetrics.h"
#include "Performance/SystemMonitor.h"
#include "Renderer/D3D11Renderer.h"
#include "Security/PairingManager.h"

#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class ReceiverApp {
public:
    ReceiverApp(Window& window, const Options& options);
    ~ReceiverApp();
    ReceiverApp(const ReceiverApp&) = delete;
    ReceiverApp& operator=(const ReceiverApp&) = delete;

    /// Run until the window closes or the benchmark ends. Throws on fatal errors.
    void run();

    /// Create the renderer, stepping down full → compatible → software (WARP).
    /// Every refusal is logged; throws with all reasons if none works.
    static std::unique_ptr<D3D11Renderer> CreateRenderer(const Window& window, GpuPreference gpu,
                                                         RendererMode firstMode, std::string& degraded);

private:
    // ── Finding the Mac ──
    void updateFinding();
    void connectTo(const std::string& address, const std::string& serviceName);

    // ── Video pipeline ──
    struct InputFrame {
        std::vector<uint8_t> data;
        FrameTimeline timeline;
    };
    struct PendingDisplay {
        uint32_t presentCount;
        FrameTimeline timeline;
    };
    void queueFrame(const uint8_t* data, size_t len, const FrameTimeline& timeline);   // network thread
    FrameTimeline takeDecodingTimeline(int64_t sampleTime);
    void onPictureDecoded(const FrameTimeline& timeline);
    void restartDecoder(uint32_t width, uint32_t height);
    void decodePending();
    void finishFrame(const FrameTimeline& t);

    // ── Loop steps ──
    void handleRendererLoss();
    void handleStreamRestart();
    void handlePairingPrompt();
    void updateOverlay(int64_t nowUs);
    void updateStatus(int64_t nowUs);
    void updatePointer();
    void renderIfNeeded(int64_t& nowUs);
    void trackScanout(int64_t nowUs);
    void tickOncePerSecond(int64_t nowUs);
    void sleepUntilNeeded(int64_t nowUs);
    AppPhase currentPhase() const;
    void setNote(const std::string& note);

    Window& window_;
    Options options_;
    std::unique_ptr<D3D11Renderer> renderer_;
    std::string rendererDegraded_;
    HANDLE wakeEvent_ = nullptr;     // set by the network thread: frame / stream change

    // Finding (until a Mac is chosen)
    std::unique_ptr<MacFinder> finder_;
    std::string rememberedMac_;
    std::string macServiceName_;
    std::string macName_;            // friendly
    std::string macAddress_;

    // Session (declaration order = reverse destruction order: the receiver's
    // network thread uses the pairing manager)
    std::unique_ptr<PairingManager> pairing_;
    std::unique_ptr<StreamReceiver> receiver_;
    std::unique_ptr<H264Decoder> decoder_;
    std::unique_ptr<InputCapture> input_;
    std::unique_ptr<ClipboardSync> clipboard_;

    // Frames from the network thread
    std::mutex inputMutex_;
    std::deque<InputFrame> inputQueue_;
    uint64_t inputDropped_ = 0;              // trimmed from the queue (inputMutex_)
    static constexpr size_t kMaxInputQueue = 4;

    // Decoding (UI thread)
    static constexpr int64_t kSampleTimeStep = 166667;   // 100 ns units, 1/60 s
    std::deque<std::pair<int64_t, FrameTimeline>> decoding_;
    int64_t nextSampleTime_ = 0;
    int consecutiveDecodeFailures_ = 0;
    KeyframeGate keyframeGate_;              // docs/SMOOTHNESS.md, step 2
    uint64_t heldReported_ = 0;
    bool pictureFresh_ = false;              // in the renderer, not presented yet
    FrameTimeline pictureTimeline_;
    uint64_t picturesOverwritten_ = 0;

    // Stream changes (network thread → UI thread)
    std::mutex streamInfoMutex_;
    bool pendingStreamRestart_ = false;
    uint32_t pendingWidth_ = 0, pendingHeight_ = 0, pendingFps_ = 0;
    uint32_t streamWidth_ = 0, streamHeight_ = 0;

    // Measurement (Phase 16)
    SystemMonitor systemMonitor_;
    PipelineMetrics metrics_;
    BenchmarkRecorder bench_;
    std::deque<PendingDisplay> pendingDisplay_;
    bool frameStatsUsable_ = true;
    bool frameStatsSeen_ = false;
    int64_t lastFrameStatsOkUs_ = 0;
    uint64_t presentsTracked_ = 0;
    int64_t lastTickUs_ = 0;
    int64_t lastOverlayUs_ = 0;
    int64_t lastConsoleUs_ = 0;
    uint64_t skippedReported_ = 0;
    std::wstring overlayText_;

    // Rendering
    std::chrono::high_resolution_clock::time_point startTime_;
    uint64_t frameCounter_ = 0;
    bool forceRender_ = true;
    bool lastShowStats_ = false;
    bool benchDone_ = false;

    // Status card (Phase 18)
    StatusCard status_;
    bool showStatus_ = true;
    AppPhase lastPhase_ = AppPhase::Searching;
    int64_t phaseSinceUs_ = 0;
    int64_t lastStatusUs_ = 0;
    std::string note_;
    bool codeSent_ = false;                  // waiting for the Mac user to click Pair

    // Pointer drawn here (docs/SMOOTHNESS.md, step 1)
    std::mutex pointerMutex_;                // network thread → UI thread
    std::unique_ptr<PointerShape> pendingShape_;
    PointerPosition pendingPosition_;
    bool pendingPositionValid_ = false;
    PointerModel pointer_;
    bool localPointerWanted_ = true;         // --cursor, F4
    HCURSOR macCursor_ = nullptr;            // Windows pointer with the Mac's image
    uint64_t macCursorShapeId_ = 0;
    int macCursorW_ = 0, macCursorH_ = 0;
    uint64_t spriteShapeId_ = 0;             // image the renderer holds
    int spriteW_ = 0, spriteH_ = 0;
    PointerSprite sprite_;
    bool spriteShown_ = false;
};
