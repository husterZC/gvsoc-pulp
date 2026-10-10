#
# Copyright (C) 2020 GreenWaves Technologies, SAS, ETH Zurich and University of Bologna
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

import gvsoc.systree
import regmap.regmap
import regmap.regmap_hjson
import regmap.regmap_c_header

class ClusterRegisters(gvsoc.systree.Component):

    def __init__(self, parent, name, num_cluster_x, num_cluster_y, boot_addr=0, nb_cores=1, cluster_id=0, sync_wakeup_addr=0, binary=None, matrix_engine=1, mxcore_owners=(), matrix_base=0, matrix_stride=0, matrix_irq=20, layout_base=0):
        super(ClusterRegisters, self).__init__(parent, name)

        self.add_sources(['pulp/chips/arche3d/logic/cluster_registers.cpp'])

        self.add_properties({
            'num_cluster_x': num_cluster_x,
            'num_cluster_y': num_cluster_y,
            'boot_addr': boot_addr,
            'nb_cores': nb_cores,
            'cluster_id': cluster_id,
            'sync_wakeup_addr': sync_wakeup_addr,
            'matrix_engine': matrix_engine, 'mxcore_owners': list(mxcore_owners),
            'matrix_base': matrix_base, 'matrix_stride': matrix_stride,
            'matrix_irq': matrix_irq, 'layout_base': layout_base,
        })

    def gen(self, builddir, installdir):
        comp_path = 'pulp/chips/arche3d/logic/snitch/snitch_cluster'
        regmap_path = f'{comp_path}/snitch_cluster_peripheral_reg.hjson'
        regmap_instance = regmap.regmap.Regmap('cluster_periph')
        regmap.regmap_hjson.import_hjson(regmap_instance, self.get_file_path(regmap_path))
        regmap.regmap_c_header.dump_to_header(regmap=regmap_instance, name='cluster_periph',
            header_path=f'{builddir}/{comp_path}/cluster_periph', headers=['regfields', 'gvsoc'])

    def o_EXTERNAL_IRQ(self, core: int, itf: gvsoc.systree.SlaveItf):
        self.itf_bind(f'external_irq_{core}', itf, signature='wire<bool>')

    def i_INPUT(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, 'input', signature='io')

    def i_BARRIER_ACK(self, core: int) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, f'barrier_req_{core}', signature='wire<bool>')

    def gen_gui(self, parent_signal):
        return gvsoc.gui.Signal(self, parent_signal, name=self.name, is_group=True, groups=["regmap"])

    def i_WAKEUP_SEND(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, 'wakeup_send', signature='io')

    def i_WAKEUP_RECV(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, 'wakeup_recv', signature='io')

    def o_WAKEUP(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('wakeup_output', itf, signature='io')

    def i_BOOT_READY(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, 'boot_ready', signature='wire<bool>')

    def o_FETCH_START(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind(f'fetch_start', itf, signature='wire<bool>')
