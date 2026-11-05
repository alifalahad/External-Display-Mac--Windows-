// =============================================================================
// ReceiverApp.cpp — Main loop of the receiver
// =============================================================================

#include "App/ReceiverApp.h"
#include "App/Dpi.h"
#include "App/Log.h"
#include "App/Utf8.h"
#include "Input/PointerCursor.h"
#include "Input/VideoViewport.h"
#include "Performance/PipelineClock.h"
#include "UI/PinPrompt.h"

#include <cstdio>
#include <cstring>
#include <ctime>

namespace {

std::string BenchmarkBasePath() {
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char name[64];
    std::strftime(name, sizeof(name), "bench-%Y%m%d-%H%M%S", &local);
    return name;
}

const wchar_t* StateName(StreamReceiver::State state) {
    switch (state) {
        case StreamReceiver::State::Disconnected: return L"Disconnected";
        case StreamReceiver::State::Connecting:   return L"Connecting";
        case StreamReceiver::State::Connected:    return L"Connected";
        case StreamReceiver::State::Streaming:    return L"Streaming";
        case StreamReceiver::State::Reconnecting: return L"Reconnecting";
        default:                                  return L"Error";
    }
}

/// Media Foundation needs COM on the thread that decodes
struct ComScope {
    ComScope() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~ComScope() { CoUninitialize(); }
};

} // namespace

// ── Construction ────────────────────────────────────────────────────────────

std::unique_ptr<D3D11Renderer> ReceiverApp::CreateRenderer(const Window& window, GpuPreference gpu,
                                                           RendererMode firstMode, std::string& degraded) {
    std::string failures;
    for (RendererMode mode : { RendererMode::Full, RendererMode::Compatible, RendererMode::Software }) {
        if (static_cast<int>(mode) < static_cast<int>(firstMode)) continue;
        try {
            auto renderer = std::make_unique<D3D11Renderer>(
                window.GetHandle(), window.GetWidth(), window.GetHeight(), gpu, mode);
            degraded = mode == RendererMode::Full ? std::string() : failures;
            if (mode != RendererMode::Full) {
                LOG_WARN("Renderer") << "Running in " << RendererModeName(mode) << " mode";
            }
            return renderer;
        } catch (const std::exception& e) {
            LOG_ERROR("Renderer") << RendererModeName(mode) << " renderer failed: " << e.what();
            failures += std::string(RendererModeName(mode)) + ": " + e.what() + "\n";
        }
    }
    throw std::runtime_error("Direct3D 11 could not be initialised on this PC.\n\n" + failures +
                             "\nUpdate the graphics driver, or try --gpu=high / --gpu=low.");
}

ReceiverApp::ReceiverApp(Window& window, const Options& options)
    : window_(window), options_(options) {
    D3D11Renderer::LogAdapters();
    renderer_ = CreateRenderer(window_, options_.gpu, options_.renderer, rendererDegraded_);
    LOG_INFO("Renderer") << "Rendering on " << WideToUtf8(renderer_->GetAdapterName()) << " ("
                         << RendererModeName(renderer_->GetMode()) << " mode)";
    wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    localPointerWanted_ = options_.localCursor;
    presentImmediately_ = options_.presentImmediately;
}

ReceiverApp::~ReceiverApp() {
    if (clipboard_) clipboard_->stop();
    if (receiver_) receiver_->disconnect();   // joins the network threads
    if (decoder_) decoder_->shutdown();
    window_.SetInputCapture(nullptr);
    window_.SetClientCursor(nullptr, false);
    if (macCursor_) DestroyIcon(macCursor_);
    if (wakeEvent_) CloseHandle(wakeEvent_);
}

// ── Finding and connecting ──────────────────────────────────────────────────

void ReceiverApp::updateFinding() {
    if (!finder_) return;
    std::vector<FoundMac> found = finder_->found();

    int pick = -1;
    int digit = window_.TakeDigitKey();
    if (digit > 0 && digit <= static_cast<int>(found.size())) pick = digit - 1;
    if (pick < 0) pick = AutoChooseMac(found, rememberedMac_, finder_->secondsSinceFirstFound());
    if (pick < 0) return;

    FoundMac chosen = found[static_cast<size_t>(pick)];
    finder_.reset();   // stop browsing
    MacFinder::RememberMac(chosen.name);
    connectTo(chosen.address, chosen.name);
}

