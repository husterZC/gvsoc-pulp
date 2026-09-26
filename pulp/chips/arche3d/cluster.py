# SPDX-License-Identifier: Apache-2.0
"""Attach the dedicated I3D DMA to the arche3d-local logic tile."""
from pulp.chips.arche3d.logic.cluster_unit import ClusterUnit, ClusterArch
from pulp.chips.arche3d.dma import I3dDma
import gvsoc.systree as st
from gvsoc.signature import IoV2SingleReq


class Arche3dCluster(ClusterUnit):
    def __init__(self, parent, name, arch, cluster_id, binary, check_pattern=False):
        ca = ClusterArch(
            nb_core_per_cluster=arch.num_core_per_cluster, base=arch.cluster_tcdm_base,
            cluster_id=cluster_id, tcdm_size=arch.cluster_tcdm_size,
            stack_base=arch.cluster_stack_base, stack_size=arch.cluster_stack_size,
            zomem_base=arch.cluster_zomem_base, zomem_size=arch.cluster_zomem_size,
            reg_base=arch.cluster_reg_base, reg_size=arch.cluster_reg_size,
            sync_base=arch.sync_base, sync_itlv=arch.sync_interleave,
            sync_special_mem=arch.sync_special_mem, insn_base=arch.instruction_mem_base,
            insn_size=arch.instruction_mem_size, nb_tcdm_banks=arch.cluster_tcdm_bank_nb,
            tcdm_bank_width=arch.cluster_tcdm_bank_width // 8,
            redmule_ce_height=arch.redmule_ce_height, redmule_ce_width=arch.redmule_ce_width,
            redmule_ce_pipe=arch.redmule_ce_pipe, redmule_elem_size=arch.redmule_elem_size,
            redmule_queue_depth=arch.redmule_queue_depth, redmule_reg_base=arch.redmule_reg_base,
            redmule_reg_size=arch.redmule_reg_size, idma_outstand_txn=arch.idma_outstand_txn,
            idma_outstand_burst=arch.idma_outstand_burst, num_cluster_x=arch.num_cluster_x, num_cluster_y=arch.num_cluster_y,
            spatz_core_list=arch.spatz_attaced_core_list, spatz_num_vlsu=arch.spatz_num_vlsu_port,
            spatz_num_fu=arch.spatz_num_function_unit, spatz_vlsu_bw=32, spatz_vreg_gather_eff=100,
            data_bandwidth=arch.noc2d_link_width // 8, idma_gather_enable=True,
            idma_collective_enable=True, core_model='fast')
        super().__init__(parent, name, ca, binary,
            extra_dma_factory=lambda tile, _: (arch.num_core_per_cluster - 2,
                I3dDma(tile, 'i3d_dma', arch, check_pattern=check_pattern)))
        # ELF data/BSS segments are whole blocks. The scalar L1 interleaver
        # accepts one bank access; load through the existing DMA splitter.
        self.get_component('instr_router').o_MAP(self.get_component('tcdm').i_DMA_INPUT(),
            base=arch.cluster_tcdm_base, size=arch.cluster_tcdm_size, rm_base=True)
        self.extra_dma.o_AXI(st.SlaveItf(self, 'i3d', signature=IoV2SingleReq()))
        self.extra_dma.o_ACTIVITY(st.SlaveItf(self, 'i3d_activity',
            signature='wire<Arche3dDmaEvent>'))

    def o_I3D(self, itf):
        self.itf_bind('i3d', itf, signature=IoV2SingleReq())

    def o_I3D_ACTIVITY(self, itf):
        self.itf_bind('i3d_activity', itf, signature='wire<Arche3dDmaEvent>')
