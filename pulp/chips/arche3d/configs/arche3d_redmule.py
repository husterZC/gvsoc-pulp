# SPDX-License-Identifier: Apache-2.0
"""The original Arche3D matrix configuration: one shared RedMule."""
from .default import FlexClusterArch as DefaultArch


class FlexClusterArch(DefaultArch):
    def __init__(self):
        super().__init__()
        self.matrix_engine = 'redmule'
        self.mxcore_fp4_core_list = []
