// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

// The ISS still uses IO v1. Keep the two protocol headers in separate models.
struct Arche3dIoAccess {
    uint64_t address, size;
    uint8_t *data;
    void *request;
    bool pending = false, error = false;
    int64_t latency = 0;
};
