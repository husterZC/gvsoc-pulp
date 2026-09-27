// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "collective_types.hpp"
#include "../float_math.hpp"

namespace arche3d_collective {
inline void combine(uint8_t type, uint8_t *dst, const uint8_t *src, uint64_t size)
{
    if (!valid(type) || type == BROADCAST) return;
    arche3d_float::RneScope rounding;
    unsigned bytes = element_bytes(type);
    auto format = type == SUM_BF16 || type == MAX_BF16 ? arche3d_float::BF16 :
        type == SUM_E5M2 || type == MAX_E5M2 ? arche3d_float::E5M2 :
        type == SUM_E4M3 || type == MAX_E4M3 ? arche3d_float::E4M3 : arche3d_float::FP16;
    for (uint64_t i = 0; i + bytes <= size; i += bytes)
    {
        uint16_t a = arche3d_float::load(dst + i, bytes);
        uint16_t b = arche3d_float::load(src + i, bytes), result;
        switch (type)
        {
            case SUM_U16: case SUM_I16: result = uint16_t(unsigned(a) + unsigned(b)); break;
            case MAX_U16: result = a > b ? a : b; break;
            case MAX_I16: result = int16_t(a) > int16_t(b) ? a : b; break;
            case SUM_FP16: case SUM_BF16: case SUM_E5M2: case SUM_E4M3:
                result = arche3d_float::add(a, b, format); break;
            default: result = arche3d_float::maximum(a, b, format); break;
        }
        arche3d_float::store(dst + i, result, bytes);
    }
}
}
