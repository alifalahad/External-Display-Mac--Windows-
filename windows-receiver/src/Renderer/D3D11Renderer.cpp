// =============================================================================
// D3D11Renderer.cpp — Direct3D 11 rendering pipeline implementation
// =============================================================================

#include "Renderer/D3D11Renderer.h"
#include "App/Log.h"
#include "Renderer/TestPatternShader.h"
#include "Performance/PipelineClock.h"
#include "App/Utf8.h"
#include "Input/VideoViewport.h"

#include <d3dcompiler.h>
#include <d3d10.h>     // ID3D10Multithread (required by Media Foundation on a shared device)
#include <dxgi1_2.h>
#include <dxgi1_3.h>
#include <dxgi1_5.h>
#include <dxgi1_6.h>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <cstdio>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

// Helper: throw on HRESULT failure with descriptive message
static void ThrowIfFailed(HRESULT hr, const char* operation) {
    if (FAILED(hr)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "%s failed (HRESULT: 0x%08lX)", operation, hr);
        throw std::runtime_error(msg);
    }
}

// =============================================================================
// Construction
// =============================================================================

D3D11Renderer::D3D11Renderer(HWND hwnd, int width, int height, GpuPreference gpuPreference,
                             RendererMode mode)
    : hwnd_(hwnd), mode_(mode), width_(width), height_(height)
{
    CreateDeviceAndSwapChain(hwnd, width, height, gpuPreference);
    CreateRenderTarget();
    CompileShaders();
    CreateConstantBuffer();

    // The diagnostics overlay is optional: a Direct2D/DirectWrite failure must
    // not take the display down with it
    try {
        CreateD2DResources();
        overlayAvailable_ = true;
    } catch (const std::exception& e) {
        LOG_WARN("Renderer") << "Diagnostics overlay unavailable: " << e.what();
        d2dContext_.Reset();
    }
    CreateNV12Resources();
    CreatePointerResources();
}

D3D11Renderer::~D3D11Renderer() {
    if (frameLatencyWaitable_) CloseHandle(frameLatencyWaitable_);
}

// =============================================================================
// Device & Swap Chain
// =============================================================================

void D3D11Renderer::LogAdapters() {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return;

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            LOGF_INFO("Renderer", "GPU %u: %s (%llu MB VRAM, vendor 0x%04X)%s", i,
                      WideToUtf8(desc.Description).c_str(),
                      (unsigned long long)(desc.DedicatedVideoMemory / (1024 * 1024)),
                      desc.VendorId, i == 0 ? "  <- Windows default" : "");
        }
        adapter.Reset();
    }
}

void D3D11Renderer::CheckDeviceLost(HRESULT hr, const char* operation) {
    if (hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET) return;
    if (!deviceLost_) {
        HRESULT reason = device_ ? device_->GetDeviceRemovedReason() : hr;
        LOGF_ERROR("Renderer", "GPU device lost during %s (0x%08lX, reason 0x%08lX)",
               operation, (unsigned long)hr, (unsigned long)reason);
    }
    deviceLost_ = true;
}