void ReceiverApp::connectTo(const std::string& address, const std::string& serviceName) {
    macAddress_ = address;
    macServiceName_ = serviceName;
    macName_ = serviceName.empty() ? address : FriendlyMacName(serviceName);
    LOG_INFO("Main") << "Connecting to " << macName_ << " at " << address;

    pairing_ = std::make_unique<PairingManager>();
    pairing_->initialize();
    decoder_ = std::make_unique<H264Decoder>();
    receiver_ = std::make_unique<StreamReceiver>();

    // Stream (re)start: the UI thread rebuilds the decoder
    receiver_->setStreamInfoCallback([this](uint32_t w, uint32_t h, uint32_t fps) {
        {
            std::lock_guard<std::mutex> lock(streamInfoMutex_);
            pendingWidth_ = w;
            pendingHeight_ = h;
            pendingFps_ = fps;
            pendingStreamRestart_ = true;
        }
        LOGF_DEBUG("Net", "Stream info queued: %ux%u @ %u FPS", w, h, fps);
        SetEvent(wakeEvent_);
    });

    // Network thread: queue H.264 frames (decoding happens on the UI thread)
    receiver_->setFrameCallback([this](const uint8_t* data, size_t len, const FrameTimeline& timeline) {
        queueFrame(data, len, timeline);
    });

    // Decoder output (UI thread) goes straight to the renderer.
    // Hardware: GPU→GPU copy of the decoded surface.
    decoder_->setTextureCallback([this](ID3D11Texture2D* texture, UINT subresource,
                                        uint32_t w, uint32_t h, int64_t sampleTime) {
        FrameTimeline timeline = takeDecodingTimeline(sampleTime);
        timeline.decodeEndUs = PipelineClock::NowUs();
        if (renderer_->UpdateFrameTexture(texture, subresource, w, h)) onPictureDecoded(timeline);
    });
    // Software fallback: upload the NV12 planes from system memory
    decoder_->setRawNV12Callback([this](const uint8_t* yData, const uint8_t* uvData,
                                        int stride, uint32_t w, uint32_t h, int64_t sampleTime) {
        FrameTimeline timeline = takeDecodingTimeline(sampleTime);
        timeline.decodeEndUs = PipelineClock::NowUs();
        renderer_->UpdateFrameNV12(yData, uvData, stride, w, h);
        onPictureDecoded(timeline);
    });

    // Pointer drawn here (docs/SMOOTHNESS.md, step 1): only the newest shape
    // and position wait for the UI thread
    // Step 7: the Mac streams at up to 120 FPS if this screen shows that many
    screenHz_ = RefreshHzForWindow(window_.GetHandle());
    LOGF_INFO("Main", "Screen refresh rate: %u Hz%s", screenHz_,
              options_.maxFps > 0 ? (" (asking the Mac for at most " + std::to_string(options_.maxFps) +
                                     " FPS: --max-fps)").c_str() : "");
    receiver_->setReceiverCaps(wantedCaps(), reportedRefreshHz());
    receiver_->enableRateFeedback(options_.adaptToDelay);
    if (options_.simulatedLinkKbps > 0) {
        LOGF_WARN("Net", "Simulating a %.0f kbps link with a %.0f ms queue (--simulate-link)",
                  options_.simulatedLinkKbps, options_.simulatedLinkQueueMs);
        receiver_->simulateLink(options_.simulatedLinkKbps, options_.simulatedLinkQueueMs);
    }
    if (options_.simulatedLossPercent > 0) {
        LOGF_WARN("Net", "Simulating %.1f%% packet loss in bursts of %d (--simulate-loss)",
                  options_.simulatedLossPercent, options_.simulatedLossBurst);
        receiver_->simulateLoss(options_.simulatedLossPercent, options_.simulatedLossBurst);
    }
    receiver_->setPointerCallbacks(
        [this](const uint8_t* data, size_t len) {
            auto shape = std::make_unique<PointerShape>();
            if (!ParsePointerShape(data, len, *shape)) {
                LOG_EVERY_MS(5000, LogLevel::Warning, "Input") << "Ignoring a malformed CURSOR_SHAPE";
                return;
            }
            {
                std::lock_guard<std::mutex> lock(pointerMutex_);
                pendingShape_ = std::move(shape);
            }
            SetEvent(wakeEvent_);
        },
        [this](const uint8_t* data, size_t len) {
            PointerPosition position;
            if (!ParsePointerPosition(data, len, position)) return;
            {
                std::lock_guard<std::mutex> lock(pointerMutex_);
                pendingPosition_ = position;
                pendingPositionValid_ = true;
            }
            SetEvent(wakeEvent_);
        });

    receiver_->setPairingManager(pairing_.get());
    std::string name = serviceName;
    receiver_->setHostResolver([name] { return FindMacAddress(name); });
    if (!receiver_->connect(address)) LOG_ERROR("Net") << "Failed to start connecting to " << address;

    input_ = std::make_unique<InputCapture>();
    input_->setSendCallback([this](const exdp::InputEventPayload& evt) {
        if (receiver_) receiver_->sendInputEvent(evt);
    });
    window_.SetInputCapture(input_.get());

    clipboard_ = std::make_unique<ClipboardSync>();
    receiver_->setClipboardCallback([this](const std::string& text) {   // Mac → Windows
        if (clipboard_) clipboard_->receiveRemoteClipboard(text);
    });
    clipboard_->setClipboardCallback([this](const std::string& text) {  // Windows → Mac
        if (receiver_) receiver_->sendClipboardData(text);
    });
    clipboard_->start(GetModuleHandle(nullptr));
    forceRender_ = true;
}

// ── Video pipeline ──────────────────────────────────────────────────────────

void ReceiverApp::queueFrame(const uint8_t* data, size_t len, const FrameTimeline& timeline) {
    {
        std::lock_guard<std::mutex> lock(inputMutex_);
        if (inputQueue_.size() >= kMaxInputQueue) {
            // Fresh frames matter more than old ones: drop up to the newest keyframe
            int lastKey = -1;
            for (int i = static_cast<int>(inputQueue_.size()) - 1; i >= 0; i--) {
                if (inputQueue_[static_cast<size_t>(i)].timeline.keyframe) { lastKey = i; break; }
            }
            if (lastKey > 0) {
                inputQueue_.erase(inputQueue_.begin(), inputQueue_.begin() + lastKey);
                inputDropped_ += static_cast<uint64_t>(lastKey);
            } else if (inputQueue_.size() >= kMaxInputQueue * 2) {
                inputDropped_ += inputQueue_.size() - kMaxInputQueue;
                inputQueue_.erase(inputQueue_.begin(), inputQueue_.end() - kMaxInputQueue);
            }
        }
        inputQueue_.push_back({ std::vector<uint8_t>(data, data + len), timeline });
    }
    SetEvent(wakeEvent_);
}

