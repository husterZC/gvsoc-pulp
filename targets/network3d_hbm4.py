# SPDX-License-Identifier: Apache-2.0
"""SoC traffic benchmark with one DRAMSys HBM4 channel per terminal."""
import importlib
import gvsoc.runner

Network = importlib.import_module('pulp.3d_network.benchmarks.network').Chip


class Chip(Network):
    def __init__(self, parent, name=None):
        super().__init__(parent, name, overrides=dict(soc=1, endpoint=3, backing=0,
            frequency=1_000_000_000, addrwidth=64, datawidth=512, burst=16, sc=8, mc=8))


class Target(gvsoc.runner.Target):
    gapy_description = '3D SoC B16 all-to-all reads with DRAMSys HBM4 channels'
    model = Chip
    name = 'network3d_hbm4'
