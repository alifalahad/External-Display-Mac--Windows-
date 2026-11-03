#pragma once
// =============================================================================
// H264Sps.h — Colour description from an H.264 SPS (portable, unit-tested)
// =============================================================================
// Every keyframe starts with an SPS. Its VUI says the sample range and the
// colour matrix (H.264 Annex E: video_full_range_flag, matrix_coefficients);
// the receiver converts accordingly (Renderer/VideoColor.h). Without a VUI the
// standard's defaults apply: video range, unspecified matrix.
// =============================================================================

#include "Renderer/VideoColor.h"

#include <cstddef>
#include <cstdint>

/// One SPS NAL unit (header byte first, no start code)
bool ParseSpsColor(const uint8_t* nal, size_t len, VideoColor& out);

/// The first SPS in an Annex B access unit (3- or 4-byte start codes)
bool FindSpsColor(const uint8_t* annexB, size_t len, VideoColor& out);