FrameTimeline ReceiverApp::takeDecodingTimeline(int64_t sampleTime) {
    // Exact match; older inputs that produced no picture are discarded
    for (size_t i = 0; i < decoding_.size(); i++) {
        if (decoding_[i].first == sampleTime) {
            FrameTimeline t = decoding_[i].second;
            decoding_.erase(decoding_.begin(), decoding_.begin() + static_cast<std::ptrdiff_t>(i) + 1);
            return t;
        }
    }
    // Time not passed through: no B-frames, so pictures come out in input order
    if (decoding_.empty()) return FrameTimeline{};
    FrameTimeline t = decoding_.front().second;
    decoding_.pop_front();
    return t;
}

void ReceiverApp::onPictureDecoded(const FrameTimeline& timeline) {
    if (pictureFresh_) picturesOverwritten_++;   // latest picture wins
    pictureTimeline_ = timeline;
    pictureFresh_ = true;
}

void ReceiverApp::restartDecoder(uint32_t width, uint32_t height) {
    // On the renderer's current device: hardware decoding when the GPU allows
    // it. The decoder can only start from a keyframe: the gate holds the
    // other frames and asks the Mac for one (decodePending).
    decoder_->shutdown();
    decoding_.clear();
    pictureFresh_ = false;
    decoder_->setD3D11Device(renderer_->GetVideoDevice());
    decoder_->initialize(width, height);
    keyframeGate_.reset();
}

void ReceiverApp::decodePending() {
    if (!decoder_) return;
    std::deque<InputFrame> frames;
    {
        std::lock_guard<std::mutex> lock(inputMutex_);
        frames.swap(inputQueue_);   // don't hold the lock while decoding
    }
    const int64_t nowUs = PipelineClock::NowUs();
    for (auto& frame : frames) {
        // A frame after a missing one waits for the keyframe (KeyframeGate.h)
        if (!keyframeGate_.admit(frame.timeline.frameSequence, frame.timeline.hasSenderTiming,
                                 frame.timeline.keyframe, nowUs)) {
            continue;
        }
        // A keyframe's SPS says how its colours map to RGB (VideoColor.h)
        VideoColor color;
        if (frame.timeline.keyframe && FindSpsColor(frame.data.data(), frame.data.size(), color) &&
            (!videoColorKnown_ || color != videoColor_)) {
            videoColor_ = color;
            videoColorKnown_ = true;
            renderer_->SetVideoColor(color, streamHeight_);
            LOG_INFO("Decoder") << "Video colours: " << VideoColorName(color, streamHeight_);
        }
        const int64_t sampleTime = nextSampleTime_;
        nextSampleTime_ += kSampleTimeStep;
        frame.timeline.decodeStartUs = PipelineClock::NowUs();
        decoding_.emplace_back(sampleTime, frame.timeline);

        if (decoder_->decode(frame.data.data(), frame.data.size(), sampleTime)) {
            metrics_.onFrameDecoded();
            consecutiveDecodeFailures_ = 0;
        } else if (++consecutiveDecodeFailures_ >= 30) {
            // Phase 15: a decoder that keeps failing is rebuilt
            LOGF_WARN("Decoder", "%d consecutive failures — reinitializing decoder", consecutiveDecodeFailures_);
            consecutiveDecodeFailures_ = 0;
            restartDecoder(streamWidth_ ? streamWidth_ : 1920, streamHeight_ ? streamHeight_ : 1080);
            break;
        }
    }
    // Pictures the decoder finished after the last input
    if (!decoding_.empty()) decoder_->drainOutput();

    // The Mac sends keyframes only when asked: at once, then every 250 ms
    // until one arrives
    if (receiver_ && receiver_->getState() == StreamReceiver::State::Streaming &&
        keyframeGate_.shouldRequestKeyframe(nowUs)) {
        LOG_DEBUG("Decoder") << "Asking the Mac for a keyframe";
        receiver_->requestKeyframe();
    }

    // Inputs that never produced a picture (e.g. before the first keyframe)
    // must not keep the loop polling
    const int64_t staleUs = PipelineClock::NowUs() - 500'000;
    while (!decoding_.empty() && decoding_.front().second.decodeStartUs < staleUs) decoding_.pop_front();
}

