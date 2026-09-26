// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

// Host-side initialization only: copied synchronously into each cache, without
// an IO request, a clock event, or a change to DRAMSys timing state.
struct Arche3dIcachePreload {
    uint64_t base;
    const uint8_t *data;
    uint64_t size;
};
