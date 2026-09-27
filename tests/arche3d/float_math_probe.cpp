// SPDX-License-Identifier: Apache-2.0
#include <pulp/chips/arche3d/logic/floonoc_v2/collective_reduction.hpp>

static flexfloat_desc_t format(unsigned index)
{
    constexpr flexfloat_desc_t formats[] = {arche3d_float::FP16, arche3d_float::BF16,
        arche3d_float::E5M2, arche3d_float::E4M3};
    return formats[index];
}

extern "C" unsigned pair(unsigned index, unsigned maximum, unsigned a, unsigned b)
{
    const uint8_t ops[][2] = {{4, 7}, {8, 9}, {10, 11}, {12, 13}};
    unsigned bytes = index < 2 ? 2 : 1;
    uint8_t dst[2], src[2];
    arche3d_float::store(dst, a, bytes);
    arche3d_float::store(src, b, bytes);
    arche3d_collective::combine(ops[index][maximum], dst, src, bytes);
    return arche3d_float::load(dst, bytes);
}

extern "C" unsigned mac(unsigned index, unsigned a, unsigned b, unsigned accumulator)
{
    arche3d_float::RneScope rounding;
    return arche3d_float::mac_fp16(a, b, accumulator, format(index));
}

extern "C" unsigned convert(unsigned source, unsigned destination, unsigned bits)
{
    arche3d_float::RneScope rounding;
    return arche3d_float::convert(bits, format(source), format(destination));
}

extern "C" unsigned environment()
{
    fenv_t saved;
    fegetenv(&saved);
    fesetround(FE_DOWNWARD);
    feclearexcept(FE_ALL_EXCEPT);
    feraiseexcept(FE_DIVBYZERO);
    unsigned rounded = pair(0, 0, 0x3c01, 0x1000); // Odd halfway rounds upward under RNE.
    bool ok = rounded == 0x3c02 && fegetround() == FE_DOWNWARD &&
        fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO;
    fesetenv(&saved);
    return ok;
}
