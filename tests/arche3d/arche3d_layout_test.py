# SPDX-License-Identifier: Apache-2.0
from pathlib import Path
import gvsoc.runner
import gvsoc.systree as st
from vp.clock_domain import Clock_domain
from pulp.chips.arche3d.logic.layout_engine import LayoutEngine
from pulp.chips.arche3d.logic.l1_fabric import L1Fabric
from pulp.chips.arche3d.logic.memory import Memory


class Board(st.Component):
    def __init__(self, parent, name, parser, options):
        super().__init__(parent, name, options=options)
        clock = Clock_domain(self, 'clock', frequency=1_000_000_000)
        chip = st.Component(self, 'chip')
        clock.o_CLOCK(chip.i_CLOCK())
        fabric = L1Fabric(chip, 'l1', banks=128, bank_width=4, size=0x6c000,
                          low_ports=1, hwpe_ports=2, hwpe_bandwidth=512)
        for i in range(128):
            bank = Memory(chip, f'bank_{i}', size=0x6c000//128, atomics=True, width_log2=0)
            chip.bind(fabric, f'out_{i}', bank, 'input')
        layout = LayoutEngine(chip, 'layout_engine', bandwidth=512, l1_base=0, l1_size=0x6c000)
        chip.bind(layout, 'tcdm', fabric, 'hwpe_0')
        check = st.Component(chip, 'check')
        check.add_sources(['tests/arche3d/layout_checks.cpp'])
        check.add_properties(dict(fixtures=str(Path(__file__).resolve().parents[3] /
                                               'build/arche3d_work/layout.bin')))
        for port, dest in [('memory', 'dma_input'), ('bus', 'bus_input'), ('low', 'in_0'), ('mid', 'hwpe_1')]:
            chip.bind(check, port, fabric, dest)
        chip.bind(check, 'control', layout, 'input')


class Target(gvsoc.runner.Target):
    def __init__(self, parser, options=None, name=None):
        super().__init__(parser, options, name=name, model=Board,
                         description='Layout numerics/streaming and real L1 priority regression')
