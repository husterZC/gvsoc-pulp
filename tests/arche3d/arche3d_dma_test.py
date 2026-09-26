# SPDX-License-Identifier: Apache-2.0
"""One-tile software test fixture; the arche3d production target stays 32 x 32."""
import importlib
from pathlib import Path
import gvsoc.runner
import gvsoc.systree as st
from vp.clock_domain import Clock_domain
from interco.router import Router
from pulp.chips.arche3d.arch import load_arch
from pulp.chips.arche3d.cluster import Arche3dCluster
from pulp.chips.arche3d.control import Control


class Board(st.Component):
    def __init__(self, parent, name, parser, options):
        super().__init__(parent, name, options=options)
        parser.add_argument('--binary')
        args, _ = parser.parse_known_args()
        arch = load_arch()
        # This fixture exercises the production tile, DMA, I3D and DRAMSys models
        # with a single endpoint; it is not an alternative architecture config.
        arch.num_cluster_x = arch.num_cluster_y = 1
        clock = Clock_domain(self, 'clock', frequency=1_000_000_000)
        chip = st.Component(self, 'chip')
        clock.o_CLOCK(chip.i_CLOCK())
        binary = str(Path(args.binary).resolve()) if args.binary else None
        tile = Arche3dCluster(chip, 'cluster_0', arch, 0, binary)
        control = Control(chip, 'control', arch, 10000)
        narrow = Router(chip, 'narrow')
        narrow.o_MAP(control.i_INPUT(0), base=arch.soc_register_base,
                     size=arch.soc_register_size, rm_base=True)
        tile.o_NARROW_SOC(narrow.i_INPUT())
        control.o_READY(tile.i_HBM_PRELOAD_DONE())
        tile.o_I3D_ACTIVITY(control.i_ACTIVITY(0))
        module = importlib.import_module('pulp.3d_network.interconnect')
        fabric = module.I3dInterconnect(chip, 'i3d', fabric=2, num_x=1, num_y=1,
            axi_addr_width=64, axi_data_width=512, source_contexts=8, memory_contexts=8,
            memory_base=arch.dram3d_start_base, memory_bytes=arch.dram3d_node_space,
            interleave_bytes=arch.dram3d_node_interleave)
        Dram = importlib.import_module('pulp.3d_network.dramsys_endpoint').DramsysEndpoint
        memory = Dram(chip, 'dram', data_width=512, dram_type=arch.dram3d_type)
        tile.o_I3D(fabric.i_INPUT(0))
        fabric.o_OUTPUT(0, memory.i_INPUT())
        # Local loopback only for unused logic-die ports in this fixture.
        tile.o_WIDE_SOC(tile.i_WIDE_INPUT())
        tile.o_SYNC_OUTPUT(tile.i_SYNC_INPUT())


class Target(gvsoc.runner.Target):
    def __init__(self, parser, options=None, name=None):
        super().__init__(parser, options, name=name, model=Board,
            description='arche3d single-tile DMA software regression')
