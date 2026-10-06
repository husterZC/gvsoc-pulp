// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <vector>

namespace arche3d_collective
{
// Native collective packet operations. These are not XDMA instructions.
enum Operation : uint8_t
{
    BROADCAST = 1,
    SUM_U16 = 2,
    SUM_I16 = 3,
    SUM_FP16 = 4,
    MAX_U16 = 5,
    MAX_I16 = 6,
    MAX_FP16 = 7,
    SUM_BF16 = 8,
    MAX_BF16 = 9,
    SUM_E5M2 = 10,
    MAX_E5M2 = 11,
    SUM_E4M3 = 12,
    MAX_E4M3 = 13,
};

constexpr unsigned SLOTS = 16;
constexpr unsigned PIPELINE_BEATS = 8; // Bounded local read and write windows, each.
constexpr unsigned HOP_CYCLES = 2;     // Includes matching and the reduction datapath.
constexpr bool valid(uint8_t type)
{
    return type >= BROADCAST && type <= MAX_E4M3;
}
constexpr unsigned element_bytes(uint8_t type)
{
    return type == BROADCAST || type >= SUM_E5M2 ? 1 : 2;
}

} // namespace arche3d_collective

// Local endpoint/NI handshake. No IO protocol objects cross this interface.
// A successful send captures the payload by value; a successful receive moves
// it into the posted receiver's bounded pipeline. All mesh transport uses flits.
struct Arche3dCollectivePacket
{
    uint8_t type = 0;
    bool column = false;
    uint16_t root = 0, line = 0;
    // Coordinate-bit match masks, as in the original X/Y mask interface.
    // Zero is a wildcard; set bits must equal the corresponding root bits.
    uint16_t x_mask = 0, y_mask = 0;
    uint32_t epoch = 0, slot = 0;
    uint32_t total_bytes = 0, offset = 0; // One descriptor/epoch covers the entire stream.
    std::vector<uint8_t> data;
    unsigned root_x() const
    {
        return column ? line : root;
    }
    unsigned root_y() const
    {
        return column ? root : line;
    }
    bool selects(unsigned x, unsigned y) const
    {
        return (x & x_mask) == (root_x() & x_mask) && (y & y_mask) == (root_y() & y_mask);
    }
};
struct Arche3dCollectiveOffer
{
    Arche3dCollectivePacket *packet;
    bool accepted = false;
};