void ReceiverApp::finishFrame(const FrameTimeline& t) {
    StageTimes stages = metrics_.onFrameDisplayed(t, receiver_ ? receiver_->clockEstimate() : ClockSync::Estimate{});
    if (options_.benchSeconds > 0 && !bench_.active() && bench_.basePath().empty()) {
        std::string base = BenchmarkBasePath();
        if (bench_.start(base, options_.benchSeconds, PipelineClock::NowUs())) {
            LOGF_INFO("Bench", "Recording %d s to %s-*.csv", options_.benchSeconds, base.c_str());
            bench_.addNote(decoder_ && decoder_->isHardwareAccelerated()
                ? "Decoder: hardware (D3D11 video engine)" : "Decoder: software (CPU)");
            bench_.addNote(std::string("Renderer: ") + RendererModeName(renderer_->GetMode()) +
                           (sizeof(void*) == 8 ? ", 64-bit build" : ", 32-bit build"));
            // Step 5: what the Network stage and the drops depend on
            char simulated[128] = "";
            if (options_.simulatedLossPercent > 0) {
                snprintf(simulated, sizeof(simulated), ", SIMULATED LOSS %.1f%% (bursts of %d)",
                         options_.simulatedLossPercent, options_.simulatedLossBurst);
            }
            if (options_.simulatedLinkKbps > 0) {
                const size_t used = strlen(simulated);
                snprintf(simulated + used, sizeof(simulated) - used, ", SIMULATED LINK %.0f kbps",
                         options_.simulatedLinkKbps);
            }
            bench_.addNote(std::string("Loss repair: parity ") + (options_.parity ? "on" : "off") +
                           ", pacing " + (options_.pacing ? "on" : "off") + "; bitrate follows " +
                           (options_.adaptToDelay ? "the delay" : "loss/RTT tiers") + simulated);
            // Step 3: what the Display stage depends on
            bench_.addNote("Window: " + std::to_string(window_.GetWidth()) + "x" +
                           std::to_string(window_.GetHeight()) + (window_.IsFullscreen() ? " full screen" : " windowed") +
                           " at " + std::to_string(MulDiv(100, static_cast<int>(window_.GetDpi()), 96)) +
                           "% scale; present " + (renderer_->PresentsImmediately() ? "at once (tearing allowed)"
                                                                                   : "on the refresh") +
                           "; screen " + std::to_string(screenHz_) + " Hz" +
                           (options_.maxFps > 0 ? ", at most " + std::to_string(options_.maxFps) + " FPS asked for"
                                                : std::string()));
        } else {
            LOGF_ERROR("Bench", "Cannot create %s-*.csv — benchmark disabled", base.c_str());
            options_.benchSeconds = 0;
        }
    }
    bench_.recordFrame(t, stages);
}

// ── Main loop ───────────────────────────────────────────────────────────────

void ReceiverApp::run() {
    ComScope com;
    startTime_ = std::chrono::high_resolution_clock::now();
    lastTickUs_ = lastConsoleUs_ = phaseSinceUs_ = PipelineClock::NowUs();
    lastShowStats_ = window_.ShowStats();

    if (options_.testPattern) {
        LOG_INFO("Main") << "Test pattern mode (no network)";
    } else if (!options_.macAddress.empty()) {
        connectTo(options_.macAddress, "");
    } else {
        rememberedMac_ = MacFinder::RememberedMac();
        finder_ = std::make_unique<MacFinder>();
        LOG_INFO("Discovery") << "Searching for Macs on the network"
                              << (rememberedMac_.empty() ? "" : " (last used: " + FriendlyMacName(rememberedMac_) + ")");
    }
    if (options_.benchSeconds > 0) {
        LOG_INFO("Bench") << "Benchmark: " << options_.benchSeconds << " s, starts with the first video frame";
    }

    while (window_.ProcessMessages()) {
        if (window_.WasResized()) {
            renderer_->Resize(window_.GetWidth(), window_.GetHeight());
            window_.ClearResizedFlag();
            forceRender_ = true;
        }
        handleRendererLoss();

        // Phase 15: laptop woke up — the old TCP connection is dead, don't
        // wait for the 15 s timeout
        if (window_.TakeResumeEvent() && receiver_) {
            LOG_INFO("Power") << "Resumed from sleep — reconnecting now";
            receiver_->forceReconnect();
        }

        handleStreamRestart();
        updateFinding();
        handlePairingPrompt();
        if (receiver_) {
            std::string message = receiver_->takeUserMessage();
            if (!message.empty()) setNote(message);
        }
        if (clipboard_) clipboard_->poll();
        if (input_) input_->flushPendingMove();
        decodePending();

        int64_t nowUs = PipelineClock::NowUs();
        updateOverlay(nowUs);
        updateStatus(nowUs);
        updatePointer();
        updateDisplayPath();
        renderIfNeeded(nowUs);
        trackScanout(nowUs);
        tickOncePerSecond(nowUs);

        if (bench_.due(nowUs)) {
            std::printf("\n%s\n", bench_.finish(nowUs).c_str());
            LOGF_INFO("Bench", "Files: %s-frames.csv, %s-seconds.csv, %s-summary.txt",
                      bench_.basePath().c_str(), bench_.basePath().c_str(), bench_.basePath().c_str());
            if (!frameStatsSeen_) LOG_INFO("Bench") << "No DXGI scan-out times — Total is measured to Present()";
            break;   // benchmark complete: exit
        }
        sleepUntilNeeded(nowUs);
    }

    if (bench_.active()) {   // Esc before the benchmark finished: keep what we have
        LOG_WARN("Bench") << "Stopped early";
        std::printf("\n%s\n", bench_.finish(PipelineClock::NowUs()).c_str());
    }
}

void ReceiverApp::handleRendererLoss() {
    // Phase 15: GPU removed/reset (driver update, TDR, hybrid GPU switch) or a
    // failed resize — rebuild the renderer instead of crashing
    if (!renderer_->IsDeviceLost()) return;
    LOG_WARN("Renderer") << "Recreating renderer…";
    renderer_.reset();   // release the old swap chain first
    for (int attempt = 1; !renderer_; attempt++) {
        try {
            renderer_ = CreateRenderer(window_, options_.gpu, options_.renderer, rendererDegraded_);
        } catch (const std::exception& e) {
            if (attempt >= 10) throw;
            LOG_WARN("Renderer") << "Recreate failed, retrying: " << e.what();
            Sleep(500);
        }
    }
    LOG_INFO("Renderer") << "Recovered on " << WideToUtf8(renderer_->GetAdapterName());
    // The new swap chain numbers its presents from scratch
    for (const auto& p : pendingDisplay_) finishFrame(p.timeline);
    pendingDisplay_.clear();
    // A hardware decoder is bound to the old device, and so was the pointer image
    if (decoder_ && streamWidth_) restartDecoder(streamWidth_, streamHeight_);
    renderer_->SetVideoColor(videoColor_, streamHeight_);
    spriteShapeId_ = 0;
    spriteW_ = spriteH_ = 0;
    forceRender_ = true;
}

