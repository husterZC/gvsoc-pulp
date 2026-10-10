# SPDX-License-Identifier: Apache-2.0
import gvsoc.systree as st


class L1Fabric(st.Component):
    """Bank arbitration: DMA/BUS, then the shared HWPE bus, then cores/VLSUs."""
    def __init__(self, parent, name, *, banks, bank_width, size, low_ports,
                 hwpe_ports, hwpe_bandwidth):
        super().__init__(parent, name)
        self.add_sources(['pulp/chips/arche3d/logic/l1_fabric.cpp'])
        self.add_properties(dict(banks=banks, bank_width=bank_width, size=size,
            low_ports=low_ports, hwpe_ports=hwpe_ports, hwpe_bandwidth=hwpe_bandwidth))
