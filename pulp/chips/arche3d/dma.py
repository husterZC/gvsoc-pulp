# SPDX-License-Identifier: Apache-2.0
"""Sparse-capable XDMA with a bounded I3D backend and distinct live AXI IDs."""
import gvsoc.systree as st
from gvsoc.signature import IoV2SingleReq


class I3dDma(st.Component):
    def __init__(self, parent, name, arch, check_pattern=False):
        super().__init__(parent, name)
        local = st.Component(self, 'local')
        local.add_sources([
            'pulp/chips/arche3d/dma.cpp',
            'pulp/chips/arche3d/logic/idma/fe/idma_fe_xdma.cpp',
            'pulp/chips/arche3d/logic/idma/me/idma_me_2d.cpp',
        ])
        local.add_properties(dict(
            gather_enable=True, collective_enable=False,
            transfer_queue_size=arch.idma_outstand_txn,
            burst_queue_size=arch.idma_outstand_burst,
            loc_base=arch.cluster_tcdm_base, loc_size=arch.cluster_tcdm_size,
            tcdm_width=arch.cluster_tcdm_bank_nb * arch.cluster_tcdm_bank_width // 8,
            memory_base=arch.dram3d_start_base,
            memory_size=arch.num_cluster_x * arch.num_cluster_y * arch.dram3d_node_space,
            interleave_bytes=arch.dram3d_node_interleave,
            axi_bytes=arch.i3d_axi_data_width // 8,
            max_burst_beats=arch.i3d_max_burst_beats))
        axi = st.Component(self, 'axi')
        axi.add_sources(['pulp/chips/arche3d/dma_axi.cpp'])
        axi.add_properties(dict(capacity=arch.idma_outstand_burst,
            id_width=arch.i3d_axi_id_width, check_pattern=check_pattern,
            memory_base=arch.dram3d_start_base, interleave=arch.dram3d_node_interleave,
            terminals=arch.num_cluster_x * arch.num_cluster_y))
        self.bind(local, 'request', axi, 'request')
        self.bind(axi, 'done', local, 'done')
        for port, signature in (('offload', 'wire<IssOffloadInsn<uint32_t>*>'),):
            self.itf_bind(port, st.SlaveItf(local, port, signature=signature),
                          signature=signature, composite_bind=True)
        for component, port, signature in (
            (local, 'offload_grant', 'wire<IssOffloadInsnGrant<uint32_t>*>'),
            (local, 'tcdm', 'io'), (local, 'index', 'io'),
            (axi, 'axi', IoV2SingleReq()), (axi, 'activity', 'wire<Arche3dDmaEvent>')):
            component.itf_bind(port, st.SlaveItf(self, port, signature=signature),
                               signature=signature)

    def i_OFFLOAD(self):
        return st.SlaveItf(self, 'offload', signature='wire<IssOffloadInsn<uint32_t>*>')

    def o_OFFLOAD_GRANT(self, itf):
        self.itf_bind('offload_grant', itf, signature='wire<IssOffloadInsnGrant<uint32_t>*>')

    def o_TCDM(self, itf):
        self.itf_bind('tcdm', itf, signature='io')

    def o_INDEX(self, itf):
        self.itf_bind('index', itf, signature='io')

    def o_AXI(self, itf):
        self.itf_bind('axi', itf, signature=IoV2SingleReq())

    def o_ACTIVITY(self, itf):
        self.itf_bind('activity', itf, signature='wire<Arche3dDmaEvent>')