void D3D11Renderer::CreateDeviceAndSwapChain(HWND hwnd, int width, int height,
                                              GpuPreference pref) {
    // 0. Pick the adapter. Default (nullptr) = Windows' choice, which on hybrid
    //    laptops is the integrated GPU that drives the built-in panel.
    ComPtr<IDXGIAdapter1> chosenAdapter;
    if (pref != GpuPreference::Default && mode_ != RendererMode::Software) {
        ComPtr<IDXGIFactory6> factory6;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory6)))) {
            factory6->EnumAdapterByGpuPreference(
                0,
                pref == GpuPreference::HighPerformance
                    ? DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE
                    : DXGI_GPU_PREFERENCE_MINIMUM_POWER,
                IID_PPV_ARGS(&chosenAdapter));
        }
        if (!chosenAdapter) {
            LOGF_WARN("Renderer", "GPU preference needs Windows 10 1803+ — using default adapter");
        }
    }

    // 1. Create D3D11 device with BGRA support (needed for D2D interop) and
    //    video support (the hardware H.264 decoder shares this device)
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (mode_ == RendererMode::Full) flags |= D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    // 11_1 is refused outright (E_INVALIDARG) by runtimes that predate it, so
    // the compatible path starts at 11_0. Shaders are compiled for whatever level we get.
    D3D_FEATURE_LEVEL fullLevels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL compatibleLevels[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };
    const bool full = mode_ == RendererMode::Full;
    D3D_DRIVER_TYPE driverType = mode_ == RendererMode::Software ? D3D_DRIVER_TYPE_WARP
                               : chosenAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;

    auto createDevice = [&](UINT createFlags) {
        D3D_FEATURE_LEVEL featureLevel;
        return D3D11CreateDevice(
            mode_ == RendererMode::Software ? nullptr : chosenAdapter.Get(),
            driverType,
            nullptr,
            createFlags,
            full ? fullLevels : compatibleLevels,
            full ? (UINT)_countof(fullLevels) : (UINT)_countof(compatibleLevels),
            D3D11_SDK_VERSION,
            &device_,
            &featureLevel,
            &context_
        );
    };
    HRESULT hr = createDevice(flags);
    // A Debug build asks for the debug layer, which only PCs with Windows'
    // optional "Graphics Tools" have; without it every device fails with this
    if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
        LOGF_WARN("Renderer", "Direct3D debug layer not installed (optional feature \"Graphics Tools\") "
                  "— continuing without it. Build Release for normal use.");
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = createDevice(flags);
    }
    bool videoDevice = SUCCEEDED(hr) && full;
    if (FAILED(hr) && full) {
        LOGF_WARN("Renderer", "No video support on this adapter (0x%08lX) — software decoding",
               (unsigned long)hr);
        hr = createDevice(flags & ~D3D11_CREATE_DEVICE_VIDEO_SUPPORT);
    }
    ThrowIfFailed(hr, "D3D11CreateDevice");
    LOGF_INFO("Renderer", "Device: feature level %X.%X, %s mode",
              (device_->GetFeatureLevel() >> 12) & 0xF, (device_->GetFeatureLevel() >> 8) & 0xF,
              RendererModeName(mode_));

    // Hardware decode writes NV12 textures that we sample as R8 (Y) + R8G8 (UV)
    UINT nv12Support = 0;
    bool nv12Sampling = SUCCEEDED(device_->CheckFormatSupport(DXGI_FORMAT_NV12, &nv12Support)) &&
                        (nv12Support & D3D11_FORMAT_SUPPORT_TEXTURE2D) &&
                        (nv12Support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE);
    ComPtr<ID3D10Multithread> multithread;
    if (videoDevice && nv12Sampling && SUCCEEDED(device_.As(&multithread))) {
        multithread->SetMultithreadProtected(TRUE);   // MF may touch the device internally
        supportsHardwareVideo_ = true;
    } else if (videoDevice) {
        LOGF_WARN("Renderer", "GPU cannot sample NV12 textures — software decoding");
    }

    // 2. Get DXGI factory from the device
    ComPtr<IDXGIDevice> dxgiDevice;
    hr = device_.As(&dxgiDevice);
    ThrowIfFailed(hr, "Query IDXGIDevice");

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    ThrowIfFailed(hr, "GetAdapter");

    // Store adapter name for display
    DXGI_ADAPTER_DESC adapterDesc;
    adapter->GetDesc(&adapterDesc);
    adapterName_ = adapterDesc.Description;

    ComPtr<IDXGIFactory2> factory;
    hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    ThrowIfFailed(hr, "Get IDXGIFactory2");

    // 3. Create swap chain. Tearing (present without waiting for the refresh)
    //    needs DXGI 1.5 and driver support.
    ComPtr<IDXGIFactory5> factory5;
    BOOL tearing = FALSE;
    if (FAILED(factory.As(&factory5)) ||
        FAILED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing)))) {
        tearing = FALSE;
    }
    CreateSwapChain(factory.Get(), hwnd, width, height, tearing == TRUE);

    // By default DXGI queues up to 3 frames: measured as ~3 refreshes (45-50 ms)
    // between Present() and scan-out. One frame in flight is enough.
    ComPtr<IDXGISwapChain2> swapChain2;
    if ((swapChainFlags_ & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) &&
        SUCCEEDED(swapChain_.As(&swapChain2)) &&
        SUCCEEDED(swapChain2->SetMaximumFrameLatency(1))) {
        frameLatencyWaitable_ = swapChain2->GetFrameLatencyWaitableObject();
    } else {
        ComPtr<IDXGIDevice1> dxgiDevice1;
        if (SUCCEEDED(device_.As(&dxgiDevice1))) dxgiDevice1->SetMaximumFrameLatency(1);
    }
    LOGF_INFO("Renderer", "Frame latency: 1 frame (%s), hardware video: %s",
           frameLatencyWaitable_ ? "waitable swap chain" : "device limit",
           supportsHardwareVideo_ ? "yes" : "no");

    // Disable Alt+Enter fullscreen toggle (we handle it ourselves)
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
}

void D3D11Renderer::CreateSwapChain(IDXGIFactory2* factory, HWND hwnd, int width, int height,
                                    bool tearingSupported) {
    // Newest first (SwapChainPlan in RendererTypes.h): the waitable object
    // (Phase 17: render only when the frame goes straight out), tearing
    // (step 3: present without waiting), then plainer chains
    HRESULT hr = E_FAIL;
    for (const SwapChainAttempt& a : SwapChainPlan(mode_, tearingSupported)) {
        const DXGI_SWAP_EFFECT effect = a.effect == SwapEffect::FlipDiscard ? DXGI_SWAP_EFFECT_FLIP_DISCARD
                                      : a.effect == SwapEffect::FlipSequential ? DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL
                                      : DXGI_SWAP_EFFECT_DISCARD;
        const UINT flags = (a.waitable ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0) |
                           (a.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
        DXGI_SWAP_CHAIN_DESC1 scd = {};
        scd.Width       = static_cast<UINT>(width);
        scd.Height      = static_cast<UINT>(height);
        scd.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.SampleDesc  = { 1, 0 };
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = 2;
        scd.SwapEffect  = effect;
        scd.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;
        scd.Flags       = flags;

        hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd, &scd, nullptr, nullptr, &swapChain_);
        if (SUCCEEDED(hr)) {
            swapChainFlags_ = flags;
            tearingAllowed_ = a.tearing;
            LOGF_INFO("Renderer", "Swap chain: %s", a.name);
            return;
        }
        LOGF_WARN("Renderer", "Swap chain (%s) refused: 0x%08lX", a.name, (unsigned long)hr);
    }
    ThrowIfFailed(hr, "CreateSwapChainForHwnd");
}

