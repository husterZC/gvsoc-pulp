# SPDX-License-Identifier: Apache-2.0
import gvsoc.systree as st


class Control(st.Component):
    def __init__(self, parent, name, arch, progress_cycles=10000, image=None):
        super().__init__(parent, name)
        self.add_sources(['pulp/chips/arche3d/control.cpp'])
        self.add_properties(dict(nx=arch.num_cluster_x, ny=arch.num_cluster_y,
            memory_base=arch.dram3d_start_base, interleave=arch.dram3d_node_interleave,
            axi_bytes=arch.i3d_axi_data_width // 8, progress_cycles=progress_cycles,
            watchdog_cycles=10_000_000, image_bytes=image.size if image else 0,
            preheat_lines=image.preheat_size // arch.icache_line_size if image else 0,
            preheat_base=image.preheat_base if image else 0,
            preheat_data=image.preheat_data.hex() if image else ''))

    def i_INPUT(self, cluster):
        return st.SlaveItf(self, f'input_{cluster}', signature='io')

    def i_ACTIVITY(self, cluster):
        return st.SlaveItf(self, f'activity_{cluster}', signature='wire<Arche3dDmaEvent>')

    def o_READY(self, itf):
        self.itf_bind('ready', itf, signature='wire<bool>')

    def i_IMAGE_LOADED(self):
        return st.SlaveItf(self, 'image_loaded', signature='wire<bool>')

    def i_CACHE_REFILLS(self, cluster):
        return st.SlaveItf(self, f'cache_refills_{cluster}', signature='wire<uint64_t>')

    def o_CACHE_PRELOAD(self, itf):
        self.itf_bind('cache_preload', itf, signature='wire<Arche3dIcachePreload>')
