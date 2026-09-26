# SPDX-License-Identifier: Apache-2.0
"""Native fat-tree/mesh/crossbar and SoC traffic, built with make TARGETS=network3d."""
import importlib
import gvsoc.runner

Chip = importlib.import_module('pulp.3d_network.benchmarks.network').Chip


class Target(gvsoc.runner.Target):
    gapy_description = '3D network traffic benchmark with optional SoC and RAM endpoints'
    model = Chip
    name = 'network3d'
