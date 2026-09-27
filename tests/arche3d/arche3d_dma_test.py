# SPDX-License-Identifier: Apache-2.0
"""Small software test fixture; the arche3d production target stays 32 x 32."""
import importlib
import gvsoc.runner
import gvsoc.systree as st
from vp.clock_domain import Clock_domain
from interco.router import Router
from gvrun.parameter import TargetParameter
from pulp.chips.arche3d.arch import load_arch
from pulp.chips.arche3d.cluster import Arche3dCluster
from pulp.chips.arche3d.control import Control
from pulp.chips.arche3d.instructions import ProgramImage, system_loader
from pulp.chips.arche3d.logic.flex_mesh_noc import FlexMeshNoC
from pulp.chips.arche3d.logic.flex_mesh_noc_v2 import FlexMeshNoCV2


class Board(st.Component):
    def __init__(self, parent, name, parser, options):
        super().__init__(parent, name, options=options)
        parser.add_argument('--binary')
        args, _ = parser.parse_known_args()
        arch = load_arch()
        clusters = TargetParameter(self, name='clusters', value=1, cast=int,
            description='Test geometry: 1, 2, or 4 tiles (2 x 2)').get_value()
        if clusters not in (1, 2, 4):
            raise ValueError('The software fixture supports 1, 2, or 4 clusters')
        # This fixture exercises the production tile, DMA, I3D and DRAMSys models
        # at reduced scale; it is not an alternative architecture config.
        nx, ny = (2, 2) if clusters == 4 else (clusters, 1)
        arch.num_cluster_x, arch.num_cluster_y = nx, ny
        clock = Clock_domain(self, 'clock', frequency=1_000_000_000)
        chip = st.Component(self, 'chip')
        clock.o_CLOCK(chip.i_CLOCK())
        image = ProgramImage(arch, args.binary)
        control = Control(chip, 'control', arch, 10000, image)
        module = importlib.import_module('pulp.3d_network.interconnect')
        fabric = module.I3dInterconnect(chip, 'i3d', fabric=2, num_x=nx, num_y=ny,
            axi_addr_width=64, axi_data_width=512, source_contexts=8, memory_contexts=8,
            memory_base=arch.dram3d_start_base, memory_bytes=arch.dram3d_vault_space,
            interleave_bytes=arch.dram3d_vault_interleave)
        Dram = importlib.import_module('pulp.3d_network.dramsys_endpoint').DramsysEndpoint
        data_noc = FlexMeshNoCV2(chip, 'noc2d', width=arch.noc2d_link_width // 8,
            nb_x_clusters=nx, nb_y_clusters=ny, ni_outstanding_reqs=arch.noc2d_outstanding)
        sync_noc = FlexMeshNoC(chip, 'sync_noc', width=4, nb_x_clusters=nx,
            nb_y_clusters=ny, ni_outstanding_reqs=arch.noc2d_outstanding,
            router_input_queue_size=arch.noc2d_outstanding, atomics=1, collective=1,
            wakeup_addr=arch.sync_wakeup_addr)
        for cluster_id in range(clusters):
            x, y = cluster_id % nx, cluster_id // nx
            terminal = x * ny + y
            tile = Arche3dCluster(chip, f'cluster_{cluster_id}', arch, cluster_id, image)
            narrow = Router(chip, f'control_router_{cluster_id}')
            narrow.o_MAP(control.i_INPUT(cluster_id), base=arch.soc_register_base,
                         size=arch.soc_register_size, rm_base=True)
            tile.o_NARROW_SOC(narrow.i_INPUT())
            control.o_READY(tile.i_BOOT_READY())
            control.o_CACHE_PRELOAD(tile.i_CACHE_PRELOAD())
            tile.o_CACHE_REFILLS(control.i_CACHE_REFILLS(cluster_id))
            if cluster_id == 0:
                system_loader(chip, arch, image, control, tile)
            tile.o_I3D_ACTIVITY(control.i_ACTIVITY(cluster_id))
            memory = Dram(chip, f'dram_{cluster_id}', data_width=512, dram_type=arch.dram3d_type)
            tile.o_I3D(fabric.i_INPUT(terminal))
            fabric.o_OUTPUT(terminal, memory.i_INPUT())
            tile.o_WIDE_SOC(data_noc.i_CLUSTER_INPUT(x, y))
            data_noc.o_MAP(tile.i_WIDE_INPUT(),
                base=arch.cluster_tcdm_remote + cluster_id * arch.cluster_tcdm_size,
                size=arch.cluster_tcdm_size, x=x+1, y=y+1)
            tile.o_SYNC_OUTPUT(sync_noc.i_CLUSTER_INPUT(x, y))
            sync_noc.o_MAP(tile.i_SYNC_INPUT(),
                base=arch.cluster_tcdm_remote + cluster_id * arch.cluster_tcdm_size,
                size=arch.cluster_tcdm_size, x=x+1, y=y+1)


class Target(gvsoc.runner.Target):
    def __init__(self, parser, options=None, name=None):
        super().__init__(parser, options, name=name, model=Board,
            description='arche3d DMA and shared-memory software regression')
