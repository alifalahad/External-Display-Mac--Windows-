// =============================================================================
// H264Decoder.cpp — Media Foundation H.264 decoder implementation
// =============================================================================

#include "H264Decoder.h"
#include <iostream>
#include <algorithm>
#include <chrono>

// Helper to safely release COM objects
template<typename T>
void SafeRelease(T** pp) {
    if (*pp) {
        (*pp)->Release();
        *pp = nullptr;
    }
}

H264Decoder::H264Decoder() {}

H264Decoder::~H264Decoder() {
    shutdown();
}

bool H264Decoder::initialize(uint32_t width, uint32_t height) {
    if (m_initialized) shutdown();

    m_width = width;
    m_height = height;
    m_bgraBuffer.resize(width * height * 4);

    // Initialize Media Foundation
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        std::cerr << "[Decoder] MFStartup failed: 0x" << std::hex << hr << std::endl;
        return false;
    }

    if (!createDecoder()) return false;
    if (!configureInput()) return false;
    if (!configureOutput()) return false;

    // Send stream start messages
    hr = m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr)) {
        std::cerr << "[Decoder] BEGIN_STREAMING failed" << std::endl;
    }

    hr = m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr)) {
        std::cerr << "[Decoder] START_OF_STREAM failed" << std::endl;
    }

    m_initialized = true;
    std::cout << "[Decoder] Initialized: " << width << "x" << height << std::endl;
    return true;
}

bool H264Decoder::createDecoder() {
    // Find H.264 decoder MFT
    MFT_REGISTER_TYPE_INFO inputType = { MFMediaType_Video, MFVideoFormat_H264 };

    IMFActivate** activateArr = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_DECODER,
        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        &inputType,
        nullptr,
        &activateArr,
        &count
    );

    if (FAILED(hr) || count == 0) {
        std::cerr << "[Decoder] No H.264 decoder found" << std::endl;
        return false;
    }

    // Activate the first (best) decoder
    hr = activateArr[0]->ActivateObject(IID_PPV_ARGS(&m_decoder));

    // Release all activations
    for (UINT32 i = 0; i < count; i++) {
        activateArr[i]->Release();
    }
    CoTaskMemFree(activateArr);

    if (FAILED(hr)) {
        std::cerr << "[Decoder] Failed to activate decoder: 0x" << std::hex << hr << std::endl;
        return false;
    }

    // Set low latency mode
    IMFAttributes* attrs = nullptr;
    hr = m_decoder->GetAttributes(&attrs);
    if (SUCCEEDED(hr) && attrs) {
        attrs->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);
        attrs->Release();
    }

    return true;
}

