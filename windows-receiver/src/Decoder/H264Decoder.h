// =============================================================================
// H264Decoder.h — Media Foundation H.264 hardware decoder
// =============================================================================
// Decodes H.264 Annex B bitstream to BGRA pixel data using Media Foundation.
//
// Pipeline: H.264 NALUs → MF H.264 Decoder (DXVA) → NV12 → CPU Convert → BGRA
//
// Note: Initial version does CPU-side NV12→BGRA conversion for simplicity.
// A later optimization can use D3D11 Video Processor for GPU conversion.
// =============================================================================

#pragma once

#include <cstdint>
#include <vector>
#include <functional>
#include <mutex>

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

class H264Decoder {
public:
    /// Called when a decoded BGRA frame is ready.
    /// Parameters: (bgraData, width, height, stride)
    using DecodedFrameCallback = std::function<void(const uint8_t*, uint32_t, uint32_t, uint32_t)>;

    H264Decoder();
    ~H264Decoder();

    /// Initialize the decoder for the given resolution
    bool initialize(uint32_t width, uint32_t height);

    /// Feed H.264 Annex B data to the decoder
    bool decode(const uint8_t* h264Data, size_t dataLen);

    /// Set callback for decoded frames
    void setDecodedFrameCallback(DecodedFrameCallback cb) { m_callback = std::move(cb); }

    /// Flush any pending frames
    void flush();

    /// Shutdown the decoder
    void shutdown();

    struct Stats {
        uint64_t framesDecoded = 0;
        uint64_t framesDropped = 0;
        double   avgDecodeLatencyMs = 0;
    };

    Stats getStats() const { return m_stats; }

private:
    bool createDecoder();
    bool configureInput();
    bool configureOutput();
    bool processOutput();
    void convertNV12toBGRA(const uint8_t* nv12, int nv12Stride,
                           uint8_t* bgra, int bgraStride,
                           int width, int height);

    IMFTransform* m_decoder = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    bool m_initialized = false;

    // Output buffer
    std::vector<uint8_t> m_bgraBuffer;

    DecodedFrameCallback m_callback;
    Stats m_stats;
};