std::string D3D11Renderer::ShaderProfile(const char* stage) const {
    // Shader Model 5 needs feature level 11; 10.x devices take SM 4.x bytecode
    D3D_FEATURE_LEVEL level = device_->GetFeatureLevel();
    const char* model = level >= D3D_FEATURE_LEVEL_11_0 ? "5_0"
                      : level >= D3D_FEATURE_LEVEL_10_1 ? "4_1" : "4_0";
    return std::string(stage) + "_" + model;
}

// =============================================================================
// Render Target
// =============================================================================

void D3D11Renderer::CreateRenderTarget() {
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    ThrowIfFailed(hr, "GetBuffer(0)");

    hr = device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &rtv_);
    ThrowIfFailed(hr, "CreateRenderTargetView");
}

void D3D11Renderer::ReleaseRenderTarget() {
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    context_->Flush();
    rtv_.Reset();
}

// =============================================================================
// Shader Compilation
// =============================================================================

void D3D11Renderer::CompileShaders() {
    const char* source = Shaders::kTestPatternHLSL;
    UINT compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
#ifdef _DEBUG
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    // Compile vertex shader
    ComPtr<ID3DBlob> vsBlob, errorBlob;
    HRESULT hr = D3DCompile(
        source, strlen(source),
        "TestPattern.hlsl",
        nullptr, nullptr,
        "VSMain", ShaderProfile("vs").c_str(),
        compileFlags, 0,
        &vsBlob, &errorBlob
    );
    if (FAILED(hr)) {
        std::string errorMsg = "Vertex shader compilation failed";
        if (errorBlob) {
            errorMsg += ": ";
            errorMsg += static_cast<const char*>(errorBlob->GetBufferPointer());
        }
        throw std::runtime_error(errorMsg);
    }

    hr = device_->CreateVertexShader(
        vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
        nullptr, &vertexShader_
    );
    ThrowIfFailed(hr, "CreateVertexShader");

    // Compile pixel shader
    ComPtr<ID3DBlob> psBlob;
    errorBlob.Reset();
    hr = D3DCompile(
        source, strlen(source),
        "TestPattern.hlsl",
        nullptr, nullptr,
        "PSMain", ShaderProfile("ps").c_str(),
        compileFlags, 0,
        &psBlob, &errorBlob
    );
    if (FAILED(hr)) {
        std::string errorMsg = "Pixel shader compilation failed";
        if (errorBlob) {
            errorMsg += ": ";
            errorMsg += static_cast<const char*>(errorBlob->GetBufferPointer());
        }
        throw std::runtime_error(errorMsg);
    }

    hr = device_->CreatePixelShader(
        psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
        nullptr, &pixelShader_
    );
    ThrowIfFailed(hr, "CreatePixelShader");
}

// =============================================================================
// Constant Buffer
// =============================================================================

void D3D11Renderer::CreateConstantBuffer() {
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth      = sizeof(ShaderConstants);
    cbd.Usage           = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags       = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags  = D3D11_CPU_ACCESS_WRITE;

    HRESULT hr = device_->CreateBuffer(&cbd, nullptr, &constantBuffer_);
    ThrowIfFailed(hr, "CreateBuffer (constants)");
}

// =============================================================================
// D2D1 / DirectWrite Resources for Stats Overlay
// =============================================================================

void D3D11Renderer::CreateD2DResources() {
    HRESULT hr;

    // Create D2D1 factory
    hr = D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED,
        IID_PPV_ARGS(&d2dFactory_)
    );
    ThrowIfFailed(hr, "D2D1CreateFactory");

    // Create D2D1 device from DXGI device
    ComPtr<IDXGIDevice> dxgiDevice;
    device_.As(&dxgiDevice);

    hr = d2dFactory_->CreateDevice(dxgiDevice.Get(), &d2dDevice_);
    ThrowIfFailed(hr, "D2D1 CreateDevice");

    // Create D2D1 device context
    hr = d2dDevice_->CreateDeviceContext(
        D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dContext_
    );
    ThrowIfFailed(hr, "D2D1 CreateDeviceContext");

    // Create DirectWrite factory
    hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf())
    );
    ThrowIfFailed(hr, "DWriteCreateFactory");

    // Create text formats
    hr = dwriteFactory_->CreateTextFormat(
        L"Consolas",           // monospace for stats
        nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        14.0f,
        L"en-us",
        &statsTextFormat_
    );
    ThrowIfFailed(hr, "CreateTextFormat (stats)");

    hr = dwriteFactory_->CreateTextFormat(
        L"Segoe UI",
        nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        16.0f,
        L"en-us",
        &titleTextFormat_
    );
    ThrowIfFailed(hr, "CreateTextFormat (title)");

    // Status card: centered title, body and key hints
    auto centered = [this](float size, DWRITE_FONT_WEIGHT weight, ComPtr<IDWriteTextFormat>& out,
                           const char* what) {
        HRESULT r = dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr, weight,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &out);
        ThrowIfFailed(r, what);
        out->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    };
    centered(30.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, statusTitleFormat_, "CreateTextFormat (status title)");
    centered(19.0f, DWRITE_FONT_WEIGHT_NORMAL, statusBodyFormat_, "CreateTextFormat (status body)");
    centered(14.0f, DWRITE_FONT_WEIGHT_NORMAL, statusFooterFormat_, "CreateTextFormat (status footer)");
}

// =============================================================================
// Resize
// =============================================================================

