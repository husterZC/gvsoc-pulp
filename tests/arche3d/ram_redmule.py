# SPDX-License-Identifier: Apache-2.0
from pulp.chips.arche3d.configs.arche3d_redmule import FlexClusterArch as RedmuleArch


class FlexClusterArch(RedmuleArch):
    def __init__(self):
        super().__init__()
        self.dram3d_backend = 'memory'