void ReceiverApp::handleStreamRestart() {
    if (!decoder_) return;
    uint32_t w, h, fps;
    {
        std::lock_guard<std::mutex> lock(streamInfoMutex_);
        if (!pendingStreamRestart_) return;
        pendingStreamRestart_ = false;
        w = pendingWidth_;
        h = pendingHeight_;
        fps = pendingFps_;
    }
    LOGF_INFO("Main", "Stream %ux%u @ %u FPS — (re)initializing decoder", w, h, fps);
    {
        std::lock_guard<std::mutex> lock(inputMutex_);
        inputQueue_.clear();   // frames of the previous stream
    }
    pendingDisplay_.clear();
    streamWidth_ = w;
    streamHeight_ = h;
    restartDecoder(w, h);
    renderer_->SetVideoColor(videoColor_, h);   // "unspecified" depends on the height
    if (h > 0) {
        float aspect = static_cast<float>(w) / static_cast<float>(h);
        window_.SetVideoAspectRatio(aspect);
        if (input_) input_->setVideoAspect(aspect);
    }
    forceRender_ = true;
}

void ReceiverApp::handlePairingPrompt() {
    // Phase 14/18: the code is typed in a dialog above the display window.
    // Modal, but nothing streams before pairing completes.
    if (!receiver_) return;
    uint32_t request = 0;
    if (!receiver_->takeCodeRequest(request)) return;

    codeSent_ = false;
    updateStatus(PipelineClock::NowUs());   // show "Pair with …" behind the dialog
    int64_t nowUs = PipelineClock::NowUs();
    forceRender_ = true;
    renderIfNeeded(nowUs);

    std::string code;
    bool entered = PromptForPIN(window_.GetHandle(), code,
        [this, request] { return receiver_->isCodeRequestCurrent(request); });
    if (!entered) {
        if (receiver_->isCodeRequestCurrent(request)) {
            LOG_WARN("Pairing") << "Pairing cancelled";
            setNote("Pairing cancelled. Restart the receiver to pair.");
            receiver_->disconnect();
        }
        return;
    }
    switch (receiver_->submitVerificationCode(request, code)) {
        case StreamReceiver::CodeResult::Sent:
            codeSent_ = true;
            setNote("");
            break;
        case StreamReceiver::CodeResult::Mismatch:
            setNote("That code does not match the Mac's. Check the digits — a new code is on its way.");
            break;
        case StreamReceiver::CodeResult::Expired:
            break;
    }
    forceRender_ = true;
}

void ReceiverApp::setNote(const std::string& note) {
    if (note_ != note) {
        note_ = note;
        lastStatusUs_ = 0;
        forceRender_ = true;
    }
}

AppPhase ReceiverApp::currentPhase() const {
    if (!receiver_) {
        if (finder_ && finder_->found().size() > 1) return AppPhase::Choosing;
        return AppPhase::Searching;
    }
    switch (receiver_->getState()) {
        case StreamReceiver::State::Connecting:
        case StreamReceiver::State::Reconnecting:
            return AppPhase::Connecting;
        case StreamReceiver::State::Disconnected:
        case StreamReceiver::State::Error:
            return AppPhase::Disconnected;
        default:
            break;
    }
    if (!receiver_->isSecure()) {
        if (codeSent_) return AppPhase::WaitingForApproval;
        return receiver_->isAwaitingCode() ? AppPhase::Pairing : AppPhase::Securing;
    }
    if (receiver_->getState() == StreamReceiver::State::Streaming && renderer_->HasVideoFrame()) {
        return AppPhase::Streaming;
    }
    return AppPhase::WaitingForPicture;
}

void ReceiverApp::updateStatus(int64_t nowUs) {
    if (options_.testPattern) {
        showStatus_ = false;
        return;
    }
    AppPhase phase = currentPhase();
    if (phase != lastPhase_) {
        // A new phase: reset its clock; old notes don't apply to a working stream
        if (phase == AppPhase::Streaming || phase == AppPhase::WaitingForPicture) note_.clear();
        if (phase != AppPhase::WaitingForApproval) codeSent_ = false;   // approved, or the connection dropped
        lastPhase_ = phase;
        phaseSinceUs_ = nowUs;
        lastStatusUs_ = 0;
        forceRender_ = true;
        LOG_DEBUG("Main") << "Phase " << static_cast<int>(phase);
    }
    showStatus_ = phase != AppPhase::Streaming;
    if (!showStatus_ || nowUs - lastStatusUs_ < 1'000'000) return;   // texts have 1 s resolution

    StatusInputs in;
    in.phase = phase;
    in.macName = macName_;
    in.address = macAddress_;
    in.secondsInPhase = (nowUs - phaseSinceUs_) / 1e6;
    in.note = note_;
    if (finder_) in.choices = finder_->found();
    if (renderer_->GetMode() != RendererMode::Full) {
        in.rendererNote = std::string("Graphics: ") + RendererModeName(renderer_->GetMode()) +
                          " mode (the GPU refused the full mode — see receiver.log).";
    }
    StatusStrings s = BuildStatus(in);
    StatusCard card{ WidenUtf8(s.title), WidenUtf8(s.body), WidenUtf8(s.footer) };
    if (card.title != status_.title || card.body != status_.body || card.footer != status_.footer) {
        status_ = card;
        forceRender_ = true;
    }
    lastStatusUs_ = nowUs;
}