void D3D11Renderer::Resize(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (width == width_ && height == height_) return;

    width_  = width;
    height_ = height;

    // Release D2D target (it holds a ref to the back buffer)
    if (d2dContext_) {
        d2dContext_->SetTarget(nullptr);
    }
    textBrush_.Reset();
    bgBrush_.Reset();
    accentBrush_.Reset();

    // Release D3D11 render target
    ReleaseRenderTarget();

    // Resize swap chain buffers. Failure must not kill the app: flag the
    // device as lost and let the owner rebuild the renderer.
    HRESULT hr = swapChain_->ResizeBuffers(
        0, static_cast<UINT>(width), static_cast<UINT>(height),
        DXGI_FORMAT_UNKNOWN, swapChainFlags_   // flags must match creation
    );
    if (FAILED(hr)) {
        LOGF_ERROR("Renderer", "ResizeBuffers failed (0x%08lX) — recreating renderer",
               (unsigned long)hr);
        deviceLost_ = true;
        return;
    }

    // Recreate render target
    try {
        CreateRenderTarget();
    } catch (const std::exception& e) {
        LOGF_ERROR("Renderer", "%s — recreating renderer", e.what());
        deviceLost_ = true;
    }
}

// =============================================================================
// Render
// =============================================================================

void D3D11Renderer::Render(float time, float frameCount, const std::wstring* overlayText,
                           const StatusCard* status, const PointerSprite* pointer) {
    if (deviceLost_ || !rtv_) return;  // owner recreates the renderer

    // ── 1. Update constant buffer ───────────────────────────────────────────
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = context_->Map(constantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        auto* cb = static_cast<ShaderConstants*>(mapped.pData);
        cb->time        = time;
        cb->resolutionX = static_cast<float>(width_);
        cb->resolutionY = static_cast<float>(height_);
        cb->frameCount  = frameCount;
        context_->Unmap(constantBuffer_.Get(), 0);
    }

    // ── 2. Set up pipeline ──────────────────────────────────────────────────
    context_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);

    // Black for letterbox/pillarbox bars; a calm dark background behind a
    // status card when there is no picture yet
    const bool showVideo = hasVideoFrame_ && frameYSRV_ && frameUVSRV_;
    float black[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    float slate[] = { 0.055f, 0.063f, 0.082f, 1.0f };
    context_->ClearRenderTargetView(rtv_.Get(), (!showVideo && status) ? slate : black);

    D3D11_VIEWPORT viewport = {};
    viewport.MaxDepth = 1.0f;

    // ── 3. Draw fullscreen triangle ─────────────────────────────────────────
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->IASetInputLayout(nullptr);
    if (showVideo) {
        // Letterbox / pillarbox to keep the video's aspect ratio (the mouse
        // mapping in InputCapture uses the same rectangle)
        VideoViewport fit = ComputeVideoViewport((float)width_, (float)height_,
                                                 (float)nv12Width_ / (float)nv12Height_);
        viewport.TopLeftX = fit.x;
        viewport.TopLeftY = fit.y;
        viewport.Width    = fit.width;
        viewport.Height   = fit.height;
        context_->RSSetViewports(1, &viewport);

        // Render NV12 video frame via GPU YUV→RGB shader
        context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
        context_->PSSetShader(nv12PixelShader_.Get(), nullptr, 0);
        context_->PSSetConstantBuffers(1, 1, videoConstants_.GetAddressOf());
        ID3D11ShaderResourceView* srvs[2] = { frameYSRV_.Get(), frameUVSRV_.Get() };
        context_->PSSetShaderResources(0, 2, srvs);
        context_->PSSetSamplers(0, 1, videoSampler_.GetAddressOf());
        context_->Draw(3, 0);

        // Unbind SRVs
        ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
        context_->PSSetShaderResources(0, 2, nullSRVs);
    } else if (!status) {
        // Render test pattern (full window)
        viewport.Width  = (float)width_;
        viewport.Height = (float)height_;
        context_->RSSetViewports(1, &viewport);

        context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
        context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
        context_->PSSetConstantBuffers(0, 1, constantBuffer_.GetAddressOf());
        context_->Draw(3, 0);
    }

    // ── 4. Direct2D: status card and diagnostics overlay ───────────────────
    const bool wantOverlay = overlayText && !overlayText->empty();
    if ((wantOverlay || status) && d2dContext_) {
        ComPtr<IDXGISurface> surface;
        hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&surface));
        ComPtr<ID2D1Bitmap1> d2dTarget;
        if (SUCCEEDED(hr)) {
            D2D1_BITMAP_PROPERTIES1 bitmapProps = {};
            bitmapProps.pixelFormat.format    = DXGI_FORMAT_B8G8R8A8_UNORM;
            bitmapProps.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
            bitmapProps.bitmapOptions         = D2D1_BITMAP_OPTIONS_TARGET
                                              | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
            bitmapProps.dpiX = 96.0f;
            bitmapProps.dpiY = 96.0f;
            hr = d2dContext_->CreateBitmapFromDxgiSurface(surface.Get(), &bitmapProps, &d2dTarget);
        }
        if (SUCCEEDED(hr)) {
            d2dContext_->SetTarget(d2dTarget.Get());
            d2dContext_->BeginDraw();
            // Laid out at 100 %, drawn at the screen's scale (sharp: text is vector)
            d2dContext_->SetTransform(D2D1::Matrix3x2F::Scale(uiScale_, uiScale_));
            if (status) DrawStatusCard(*status, showVideo);
            if (wantOverlay) DrawOverlay(*overlayText);
            d2dContext_->SetTransform(D2D1::Matrix3x2F::Identity());
            d2dContext_->EndDraw();
            d2dContext_->SetTarget(nullptr);
        }
    }

    // ── 5. The Mac's pointer, on top of everything ──────────────────────────
    if (pointer) DrawPointer(*pointer);

    // ── 6. Present: on the next refresh, or at once (step 3, may tear) ──────
    const PresentParams present = ChoosePresent(presentImmediately_, tearingAllowed_);
    hr = swapChain_->Present(present.syncInterval, present.allowTearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    CheckDeviceLost(hr, "Present");
}

