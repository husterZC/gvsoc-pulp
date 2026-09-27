// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cpu/iss/include/iss.hpp>
#include <cpu/iss/include/isa_lib/macros.h>
#include <cpu/iss/include/isa_lib/int.h>

enum class Arche3dSpecialFunction { Exp, Sin, Cos, Sqrt, Reciprocal };

// Round a finite binary64 approximation to one of the four narrow formats.
// Doing this explicitly avoids host rounding-mode dependence and implements
// all five RISC-V rounding modes, gradual underflow and directed overflow.
static inline uint64_t arche3d_round_small_float(Iss *iss, double value,
    unsigned exp, unsigned mant, unsigned rm, bool inexact)
{
    uint64_t sign = std::signbit(value) ? UINT64_C(1) << (exp + mant) : 0;
    double magnitude = std::fabs(value);
    unsigned bias = (1U << (exp - 1)) - 1;
    uint64_t infinity = ((UINT64_C(1) << exp) - 1) << mant;
    if (magnitude == 0.0)
    {
        if (inexact) set_fflags(iss, 3); // NX | UF; tininess after rounding.
        return sign;
    }

    int exponent;
    std::frexp(magnitude, &exponent);
    int step = std::max(exponent - 1 - (int)mant, 1 - (int)bias - (int)mant);
    double scaled = std::ldexp(magnitude, -step);
    uint64_t significand = (uint64_t)scaled;
    double remainder = scaled - significand;
    bool increment = (rm == 0 && (remainder > 0.5 ||
        (remainder == 0.5 && (significand & 1)))) ||
        (rm == 4 && remainder >= 0.5) ||
        (rm == 2 && sign && remainder != 0) ||
        (rm == 3 && !sign && remainder != 0);
    significand += increment;
    inexact |= remainder != 0;
    if (significand >= (UINT64_C(1) << (mant + 1)))
    {
        significand >>= 1;
        ++step;
    }

    int encoded_exp = step + mant + bias;
    if (encoded_exp >= (1 << exp) - 1)
    {
        set_fflags(iss, 5); // OF | NX
        bool to_infinity = rm == 0 || rm == 4 || (rm == 2 && sign) ||
            (rm == 3 && !sign);
        return sign | (to_infinity ? infinity : infinity - 1);
    }
    uint64_t result = significand < (UINT64_C(1) << mant) ? significand :
        ((uint64_t)encoded_exp << mant) | (significand - (UINT64_C(1) << mant));
    if (inexact) set_fflags(iss, result < (UINT64_C(1) << mant) ? 3 : 1);
    return sign | result;
}

// exp is base e; sin/cos take radians; reciprocal is rounded 1/x, not a
// seven-bit estimate. Transcendentals use host binary64 libm, not an RTL
// polynomial approximation. See the target documentation for accuracy limits.
static inline uint64_t arche3d_small_special(Iss *iss, uint64_t bits,
    unsigned exp, unsigned mant, unsigned rm, Arche3dSpecialFunction operation)
{
    uint64_t sign = bits & (UINT64_C(1) << (exp + mant));
    uint64_t infinity = ((UINT64_C(1) << exp) - 1) << mant;
    uint64_t magnitude = bits & ((UINT64_C(1) << (exp + mant)) - 1);
    uint64_t nan = infinity | (UINT64_C(1) << (mant - 1));
    uint64_t one = ((UINT64_C(1) << (exp - 1)) - 1) << mant;
    if (magnitude > infinity)
    {
        if (!(magnitude & (UINT64_C(1) << (mant - 1)))) set_fflags(iss, 16);
        return nan;
    }
    if (operation == Arche3dSpecialFunction::Reciprocal && magnitude == 0)
    {
        set_fflags(iss, 8); // DZ, including -0 -> -infinity.
        return sign | infinity;
    }
    if (operation == Arche3dSpecialFunction::Sqrt && sign && magnitude != 0)
    {
        set_fflags(iss, 16);
        return nan;
    }
    if (magnitude == infinity)
    {
        if (operation == Arche3dSpecialFunction::Exp) return sign ? 0 : infinity;
        if (operation == Arche3dSpecialFunction::Sqrt) return infinity;
        if (operation == Arche3dSpecialFunction::Reciprocal) return sign;
        set_fflags(iss, 16); // sin/cos of either infinity.
        return nan;
    }
    if (magnitude == 0)
    {
        return operation == Arche3dSpecialFunction::Exp ||
            operation == Arche3dSpecialFunction::Cos ? one : sign;
    }

    // The host evaluation always uses nearest rounding. Destination rounding
    // is separate, and host exception state is not architectural fflags.
    int old_round = fegetround();
    fesetround(FE_TONEAREST);
    flexfloat_t input;
    ff_init(&input, {(uint8_t)exp, (uint8_t)mant});
    flexfloat_set_bits(&input, bits & ((UINT64_C(1) << (exp + mant + 1)) - 1));
    double x = ff_get_double(&input);
    double value;
    bool transcendental = false;
    switch (operation)
    {
        case Arche3dSpecialFunction::Exp:
            // Avoid binary64 overflow/underflow far outside every destination
            // format's useful exp input range. Retain a finite nonzero value
            // so destination rounding can still choose max finite / min subnormal.
            value = x > 700 ? 1e300 : x < -700 ? 1e-300 : std::exp(x);
            if (value == 1) value = std::nextafter(value, x > 0 ? 2.0 : 0.0);
            transcendental = true;
            break;
        case Arche3dSpecialFunction::Sin:
            value = std::sin(x);
            if (value == x) value = std::nextafter(value, 0.0);
            transcendental = true;
            break;
        case Arche3dSpecialFunction::Cos:
            value = std::cos(x);
            if (value == 1) value = std::nextafter(value, 0.0);
            transcendental = true;
            break;
        case Arche3dSpecialFunction::Sqrt: value = std::sqrt(x); break;
        case Arche3dSpecialFunction::Reciprocal: value = 1.0 / x; break;
    }
    uint64_t result = arche3d_round_small_float(iss, value, exp, mant, rm, transcendental);
    fesetround(old_round);
    return result;
}

