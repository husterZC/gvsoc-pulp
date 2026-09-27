# SPDX-License-Identifier: Apache-2.0
"""SoftHier logic die, I3D interconnect die, and distributed DRAMSys channels."""
import importlib
import os
import gvsoc.systree as st
from gvrun.parameter import TargetParameter
from vp.clock_domain import Clock_domain
from interco.router import Router
from pulp.chips.arche3d.logic.flex_mesh_noc import FlexMeshNoC
from pulp.chips.arche3d.logic.flex_mesh_noc_v2 import FlexMeshNoCV2
from pulp.chips.arche3d.arch import load_arch
from pulp.chips.arche3d.cluster import Arche3dCluster
from pulp.chips.arche3d.control import Control
from pulp.chips.arche3d.instructions import ProgramImage, system_loader


class Board(st.Component):
    def __init__(self, parent, name, parser, options):
        super().__init__(parent, name, options=options)
        config = TargetParameter(self, name='config', value=os.environ.get('ARCHE3D_CONFIG', 'default'),
            cast=str, description='arche3d configuration name or Python file').get_value()
        arch = load_arch(config)
        self.add_property('architecture', vars(arch))
        progress = TargetParameter(self, name='progress_cycles', value=10000, cast=int,
            description='Progress interval; 0 disables periodic output').get_value()
        memory_init = TargetParameter(self, name='memory_init', value='zero', cast=str,
            description='DRAM initialization: zero or pattern (benchmark only)').get_value()
        if memory_init not in ('zero', 'pattern'):
            raise ValueError('memory_init must be zero or pattern')
        parser.add_argument('--binary', help='arche3d SDK ELF executable')
        args, _ = parser.parse_known_args()
        image = ProgramImage(arch, args.binary, initial_pattern=memory_init == 'pattern')
        clock = Clock_domain(self, 'clock', frequency=1_000_000_000)
        chip = st.Component(self, 'chip')
        clock.o_CLOCK(chip.i_CLOCK())
        control = Control(chip, 'control', arch, progress, image)
        nx, ny = arch.num_cluster_x, arch.num_cluster_y
        network_module = importlib.import_module('pulp.3d_network.interconnect')
        dram_module = importlib.import_module('pulp.3d_network.dramsys_endpoint')
        network = network_module.I3dInterconnect(chip, 'i3d_interconnect',
            fabric={'fattree': 0, 'mesh': 1, 'xbar': 2}[arch.i3d_fabric], num_x=nx, num_y=ny,
            num_levels=arch.i3d_fabric_level,
            routing_mode={'nca hash': 1, 'adaptive nca': 2}[arch.i3d_route_algo.lower().replace('_', ' ')],
            io_spill=arch.i3d_io_spill, axi_addr_width=arch.i3d_axi_addr_width,
            axi_data_width=arch.i3d_axi_data_width, axi_id_width=arch.i3d_axi_id_width,
            axi_len_width=arch.i3d_axi_len_width, source_contexts=arch.i3d_source_contexts,
            memory_contexts=arch.i3d_memory_contexts, max_burst_beats=arch.i3d_max_burst_beats,
            memory_base=arch.dram3d_start_base, interleave_bytes=arch.dram3d_vault_interleave,
            memory_bytes=arch.dram3d_vault_space)
        data_noc = FlexMeshNoCV2(chip, 'noc2d', width=arch.noc2d_link_width // 8,
            nb_x_clusters=nx, nb_y_clusters=ny, ni_outstanding_reqs=arch.noc2d_outstanding)
        sync_noc = FlexMeshNoC(chip, 'sync_noc', width=4, nb_x_clusters=nx, nb_y_clusters=ny,
            ni_outstanding_reqs=arch.noc2d_outstanding,
            router_input_queue_size=arch.noc2d_outstanding, atomics=1, collective=1,
            wakeup_addr=arch.sync_wakeup_addr)

        for cluster_id in range(nx * ny):
            x, y = cluster_id % nx, cluster_id // nx
            terminal = x * ny + y  # I3D terminals use x-major indexing.
            cluster = Arche3dCluster(chip, f'cluster_{cluster_id}', arch, cluster_id, image,
                check_pattern=memory_init == 'pattern')
            cluster.o_I3D(network.i_INPUT(terminal))
            cluster.o_I3D_ACTIVITY(control.i_ACTIVITY(cluster_id))
            memory = dram_module.DramsysEndpoint(chip, f'dram3d_{terminal}',
                dram_type=arch.dram3d_type, data_width=arch.i3d_axi_data_width,
                benchmark_init=memory_init == 'pattern', endpoint_id=terminal,
                init_size=arch.dram3d_vault_space)
            network.o_OUTPUT(terminal, memory.i_INPUT())
            narrow = Router(chip, f'control_router_{cluster_id}')
            narrow.o_MAP(control.i_INPUT(cluster_id), base=arch.soc_register_base,
                size=arch.soc_register_size, rm_base=True)
            cluster.o_NARROW_SOC(narrow.i_INPUT())
            control.o_READY(cluster.i_BOOT_READY())
            control.o_CACHE_PRELOAD(cluster.i_CACHE_PRELOAD())
            cluster.o_CACHE_REFILLS(control.i_CACHE_REFILLS(cluster_id))
            if cluster_id == 0:
                system_loader(chip, arch, image, control, cluster)
            cluster.o_WIDE_SOC(data_noc.i_CLUSTER_INPUT(x, y))
            data_noc.o_MAP(cluster.i_WIDE_INPUT(),
                base=arch.cluster_tcdm_remote + cluster_id * arch.cluster_tcdm_size,
                size=arch.cluster_tcdm_size, x=x+1, y=y+1)
            cluster.o_SYNC_OUTPUT(sync_noc.i_CLUSTER_INPUT(x, y))
            sync_noc.o_MAP(cluster.i_SYNC_INPUT(),
                base=arch.cluster_tcdm_remote + cluster_id * arch.cluster_tcdm_size,
                size=arch.cluster_tcdm_size, x=x+1, y=y+1)
