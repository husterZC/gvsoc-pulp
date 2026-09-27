// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

namespace arche3d_collective {
// The five-bit XDMA operation field and legacy sideband use these same codes.
// Existing integer, binary16 and broadcast encodings are preserved.
enum Operation : uint8_t {
    BROADCAST = 1,
    SUM_U16 = 2, SUM_I16 = 3, SUM_FP16 = 4,
    MAX_U16 = 5, MAX_I16 = 6, MAX_FP16 = 7,
    SUM_BF16 = 8, MAX_BF16 = 9,
    SUM_E5M2 = 10, MAX_E5M2 = 11,
    SUM_E4M3 = 12, MAX_E4M3 = 13,
};

constexpr unsigned REDUCTION_CYCLES = 1;
constexpr bool valid(uint8_t type) { return type >= BROADCAST && type <= MAX_E4M3; }
constexpr unsigned element_bytes(uint8_t type)
{
    return type == BROADCAST || type >= SUM_E5M2 ? 1 : 2;
}
}
