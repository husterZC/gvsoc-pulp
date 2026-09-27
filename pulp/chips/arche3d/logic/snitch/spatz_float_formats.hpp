// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cpu/iss/include/isa/priv.hpp>
#include <cpu/iss/include/isa/rv32v_timed.hpp>

// Spatz handlers execute asynchronously. Drain older work before changing its
// format or rounding mode, or reading/clearing accrued exceptions. Otherwise a
// queued instruction could use the next frm value or set fflags after a clear.
// Keep CSR encodings, VL, VTYPE and all vector-register bits unchanged.
template <iss_reg_t (*handler)(Iss *, iss_insn_t *, iss_reg_t)>
static inline iss_reg_t arche3d_format_csr_exec(Iss *iss, iss_insn_t *insn,
                                               iss_reg_t pc, bool writes)
{
    bool changes_format = UIM_GET(0) == 0x800 && writes;
    bool accesses_fp_state = UIM_GET(0) >= 1 && UIM_GET(0) <= 3;
    if ((changes_format || accesses_fp_state) && !iss->vu.queue_is_empty())
    {
        return pc;
    }

    iss_reg_t next_pc = handler(iss, insn, pc);
    if (changes_format)
    {
        // Apply immediately even when software does not issue another vsetvli.
        extract_vector_format(iss, iss->vector.sewb * 8,
                              &iss->vector.mant, &iss->vector.exp);
    }
    return next_pc;
}

static inline iss_reg_t arche3d_csrrw_exec(Iss *iss, iss_insn_t *insn, iss_reg_t pc)
{
    return arche3d_format_csr_exec<csrrw_exec>(iss, insn, pc, true);
}

static inline iss_reg_t arche3d_csrrs_exec(Iss *iss, iss_insn_t *insn, iss_reg_t pc)
{
    return arche3d_format_csr_exec<csrrs_exec>(iss, insn, pc, REG_IN(0) != 0);
}

static inline iss_reg_t arche3d_csrrc_exec(Iss *iss, iss_insn_t *insn, iss_reg_t pc)
{
    return arche3d_format_csr_exec<csrrc_exec>(iss, insn, pc, REG_IN(0) != 0);
}

static inline iss_reg_t arche3d_csrrwi_exec(Iss *iss, iss_insn_t *insn, iss_reg_t pc)
{
    return arche3d_format_csr_exec<csrrwi_exec>(iss, insn, pc, true);
}

static inline iss_reg_t arche3d_csrrsi_exec(Iss *iss, iss_insn_t *insn, iss_reg_t pc)
{
    return arche3d_format_csr_exec<csrrsi_exec>(iss, insn, pc, UIM_GET(1) != 0);
}

static inline iss_reg_t arche3d_csrrci_exec(Iss *iss, iss_insn_t *insn, iss_reg_t pc)
{
    return arche3d_format_csr_exec<csrrci_exec>(iss, insn, pc, UIM_GET(1) != 0);
}
