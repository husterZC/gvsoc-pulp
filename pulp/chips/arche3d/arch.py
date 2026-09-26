# SPDX-License-Identifier: Apache-2.0
"""One architecture configuration shared by the hardware and SDK generator."""
import importlib
import importlib.util
from pathlib import Path


def load_arch(config='default'):
    path = Path(config)
    if path.is_file():
        spec = importlib.util.spec_from_file_location('arche3d_user_config', path.resolve())
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
    else:
        if not config.isidentifier():
            raise ValueError(f'No arche3d configuration: {config}')
        module = importlib.import_module(f'pulp.chips.arche3d.configs.{config}')
    arch = module.FlexClusterArch()
    if (arch.num_cluster_x, arch.num_cluster_y) != (32, 32):
        raise ValueError('arche3d currently requires a 32 x 32 logic die')
    if arch.num_core_per_cluster < 2:
        raise ValueError('arche3d requires separate I3D and logic-die DMA cores')
    if arch.num_core_per_cluster - 2 in arch.spatz_attaced_core_list:
        raise ValueError('The I3D DMA core (n-2) must be separate from the Spatz cores')
    if arch.i3d_fabric not in ('fattree', 'mesh', 'xbar'):
        raise ValueError('i3d_fabric must be fattree, mesh, or xbar')
    if arch.i3d_fabric == 'fattree' and arch.i3d_fabric_level != 3:
        raise ValueError('32 x 32 fat tree requires i3d_fabric_level=3')
    if arch.i3d_route_algo.lower().replace('_', ' ') not in ('nca hash', 'adaptive nca'):
        raise ValueError('i3d_route_algo must be NCA_HASH or adaptive nca')
    width = arch.i3d_axi_data_width
    if width < 8 or width > 1024 or width & (width - 1):
        raise ValueError('AXI data width must be a power of two from 8 to 1024 bits')
    if not 1 <= arch.i3d_axi_id_width <= 16:
        raise ValueError('AXI ID width must be between 1 and 16')
    if not 1 <= arch.i3d_max_burst_beats <= 1 << arch.i3d_axi_len_width:
        raise ValueError('AXI burst length does not fit the LEN width')
    if arch.idma_outstand_burst > 1 << arch.i3d_axi_id_width:
        raise ValueError('DMA outstanding bursts exceed the distinct AXI ID space')
    for attr in ('idma_outstand_txn', 'idma_outstand_burst', 'i3d_source_contexts',
                 'i3d_memory_contexts', 'dram3d_node_interleave', 'dram3d_node_space'):
        if getattr(arch, attr) <= 0:
            raise ValueError(f'{attr} must be positive')
    if arch.dram3d_node_space % arch.dram3d_node_interleave:
        raise ValueError('DRAM node space must contain complete interleaving stripes')
    if arch.dram3d_node_interleave % (width // 8):
        raise ValueError('DRAM interleave must be an integral number of AXI beats')
    if arch.dram3d_start_base % arch.dram3d_node_interleave:
        raise ValueError('DRAM base must be interleave aligned')
    if arch.dram3d_start_base + 1024 * arch.dram3d_node_space > 1 << arch.i3d_axi_addr_width:
        raise ValueError('DRAM address range exceeds AXI address width')
    return arch
