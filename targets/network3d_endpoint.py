# SPDX-License-Identifier: Apache-2.0
"""Isolated memory endpoint bandwidth and backpressure benchmark."""
import importlib
import gvsoc.runner

Chip = importlib.import_module('pulp.3d_network.benchmarks.endpoint').Chip


class Target(gvsoc.runner.Target):
    gapy_description = 'Single 3D network memory endpoint bandwidth benchmark'
    model = Chip
    name = 'network3d_endpoint'
