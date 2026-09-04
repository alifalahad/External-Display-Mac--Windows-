// =============================================================================
// H264Decoder.h — Media Foundation H.264 hardware decoder
// =============================================================================
// Decodes H.264 Annex B bitstream using Media Foundation (DXVA).
// Outputs raw NV12 data for GPU-side YUV→RGB conversion.
//
// Pipeline: H.264 NALUs → MF H.264 Decoder (DXVA) → NV12 → GPU shader
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
    /// Called when raw NV12 data is ready for GPU upload.
    /// Parameters: (nv12Data, stride, width, height)
    using RawNV12Callback = std::function<void(const uint8_t*, int, uint32_t, uint32_t)>;

    H264Decoder();
    ~H264Decoder();

    /// Initialize the decoder for the given resolution
    bool initialize(uint32_t width, uint32_t height);

    /// Feed H.264 Annex B data to the decoder
    bool decode(const uint8_t* h264Data, size_t dataLen);

    /// Set callback for raw NV12 output (for GPU conversion)
    void setRawNV12Callback(RawNV12Callback cb) { m_rawCallback = std::move(cb); }

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

    IMFTransform* m_decoder = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    bool m_initialized = false;

    RawNV12Callback m_rawCallback;
    Stats m_stats;
};
