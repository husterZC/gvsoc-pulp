# SPDX-License-Identifier: Apache-2.0
"""Cycle models of native and AXI-wrapped fat-tree, mesh and crossbar networks.

Import with importlib.import_module('pulp.3d_network.interconnect').
Source ports use IO_v2 SingleReq. I3D memory ports use IO_v2 Beat so that
external endpoints determine response timing and read concurrency.
"""
import gvsoc.systree
from gvsoc.signature import IoV2SingleReq, IoV2Beat


class _Interconnect(gvsoc.systree.Component):
    def __init__(self, parent, name, *, i3d, fabric, num_x, num_y, num_levels=3,
                 routing_mode=1, io_spill=2, data_width=64, addr_width=32,
                 source_contexts=4, memory_contexts=4, max_burst_beats=256,
                 axi_addr_width=32, axi_data_width=64, axi_id_width=10,
                 axi_len_width=8, memory_base=0,
                 interleave_bytes=4096, memory_bytes=4096):
        super().__init__(parent, name)
        values = dict(locals())
        for key in ('self', 'parent', 'name', '__class__'):
            values.pop(key, None)
        self.add_properties(values)
        self.add_sources(['pulp/3d_network/interconnect.cpp'])
        self.terminals = num_x * num_y
        self.output_signature = IoV2Beat(axi_data_width // 8) if i3d else IoV2SingleReq()

    def i_INPUT(self, terminal):
        """Source terminal, indexed x * num_y + y."""
        if not 0 <= terminal < self.terminals:
            raise ValueError('terminal outside network')
        return gvsoc.systree.SlaveItf(self, f'input_{terminal}',
                                     signature=IoV2SingleReq())

    def o_OUTPUT(self, terminal, itf):
        """Native ejection port, or I3D memory with a translated local address."""
        if not 0 <= terminal < self.terminals:
            raise ValueError('terminal outside network')
        self.itf_bind(f'output_{terminal}', itf, signature=self.output_signature)


class FatTreeInterconnect(_Interconnect):
    def __init__(self, parent, name, *, num_levels=3, routing_mode=1,
                 data_width=64, addr_width=32):
        x, y = 4, 2
        for level in range(2, num_levels + 1):
            scale = 16 if level == num_levels else 8
            if level % 2:
                y *= scale
            else:
                x *= scale
        super().__init__(parent, name, i3d=False, fabric=0, num_x=x, num_y=y,
                         num_levels=num_levels, routing_mode=routing_mode,
                         data_width=data_width, addr_width=addr_width)


class MeshInterconnect(_Interconnect):
    def __init__(self, parent, name, *, num_x=32, num_y=32, io_spill=2,
                 data_width=64, addr_width=32):
        super().__init__(parent, name, i3d=False, fabric=1, num_x=num_x,
                         num_y=num_y, io_spill=io_spill, routing_mode=0,
                         data_width=data_width, addr_width=addr_width)


class XbarInterconnect(_Interconnect):
    """Full packet crossbar with num_x * num_y paired input/output ports."""
    def __init__(self, parent, name, *, num_x=32, num_y=32, io_spill=2,
                 data_width=64, addr_width=32):
        super().__init__(parent, name, i3d=False, fabric=2, num_x=num_x,
                         num_y=num_y, io_spill=io_spill, routing_mode=0,
                         data_width=data_width, addr_width=addr_width)


class I3dInterconnect(_Interconnect):
    """AXI transaction wrapper for a 3D network with external memory endpoints."""
    def __init__(self, parent, name, *, fabric=0, num_x=32, num_y=32, **kwargs):
        super().__init__(parent, name, i3d=True, fabric=fabric,
                         num_x=num_x, num_y=num_y, **kwargs)
