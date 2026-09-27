// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cfenv>
#include <cstdint>
#include <cpu/iss/flexfloat/flexfloat.h>

// Scalar Snitch and vector Spatz already use this same GVSoC FlexFloat library.
// These adapters give non-CPU engines the same encodings and rounding without
// depending on an ISS instance or changing a core's floating-point environment.
namespace arche3d_float {

constexpr flexfloat_desc_t FP16{5, 10};
constexpr flexfloat_desc_t BF16{8, 7};
constexpr flexfloat_desc_t E5M2{5, 2};
constexpr flexfloat_desc_t E4M3{4, 3}; // IEEE-like: includes infinity, not E4M3FN.
constexpr flexfloat_desc_t FP32{8, 23};
constexpr flexfloat_desc_t FP64{11, 52};

class RneScope {
public:
    RneScope() { std::feholdexcept(&saved); std::fesetround(FE_TONEAREST); }
    ~RneScope() { std::fesetenv(&saved); }
    RneScope(const RneScope &) = delete;
    RneScope &operator=(const RneScope &) = delete;
private:
    std::fenv_t saved;
};

inline flexfloat_t decode(uint32_t bits, flexfloat_desc_t format)
{
    flexfloat_t value;
    ff_init(&value, format);
    flexfloat_set_bits(&value, bits);
    return value;
}

inline bool is_nan(uint16_t bits, flexfloat_desc_t format)
{
    unsigned fraction_mask = (1u << format.frac_bits) - 1;
    unsigned exponent_mask = ((1u << format.exp_bits) - 1) << format.frac_bits;
    return (bits & exponent_mask) == exponent_mask && (bits & fraction_mask);
}

inline uint16_t canonical_nan(flexfloat_desc_t format)
{
    return (((1u << format.exp_bits) - 1) << format.frac_bits)
        | (1u << (format.frac_bits - 1));
}

inline uint32_t encode(flexfloat_t &value)
{
    // FlexFloat may sign-extend narrow negative encodings in its uint64 result.
    // Use a 64-bit mask so encoding binary32 never shifts a 32-bit value by 32.
    const unsigned width = value.desc.exp_bits + value.desc.frac_bits + 1;
    return flexfloat_get_bits(&value) & ((uint64_t{1} << width) - 1);
}

inline uint32_t convert(uint32_t bits, flexfloat_desc_t from, flexfloat_desc_t to)
{
    auto source = decode(bits, from);
    flexfloat_t result;
    ff_cast(&result, &source, to);
    return encode(result);
}

inline uint16_t add(uint16_t a, uint16_t b, flexfloat_desc_t format)
{
    auto left = decode(a, format), right = decode(b, format), result = decode(0, format);
    ff_add(&result, &left, &right);
    return encode(result);
}

inline uint16_t maximum(uint16_t a, uint16_t b, flexfloat_desc_t format)
{
    // RISC-V FMAX: one NaN returns the number, two NaNs return canonical NaN.
    if (is_nan(a, format)) return is_nan(b, format) ? canonical_nan(format) : b;
    if (is_nan(b, format)) return a;
    auto left = decode(a, format), right = decode(b, format), result = decode(0, format);
    ff_max(&result, &left, &right); // Includes deterministic signed-zero handling.
    return encode(result);
}

inline uint32_t mac_fp32(uint16_t a, uint16_t b, uint32_t accumulator,
                        flexfloat_desc_t input_format)
{
    auto left = decode(a, input_format), right = decode(b, input_format);
    auto acc = decode(accumulator, FP32);
    flexfloat_t wide_left, wide_right, wide_acc, product_sum, result;
    // FlexFloat's FMA takes equally typed operands. Widen exactly to its double
    // backend so BF16 inputs keep their exponent range until the fused result.
    // The architectural accumulator is ONLY the rounded binary32 bits below.
    ff_cast(&wide_left, &left, FP64);
    ff_cast(&wide_right, &right, FP64);
    ff_cast(&wide_acc, &acc, FP64);
    ff_init(&product_sum, FP64);
    ff_fma(&product_sum, &wide_left, &wide_right, &wide_acc);
    ff_cast(&result, &product_sum, FP32);
    return encode(result);
}

inline uint16_t load(const uint8_t *data, unsigned bytes)
{
    return data[0] | (bytes == 2 ? uint16_t(data[1]) << 8 : 0);
}

inline void store(uint8_t *data, uint16_t value, unsigned bytes)
{
    data[0] = value;
    if (bytes == 2) data[1] = value >> 8;
}
}
