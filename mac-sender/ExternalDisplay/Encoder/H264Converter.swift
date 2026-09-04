// =============================================================================
// H264Converter.swift — AVCC ↔ Annex B format conversion
// =============================================================================
// VideoToolbox outputs H.264 in AVCC format (length-prefixed NALUs with
// SPS/PPS stored in the format description). For network transmission,
// we convert to Annex B format (00 00 00 01 start codes) which is
// self-delimiting and universally supported by decoders.
// =============================================================================

import Foundation
import CoreMedia
import VideoToolbox

enum H264Converter {

    /// Start code for Annex B NAL units
    private static let startCode: [UInt8] = [0x00, 0x00, 0x00, 0x01]

    /// Extract H.264 Annex B data from a VTCompressionSession output sample.
    ///
    /// For keyframes: prepends SPS + PPS parameter sets.
    /// For all frames: converts AVCC length-prefixed NALUs to start-code NALUs.
    ///
    /// - Parameters:
    ///   - sampleBuffer: Encoded CMSampleBuffer from VTCompressionSession
    ///   - isKeyframe: Whether this sample is a keyframe (IDR)
    /// - Returns: Annex B encoded data ready for network transmission, or nil
    static func annexBData(
        from sampleBuffer: CMSampleBuffer,
        isKeyframe: Bool
    ) -> Data? {
        guard let formatDesc = CMSampleBufferGetFormatDescription(sampleBuffer),
              let dataBuffer = CMSampleBufferGetDataBuffer(sampleBuffer)
        else {
            return nil
        }

        var result = Data()

        // ── Prepend SPS + PPS on keyframes ──────────────────────────────────
        if isKeyframe {
            // Get number of parameter sets
            var paramCount = 0
            CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                formatDesc,
                parameterSetIndex: 0,
                parameterSetPointerOut: nil,
                parameterSetSizeOut: nil,
                parameterSetCountOut: &paramCount,
                nalUnitHeaderLengthOut: nil
            )

            // Extract each parameter set (SPS=0, PPS=1, ...)
            for i in 0..<paramCount {
                var paramPtr: UnsafePointer<UInt8>?
                var paramSize = 0
                let status = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                    formatDesc,
                    parameterSetIndex: i,
                    parameterSetPointerOut: &paramPtr,
                    parameterSetSizeOut: &paramSize,
                    parameterSetCountOut: nil,
                    nalUnitHeaderLengthOut: nil
                )
                if status == noErr, let paramPtr = paramPtr {
                    result.append(contentsOf: startCode)
                    result.append(paramPtr, count: paramSize)
                }
            }
        }

        // ── Convert AVCC NALUs to Annex B ───────────────────────────────────
        var totalLength = 0
        var bufferPtr: UnsafeMutablePointer<Int8>?
        let status = CMBlockBufferGetDataPointer(
            dataBuffer,
            atOffset: 0,
            lengthAtOffsetOut: nil,
            totalLengthOut: &totalLength,
            dataPointerOut: &bufferPtr
        )
        guard status == kCMBlockBufferNoErr, let bufferPtr = bufferPtr else {
            return nil
        }

        // Get NALU length prefix size (usually 4 bytes for AVCC)
        var naluLengthSize: Int32 = 0
        CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
            formatDesc,
            parameterSetIndex: 0,
            parameterSetPointerOut: nil,
            parameterSetSizeOut: nil,
            parameterSetCountOut: nil,
            nalUnitHeaderLengthOut: &naluLengthSize
        )
        let lengthSize = Int(naluLengthSize)

        // Walk through AVCC buffer, replacing length prefixes with start codes
        var offset = 0
        while offset < totalLength - lengthSize {
            // Read NALU length (big-endian)
            var naluLength: UInt32 = 0
            memcpy(&naluLength, bufferPtr + offset, lengthSize)
            naluLength = CFSwapInt32BigToHost(naluLength)
            offset += lengthSize

            guard naluLength > 0, offset + Int(naluLength) <= totalLength else {
                break
            }

            // Write start code + NALU data
            result.append(contentsOf: startCode)
            result.append(Data(bytes: bufferPtr + offset, count: Int(naluLength)))
            offset += Int(naluLength)
        }

        return result.isEmpty ? nil : result
    }
}
