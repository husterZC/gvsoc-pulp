# SPDX-License-Identifier: Apache-2.0
"""Temporary diagnostic: production DRAM timing, selectable AXI response arbitration."""
import os
import hbm4_benchmark
import gvsoc.runner
from gvrun.parameter import TargetParameter

class Chip(hbm4_benchmark.Chip):
    def __init__(self, parent, name=None):
        super().__init__(parent, name)
        rr = TargetParameter(self, name='response_rr', value=0, cast=int,
                             description='diagnostic ready-beat round robin').get_value()
        for name, child in self.components.items():
            if name.startswith('memory_'):
                child.sources = [os.environ['NETWORK3D_PROBE_SOURCE']]
                child.add_properties(dict(response_rr=bool(rr)))

class Target(gvsoc.runner.Target):
    model = Chip
    name = 'hbm4_probe'