template <Arche3dSpecialFunction operation, unsigned bits, bool force_bf16 = false>
static inline iss_reg_t arche3d_scalar_special_exec(Iss *iss, iss_insn_t *insn, iss_reg_t pc)
{
    unsigned rm = force_bf16 ? 7 : UIM_GET(0);
    if (rm == 7) rm = iss->csr.fcsr.frm;
    if (rm > 4)
    {
        iss->exception.raise(pc, ISS_EXCEPT_ILLEGAL);
        return pc;
    }
    bool alternate = force_bf16 || iss->csr_fmode.value == 3;
    unsigned exp = bits == 16 ? (alternate ? 8 : 5) : (alternate ? 4 : 5);
    FREG_SET(0, arche3d_small_special(iss, FREG_GET(0), exp, bits - exp - 1, rm, operation));
    return iss_insn_next(iss, insn, pc);
}

#define ARCHE3D_SCALAR_SPECIAL(name, operation) \
    static inline iss_reg_t arche3d_f##name##_b_exec(Iss *iss, iss_insn_t *insn, iss_reg_t pc) \
    { return arche3d_scalar_special_exec<Arche3dSpecialFunction::operation, 8>(iss, insn, pc); } \
    static inline iss_reg_t arche3d_f##name##_h_exec(Iss *iss, iss_insn_t *insn, iss_reg_t pc) \
    { return arche3d_scalar_special_exec<Arche3dSpecialFunction::operation, 16>(iss, insn, pc); }
ARCHE3D_SCALAR_SPECIAL(exp, Exp)
ARCHE3D_SCALAR_SPECIAL(sin, Sin)
ARCHE3D_SCALAR_SPECIAL(cos, Cos)
ARCHE3D_SCALAR_SPECIAL(sqrt, Sqrt)
ARCHE3D_SCALAR_SPECIAL(recip, Reciprocal)
#undef ARCHE3D_SCALAR_SPECIAL

static inline iss_reg_t arche3d_fsqrt_ah_exec(Iss *iss, iss_insn_t *insn, iss_reg_t pc)
{
    return arche3d_scalar_special_exec<Arche3dSpecialFunction::Sqrt, 16, true>(iss, insn, pc);
}

#ifdef CONFIG_GVSOC_ISS_USE_SPATZ
#include <cpu/iss/include/isa/rv32v_timed.hpp>

template <Arche3dSpecialFunction operation>
static inline iss_reg_t arche3d_vector_special_exec(Iss *iss, iss_insn_t *insn, iss_reg_t pc)
{
    unsigned sewb = iss->vector.sewb;
    unsigned rm = iss->csr.fcsr.frm;
    if (sewb > 2 || rm > 4)
    {
        iss->exception.raise(pc, ISS_EXCEPT_ILLEGAL);
        return pc;
    }
    for (unsigned i = VSTART; i < VEND; ++i)
    {
        if (velem_is_active(iss, i, UIM_GET(0)))
        {
            uint64_t input = velem_get_value(iss, REG_IN(0), i, sewb, iss->vector.lmul);
            uint64_t output = arche3d_small_special(iss, input, iss->vector.exp,
                iss->vector.mant, rm, operation);
            velem_set_value(iss, REG_OUT(0), i, sewb, output);
        }
    }
    return iss_insn_next(iss, insn, pc);
}

#define ARCHE3D_VECTOR_SPECIAL(name, operation) \
    static inline iss_reg_t arche3d_vf##name##_v_exec(Iss *iss, iss_insn_t *insn, iss_reg_t pc) \
    { return arche3d_vector_special_exec<Arche3dSpecialFunction::operation>(iss, insn, pc); }
ARCHE3D_VECTOR_SPECIAL(exp, Exp)
ARCHE3D_VECTOR_SPECIAL(sin, Sin)
ARCHE3D_VECTOR_SPECIAL(cos, Cos)
ARCHE3D_VECTOR_SPECIAL(recip, Reciprocal)
#undef ARCHE3D_VECTOR_SPECIAL

static inline iss_reg_t arche3d_vfsqrt_v_exec(Iss *iss, iss_insn_t *insn, iss_reg_t pc)
{
    if (iss->vector.sewb > 2) return vfsqrt_v_exec(iss, insn, pc);
    return arche3d_vector_special_exec<Arche3dSpecialFunction::Sqrt>(iss, insn, pc);
}
#endif
