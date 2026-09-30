# Copyright (C) 2024 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
"""Per-bank bulk-access timing, ported from soft_hier_old c84bf3cdfc5c."""
import gvsoc.systree


class PriorityArbiterFilter(gvsoc.systree.Component):
    def __init__(self, parent, name, bank_width):
        super().__init__(parent, name)
        self.add_sources(['pulp/chips/arche3d/logic/priority_arbiter_filter.cpp'])
        self.add_properties({'bank_width': bank_width})
