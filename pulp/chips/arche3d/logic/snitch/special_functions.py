# SPDX-License-Identifier: Apache-2.0
"""Arche3d scalar/Spatz special functions; no changes to the shared ISA."""

from cpu.iss.isa_gen.isa_gen import (
    InVRegF, Instr, IsaSubset, OutVRegF, Range, UnsignedImm,
)
from cpu.iss.isa_gen.isa_riscv_gen import F_F, ui12_3


# Modeling assumptions, not RTL-calibrated SFU latencies. Vector execution
# also includes the existing Spatz element-count / compute-width duration.
SPECIAL_FUNCTION_LATENCIES = {
    'exp': 5,
    'sin': 11,
    'cos': 11,
    'sqrt': 5,
    'recip': 5,
}


def extend_arche3d_special_functions(isa):
    isa.add_include('<pulp/chips/arche3d/logic/snitch/special_functions.hpp>')
    scalar = []
    for operation, name in enumerate(('exp', 'sin', 'cos', 'recip')):
        for width, suffix in enumerate(('b', 'h')):
            instr = Instr(
                f'f{name}.{suffix}', F_F(suffix, ui12_3),
                f'{2 * operation + width:07b} 00000 ----- --- ----- 1011011',
                tags=['fp_op'],
            )
            instr.set_exec_label('arche3d_' + instr.name)
            instr.set_latency(SPECIAL_FUNCTION_LATENCIES[name])
            instr.get_out_reg(0).set_latency(SPECIAL_FUNCTION_LATENCIES[name])
            scalar.append(instr)
    isa.add_isa(IsaSubset('arche3d_sfu', scalar))

    # Preserve the original sqrt encodings. Local handlers give all five
    # narrow functions the same rounding, NaN and exception behavior.
    for name in ('fsqrt_h', 'fsqrt_b', 'fsqrt_ah'):
        instr = isa.get_insn(name)
        if instr is not None:
            instr.set_exec_label('arche3d_' + name)
            instr.set_latency(SPECIAL_FUNCTION_LATENCIES['sqrt'])
            instr.get_out_reg(0).set_latency(SPECIAL_FUNCTION_LATENCIES['sqrt'])

    vector = isa.get_isa('v')
    if vector is None:
        return
    for operation, name in enumerate(('exp', 'sin', 'cos', 'recip')):
        instr = Instr(
            f'vf{name}.v',
            [OutVRegF(0, Range(7, 5)), InVRegF(0, Range(20, 5)),
             UnsignedImm(0, Range(25, 1))],
            f'{32 + operation:06b} - ----- 00000 001 ----- 1011011',
            tags=['fp_op'],
        )
        instr.set_exec_label('arche3d_' + instr.name)
        instr.set_latency(SPECIAL_FUNCTION_LATENCIES[name])
        instr.set_isa(vector)
        vector.instrs.append(instr)
        vector.instrs_dict[instr.name] = instr
        isa.instrs_dict[instr.name] = instr
        for tag in instr.tags:
            isa.add_insn_tag(instr, tag)

    isa.get_insn('vfsqrt_v').set_exec_label('arche3d_vfsqrt_v')
    isa.get_insn('vfsqrt_v').set_latency(SPECIAL_FUNCTION_LATENCIES['sqrt'])
    # Keep the legacy vfexp.vv encoding, including its source in bits 19:15.
    isa.get_insn('vfexp_vv').set_latency(SPECIAL_FUNCTION_LATENCIES['exp'])
