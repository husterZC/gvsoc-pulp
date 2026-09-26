# SPDX-License-Identifier: Apache-2.0
import gvsoc.systree as st


class Control(st.Component):
    def __init__(self, parent, name, arch, progress_cycles=10000):
        super().__init__(parent, name)
        self.add_sources(['pulp/chips/arche3d/control.cpp'])
        self.add_properties(dict(nx=arch.num_cluster_x, ny=arch.num_cluster_y,
            memory_base=arch.dram3d_start_base, interleave=arch.dram3d_node_interleave,
            axi_bytes=arch.i3d_axi_data_width // 8, progress_cycles=progress_cycles,
            watchdog_cycles=10_000_000))

    def i_INPUT(self, cluster):
        return st.SlaveItf(self, f'input_{cluster}', signature='io')

    def i_ACTIVITY(self, cluster):
        return st.SlaveItf(self, f'activity_{cluster}', signature='wire<Arche3dDmaEvent>')

    def o_READY(self, itf):
        self.itf_bind('ready', itf, signature='wire<bool>')