bool D3D11Renderer::SetUiScale(float scale) {
    scale = (std::min)((std::max)(scale, 1.0f), 4.0f);
    if (scale == uiScale_) return false;
    uiScale_ = scale;
    return true;
}

// =============================================================================
// Pointer (docs/SMOOTHNESS.md, step 1)
// =============================================================================

void D3D11Renderer::CreatePointerResources() {
    // Optional like the overlay: without it the Mac keeps the pointer in the video
    const char* pointerPS = R"(
        Texture2D    pointerTex : register(t0);
        SamplerState samp       : register(s0);

        struct PSInput {
            float4 pos : SV_Position;
            float2 uv  : TEXCOORD0;
        };

        float4 main(PSInput input) : SV_Target {
            return pointerTex.Sample(samp, input.uv);   // premultiplied
        }
    )";
    ComPtr<ID3DBlob> psBlob, errBlob;
    HRESULT hr = D3DCompile(pointerPS, strlen(pointerPS), "pointerPS",
        nullptr, nullptr, "main", ShaderProfile("ps").c_str(), 0, 0, &psBlob, &errBlob);
    if (FAILED(hr)) {
        LOGF_WARN("Renderer", "Pointer shader compilation failed: %s — pointer stays in the video",
                  errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "(no message)");
        return;
    }

    // out = pointer + (1 - pointer alpha) × picture
    D3D11_BLEND_DESC blend = {};
    blend.RenderTarget[0].BlendEnable           = TRUE;
    blend.RenderTarget[0].SrcBlend              = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    D3D11_SAMPLER_DESC sampler = {};
    sampler.Filter   = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;

    ComPtr<ID3D11PixelShader> shader;
    if (FAILED(hr = device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                               nullptr, &shader)) ||
        FAILED(hr = device_->CreateBlendState(&blend, &pointerBlend_)) ||
        FAILED(hr = device_->CreateSamplerState(&sampler, &pointerSampler_))) {
        LOGF_WARN("Renderer", "Pointer resources failed (0x%08lX) — pointer stays in the video",
                  (unsigned long)hr);
        return;
    }
    pointerPixelShader_ = shader;
}

void D3D11Renderer::SetPointerImage(const uint8_t* bgra, int width, int height) {
    pointerSRV_.Reset();
    pointerWidth_ = pointerHeight_ = 0;
    if (!bgra || width < 1 || height < 1 || deviceLost_ || !pointerPixelShader_) return;

    // RGBA is sampleable on every feature level
    std::vector<uint8_t> rgba(bgra, bgra + static_cast<size_t>(width) * height * 4);
    for (size_t i = 0; i < rgba.size(); i += 4) std::swap(rgba[i], rgba[i + 2]);

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = { rgba.data(), static_cast<UINT>(width * 4), 0 };

    ComPtr<ID3D11Texture2D> texture;
    HRESULT hr = device_->CreateTexture2D(&desc, &init, &texture);
    if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(texture.Get(), nullptr, &pointerSRV_);
    if (FAILED(hr)) {
        CheckDeviceLost(hr, "pointer texture");
        char code[16];
        snprintf(code, sizeof(code), "0x%08lX", (unsigned long)hr);
        LOG_EVERY_MS(5000, LogLevel::Warning, "Renderer") << "Pointer texture failed: " << code;
        pointerSRV_.Reset();
        return;
    }
    pointerWidth_ = width;
    pointerHeight_ = height;
}

void D3D11Renderer::DrawPointer(const PointerSprite& pointer) {
    if (!pointerSRV_ || !pointerPixelShader_ || pointer.width != pointerWidth_ ||
        pointer.height != pointerHeight_) {
        return;
    }
    // The fullscreen-triangle vertex shader fills the viewport: a viewport the
    // size of the image draws it 1:1, also partly outside the window
    D3D11_VIEWPORT viewport = {};
    viewport.TopLeftX = static_cast<float>(pointer.x);
    viewport.TopLeftY = static_cast<float>(pointer.y);
    viewport.Width    = static_cast<float>(pointer.width);
    viewport.Height   = static_cast<float>(pointer.height);
    viewport.MaxDepth = 1.0f;

    const float blendFactor[4] = { 0, 0, 0, 0 };
    context_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);   // Direct2D may have drawn in between
    context_->OMSetBlendState(pointerBlend_.Get(), blendFactor, 0xFFFFFFFF);
    context_->RSSetViewports(1, &viewport);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->IASetInputLayout(nullptr);
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context_->PSSetShader(pointerPixelShader_.Get(), nullptr, 0);
    context_->PSSetShaderResources(0, 1, pointerSRV_.GetAddressOf());
    context_->PSSetSamplers(0, 1, pointerSampler_.GetAddressOf());
    context_->Draw(3, 0);

    // The video and test pattern draw opaque
    ID3D11ShaderResourceView* none = nullptr;
    context_->PSSetShaderResources(0, 1, &none);
    context_->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFF);
}

// =============================================================================
// Direct2D text (called between BeginDraw and EndDraw)
// =============================================================================

