# SPDX-License-Identifier: Apache-2.0
"""Native fat-tree/mesh/crossbar and I3D traffic, built with make TARGETS=network3d."""
import importlib
import gvsoc.runner

Chip = importlib.import_module('pulp.3d_network.benchmarks.network').Chip


class Target(gvsoc.runner.Target):
    gapy_description = '3D network traffic benchmark with optional I3D and RAM endpoints'
    model = Chip
    name = 'network3d'
