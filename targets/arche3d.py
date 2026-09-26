# SPDX-License-Identifier: Apache-2.0
import gvsoc.runner
from pulp.chips.arche3d.system import Board


class Target(gvsoc.runner.Target):
    def __init__(self, parser, options=None, name=None):
        super().__init__(parser, options, model=Board, name=name,
            description='arche3d: SoftHier logic die, I3D interconnect, stacked DRAM')
