# SPDX-License-Identifier: Apache-2.0
import benchmark
import gvsoc.runner


class Chip(benchmark.Chip):
    def __init__(self, parent, name=None):
        super().__init__(parent, name, overrides=dict(soc=1, endpoint=3, backing=0,
            frequency=1_000_000_000, addrwidth=64, datawidth=512, burst=16, sc=8, mc=8))


class Target(gvsoc.runner.Target):
    gapy_description = 'SoC B16 all-to-all with one DRAMSys HBM4 channel per endpoint'
    model = Chip
    name = 'hbm4_benchmark'
