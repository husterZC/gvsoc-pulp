// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>
#include "format.hpp"

namespace mxcore_fp4 {
// FP4 E2M1 values multiplied by two, including both signs of zero.
inline int twice(uint8_t x) {
    constexpr int magnitude[8] = {0, 1, 2, 3, 4, 6, 8, 12};
    return (x & 8) ? -magnitude[x & 7] : magnitude[x & 7];
}

inline uint8_t quantize(float value, int exponent) {
    constexpr double values[8] = {0, .5, 1, 1.5, 2, 3, 4, 6};
    double scaled = std::ldexp(std::abs(double(value)), -exponent);
    unsigned code = 0;
    double distance = scaled;
    for (unsigned i=1; i<8; ++i) {
        double candidate = std::abs(scaled-values[i]);
        if (candidate < distance || (candidate == distance && !(i & 1))) {
            code=i; distance=candidate;
        }
    }
    return uint8_t(code | (std::signbit(value) ? 8 : 0));
}

struct Compute {
    unsigned m=0, n=0, k=0;
    OutputFormat format=OutputFormat::MXFP4;
    std::array<std::vector<uint8_t>, 6> data;
    std::array<std::vector<bool>, 4> present;
    std::vector<bool> ready;

    void init(unsigned rows, unsigned cols, unsigned inner, OutputFormat result=OutputFormat::MXFP4) {
        m=rows; n=cols; k=inner;
        format=result;
        std::array<unsigned,6> sizes{m*k/2, n*k/2, m*k/32, n*k/32,
                                    m*n*output_bits(format)/8, format==OutputFormat::MXFP4 ? m*n/32 : 0};
        for (unsigned i=0; i<6; ++i) data[i].assign(sizes[i], 0);
        for (unsigned i=0; i<4; ++i) present[i].assign(sizes[i], false);
        ready.assign(m*n/32, false);
    }

    uint8_t input(unsigned region, unsigned offset) const {
        if (offset >= data[region].size() || !present[region][offset])
            throw std::runtime_error("MXCoreFP4 profile reads output before its operands arrive");
        return data[region][offset];
    }

    // Output block order: M tile, N tile, row. Each block has 32 columns.
    void block(unsigned block_index) {
        if (ready.at(block_index)) return;
        unsigned tile=block_index/32;
        unsigned row=(tile/(n/32))*32 + block_index%32;
        unsigned col=(tile%(n/32))*32;
        std::array<float,32> output{};
        float maximum=0;
        bool poison=false;
        for (unsigned j=0; j<32; ++j) {
            float acc=0;
            for (unsigned kb=0; kb<k/32; ++kb) {
                unsigned ai=(row/32)*(k/32)*32 + kb*32 + row%32;
                unsigned bi=(col/32)*(k/32)*32 + kb*32 + j;
                uint8_t sa=input(2,ai), sb=input(3,bi);
                int dot=0;
                for (unsigned h=0; h<16; ++h) {
                    uint8_t a=input(0, ai*16+h), b=input(1, bi*16+h);
                    dot += twice(a & 15)*twice(b & 15) + twice(a >> 4)*twice(b >> 4);
                }
                if (sa == 255 || sb == 255) acc=std::numeric_limits<float>::quiet_NaN();
                else {
                    // Exact integer dot product, E8M0 scaling, one FP32 RNE per
                    // 32-value dot-plus-accumulator operation (not per multiply).
                    long double sum=std::ldexp(static_cast<long double>(dot), int(sa)+int(sb)-256);
                    acc=static_cast<float>(sum + static_cast<long double>(acc));
                }
            }
            output[j]=acc;
            poison |= !std::isfinite(acc);
            maximum=std::max(maximum, std::abs(acc));
        }
        if (format==OutputFormat::MXFP4) {
            int exponent=maximum ? std::max(-127, std::min(127, std::ilogb(maximum)-2)) : 0;
            data[5][block_index]=poison ? 255 : exponent+127;
            for (unsigned j=0; j<16; ++j)
                data[4][block_index*16+j]=poison ? 0 :
                    quantize(output[2*j],exponent) | (quantize(output[2*j+1],exponent)<<4);
        } else {
            unsigned bytes=output_bits(format)/8;
            for (unsigned j=0; j<32; ++j) {
                uint32_t encoded=encode_output(output[j],format);
                for (unsigned b=0; b<bytes; ++b)
                    data[4][(block_index*32+j)*bytes+b]=encoded>>(8*b);
            }
        }
        ready[block_index]=true;
    }
};
}
