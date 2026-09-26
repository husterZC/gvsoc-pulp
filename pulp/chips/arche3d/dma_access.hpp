// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

// IO v1 and IO v2 declare incompatible vp::IoReq types. Only this protocol-neutral
// descriptor crosses the wire between the two DMA components. Its owner retains
// the descriptor and data until completion, including all annotated latency.
struct Arche3dAccess {
    uint64_t address = 0, size = 0;
    uint8_t *data = nullptr;
    uint32_t id = 0;
    bool write = false, error = false;
    void *owner = nullptr;
};

struct Arche3dDmaEvent {
    uint64_t address, size;
    uint32_t id;
    bool write, completed;
};
