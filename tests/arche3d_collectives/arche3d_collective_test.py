# SPDX-License-Identifier: Apache-2.0
"""Production native collective endpoints/routers, without software cores or DRAM."""
import os
import gvsoc.systree as st
import gvsoc.runner
from vp.clock_domain import Clock_domain
from gvrun.parameter import TargetParameter
from pulp.chips.arche3d.logic.flex_mesh_noc_v2 import FlexMeshNoCV2


class Board(st.Component):
    def __init__(self, parent, name, parser, options):
        super().__init__(parent, name, options=options)
        nx = TargetParameter(self, name='nx', value=4, cast=int, description='Columns').get_value()
        ny = TargetParameter(self, name='ny', value=4, cast=int, description='Rows').get_value()
        if not 1 <= nx <= 32 or not 1 <= ny <= 32:
            raise ValueError('Use 1..32 nodes per axis')
        l1_base = TargetParameter(self, name='l1_base', value=0, cast=int,
            description='Local L1 address base').get_value()
        clock = Clock_domain(self, 'clock', frequency=1_000_000_000)
        soc = st.Component(self, 'soc')
        clock.o_CLOCK(soc.i_CLOCK())
        noc = FlexMeshNoCV2(soc, 'noc', width=128, l1_base=l1_base, nb_x_clusters=nx, nb_y_clusters=ny,
            ni_outstanding_reqs=int(os.environ.get('COLLECTIVE_NI_CAPACITY', '2')),
            router_input_queue_size=int(os.environ.get('COLLECTIVE_ROUTER_CAPACITY', '1')))
        test = st.Component(soc, 'test')
        test.add_sources(['collective_probe.cpp'])
        test.add_properties(dict(nx=nx, ny=ny, l1_base=l1_base,
            stream_perf=int(os.environ.get('COLLECTIVE_NI_CAPACITY', '2')) >= 64 and
                        int(os.environ.get('COLLECTIVE_ROUTER_CAPACITY', '1')) >= 2))
        for node in range(nx * ny):
            x, y = node % nx, node // nx
            test.itf_bind(f'out_{node}', noc.i_CLUSTER_INPUT(x, y), signature='io')
            test.itf_bind(f'control_{node}', noc.i_COLLECTIVE(x, y), signature='io')
            noc.o_MAP(st.SlaveItf(test, f'mem_{node}', signature='io'),
                base=0x30000000 + node * 0x10000, size=0x10000, x=x+1, y=y+1)


class Target(gvsoc.runner.Target):
    def __init__(self, parser, options=None, name=None):
        super().__init__(parser, options, name=name, model=Board,
            description='Native row/column collective correctness, backpressure and latency')