void D3D11Renderer::DrawStatusCard(const StatusCard& status, bool overVideo) {
    ComPtr<ID2D1SolidColorBrush> titleBrush, bodyBrush, footerBrush, cardBrush, accentBrush;
    d2dContext_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.97f), &titleBrush);
    d2dContext_->CreateSolidColorBrush(D2D1::ColorF(0.80f, 0.83f, 0.88f, 1.0f), &bodyBrush);
    d2dContext_->CreateSolidColorBrush(D2D1::ColorF(0.55f, 0.58f, 0.64f, 1.0f), &footerBrush);
    d2dContext_->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.13f, 0.16f, overVideo ? 0.88f : 1.0f), &cardBrush);
    d2dContext_->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.7f, 1.0f, 1.0f), &accentBrush);

    // In 100 % units: the transform scales them to the screen
    const float w = width_ / uiScale_, h = height_ / uiScale_;
    const float cardWidth = (std::min)(640.0f, w - 40.0f);
    const float padding = 28.0f;

    // Measure the body so the card fits it
    ComPtr<IDWriteTextLayout> body;
    float bodyHeight = 0;
    if (!status.body.empty() &&
        SUCCEEDED(dwriteFactory_->CreateTextLayout(status.body.c_str(), (UINT32)status.body.size(),
                                                   statusBodyFormat_.Get(), cardWidth - 2 * padding,
                                                   h, &body))) {
        DWRITE_TEXT_METRICS m{};
        body->GetMetrics(&m);
        bodyHeight = m.height;
    }
    const float titleHeight = 44.0f;
    const float cardHeight = padding + titleHeight + 10.0f + bodyHeight + padding;
    const float left = (w - cardWidth) / 2.0f, top = (h - cardHeight) / 2.0f;

    d2dContext_->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(left, top, left + cardWidth, top + cardHeight), 14.0f, 14.0f),
        cardBrush.Get());
    d2dContext_->FillRectangle(D2D1::RectF(left + padding, top + padding + titleHeight + 2,
                                           left + padding + 48.0f, top + padding + titleHeight + 4),
                               accentBrush.Get());
    d2dContext_->DrawText(status.title.c_str(), (UINT32)status.title.size(), statusTitleFormat_.Get(),
                          D2D1::RectF(left + padding, top + padding, left + cardWidth - padding,
                                      top + padding + titleHeight),
                          titleBrush.Get());
    if (body) {
        d2dContext_->DrawTextLayout(D2D1::Point2F(left + padding, top + padding + titleHeight + 10.0f),
                                    body.Get(), bodyBrush.Get());
    }
    if (!status.footer.empty()) {
        d2dContext_->DrawText(status.footer.c_str(), (UINT32)status.footer.size(), statusFooterFormat_.Get(),
                              D2D1::RectF(20.0f, h - 44.0f, w - 20.0f, h - 12.0f), footerBrush.Get());
    }
}

void D3D11Renderer::DrawOverlay(const std::wstring& text) {
    // Lay the text out first so the panel fits it exactly
    const float margin = 12.0f, padding = 10.0f, titleHeight = 24.0f;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwriteFactory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
            statsTextFormat_.Get(), width_ / uiScale_, height_ / uiScale_, &layout))) {
        return;
    }
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    float panelWidth  = (std::max)(metrics.widthIncludingTrailingWhitespace, 400.0f) + 2 * padding;
    float panelHeight = metrics.height + titleHeight + 2 * padding;

    ComPtr<ID2D1SolidColorBrush> textBrush, bgBrush, accentBrush;
    d2dContext_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f), &textBrush);
    d2dContext_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.70f), &bgBrush);
    d2dContext_->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.7f, 1.0f, 0.95f), &accentBrush);

    d2dContext_->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(margin, margin, margin + panelWidth, margin + panelHeight), 6.0f, 6.0f),
        bgBrush.Get());
    const wchar_t* title = L"Diagnostics   F2 hide · F11 fullscreen · Esc quit";
    d2dContext_->DrawText(title, static_cast<UINT32>(wcslen(title)), titleTextFormat_.Get(),
                          D2D1::RectF(margin + padding, margin + 6, margin + panelWidth - padding,
                                      margin + titleHeight + 4),
                          accentBrush.Get());
    d2dContext_->DrawLine(D2D1::Point2F(margin + padding, margin + titleHeight + 6),
                          D2D1::Point2F(margin + panelWidth - padding, margin + titleHeight + 6),
                          accentBrush.Get(), 0.5f);
    d2dContext_->DrawTextLayout(D2D1::Point2F(margin + padding, margin + titleHeight + padding),
                                layout.Get(), textBrush.Get());
}

// =============================================================================
// Frame statistics (Phase 16)
// =============================================================================

bool D3D11Renderer::WaitForFrameSlot(DWORD timeoutMs) {
    if (!frameLatencyWaitable_ || deviceLost_) return true;
    return WaitForSingleObjectEx(frameLatencyWaitable_, timeoutMs, TRUE) == WAIT_OBJECT_0;
}

bool D3D11Renderer::GetLastPresentCount(uint32_t& presentCount) const {
    if (!swapChain_ || deviceLost_) return false;
    UINT count = 0;
    if (FAILED(swapChain_->GetLastPresentCount(&count))) return false;
    presentCount = count;
    return true;
}