uint32_t ReceiverApp::wantedCaps() const {
    uint32_t caps = 0;
    if (localPointerWanted_ && renderer_->CanDrawPointer()) caps |= exdp::CAP_LOCAL_CURSOR;
    if (options_.parity) caps |= exdp::CAP_PARITY;     // docs/SMOOTHNESS.md, step 5
    if (options_.pacing) caps |= exdp::CAP_PACING;
    return caps;
}

void ReceiverApp::updateDisplayPath() {
    // F5: compare presenting on the next refresh with presenting at once
    if (window_.TakePresentToggle()) {
        presentImmediately_ = !presentImmediately_;
        if (presentImmediately_ && !renderer_->CanPresentImmediately()) {
            LOG_WARN("Renderer") << "Presenting at once is not available here (needs Windows 10 1607+ and "
                                    "driver support): presenting on the refresh (F5)";
        } else {
            LOG_INFO("Renderer") << "Present " << (presentImmediately_ ? "at once (may tear)" : "on the next refresh")
                                 << " (F5)";
        }
        lastOverlayUs_ = 0;   // the overlay names the mode
    }
    renderer_->SetPresentImmediately(presentImmediately_);
    // Step 7: on another monitor, or in another display mode, the refresh rate
    // may differ: the Mac is told (RECEIVER_CAPS, from updatePointer)
    if (window_.TakeScreenChange()) {
        const uint32_t hz = RefreshHzForWindow(window_.GetHandle());
        if (hz != screenHz_) {
            LOGF_INFO("Main", "Screen refresh rate now %u Hz (was %u)", hz, screenHz_);
            screenHz_ = hz;
            lastOverlayUs_ = 0;   // the overlay names it
        }
    }
    // Real pixels (per-monitor DPI): the status card and overlay scale themselves
    if (renderer_->SetUiScale(static_cast<float>(window_.GetDpi()) / 96.0f)) forceRender_ = true;
}

void ReceiverApp::updatePointer() {
    // F4: compare the pointer drawn here with the pointer in the video
    if (window_.TakePointerToggle()) {
        localPointerWanted_ = !localPointerWanted_;
        LOG_INFO("Input") << "Pointer " << (localPointerWanted_ ? "drawn on this PC" : "in the video") << " (F4)";
        lastOverlayUs_ = 0;   // the overlay names the mode
    }

    // What the Mac sent since the last loop (only the newest counts)
    {
        std::lock_guard<std::mutex> lock(pointerMutex_);
        if (pendingShape_) {
            pointer_.onShape(std::move(*pendingShape_));
            pendingShape_.reset();
        }
        if (pendingPositionValid_) {
            pointer_.onMacPosition(pendingPosition_);
            pendingPositionValid_ = false;
        }
    }

    // Drawn here only if this renderer can; otherwise the Mac keeps it in the video
    const bool local = localPointerWanted_ && renderer_->CanDrawPointer();
    if (receiver_) receiver_->setReceiverCaps(wantedCaps(), reportedRefreshHz());
    pointer_.setLocalPointer(local);
    pointer_.setForwarding(input_ && input_->isEnabled());

    const int w = window_.GetWidth(), h = window_.GetHeight();
    const float aspect = window_.GetVideoAspectRatio();
    int mx = 0, my = 0;
    if (window_.TakeMouseMove(mx, my)) {
        float u = 0, v = 0;
        WindowToVideo(mx, my, w, h, aspect, u, v);
        pointer_.onLocalMove(u, v);
    }

    // Before the picture (status card, pairing): Windows' normal arrow
    const bool streaming = !showStatus_ && renderer_->HasVideoFrame();
    const PointerView view = streaming ? pointer_.view() : PointerView::System;
    const PointerShape* shape = pointer_.shape();

    // Windows' own pointer, wearing the Mac's image
    HCURSOR retired = nullptr;
    if (view == PointerView::MacShape && shape) {
        const PointerPlacement place = PlacePointer(*shape, 0, 0, w, h, aspect, streamWidth_);
        if (!macCursor_ || macCursorShapeId_ != shape->id || macCursorW_ != place.width ||
            macCursorH_ != place.height) {
            std::vector<uint8_t> pixels = ResampleBgra(shape->bgra, shape->width, shape->height,
                                                       place.width, place.height);
            UnpremultiplyBgra(pixels);
            HCURSOR cursor = pixels.empty() ? nullptr
                : CreatePointerCursor(pixels.data(), place.width, place.height, place.hotspotX, place.hotspotY);
            if (cursor) {
                retired = macCursor_;
                macCursor_ = cursor;
                macCursorShapeId_ = shape->id;
                macCursorW_ = place.width;
                macCursorH_ = place.height;
            } else {
                LOG_EVERY_MS(5000, LogLevel::Warning, "Input") << "Could not make a Windows pointer from the Mac's image";
            }
        }
    }
    window_.SetClientCursor(view == PointerView::MacShape ? macCursor_ : nullptr, view == PointerView::Hidden);
    if (retired) DestroyIcon(retired);   // replaced above: no longer set

    // The pointer we draw: where the Mac says it is
    if (streaming && shape && pointer_.drawPointer()) {
        const PointerPosition mac = pointer_.macPosition();
        const PointerPlacement place = PlacePointer(*shape, mac.x, mac.y, w, h, aspect, streamWidth_);
        if (spriteShapeId_ != shape->id || spriteW_ != place.width || spriteH_ != place.height) {
            std::vector<uint8_t> pixels = ResampleBgra(shape->bgra, shape->width, shape->height,
                                                       place.width, place.height);
            renderer_->SetPointerImage(pixels.empty() ? nullptr : pixels.data(), place.width, place.height);
            spriteShapeId_ = shape->id;
            spriteW_ = place.width;
            spriteH_ = place.height;
            forceRender_ = true;
        }
        const PointerSprite next{ place.x, place.y, place.width, place.height };
        if (!spriteShown_ || next != sprite_) {
            sprite_ = next;
            spriteShown_ = true;
            forceRender_ = true;
        }
    } else if (spriteShown_) {
        spriteShown_ = false;
        forceRender_ = true;
    }
}

