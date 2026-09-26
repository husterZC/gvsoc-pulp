# SPDX-License-Identifier: Apache-2.0
"""Memory endpoint with the AXI beat service policy of axi_sim_mem.sv."""
import gvsoc.systree
from gvsoc.signature import IoV2Beat, IoV2SingleReq


class MemoryEndpoint(gvsoc.systree.Component):
    def __init__(self, parent, name, *, data_width=64, size=4096, read_slots=4,
                 benchmark_init=False, endpoint_id=0):
        super().__init__(parent, name)
        if data_width < 8 or data_width > 1024 or data_width & (data_width - 1):
            raise ValueError('data_width must be a power of two from 8 to 1024 bits')
        if size < 1 or read_slots < 1:
            raise ValueError('size and read_slots must be positive')
        self.data_width = data_width
        self.add_properties(dict(data_width=data_width, size=size, read_slots=read_slots,
                                 benchmark_init=benchmark_init, endpoint_id=endpoint_id))
        self.add_sources(['pulp/3d_network/memory_endpoint.cpp'])

    def i_INPUT(self):
        """Local byte addresses; one read burst or one write beat per request."""
        return gvsoc.systree.SlaveItf(self, 'input', signature=IoV2Beat(self.data_width // 8))

    def o_OUTPUT(self, itf):
        """Optional backing storage. Unbound: use the endpoint's internal RAM.

        The backend sees one SingleReq transaction per burst. Its completion
        and latency annotations gate responses in addition to endpoint timing.
        """
        self.itf_bind('output', itf, signature=IoV2SingleReq())