bool D3D11Renderer::GetLastDisplayedPresent(uint32_t& presentCount, int64_t& displayUs) const {
    if (!swapChain_ || deviceLost_) return false;
    // Flip-model swap chains report statistics in windowed mode too. Fails with
    // DXGI_ERROR_FRAME_STATISTICS_DISJOINT around mode changes — just skip.
    DXGI_FRAME_STATISTICS stats{};
    if (FAILED(swapChain_->GetFrameStatistics(&stats)) || stats.SyncQPCTime.QuadPart == 0) {
        return false;
    }
    presentCount = stats.PresentCount;
    displayUs = PipelineClock::QpcToUs(stats.SyncQPCTime.QuadPart);
    return true;
}

// =============================================================================
// NV12 GPU Video Rendering
// =============================================================================

void D3D11Renderer::CreateNV12Resources() {
    // Bilinear sampler for Y and UV textures
    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    device_->CreateSamplerState(&samplerDesc, &videoSampler_);

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(VideoConstantsData);
    cbd.Usage     = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    device_->CreateBuffer(&cbd, nullptr, &videoConstants_);
    videoConstantsData_.uvScale[0] = videoConstantsData_.uvScale[1] = 1.0f;
    SetVideoColor(VideoColor{}, 0);   // full-range BT.709 until the stream says otherwise

    // YCbCr → RGB. Range and matrix come from the stream's SPS
    // (VideoColor.h); the Mac app sends full-range BT.709.
    const char* nv12PS = R"(
        Texture2D    yTex  : register(t0);
        Texture2D    uvTex : register(t1);
        SamplerState samp  : register(s0);

        cbuffer VideoConstants : register(b1) {
            float2 uvScale;            // part of the texture holding the picture (decoder
                                       // surfaces are padded, e.g. 1088 rows for 1080p)
            float  yOffset, yScale;    // Y' = (Y - yOffset) * yScale
            float  cOffset, cScale;    // C' = (C - cOffset) * cScale
            float  crR, cbG, crG, cbB; // matrix (BT.709 or BT.601)
        };

        struct PSInput {
            float4 pos : SV_Position;
            float2 uv  : TEXCOORD0;
        };

        float4 main(PSInput input) : SV_Target {
            float2 texCoord = input.uv * uvScale;
            float y = (yTex.Sample(samp, texCoord).r - yOffset) * yScale;
            float2 c = (uvTex.Sample(samp, texCoord).rg - cOffset) * cScale;   // (Cb, Cr)

            float R = y + crR * c.y;
            float G = y - cbG * c.x - crG * c.y;
            float B = y + cbB * c.x;
            return float4(saturate(float3(R, G, B)), 1.0);
        }
    )";

    ComPtr<ID3DBlob> psBlob, errBlob;
    HRESULT hr = D3DCompile(nv12PS, strlen(nv12PS), "nv12PS",
        nullptr, nullptr, "main", ShaderProfile("ps").c_str(), 0, 0, &psBlob, &errBlob);
    if (FAILED(hr)) {
        LOGF_ERROR("Renderer", "NV12 shader compilation failed: %s",
                   errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "(no message)");
        return;
    }

    device_->CreatePixelShader(
        psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
        nullptr, &nv12PixelShader_
    );
}

void D3D11Renderer::EnsureNV12Textures(uint32_t width, uint32_t height) {
    uint32_t evenHeightWanted = (height + 1) & ~1;
    if (yTexture_) {
        D3D11_TEXTURE2D_DESC have;
        yTexture_->GetDesc(&have);
        if (have.Width == width && have.Height == evenHeightWanted) return;
    }

    yTexture_.Reset();
    uvTexture_.Reset();
    ySRV_.Reset();
    uvSRV_.Reset();

    uint32_t evenHeight = (height + 1) & ~1;  // NV12 needs even height

    // Y plane: full resolution, R8_UNORM (one byte per pixel)
    D3D11_TEXTURE2D_DESC yDesc = {};
    yDesc.Width  = width;
    yDesc.Height = evenHeight;
    yDesc.MipLevels = 1;
    yDesc.ArraySize = 1;
    yDesc.Format = DXGI_FORMAT_R8_UNORM;
    yDesc.SampleDesc.Count = 1;
    yDesc.Usage = D3D11_USAGE_DYNAMIC;
    yDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    yDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    HRESULT hr = device_->CreateTexture2D(&yDesc, nullptr, &yTexture_);
    if (FAILED(hr)) return;

    // UV plane: half width, half height, R8G8_UNORM (U+V interleaved)
    D3D11_TEXTURE2D_DESC uvDesc = yDesc;
    uvDesc.Width  = width / 2;
    uvDesc.Height = evenHeight / 2;
    uvDesc.Format = DXGI_FORMAT_R8G8_UNORM;

    hr = device_->CreateTexture2D(&uvDesc, nullptr, &uvTexture_);
    if (FAILED(hr)) return;

    // Create SRVs
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    srvDesc.Format = DXGI_FORMAT_R8_UNORM;
    hr = device_->CreateShaderResourceView(yTexture_.Get(), &srvDesc, &ySRV_);
    if (FAILED(hr)) return;

    srvDesc.Format = DXGI_FORMAT_R8G8_UNORM;
    hr = device_->CreateShaderResourceView(uvTexture_.Get(), &srvDesc, &uvSRV_);
    if (FAILED(hr)) return;

    LOGF_DEBUG("Renderer", "NV12 textures: Y=%ux%u, UV=%ux%u",
        width, evenHeight, width/2, evenHeight/2);
}

