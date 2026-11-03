// =============================================================================
// H264Sps.cpp — Colour description from an H.264 SPS (H.264 7.3.2.1.1, E.1.1)
// =============================================================================

#include "Decoder/H264Sps.h"

#include <vector>

namespace {

/// Exp-Golomb bit reader over the RBSP. Reading past the end sets `failed`.
class BitReader {
public:
    explicit BitReader(std::vector<uint8_t> rbsp) : data_(std::move(rbsp)) {}
    bool failed() const { return failed_; }

    uint32_t bits(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; i++) {
            if (pos_ >= data_.size() * 8) { failed_ = true; return 0; }
            v = (v << 1) | ((data_[pos_ / 8] >> (7 - pos_ % 8)) & 1u);
            pos_++;
        }
        return v;
    }
    uint32_t ue() {
        int zeros = 0;
        while (bits(1) == 0) {
            if (failed_ || ++zeros > 31) { failed_ = true; return 0; }
        }
        return zeros ? ((1u << zeros) - 1 + bits(zeros)) : 0;
    }
    int32_t se() {
        const uint32_t k = ue();
        return (k & 1) ? static_cast<int32_t>((k + 1) / 2) : -static_cast<int32_t>(k / 2);
    }

private:
    std::vector<uint8_t> data_;
    size_t pos_ = 0;
    bool failed_ = false;
};

void SkipScalingList(BitReader& r, int size) {
    int last = 8, next = 8;
    for (int j = 0; j < size && !r.failed(); j++) {
        if (next != 0) next = (last + r.se() + 256) % 256;
        last = next == 0 ? last : next;
    }
}

} // namespace

bool ParseSpsColor(const uint8_t* nal, size_t len, VideoColor& out) {
    if (!nal || len < 4 || (nal[0] & 0x1F) != 7) return false;

    // NAL payload → RBSP: drop the emulation-prevention byte in 00 00 03
    std::vector<uint8_t> rbsp;
    rbsp.reserve(len);
    int zeros = 0;
    for (size_t i = 1; i < len; i++) {
        if (zeros >= 2 && nal[i] == 0x03) { zeros = 0; continue; }
        zeros = nal[i] == 0 ? zeros + 1 : 0;
        rbsp.push_back(nal[i]);
    }
    BitReader r(std::move(rbsp));

    const uint32_t profile = r.bits(8);
    r.bits(16);                                   // constraint flags, level
    r.ue();                                       // seq_parameter_set_id
    switch (profile) {
        case 100: case 110: case 122: case 244: case 44: case 83:
        case 86: case 118: case 128: case 138: case 139: case 134: case 135: {
            const uint32_t chroma = r.ue();
            if (chroma == 3) r.bits(1);           // separate_colour_plane_flag
            r.ue();                               // bit depths
            r.ue();
            r.bits(1);                            // qpprime_y_zero_transform_bypass_flag
            if (r.bits(1)) {                      // seq_scaling_matrix_present_flag
                for (int i = 0; i < (chroma != 3 ? 8 : 12) && !r.failed(); i++) {
                    if (r.bits(1)) SkipScalingList(r, i < 6 ? 16 : 64);
                }
            }
            break;
        }
        default:
            break;
    }
    r.ue();                                       // log2_max_frame_num_minus4
    const uint32_t pocType = r.ue();
    if (pocType == 0) {
        r.ue();
    } else if (pocType == 1) {
        r.bits(1);
        r.se();
        r.se();
        const uint32_t cycle = r.ue();
        for (uint32_t i = 0; i < cycle && i < 256 && !r.failed(); i++) r.se();
    }
    r.ue();                                       // max_num_ref_frames
    r.bits(1);                                    // gaps_in_frame_num_value_allowed_flag
    r.ue();                                       // pic_width_in_mbs_minus1
    r.ue();                                       // pic_height_in_map_units_minus1
    if (!r.bits(1)) r.bits(1);                    // frame_mbs_only_flag, mb_adaptive_frame_field_flag
    r.bits(1);                                    // direct_8x8_inference_flag
    if (r.bits(1)) { r.ue(); r.ue(); r.ue(); r.ue(); }   // frame cropping

    // Defaults of the standard when the VUI says nothing
    VideoColor color;
    color.fullRange = false;
    color.matrix = 2;
    if (r.bits(1)) {                              // vui_parameters_present_flag
        if (r.bits(1) && r.bits(8) == 255) r.bits(32);   // aspect ratio (Extended_SAR: sar w, h)
        if (r.bits(1)) r.bits(1);                 // overscan
        if (r.bits(1)) {                          // video_signal_type_present_flag
            r.bits(3);                            // video_format
            color.fullRange = r.bits(1) == 1;
            if (r.bits(1)) {                      // colour_description_present_flag
                r.bits(8);                        // colour_primaries
                r.bits(8);                        // transfer_characteristics
                color.matrix = static_cast<int>(r.bits(8));
            }
        }
    }
    if (r.failed()) return false;
    out = color;
    return true;
}

bool FindSpsColor(const uint8_t* data, size_t len, VideoColor& out) {
    if (!data) return false;
    // NAL units start after 00 00 01 (a 4-byte start code ends the same way)
    size_t i = 0;
    while (i + 3 <= len) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            const size_t start = i + 3;
            size_t end = start;
            while (end + 3 <= len && !(data[end] == 0 && data[end + 1] == 0 &&
                                       (data[end + 2] == 1 || (data[end + 2] == 0 && end + 3 < len && data[end + 3] == 1)))) {
                end++;
            }
            if (end + 3 > len) end = len;
            if (start < len && (data[start] & 0x1F) == 7) return ParseSpsColor(data + start, end - start, out);
            i = end;
        } else {
            i++;
        }
    }
    return false;
}