void ReceiverApp::updateOverlay(int64_t nowUs) {
    // Rebuilt 4× per second (DirectWrite layout is not free)
    if (window_.ShowStats() != lastShowStats_) {
        lastShowStats_ = window_.ShowStats();
        lastOverlayUs_ = 0;
        forceRender_ = true;
    }
    if (!window_.ShowStats() || nowUs - lastOverlayUs_ < 250'000) return;
    lastOverlayUs_ = nowUs;

    OverlayInputs in;
    in.metrics = metrics_.snapshot();
    in.pc = systemMonitor_.latest();
    in.gpuName = renderer_->GetAdapterName();
    in.hardwareDecode = decoder_ && decoder_->isHardwareAccelerated();
    in.screenHz = screenHz_;
    in.benchmarking = bench_.active();
    if (receiver_) {
        in.sync = receiver_->clockEstimate();
        in.mac = receiver_->senderStats();
        wchar_t line[256];
        const std::wstring colour = WidenUtf8(VideoColorName(videoColor_, streamHeight_));
        swprintf(line, 256, L"%ls%ls %ux%u, %ls -> window %dx%d at %d%%\n"
                            L"Pointer %ls (F4)  present %ls (F5)",
                 StateName(receiver_->getState()), receiver_->isSecure() ? L" (encrypted)" : L"",
                 streamWidth_, streamHeight_, colour.c_str(), window_.GetWidth(), window_.GetHeight(),
                 MulDiv(100, static_cast<int>(window_.GetDpi()), 96),
                 localPointerWanted_ && renderer_->CanDrawPointer() ? L"drawn here" : L"in the video",
                 renderer_->PresentsImmediately() ? L"at once" : L"on refresh");
        in.connection = line;
    } else {
        in.connection = options_.testPattern ? L"Test pattern (no Mac connection)" : L"Looking for a Mac";
    }
    overlayText_ = FormatDiagnostics(in);
    forceRender_ = true;
}

void ReceiverApp::renderIfNeeded(int64_t& nowUs) {
    const bool animating = options_.testPattern;   // the test pattern moves every frame
    if (!pictureFresh_ && !forceRender_ && !animating) return;

    // Wait for a free frame slot, then take whatever arrived meanwhile: the
    // frame we present is the newest one, not the one that woke us
    renderer_->WaitForFrameSlot(100);
    decodePending();

    float elapsed = std::chrono::duration<float>(std::chrono::high_resolution_clock::now() - startTime_).count();
    const int64_t renderStartUs = PipelineClock::NowUs();
    renderer_->Render(elapsed, static_cast<float>(frameCounter_),
                      window_.ShowStats() ? &overlayText_ : nullptr,
                      showStatus_ ? &status_ : nullptr,
                      spriteShown_ ? &sprite_ : nullptr);
    nowUs = PipelineClock::NowUs();
    metrics_.onRender((nowUs - renderStartUs) / 1000.0);
    frameCounter_++;
    forceRender_ = false;

    // Phase 16: follow each new video frame until it is on screen
    if (!pictureFresh_) return;
    pictureFresh_ = false;
    FrameTimeline presented = pictureTimeline_;
    presented.presentUs = nowUs;
    uint32_t presentCount = 0;
    if (frameStatsUsable_ && renderer_->GetLastPresentCount(presentCount)) {
        if (lastFrameStatsOkUs_ == 0) lastFrameStatsOkUs_ = nowUs;
        presentsTracked_++;
        pendingDisplay_.push_back({ presentCount, presented });
        if (pendingDisplay_.size() > 8) {   // statistics stalled: give up on the oldest
            finishFrame(pendingDisplay_.front().timeline);
            pendingDisplay_.pop_front();
        }
    } else {
        finishFrame(presented);   // no scan-out time available
    }
}

void ReceiverApp::trackScanout(int64_t nowUs) {
    if (pendingDisplay_.empty()) return;
    uint32_t shownCount = 0;
    int64_t shownUs = 0;
    bool statsOk = renderer_->GetLastDisplayedPresent(shownCount, shownUs);
    if (statsOk) lastFrameStatsOkUs_ = nowUs;

    // Give up on scan-out times when the driver does not deliver them:
    // failing for 2 s, or 120 presents without a single match
    const char* why = nullptr;
    if (!statsOk && nowUs - lastFrameStatsOkUs_ > 2'000'000) why = "not provided";
    else if (!frameStatsSeen_ && presentsTracked_ > 120) why = "never matched a present";
    if (why && frameStatsUsable_) {
        LOGF_WARN("Perf", "DXGI frame statistics %s — Display stage unavailable, Total measured to Present()", why);
        frameStatsUsable_ = false;
    }

    while (!pendingDisplay_.empty()) {
        PendingDisplay& p = pendingDisplay_.front();
        int32_t ahead = static_cast<int32_t>(shownCount - p.presentCount);
        if (statsOk && frameStatsUsable_) {
            if (ahead < 0 && ahead > -1000) break;   // not on screen yet
            int64_t delayUs = shownUs - p.timeline.presentUs;
            if (ahead == 0 && delayUs >= -1000 && delayUs < 100'000) {
                p.timeline.displayUs = (std::max)(shownUs, p.timeline.presentUs);
                frameStatsSeen_ = true;
            }
        } else if (frameStatsUsable_ && nowUs - p.timeline.presentUs < 250'000) {
            break;   // statistics briefly unavailable: wait a little
        }
        finishFrame(p.timeline);
        pendingDisplay_.pop_front();
    }
}

void ReceiverApp::tickOncePerSecond(int64_t nowUs) {
    if (nowUs - lastTickUs_ < 1'000'000) return;
    lastTickUs_ = nowUs;

    NetworkCounters net;
    if (receiver_) {
        auto ns = receiver_->getStats();
        net.bytesReceived = ns.bytesReceived;
        net.packetsReceived = ns.packetsReceived;
        net.packetsLost = ns.packetsLost;
        net.framesExpected = ns.framesExpected;
        net.framesReceived = ns.framesReceived;
        net.jitterMs = ns.jitterMs;
        net.keyframesReceived = ns.keyframesReceived;
        net.keyframeRequests = ns.keyframeRequests;
        net.fragmentsRebuilt = ns.fragmentsRebuilt;
        net.framesRebuilt = ns.framesRebuilt;
        net.queuingDelayUs = ns.queuingDelayUs;
        net.rateState = static_cast<uint8_t>(ns.rateState);
    }
    if (keyframeGate_.framesHeld() > heldReported_) {
        metrics_.onFramesHeld(keyframeGate_.framesHeld() - heldReported_);
        heldReported_ = keyframeGate_.framesHeld();
    }
    uint64_t skippedNow;
    {
        std::lock_guard<std::mutex> lock(inputMutex_);
        skippedNow = inputDropped_;
    }
    skippedNow += picturesOverwritten_;
    if (skippedNow > skippedReported_) {
        metrics_.onFramesSkipped(skippedNow - skippedReported_);
        skippedReported_ = skippedNow;
    }
    metrics_.tick(nowUs, net);

    MetricsSnapshot snap = metrics_.snapshot();
    const StageSummary& total = snap.stages[static_cast<size_t>(Stage::Total)];
    if (receiver_) receiver_->setReportedLatencyUs(total.samples ? static_cast<uint32_t>(total.avgMs * 1000) : 0);
    ClockSync::Estimate sync = receiver_ ? receiver_->clockEstimate() : ClockSync::Estimate{};
    SenderSample mac = receiver_ ? receiver_->senderStats() : SenderSample{};
    bench_.recordSecond(nowUs, snap, systemMonitor_.latest(), mac, sync);

    // Log summary every 5 s while streaming (the overlay has the full breakdown)
    if (nowUs - lastConsoleUs_ < 5'000'000 || lastPhase_ != AppPhase::Streaming) return;
    lastConsoleUs_ = nowUs;
    auto stageAvg = [&snap](Stage s) {
        const StageSummary& x = snap.stages[static_cast<size_t>(s)];
        return x.samples ? x.avgMs : -1.0;
    };
    LOGF_INFO("Perf", "shown %.1f FPS (recv %.1f, rendered %.1f) | %.1f Mbps | RTT %.1f ms | "
              "jitter %.1f ms | drop %.2f%% | loss %.2f%%",
              snap.presentedFps, snap.receivedFps, snap.renderedFps, snap.bitrateMbps,
              sync.valid ? sync.smoothedRttMs : -1.0, snap.jitterMs,
              snap.frameDropPercent, snap.packetLossPercent);
    LOGF_INFO("Perf", "latency ms: capture %.1f  encode %.1f  send %.1f  network %.1f  queue %.1f  "
              "decode %.1f  render %.1f  display %.1f  | total %.1f (p95 %.1f)",
              stageAvg(Stage::Capture), stageAvg(Stage::Encode), stageAvg(Stage::Send),
              stageAvg(Stage::Network), stageAvg(Stage::Queue), stageAvg(Stage::Decode),
              stageAvg(Stage::Render), stageAvg(Stage::Display),
              stageAvg(Stage::Total), total.samples ? total.p95Ms : -1.0);
}

void ReceiverApp::sleepUntilNeeded(int64_t nowUs) {
    if (options_.testPattern) return;   // animating
    int64_t waitUs = 100'000;           // pairing requests, resume, state changes
    waitUs = (std::min)(waitUs, lastTickUs_ + 1'000'000 - nowUs);
    if (window_.ShowStats()) waitUs = (std::min)(waitUs, lastOverlayUs_ + 250'000 - nowUs);
    // Decoder still holds frames: check soon. A scan-out time is due within a
    // refresh or two: half a refresh period is precise enough.
    if (!decoding_.empty()) waitUs = (std::min)(waitUs, (int64_t)4'000);
    if (!pendingDisplay_.empty()) waitUs = (std::min)(waitUs, (int64_t)8'000);
    // A mouse move held back by the 120 Hz limit
    const int64_t moveDueUs = input_ ? input_->pendingMoveDueInUs() : -1;
    if (moveDueUs >= 0) waitUs = (std::min)(waitUs, moveDueUs);
    if (waitUs > 0) {
        MsgWaitForMultipleObjectsEx(1, &wakeEvent_, static_cast<DWORD>((waitUs + 999) / 1000),
                                    QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}
