# SPDX-License-Identifier: Apache-2.0
import gvsoc.systree as st


class LayoutEngine(st.Component):
    """L1 transpose and pipelined MXFP4 block conversion on the shared HWPE bus."""
    def __init__(self, parent, name, *, bandwidth, l1_base, l1_size, conversion_latency=5):
        super().__init__(parent, name)
        self.add_sources(['cpu/iss/flexfloat/flexfloat.c',
                          'pulp/chips/arche3d/logic/layout_engine.cpp'])
        self.add_properties(dict(bandwidth=bandwidth, l1_base=l1_base,
            l1_size=l1_size, conversion_latency=conversion_latency))

    def i_INPUT(self):
        return st.SlaveItf(self, 'input', signature='io')

    def o_TCDM(self, itf):
        self.itf_bind('tcdm', itf, signature='io')