void D3D11Renderer::UpdateFrameNV12(const uint8_t* yData, const uint8_t* uvData,
                                     int nv12Stride, uint32_t frameWidth,
                                     uint32_t frameHeight) {
    if (deviceLost_) return;
    EnsureNV12Textures(frameWidth, frameHeight);
    if (!yTexture_ || !uvTexture_) return;

    uint32_t evenHeight = (frameHeight + 1) & ~1;

    // Upload Y plane
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = context_->Map(yTexture_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        for (uint32_t row = 0; row < frameHeight; row++) {
            memcpy(
                static_cast<uint8_t*>(mapped.pData) + row * mapped.RowPitch,
                yData + row * nv12Stride,
                frameWidth
            );
        }
        // Fill padding rows with black (Y=0 for full-range) to avoid green line
        for (uint32_t row = frameHeight; row < evenHeight; row++) {
            memset(
                static_cast<uint8_t*>(mapped.pData) + row * mapped.RowPitch,
                0, frameWidth
            );
        }
        context_->Unmap(yTexture_.Get(), 0);
    }

    // Upload UV plane
    hr = context_->Map(uvTexture_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        uint32_t uvHeight = evenHeight / 2;
        uint32_t uvDataRows = (frameHeight + 1) / 2;  // Actual UV rows with data
        for (uint32_t row = 0; row < uvDataRows; row++) {
            memcpy(
                static_cast<uint8_t*>(mapped.pData) + row * mapped.RowPitch,
                uvData + row * nv12Stride,
                frameWidth
            );
        }
        // Fill UV padding rows with neutral chroma (128=0.5 → no color)
        for (uint32_t row = uvDataRows; row < uvHeight; row++) {
            memset(
                static_cast<uint8_t*>(mapped.pData) + row * mapped.RowPitch,
                128, frameWidth
            );
        }
        context_->Unmap(uvTexture_.Get(), 0);
    }

    nv12Width_ = frameWidth;
    nv12Height_ = frameHeight;
    frameYSRV_ = ySRV_;
    frameUVSRV_ = uvSRV_;
    SetVideoUVScale(1.0f, static_cast<float>(frameHeight) / static_cast<float>(evenHeight));
    hasVideoFrame_ = true;
}

void D3D11Renderer::SetVideoUVScale(float x, float y) {
    if (x == videoConstantsData_.uvScale[0] && y == videoConstantsData_.uvScale[1]) return;
    videoConstantsData_.uvScale[0] = x;
    videoConstantsData_.uvScale[1] = y;
    UploadVideoConstants();
}

void D3D11Renderer::SetVideoColor(const VideoColor& color, uint32_t height) {
    const VideoColorConstants k = ColorConstantsFor(color, height);
    VideoConstantsData& d = videoConstantsData_;
    d.yOffset = k.yOffset;
    d.yScale = k.yScale;
    d.cOffset = k.cOffset;
    d.cScale = k.cScale;
    d.crR = k.crR;
    d.cbG = k.cbG;
    d.crG = k.crG;
    d.cbB = k.cbB;
    UploadVideoConstants();
}

void D3D11Renderer::UploadVideoConstants() {
    if (!videoConstants_ || deviceLost_) return;
    context_->UpdateSubresource(videoConstants_.Get(), 0, nullptr, &videoConstantsData_, 0, 0);
}

bool D3D11Renderer::UpdateFrameTexture(ID3D11Texture2D* source, UINT subresource,
                                       uint32_t frameWidth, uint32_t frameHeight) {
    if (deviceLost_ || !source) return false;

    D3D11_TEXTURE2D_DESC srcDesc;
    source->GetDesc(&srcDesc);
    if (srcDesc.Format != DXGI_FORMAT_NV12) return false;

    // Our copy target: same size as the decoder surface so a whole-subresource
    // copy is valid; the shader crops the padding with uvScale
    D3D11_TEXTURE2D_DESC have{};
    if (hwTexture_) hwTexture_->GetDesc(&have);
    if (!hwTexture_ || have.Width != srcDesc.Width || have.Height != srcDesc.Height) {
        hwTexture_.Reset();
        hwYSRV_.Reset();
        hwUVSRV_.Reset();

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = srcDesc.Width;
        desc.Height = srcDesc.Height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &hwTexture_);
        if (FAILED(hr)) {
            CheckDeviceLost(hr, "CreateTexture2D (NV12)");
            return false;
        }

        // Plane views: R8 = luma (full size), R8G8 = interleaved chroma (half size)
        D3D11_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        srv.Format = DXGI_FORMAT_R8_UNORM;
        if (FAILED(device_->CreateShaderResourceView(hwTexture_.Get(), &srv, &hwYSRV_))) return false;
        srv.Format = DXGI_FORMAT_R8G8_UNORM;
        if (FAILED(device_->CreateShaderResourceView(hwTexture_.Get(), &srv, &hwUVSRV_))) return false;

        LOGF_INFO("Renderer", "Hardware frame texture: %ux%u NV12 (picture %ux%u)",
               srcDesc.Width, srcDesc.Height, frameWidth, frameHeight);
    }

    // Copy before the decoder reuses its surface — stays on the GPU
    context_->CopySubresourceRegion(hwTexture_.Get(), 0, 0, 0, 0, source, subresource, nullptr);

    nv12Width_ = frameWidth;
    nv12Height_ = frameHeight;
    frameYSRV_ = hwYSRV_;
    frameUVSRV_ = hwUVSRV_;
    SetVideoUVScale(static_cast<float>(frameWidth) / static_cast<float>(srcDesc.Width),
                    static_cast<float>(frameHeight) / static_cast<float>(srcDesc.Height));
    hasVideoFrame_ = true;
    return true;
}
