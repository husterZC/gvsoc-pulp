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
    # Older standalone configs retain the existing DRAMSys behavior.
    arch.dram3d_backend = getattr(arch, 'dram3d_backend', 'dramsys')
    arch.dram3d_ram_slots = getattr(arch, 'dram3d_ram_slots', 4)
    if arch.dram3d_backend not in ('dramsys', 'memory'):
        raise ValueError('dram3d_backend must be dramsys or memory')
    if (arch.dram3d_backend == 'memory' and
            (not isinstance(arch.dram3d_ram_slots, int) or arch.dram3d_ram_slots <= 0)):
        raise ValueError('dram3d_ram_slots must be a positive integer for the memory backend')
    if (arch.num_cluster_x, arch.num_cluster_y) != (32, 32):
        raise ValueError('arche3d currently requires a 32 x 32 logic die')
    if arch.soc_register_size < 0x2000:
        raise ValueError('System register window must include control and collective MMIO (8 KiB)')
    if arch.num_core_per_cluster < 2:
        raise ValueError('arche3d requires separate I3D and logic-die DMA cores')
    if arch.num_core_per_cluster - 2 in arch.spatz_attaced_core_list:
        raise ValueError('The I3D DMA core (n-2) must be separate from the Spatz cores')
    if (arch.cluster_stack_size <= 0 or
            arch.cluster_stack_size % (16 * arch.num_core_per_cluster)):
        raise ValueError('The stack reservation must give each core a positive, 16-byte-aligned stack')
    if arch.cluster_stack_base % 16:
        raise ValueError('The TCDM stack base must be 16-byte aligned')
    if (arch.cluster_stack_base < arch.cluster_tcdm_base + 64 or
            arch.cluster_stack_base + arch.cluster_stack_size !=
            arch.cluster_tcdm_base + arch.cluster_tcdm_size):
        raise ValueError('Stacks must occupy the top of TCDM and leave space below for data')
    remote_end = (arch.cluster_tcdm_remote + arch.cluster_tcdm_size *
                  arch.num_cluster_x * arch.num_cluster_y)
    if (arch.cluster_tcdm_remote < 0 or arch.cluster_tcdm_remote % 4 or
            remote_end > 1 << 32):
        raise ValueError('Remote L1 must be word aligned and fit the RV32 address space')
    if (arch.sync_wakeup_addr < 0 or arch.sync_wakeup_addr % 4 or
            arch.sync_wakeup_addr + 4 > 1 << 32):
        raise ValueError('The wakeup command must be word aligned and fit the RV32 address space')
    for base, size in ((arch.cluster_tcdm_remote, remote_end - arch.cluster_tcdm_remote),
                       (arch.cluster_tcdm_base, arch.cluster_tcdm_size),
                       (arch.cluster_reg_base, arch.cluster_reg_size + 128),
                       (arch.redmule_reg_base, arch.redmule_reg_size),
                       (arch.instruction_base, arch.num_cluster_x * arch.num_cluster_y *
                        arch.dram3d_vault_interleave),
                       (arch.soc_register_base, arch.soc_register_size)):
        if arch.sync_wakeup_addr < base + size and base < arch.sync_wakeup_addr + 4:
            raise ValueError('The wakeup command must not overlap L1 or other scalar mappings')
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
                 'i3d_memory_contexts', 'dram3d_vault_interleave', 'dram3d_vault_space'):
        if getattr(arch, attr) <= 0:
            raise ValueError(f'{attr} must be positive')
    if arch.dram3d_vault_space % arch.dram3d_vault_interleave:
        raise ValueError('DRAM vault space must contain complete interleaving stripes')
    if arch.dram3d_vault_space < 2 * arch.dram3d_vault_interleave:
        raise ValueError('DRAM needs a program stripe and at least one data stripe per vault')
    if arch.dram3d_vault_interleave % (width // 8):
        raise ValueError('DRAM interleave must be an integral number of AXI beats')
    if arch.dram3d_start_base % arch.dram3d_vault_interleave:
        raise ValueError('DRAM base must be interleave aligned')
    if arch.dram3d_start_base + 1024 * arch.dram3d_vault_space > 1 << arch.i3d_axi_addr_width:
        raise ValueError('DRAM address range exceeds AXI address width')
    for attr in ('icache_size', 'icache_line_size', 'icache_core_width'):
        value = getattr(arch, attr)
        if value <= 0 or value & (value - 1):
            raise ValueError(f'{attr} must be a positive power of two')
    if arch.icache_line_size < 32 or arch.icache_size < arch.icache_line_size:
        raise ValueError('Instruction lines must contain the 32-byte ISS prefetch, and fit the cache')
    if arch.icache_core_width < 256:
        raise ValueError('Each instruction port must supply at least a 256-bit ISS prefetch per cycle')
    if arch.dram3d_vault_interleave % arch.icache_line_size:
        raise ValueError('DRAM stripes must contain whole instruction cache lines')
    image_size = arch.num_cluster_x * arch.num_cluster_y * arch.dram3d_vault_interleave
    if (arch.instruction_base % arch.icache_line_size or arch.instruction_base < 0 or
            arch.instruction_base + image_size > 1 << 32):
        raise ValueError('The instruction alias must be cache-line aligned and fit RV32')
    for base, size in ((arch.cluster_tcdm_base, arch.cluster_tcdm_size),
                       (arch.cluster_tcdm_remote, remote_end - arch.cluster_tcdm_remote),
                       (arch.cluster_reg_base, arch.cluster_reg_size + 128),
                       (arch.redmule_reg_base, arch.redmule_reg_size),
                       (arch.soc_register_base, arch.soc_register_size)):
        if arch.instruction_base < base + size and base < arch.instruction_base + image_size:
            raise ValueError('The instruction alias overlaps another scalar mapping')
    return arch
