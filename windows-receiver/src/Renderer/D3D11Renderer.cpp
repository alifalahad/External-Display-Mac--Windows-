// =============================================================================
// D3D11Renderer.cpp — Direct3D 11 rendering pipeline implementation
// =============================================================================

#include "Renderer/D3D11Renderer.h"
#include "Renderer/TestPatternShader.h"
#include "Performance/FrameStats.h"

#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <stdexcept>
#include <string>
#include <cstdio>

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

D3D11Renderer::D3D11Renderer(HWND hwnd, int width, int height)
    : hwnd_(hwnd), width_(width), height_(height)
{
    CreateDeviceAndSwapChain(hwnd, width, height);
    CreateRenderTarget();
    CompileShaders();
    CreateConstantBuffer();
    CreateD2DResources();
    CreateNV12Resources();
}

// =============================================================================
// Device & Swap Chain
// =============================================================================

void D3D11Renderer::CreateDeviceAndSwapChain(HWND hwnd, int width, int height) {
    // 1. Create D3D11 device with BGRA support (needed for D2D interop)
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
    };

    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDevice(
        nullptr,                    // default adapter
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,                    // no software rasterizer
        flags,
        featureLevels,
        _countof(featureLevels),
        D3D11_SDK_VERSION,
        &device_,
        &featureLevel,
        &context_
    );
    ThrowIfFailed(hr, "D3D11CreateDevice");

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

    // 3. Create swap chain
    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width       = static_cast<UINT>(width);
    scd.Height      = static_cast<UINT>(height);
    scd.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc  = { 1, 0 };
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;

    hr = factory->CreateSwapChainForHwnd(
        device_.Get(), hwnd, &scd, nullptr, nullptr, &swapChain_
    );
    ThrowIfFailed(hr, "CreateSwapChainForHwnd");

    // Disable Alt+Enter fullscreen toggle (we handle it ourselves)
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
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
        "VSMain", "vs_5_0",
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
        "PSMain", "ps_5_0",
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

    // Resize swap chain buffers
    HRESULT hr = swapChain_->ResizeBuffers(
        0, static_cast<UINT>(width), static_cast<UINT>(height),
        DXGI_FORMAT_UNKNOWN, 0
    );
    ThrowIfFailed(hr, "ResizeBuffers");

    // Recreate render target
    CreateRenderTarget();
}

// =============================================================================
// Render
// =============================================================================

