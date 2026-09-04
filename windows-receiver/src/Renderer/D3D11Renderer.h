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

    // Upload raw NV12 Y and UV planes for GPU-side YUV→RGB conversion
    void UpdateFrameNV12(const uint8_t* yData, const uint8_t* uvData,
                         int nv12Stride, uint32_t frameWidth, uint32_t frameHeight);

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

    // ── NV12 GPU video rendering ──
    ComPtr<ID3D11Texture2D>          yTexture_;      // Y plane (R8_UNORM)
    ComPtr<ID3D11Texture2D>          uvTexture_;     // UV plane (R8G8_UNORM, half-res)
    ComPtr<ID3D11ShaderResourceView> ySRV_;
    ComPtr<ID3D11ShaderResourceView> uvSRV_;
    ComPtr<ID3D11SamplerState>       videoSampler_;
    ComPtr<ID3D11PixelShader>        nv12PixelShader_;
    uint32_t nv12Width_  = 0;
    uint32_t nv12Height_ = 0;
    bool hasVideoFrame_ = false;
    void CreateNV12Resources();
    void EnsureNV12Textures(uint32_t width, uint32_t height);

    // Shader constant buffer layout (must be 16-byte aligned)
    struct alignas(16) ShaderConstants {
        float time;
        float resolutionX;
        float resolutionY;
        float frameCount;
    };
};
