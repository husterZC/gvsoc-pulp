# SPDX-License-Identifier: Apache-2.0
"""Select the same distributed-memory endpoint for the chip and test fixture."""
import importlib


def create_memory_endpoint(parent, name, arch, terminal, image, benchmark_init=False):
    common = dict(data_width=arch.i3d_axi_data_width,
                  benchmark_init=benchmark_init, endpoint_id=terminal,
                  preload_file=image.binary, preload_segments=image.channel_preloads[terminal])
    if arch.dram3d_backend == 'memory':
        module = importlib.import_module('pulp.3d_network.memory_endpoint')
        return module.MemoryEndpoint(parent, name, size=arch.dram3d_vault_space,
                                     read_slots=arch.dram3d_ram_slots, **common)
    if arch.dram3d_backend == 'dramsys':
        module = importlib.import_module('pulp.3d_network.dramsys_endpoint')
        return module.DramsysEndpoint(parent, name, dram_type=arch.dram3d_type,
                                     init_size=arch.dram3d_vault_space, **common)
    raise ValueError('dram3d_backend must be dramsys or memory')