void D3D11Renderer::Render(float time, float frameCount, const FrameStats* stats) {
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

    // Clear to black (for letterbox/pillarbox bars)
    float black[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    context_->ClearRenderTargetView(rtv_.Get(), black);

    D3D11_VIEWPORT viewport = {};
    viewport.MaxDepth = 1.0f;

    // ── 3. Draw fullscreen triangle ─────────────────────────────────────────
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->IASetInputLayout(nullptr);
    if (hasVideoFrame_ && ySRV_ && uvSRV_) {
        // Calculate letterboxed viewport to maintain video aspect ratio
        float videoAspect  = (float)nv12Width_ / (float)nv12Height_;
        float windowAspect = (float)width_ / (float)height_;

        if (windowAspect > videoAspect) {
            // Window wider than video → pillarbox (black bars on sides)
            viewport.Height = (float)height_;
            viewport.Width  = (float)height_ * videoAspect;
            viewport.TopLeftX = ((float)width_ - viewport.Width) / 2.0f;
        } else {
            // Window taller than video → letterbox (black bars top/bottom)
            viewport.Width  = (float)width_;
            viewport.Height = (float)width_ / videoAspect;
            viewport.TopLeftY = ((float)height_ - viewport.Height) / 2.0f;
        }
        context_->RSSetViewports(1, &viewport);

        // Render NV12 video frame via GPU YUV→RGB shader
        context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
        context_->PSSetShader(nv12PixelShader_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[2] = { ySRV_.Get(), uvSRV_.Get() };
        context_->PSSetShaderResources(0, 2, srvs);
        context_->PSSetSamplers(0, 1, videoSampler_.GetAddressOf());
        context_->Draw(3, 0);

        // Unbind SRVs
        ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
        context_->PSSetShaderResources(0, 2, nullSRVs);
    } else {
        // Render test pattern (full window)
        viewport.Width  = (float)width_;
        viewport.Height = (float)height_;
        context_->RSSetViewports(1, &viewport);

        context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
        context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
        context_->PSSetConstantBuffers(0, 1, constantBuffer_.GetAddressOf());
        context_->Draw(3, 0);
    }

    // ── 4. D2D stats overlay ────────────────────────────────────────────────
    if (stats && d2dContext_) {
        // Get current back buffer as DXGI surface for D2D target
        ComPtr<IDXGISurface> surface;
        hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&surface));
        if (SUCCEEDED(hr)) {
            // Create D2D bitmap from the surface
            D2D1_BITMAP_PROPERTIES1 bitmapProps = {};
            bitmapProps.pixelFormat.format    = DXGI_FORMAT_B8G8R8A8_UNORM;
            bitmapProps.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
            bitmapProps.bitmapOptions         = D2D1_BITMAP_OPTIONS_TARGET
                                              | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
            bitmapProps.dpiX = 96.0f;
            bitmapProps.dpiY = 96.0f;

            ComPtr<ID2D1Bitmap1> d2dTarget;
            hr = d2dContext_->CreateBitmapFromDxgiSurface(
                surface.Get(), &bitmapProps, &d2dTarget
            );

            if (SUCCEEDED(hr)) {
                d2dContext_->SetTarget(d2dTarget.Get());
                d2dContext_->BeginDraw();

                // Create brushes (recreated each frame since target changes)
                ComPtr<ID2D1SolidColorBrush> textBrush, bgBrush, accentBrush;
                d2dContext_->CreateSolidColorBrush(
                    D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f), &textBrush
                );
                d2dContext_->CreateSolidColorBrush(
                    D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.65f), &bgBrush
                );
                d2dContext_->CreateSolidColorBrush(
                    D2D1::ColorF(0.2f, 0.7f, 1.0f, 0.95f), &accentBrush
                );

                // Stats panel dimensions
                float panelWidth  = 340.0f;
                float panelHeight = 150.0f;
                float margin      = 12.0f;
                float panelX      = margin;
                float panelY      = margin;

                // Draw background rectangle
                D2D1_ROUNDED_RECT panelRect = D2D1::RoundedRect(
                    D2D1::RectF(panelX, panelY,
                                panelX + panelWidth, panelY + panelHeight),
                    6.0f, 6.0f
                );
                d2dContext_->FillRoundedRectangle(panelRect, bgBrush.Get());

                // Draw title
                D2D1_RECT_F titleRect = D2D1::RectF(
                    panelX + 10, panelY + 6,
                    panelX + panelWidth - 10, panelY + 28
                );
                d2dContext_->DrawText(
                    L"External Display Receiver", 25,
                    titleTextFormat_.Get(), titleRect, accentBrush.Get()
                );

                // Draw separator line
                d2dContext_->DrawLine(
                    D2D1::Point2F(panelX + 10, panelY + 30),
                    D2D1::Point2F(panelX + panelWidth - 10, panelY + 30),
                    accentBrush.Get(), 0.5f
                );

                // Format stats text
                wchar_t statsText[512];
                float dropPct = (stats->GetTotalFrames() > 0)
                    ? (static_cast<float>(stats->GetDroppedFrames())
                       / static_cast<float>(stats->GetTotalFrames()) * 100.0f)
                    : 0.0f;

                swprintf_s(statsText, sizeof(statsText) / sizeof(statsText[0]),
                    L"FPS:         %.1f\n"
                    L"Frame Time:  %.2f ms (avg)\n"
                    L"             %.2f ms (min)  %.2f ms (max)\n"
                    L"Dropped:     %llu / %llu (%.2f%%)\n"
                    L"Resolution:  %d x %d\n"
                    L"GPU:         %s",
                    stats->GetFPS(),
                    stats->GetAvgFrameTimeMs(),
                    stats->GetMinFrameTimeMs(), stats->GetMaxFrameTimeMs(),
                    stats->GetDroppedFrames(), stats->GetTotalFrames(), dropPct,
                    width_, height_,
                    adapterName_.c_str()
                );

                D2D1_RECT_F statsRect = D2D1::RectF(
                    panelX + 10, panelY + 34,
                    panelX + panelWidth - 10, panelY + panelHeight - 6
                );
                d2dContext_->DrawText(
                    statsText, static_cast<UINT32>(wcslen(statsText)),
                    statsTextFormat_.Get(), statsRect, textBrush.Get()
                );

                // Draw key hints at bottom-right
                const wchar_t* hints = L"F11: Toggle Fullscreen  |  ESC: Exit";
                D2D1_RECT_F hintsRect = D2D1::RectF(
                    static_cast<float>(width_) - 320.0f,
                    static_cast<float>(height_) - 28.0f,
                    static_cast<float>(width_) - 10.0f,
                    static_cast<float>(height_) - 6.0f
                );
                d2dContext_->DrawText(
                    hints, static_cast<UINT32>(wcslen(hints)),
                    statsTextFormat_.Get(), hintsRect, textBrush.Get()
                );

                d2dContext_->EndDraw();
                d2dContext_->SetTarget(nullptr);
            }
        }
    }

    // ── 5. Present (VSync) ──────────────────────────────────────────────────
    swapChain_->Present(1, 0);  // SyncInterval=1 → VSync
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

    // BT.709 YUV→RGB pixel shader (limited range)
    const char* nv12PS = R"(
        Texture2D    yTex  : register(t0);
        Texture2D    uvTex : register(t1);
        SamplerState samp  : register(s0);

        struct PSInput {
            float4 pos : SV_Position;
            float2 uv  : TEXCOORD0;
        };

        float4 main(PSInput input) : SV_Target {
            float y  = yTex.Sample(samp, input.uv).r;
            float2 uv_val = uvTex.Sample(samp, input.uv).rg;

            // NV12: .r = Cb (U), .g = Cr (V)
            float cb = uv_val.r;
            float cr = uv_val.g;

            // BT.709 limited-range YCbCr → RGB
            // Y  : [16/255, 235/255] → [0, 1]
            // CbCr: [16/255, 240/255] → [-0.5, 0.5]
            float Y  = (y  - 16.0 / 255.0) * (255.0 / 219.0);
            float Cb = (cb - 128.0 / 255.0) * (255.0 / 224.0);
            float Cr = (cr - 128.0 / 255.0) * (255.0 / 224.0);

            // BT.709 matrix
            float R = Y + 1.5748 * Cr;
            float G = Y - 0.1873 * Cb - 0.4681 * Cr;
            float B = Y + 1.8556 * Cb;

            return float4(saturate(float3(R, G, B)), 1.0);
        }
    )";

    ComPtr<ID3DBlob> psBlob, errBlob;
    HRESULT hr = D3DCompile(nv12PS, strlen(nv12PS), "nv12PS",
        nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, &errBlob);
    if (FAILED(hr)) {
        if (errBlob) {
            OutputDebugStringA((char*)errBlob->GetBufferPointer());
        }
        return;
    }

    device_->CreatePixelShader(
        psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
        nullptr, &nv12PixelShader_
    );
}

void D3D11Renderer::EnsureNV12Textures(uint32_t width, uint32_t height) {
    if (nv12Width_ == width && nv12Height_ == height && yTexture_) {
        return;
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

    nv12Width_ = width;
    nv12Height_ = height;

    printf("[Renderer] NV12 textures: Y=%ux%u, UV=%ux%u\n",
        width, evenHeight, width/2, evenHeight/2);
}

void D3D11Renderer::UpdateFrameNV12(const uint8_t* yData, const uint8_t* uvData,
                                     int nv12Stride, uint32_t frameWidth,
                                     uint32_t frameHeight) {
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
        context_->Unmap(yTexture_.Get(), 0);
    }

    // Upload UV plane (pointer provided directly by decoder — no offset guessing)
    hr = context_->Map(uvTexture_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        uint32_t uvHeight = evenHeight / 2;
        for (uint32_t row = 0; row < uvHeight; row++) {
            memcpy(
                static_cast<uint8_t*>(mapped.pData) + row * mapped.RowPitch,
                uvData + row * nv12Stride,
                frameWidth  // UV row = width bytes (U0V0 U1V1 ...)
            );
        }
        context_->Unmap(uvTexture_.Get(), 0);
    }

    hasVideoFrame_ = true;
}
