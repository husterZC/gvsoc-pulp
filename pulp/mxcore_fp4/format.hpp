// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <cstring>

namespace mxcore_fp4 {
enum class OutputFormat : unsigned { MXFP4, FP32, BF16, FP16, E4M3, E5M2 };

inline unsigned output_bits(OutputFormat format) {
    switch (format) {
    case OutputFormat::MXFP4:
        return 4;
    case OutputFormat::FP32:
        return 32;
    case OutputFormat::BF16:
    case OutputFormat::FP16:
        return 16;
    case OutputFormat::E4M3:
    case OutputFormat::E5M2:
        return 8;
    }
    return 0;
}

// IEEE encodings, RNE, gradual underflow and infinity on overflow. E4M3
// includes infinities/NaNs, matching Arche3D's existing FP8 conversion API.
inline uint32_t encode_output(float value, OutputFormat format) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    if (format == OutputFormat::FP32)
        return bits;
    unsigned eb = format == OutputFormat::BF16 ? 8 : format == OutputFormat::E4M3 ? 4 : 5;
    unsigned mb = output_bits(format) - eb - 1;
    uint32_t sign = (bits >> 31) << (eb + mb);
    uint32_t infinity = ((1u << eb) - 1) << mb;
    unsigned exp = (bits >> 23) & 255;
    uint64_t significand = (bits & 0x7fffff) | (exp ? 0x800000 : 0);
    if (exp == 255)
        return sign | infinity | ((bits & 0x7fffff) ? 1u << (mb - 1) : 0);
    int target = (exp ? int(exp) - 127 : -126) + (1 << (eb - 1)) - 1;
    unsigned shift = 23 - mb + (target <= 0 ? 1 - target : 0);
    if (shift >= 64)
        return sign;
    uint64_t rounded = significand >> shift;
    uint64_t remainder = significand & ((uint64_t(1) << shift) - 1);
    uint64_t halfway = uint64_t(1) << (shift - 1);
    if (remainder > halfway || (remainder == halfway && (rounded & 1)))
        ++rounded;
    if (target <= 0 || exp == 0)
        return sign | uint32_t(rounded);
    if (rounded >= (uint64_t(1) << (mb + 1))) {
        rounded >>= 1;
        ++target;
    }
    if (target >= (1 << eb) - 1)
        return sign | infinity;
    return sign | (uint32_t(target) << mb) | (uint32_t(rounded) & ((1u << mb) - 1));
}
} // namespace mxcore_fp4
