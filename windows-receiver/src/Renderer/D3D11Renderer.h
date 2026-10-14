#pragma once
// =============================================================================
// D3D11Renderer.h — Direct3D 11 GPU rendering pipeline
// =============================================================================
// Manages the D3D11 device, swap chain, shaders, and rendering.
// Renders an animated test pattern via GPU shader.
// Includes D2D1/DirectWrite overlay for on-screen diagnostics, and exposes DXGI
// frame statistics (when the frame actually reached the screen).
// =============================================================================

#include <d3d11.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>

/// Which GPU renders on hybrid (Intel + NVIDIA/AMD) laptops.
/// Default = the adapter Windows picks (normally the one wired to the panel).
enum class GpuPreference { Default, HighPerformance, MinimumPower };

class D3D11Renderer {
public:
    D3D11Renderer(HWND hwnd, int width, int height,
                  GpuPreference gpuPreference = GpuPreference::Default);
    ~D3D11Renderer();
    D3D11Renderer(const D3D11Renderer&) = delete;
    D3D11Renderer& operator=(const D3D11Renderer&) = delete;

    // Render one frame: video (or test pattern) + optional diagnostics text.
    // Presents with VSync. Call WaitForFrameSlot() first.
    void Render(float time, float frameCount, const std::wstring* overlayText = nullptr);

    // Phase 17: block until the swap chain can take a frame without queueing
    // behind older ones (max frame latency 1). Rendering right after this —
    // with the newest picture — keeps Present→scan-out to about one refresh.
    // Returns false on timeout (render anyway).
    bool WaitForFrameSlot(DWORD timeoutMs);

    // Present count of the most recent Present() (DXGI numbering)
    bool GetLastPresentCount(uint32_t& presentCount) const;

    // Most recent present the display has shown, and when (vblank, QPC µs).
    // False when the driver/compositor does not provide frame statistics.
    bool GetLastDisplayedPresent(uint32_t& presentCount, int64_t& displayUs) const;

    // Handle window resize
    void Resize(int width, int height);

    // Get GPU adapter name
    std::wstring GetAdapterName() const { return adapterName_; }

    // Software-decoded frame: upload NV12 Y and UV planes from system memory
    void UpdateFrameNV12(const uint8_t* yData, const uint8_t* uvData,
                         int nv12Stride, uint32_t frameWidth, uint32_t frameHeight);

    // Hardware-decoded frame: GPU→GPU copy of one NV12 texture slice
    // (the decoder's texture must come from GetDevice())
    bool UpdateFrameTexture(ID3D11Texture2D* source, UINT subresource,
                            uint32_t frameWidth, uint32_t frameHeight);

    // Device for the hardware decoder, or nullptr when it cannot decode into
    // textures this renderer can sample (no video support / no NV12 views)
    ID3D11Device* GetVideoDevice() const { return supportsHardwareVideo_ ? device_.Get() : nullptr; }

    // Whether we have a video frame to display
    bool HasVideoFrame() const { return hasVideoFrame_; }

    // GPU removed/reset (driver update, TDR, hybrid GPU switch) or swap chain
    // could not be resized. The owner must destroy and recreate the renderer.
    bool IsDeviceLost() const { return deviceLost_; }

    // Print every GPU adapter (helps diagnose hybrid-GPU selection)
    static void LogAdapters();

private:
    template<typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    void CreateDeviceAndSwapChain(HWND hwnd, int width, int height, GpuPreference pref);
    void CheckDeviceLost(HRESULT hr, const char* operation);
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
    bool deviceLost_ = false;
    void CreateNV12Resources();
    void EnsureNV12Textures(uint32_t width, uint32_t height);
    void SetVideoUVScale(float x, float y);

    // Frame currently drawn: the software textures' views or the hardware
    // texture's views, whichever was updated last
    ComPtr<ID3D11ShaderResourceView> frameYSRV_;
    ComPtr<ID3D11ShaderResourceView> frameUVSRV_;
    ComPtr<ID3D11Buffer>             videoConstants_;   // uvScale: crop decoder padding
    float uvScaleX_ = 0, uvScaleY_ = 0;

    // Hardware decode target (NV12, same size as the decoder's surfaces)
    ComPtr<ID3D11Texture2D>          hwTexture_;
    ComPtr<ID3D11ShaderResourceView> hwYSRV_;
    ComPtr<ID3D11ShaderResourceView> hwUVSRV_;
    bool supportsHardwareVideo_ = false;

    // Frame latency (Phase 17)
    HANDLE frameLatencyWaitable_ = nullptr;
    UINT   swapChainFlags_ = 0;

    // Shader constant buffer layout (must be 16-byte aligned)
    struct alignas(16) ShaderConstants {
        float time;
        float resolutionX;
        float resolutionY;
        float frameCount;
    };
};
