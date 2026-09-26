# SPDX-License-Identifier: Apache-2.0
"""Full 32 x 32 sync-NoC regression without software cores or DRAMSys."""
import gvsoc.runner
import gvsoc.systree as st
from vp.clock_domain import Clock_domain
from pulp.chips.arche3d.arch import load_arch
from pulp.chips.arche3d.logic.flex_mesh_noc import FlexMeshNoC


class Board(st.Component):
    def __init__(self, parent, name, parser, options):
        super().__init__(parent, name, options=options)
        arch = load_arch()
        clock = Clock_domain(self, 'clock', frequency=1_000_000_000)
        chip = st.Component(self, 'chip')
        clock.o_CLOCK(chip.i_CLOCK())
        test = st.Component(chip, 'test')
        test.add_sources(['tests/arche3d/sync_checks.cpp'])
        test.add_properties(dict(nx=32, ny=32, remote_base=arch.cluster_tcdm_remote,
            l1_size=arch.cluster_tcdm_size, wakeup_addr=arch.sync_wakeup_addr))
        noc = FlexMeshNoC(chip, 'sync_noc', width=4, nb_x_clusters=32, nb_y_clusters=32,
            ni_outstanding_reqs=2, router_input_queue_size=1, atomics=1, collective=1,
            wakeup_addr=arch.sync_wakeup_addr)
        for cluster in range(1024):
            x, y = cluster % 32, cluster // 32
            test.itf_bind(f'source_{cluster}', noc.i_CLUSTER_INPUT(x, y), signature='io')
            noc.o_MAP(st.SlaveItf(test, f'target_{cluster}', signature='io'),
                base=arch.cluster_tcdm_remote + cluster * arch.cluster_tcdm_size,
                size=arch.cluster_tcdm_size, x=x+1, y=y+1, name=f'cluster_{cluster}')


class Target(gvsoc.runner.Target):
    def __init__(self, parser, options=None, name=None):
        super().__init__(parser, options, name=name, model=Board,
            description='arche3d full-size sync-NoC multicast and backpressure checks')
