#pragma once
// =============================================================================
// D3D11Renderer.h — Direct3D 11 GPU rendering pipeline
// =============================================================================
// Manages the D3D11 device, swap chain, shaders, and rendering.
// Renders an animated test pattern via GPU shader.
// Includes D2D1/DirectWrite overlay for on-screen statistics.
// =============================================================================

#include <d3d11.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <string>

class FrameStats;

class D3D11Renderer {
public:
    D3D11Renderer(HWND hwnd, int width, int height);
    ~D3D11Renderer() = default; // ComPtr handles cleanup

    // Render one frame: test pattern + optional stats overlay
    void Render(float time, float frameCount, const FrameStats* stats = nullptr);

    // Handle window resize
    void Resize(int width, int height);

    // Get GPU adapter name
    std::wstring GetAdapterName() const { return adapterName_; }

    // Upload a decoded BGRA frame for rendering instead of the test pattern
    void UpdateFrame(const uint8_t* bgraData, uint32_t frameWidth,
                     uint32_t frameHeight, uint32_t stride);

    // Whether we have a video frame to display
    bool HasVideoFrame() const { return hasVideoFrame_; }

private:
    template<typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    void CreateDeviceAndSwapChain(HWND hwnd, int width, int height);
    void CreateRenderTarget();
    void CompileShaders();
    void CreateConstantBuffer();
    void CreateD2DResources();
    void ReleaseRenderTarget();

    // ── D3D11 core ──
    ComPtr<ID3D11Device>           device_;
    ComPtr<ID3D11DeviceContext>    context_;
    ComPtr<IDXGISwapChain1>        swapChain_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11VertexShader>     vertexShader_;
    ComPtr<ID3D11PixelShader>      pixelShader_;
    ComPtr<ID3D11Buffer>           constantBuffer_;

    // ── D2D1/DirectWrite overlay ──
    ComPtr<ID2D1Factory1>          d2dFactory_;
    ComPtr<ID2D1Device>            d2dDevice_;
    ComPtr<ID2D1DeviceContext>     d2dContext_;
    ComPtr<IDWriteFactory>         dwriteFactory_;
    ComPtr<IDWriteTextFormat>      statsTextFormat_;
    ComPtr<IDWriteTextFormat>      titleTextFormat_;
    ComPtr<ID2D1SolidColorBrush>   textBrush_;
    ComPtr<ID2D1SolidColorBrush>   bgBrush_;
    ComPtr<ID2D1SolidColorBrush>   accentBrush_;

    HWND hwnd_ = nullptr;
    int  width_  = 0;
    int  height_ = 0;
    std::wstring adapterName_;

    // ── Video frame texture ──
    ComPtr<ID3D11Texture2D>          videoTexture_;
    ComPtr<ID3D11ShaderResourceView> videoSRV_;
    ComPtr<ID3D11SamplerState>       videoSampler_;
    ComPtr<ID3D11PixelShader>        videoPixelShader_;
    uint32_t videoWidth_  = 0;
    uint32_t videoHeight_ = 0;
    bool hasVideoFrame_ = false;
    void CreateVideoResources();
    void EnsureVideoTexture(uint32_t width, uint32_t height);

    // Shader constant buffer layout (must be 16-byte aligned)
    struct alignas(16) ShaderConstants {
        float time;
        float resolutionX;
        float resolutionY;
        float frameCount;
    };
};
