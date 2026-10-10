// SPDX-License-Identifier: Apache-2.0
#ifndef PULP_MXCORE_FP4_REGS_H
#define PULP_MXCORE_FP4_REGS_H
#include <stdint.h>

#define MXCORE_FP4_TRIGGER      0x00
#define MXCORE_FP4_ACQUIRE      0x04
#define MXCORE_FP4_FINISHED     0x08
#define MXCORE_FP4_STATUS       0x0c
#define MXCORE_FP4_RUNNING_JOB  0x10
#define MXCORE_FP4_SOFT_CLEAR   0x14
#define MXCORE_FP4_VECTOR_A     0x20
#define MXCORE_FP4_VECTORS_B    0x24
#define MXCORE_FP4_SCALE_A      0x28
#define MXCORE_FP4_SCALE_B      0x2c
#define MXCORE_FP4_RESULT       0x30
#define MXCORE_FP4_RESULT_SCALE 0x34
#define MXCORE_FP4_GEMM_SIZE    0x38
#define MXCORE_FP4_CTRL_ENGINE  0x3c
#define MXCORE_FP4_TILE_COUNTS  0x40
#define MXCORE_FP4_A_TILE_BITS  0x44
#define MXCORE_FP4_B_TILE_BITS  0x48
#define MXCORE_FP4_C_TILE_BITS  0x4c
#define MXCORE_FP4_ITER_COUNT   0x50
// GVSoC diagnostic extensions (not upstream RTL registers).
#define MXCORE_FP4_CYCLES       0x60
#define MXCORE_FP4_ERROR        0x64
#define MXCORE_FP4_NOMINAL_CYCLES 0x68
#define MXCORE_FP4_STALL_CYCLES  0x6c
#define MXCORE_FP4_READ_BYTES    0x70
#define MXCORE_FP4_WRITE_BYTES   0x74
#define MXCORE_FP4_START_LOW     0x78
#define MXCORE_FP4_START_HIGH    0x7c
#define MXCORE_FP4_OUTPUT_MXFP4  0u
#define MXCORE_FP4_OUTPUT_FP32   1u
#define MXCORE_FP4_OUTPUT_BF16   2u
#define MXCORE_FP4_OUTPUT_FP16   3u
#define MXCORE_FP4_OUTPUT_E4M3   4u
#define MXCORE_FP4_OUTPUT_E5M2   5u
// CTRL_ENGINE[25:23] selects result storage, independently of FP32 accumulation.
#define MXCORE_FP4_CONTROL_FORMAT(format) \
    (((format)<<23)|((format)==0 ? (1u<<21) : 0)|(8u<<9)|(19u<<3))
#define MXCORE_FP4_CONTROL      ((1u<<21)|(8u<<9)|(19u<<3))

// Call after acquiring job 0, with 32-byte-aligned pointers and a supported
// M,N,K. A/B and all scale arrays must use the tiled layout in README.md.
// Data must be visible to the accelerator before writing TRIGGER.
static inline void mxcore_fp4_program(volatile uint32_t *mmio,
    uint32_t a, uint32_t b, uint32_t sa, uint32_t sb, uint32_t c, uint32_t sc,
    unsigned m, unsigned n, unsigned k)
{
    mmio[MXCORE_FP4_VECTOR_A/4]=a; mmio[MXCORE_FP4_VECTORS_B/4]=b;
    mmio[MXCORE_FP4_SCALE_A/4]=sa; mmio[MXCORE_FP4_SCALE_B/4]=sb;
    mmio[MXCORE_FP4_RESULT/4]=c; mmio[MXCORE_FP4_RESULT_SCALE/4]=sc;
    mmio[MXCORE_FP4_GEMM_SIZE/4]=m|(k<<10)|(n<<22);
    mmio[MXCORE_FP4_CTRL_ENGINE/4]=MXCORE_FP4_CONTROL;
    mmio[MXCORE_FP4_TILE_COUNTS/4]=(m/32)|((n/32)<<4)|((k/16)<<9)|((k/32)<<16);
    mmio[MXCORE_FP4_A_TILE_BITS/4]=2048; mmio[MXCORE_FP4_B_TILE_BITS/4]=2048;
    mmio[MXCORE_FP4_C_TILE_BITS/4]=4096; mmio[MXCORE_FP4_ITER_COUNT/4]=k;
}
#endif
