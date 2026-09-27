# SPDX-License-Identifier: Apache-2.0
"""Check custom opcode allocation against the complete arche3d ISA."""

import sys
import unittest
from pathlib import Path

# Use the checked-out model generators, independent of the install prefix.
root = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(root / 'core/models'), str(root / 'pulp')]

from cpu.iss.isa_gen.isa_riscv_gen import RiscvIsa
from cpu.iss.isa_gen.isa_smallfloats import Xf16, Xf16alt, Xf8, Xfaux, XfvecSnitch
from pulp.chips.arche3d.logic.snitch.snitch_isa import (
    Rv32frep, Rv32redmule, Rv32ssr, Xdma,
)
from pulp.chips.arche3d.logic.snitch.soft_hier_rvv import extend_arche3d_rvv
from pulp.chips.arche3d.logic.snitch.special_functions import extend_arche3d_special_functions


def overlaps(left, right):
    return len(left) == len(right) and all(
        a == b or a == '-' or b == '-' for a, b in zip(left, right)
    )


class SpecialFunctionEncodings(unittest.TestCase):
    def test_no_collision(self):
        for vector in (False, True):
            with self.subTest(vector=vector):
                extensions = [Xdma(), Rv32redmule(), Xf16(), Xf16alt(),
                              Xf8(), XfvecSnitch(), Xfaux()]
                if not vector:
                    extensions += [Rv32frep(), Rv32ssr()]
                isa = RiscvIsa('arche3d_encoding_test',
                              'rv32imfdva' if vector else 'rv32imfda',
                              extensions=extensions)
                extend_arche3d_rvv(isa)
                old = {insn.name: insn.encoding for insn in isa.get_insns()}
                extend_arche3d_special_functions(isa)
                new = [insn for insn in isa.get_insns() if insn.name not in old]
                self.assertEqual(len(new), 12 if vector else 8)
                for insn in new:
                    for other in isa.get_insns():
                        if insn is not other and other.active:
                            self.assertFalse(overlaps(insn.encoding, other.encoding),
                                             f'{insn.label} overlaps {other.label}')
                for name, encoding in old.items():
                    self.assertEqual(isa.get_insn(name).encoding, encoding)


if __name__ == '__main__':
    unittest.main()
