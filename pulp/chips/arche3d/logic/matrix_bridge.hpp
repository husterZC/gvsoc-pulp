// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

// IO v1 and v2 deliberately live in separate translation units.
struct MatrixAccess {
    uint64_t addr, bytes;
    uint8_t *data;
    bool write;
    void *original;
    bool pending = false, error = false;
    int64_t latency = 0;
};