bool H264Decoder::configureInput() {
    IMFMediaType* inputType = nullptr;
    HRESULT hr = MFCreateMediaType(&inputType);
    if (FAILED(hr)) return false;

    inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(inputType, MF_MT_FRAME_SIZE, m_width, m_height);
    MFSetAttributeRatio(inputType, MF_MT_FRAME_RATE, 60, 1);
    MFSetAttributeRatio(inputType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

    hr = m_decoder->SetInputType(0, inputType, 0);
    inputType->Release();

    if (FAILED(hr)) {
        std::cerr << "[Decoder] SetInputType failed: 0x" << std::hex << hr << std::endl;
        return false;
    }

    return true;
}

bool H264Decoder::configureOutput() {
    // Enumerate available output types and pick NV12
    IMFMediaType* outputType = nullptr;
    bool foundNV12 = false;

    for (DWORD i = 0; ; i++) {
        HRESULT hr = m_decoder->GetOutputAvailableType(0, i, &outputType);
        if (FAILED(hr)) break;

        GUID subtype;
        outputType->GetGUID(MF_MT_SUBTYPE, &subtype);

        if (subtype == MFVideoFormat_NV12) {
            // Set this as output type
            hr = m_decoder->SetOutputType(0, outputType, 0);
            if (SUCCEEDED(hr)) {
                foundNV12 = true;
                outputType->Release();
                break;
            }
        }
        outputType->Release();
    }

    if (!foundNV12) {
        std::cerr << "[Decoder] NV12 output not available" << std::endl;
        return false;
    }

    std::cout << "[Decoder] Output configured: NV12" << std::endl;
    return true;
}

bool H264Decoder::decode(const uint8_t* h264Data, size_t dataLen) {
    if (!m_initialized || !m_decoder) return false;

    auto startTime = std::chrono::high_resolution_clock::now();

    // Create input sample
    IMFSample* sample = nullptr;
    IMFMediaBuffer* buffer = nullptr;

    HRESULT hr = MFCreateMemoryBuffer((DWORD)dataLen, &buffer);
    if (FAILED(hr)) return false;

    BYTE* bufData = nullptr;
    hr = buffer->Lock(&bufData, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
        memcpy(bufData, h264Data, dataLen);
        buffer->Unlock();
    }
    buffer->SetCurrentLength((DWORD)dataLen);

    hr = MFCreateSample(&sample);
    if (FAILED(hr)) {
        buffer->Release();
        return false;
    }
    sample->AddBuffer(buffer);

    // Feed to decoder
    hr = m_decoder->ProcessInput(0, sample, 0);
    sample->Release();
    buffer->Release();

    if (FAILED(hr)) {
        if (hr == MF_E_NOTACCEPTING) {
            // Decoder has output pending — drain it first
            while (processOutput()) {}
            // Retry input
            // (Need to recreate sample — simplified: just skip this frame)
        }
        return false;
    }

    // Try to get output
    bool gotOutput = false;
    while (processOutput()) {
        gotOutput = true;
    }

    if (gotOutput) {
        auto endTime = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(endTime - startTime).count();

        // Update stats with simple moving average
        m_stats.framesDecoded++;
        double alpha = 0.05; // Smoothing factor
        m_stats.avgDecodeLatencyMs = m_stats.avgDecodeLatencyMs * (1.0 - alpha) + ms * alpha;
    }

    return true;
}

bool H264Decoder::processOutput() {
    if (!m_decoder) return false;

    MFT_OUTPUT_DATA_BUFFER outputData{};
    DWORD status = 0;

    // Check if decoder allocates its own samples
    MFT_OUTPUT_STREAM_INFO streamInfo{};
    m_decoder->GetOutputStreamInfo(0, &streamInfo);

    IMFSample* outputSample = nullptr;
    IMFMediaBuffer* outputBuffer = nullptr;

    if (!(streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
        // We need to provide the output buffer
        HRESULT hr = MFCreateMemoryBuffer(streamInfo.cbSize, &outputBuffer);
        if (FAILED(hr)) return false;

        hr = MFCreateSample(&outputSample);
        if (FAILED(hr)) {
            outputBuffer->Release();
            return false;
        }
        outputSample->AddBuffer(outputBuffer);
        outputData.pSample = outputSample;
    }

    HRESULT hr = m_decoder->ProcessOutput(0, 1, &outputData, &status);

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        SafeRelease(&outputSample);
        SafeRelease(&outputBuffer);
        return false;
    }

    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        // Output format changed — reconfigure
        configureOutput();
        SafeRelease(&outputSample);
        SafeRelease(&outputBuffer);
        return false;
    }

    if (FAILED(hr)) {
        SafeRelease(&outputSample);
        SafeRelease(&outputBuffer);
        return false;
    }

    // Get the sample (either ours or decoder-provided)
    IMFSample* resultSample = outputData.pSample;
    if (!resultSample) {
        SafeRelease(&outputSample);
        SafeRelease(&outputBuffer);
        return false;
    }

    // Extract NV12 data and convert to BGRA
    IMFMediaBuffer* resultBuffer = nullptr;
    hr = resultSample->ConvertToContiguousBuffer(&resultBuffer);
    if (SUCCEEDED(hr)) {
        BYTE* nv12Data = nullptr;
        DWORD nv12Len = 0;
        hr = resultBuffer->Lock(&nv12Data, nullptr, &nv12Len);
        if (SUCCEEDED(hr)) {
            // NV12 stride is typically width aligned to 16/32 bytes
            int stride = ((int)m_width + 15) & ~15;  // Align to 16

            convertNV12toBGRA(nv12Data, stride,
                              m_bgraBuffer.data(), m_width * 4,
                              m_width, m_height);

            resultBuffer->Unlock();

            // Deliver decoded frame
            if (m_callback) {
                m_callback(m_bgraBuffer.data(), m_width, m_height, m_width * 4);
            }
        }
        resultBuffer->Release();
    }

    // Clean up
    if (outputData.pSample != outputSample) {
        // Decoder provided its own sample — release it
        outputData.pSample->Release();
    }
    SafeRelease(&outputSample);
    SafeRelease(&outputBuffer);

    return true;
}

void H264Decoder::convertNV12toBGRA(const uint8_t* nv12, int nv12Stride,
                                     uint8_t* bgra, int bgraStride,
                                     int width, int height) {
    // NV12 layout:
    //   Y plane:  height lines, each nv12Stride bytes
    //   UV plane: height/2 lines, each nv12Stride bytes (U,V interleaved)

    const uint8_t* yPlane = nv12;
    const uint8_t* uvPlane = nv12 + nv12Stride * height;

    for (int y = 0; y < height; y++) {
        const uint8_t* yRow = yPlane + y * nv12Stride;
        const uint8_t* uvRow = uvPlane + (y / 2) * nv12Stride;
        uint8_t* bgraRow = bgra + y * bgraStride;

        for (int x = 0; x < width; x++) {
            int Y = yRow[x];
            int U = uvRow[(x & ~1)] - 128;
            int V = uvRow[(x & ~1) + 1] - 128;

            // BT.601 conversion
            int R = Y + ((351 * V) >> 8);
            int G = Y - ((179 * V + 86 * U) >> 8);
            int B = Y + ((443 * U) >> 8);

            bgraRow[x * 4 + 0] = (uint8_t)std::clamp(B, 0, 255);  // B
            bgraRow[x * 4 + 1] = (uint8_t)std::clamp(G, 0, 255);  // G
            bgraRow[x * 4 + 2] = (uint8_t)std::clamp(R, 0, 255);  // R
            bgraRow[x * 4 + 3] = 255;                               // A
        }
    }
}

void H264Decoder::flush() {
    if (m_decoder) {
        m_decoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }
}

void H264Decoder::shutdown() {
    if (m_decoder) {
        m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        m_decoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);

        // Drain remaining output
        while (processOutput()) {}

        SafeRelease(&m_decoder);
    }

    m_initialized = false;
    MFShutdown();
    std::cout << "[Decoder] Shutdown. Decoded " << m_stats.framesDecoded << " frames" << std::endl;
}
