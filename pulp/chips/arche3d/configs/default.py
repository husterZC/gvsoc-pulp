# Copyright (C) 2025 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
# Author: Chi Zhang <chizhang@ethz.ch>

class FlexClusterArch:
    def __init__(self):
        self.num_cluster_x = 32
        self.num_cluster_y = 32
        self.num_core_per_cluster = 6
        self.cluster_tcdm_bank_width = 32
        self.cluster_tcdm_bank_nb = 128
        self.cluster_tcdm_base = 0x00000000
        self.cluster_tcdm_size = 0x00060000
        self.cluster_tcdm_remote = 0x30000000
        self.cluster_stack_base = 0x10000000
        self.cluster_stack_size = 0x00020000
        self.cluster_zomem_base = 0x18000000
        self.cluster_zomem_size = 0x00020000
        self.cluster_reg_base = 0x20000000
        self.cluster_reg_size = 0x00000200

        self.spatz_attaced_core_list = [0, 1, 2, 3]
        self.spatz_num_vlsu_port = 8
        self.spatz_num_function_unit = 4
        self.redmule_ce_height = 32
        self.redmule_ce_width = 16
        self.redmule_ce_pipe = 1
        self.redmule_elem_size = 2
        self.redmule_queue_depth = 1
        self.redmule_reg_base = 0x20020000
        self.redmule_reg_size = 0x00000200

        self.idma_outstand_txn = 64
        self.idma_outstand_burst = 256
        self.noc2d_outstanding = 64
        self.noc2d_link_width = 1024

        self.i3d_fabric = 'fattree'
        self.i3d_fabric_level = 3
        self.i3d_route_algo = 'adaptive nca'
        self.i3d_io_spill = 2
        self.i3d_axi_addr_width = 64
        self.i3d_axi_data_width = 512
        self.i3d_axi_id_width = 10
        self.i3d_axi_len_width = 8
        self.i3d_source_contexts = 8
        self.i3d_memory_contexts = 8
        self.i3d_max_burst_beats = 256

        self.dram3d_type = 'hbm4-emu-fast.json'
        self.dram3d_start_base = 0x100000000
        self.dram3d_node_space = 0x8000
        self.dram3d_node_interleave = 0x8000

        self.instruction_mem_base = 0x80000000
        self.instruction_mem_size = 0x00010000
        self.soc_register_base = 0x90000000
        self.soc_register_size = 0x00010000
        self.soc_register_eoc = 0x90000000
        self.soc_register_wakeup = 0x90000004
        self.sync_base = 0x40000000
        self.sync_interleave = 0x00000080
        self.sync_special_mem = 0x00000040
