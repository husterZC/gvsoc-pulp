# SPDX-License-Identifier: Apache-2.0
import gvsoc.systree as st


class MatrixBridge(st.Component):
    """Legacy cluster MMIO/RAM to MXCore's IO v2 single-request ports."""
    def __init__(self, parent, name, engine, l1_base=0):
        super().__init__(parent, name)
        legacy = st.Component(self, 'legacy')
        legacy.add_properties(dict(l1_base=l1_base))
        legacy.add_sources(['pulp/chips/arche3d/logic/matrix_bridge.cpp'])
        modern = st.Component(self, 'modern')
        modern.add_sources(['pulp/chips/arche3d/logic/matrix_bridge_v2.cpp'])
        for source, port, target, dest in (
                (legacy, 'config_request', modern, 'config_request'),
                (modern, 'config_done', legacy, 'config_done'),
                (modern, 'memory_request', legacy, 'memory_request'),
                (legacy, 'memory_done', modern, 'memory_done')):
            source.itf_bind(port, st.SlaveItf(target, dest, signature='wire<MatrixAccess *>'),
                            signature='wire<MatrixAccess *>')
        self.bind(modern, 'config', self, 'config')
        self.itf_bind('config', engine.i_INPUT(), signature='io_v2')
        engine.o_OUT(st.SlaveItf(self, 'memory_v2', signature='io_v2'))
        self.bind(self, 'memory_v2', modern, 'memory')
        self.bind(self, 'input', legacy, 'input')
        self.bind(legacy, 'memory', self, 'memory')

    def i_INPUT(self):
        return st.SlaveItf(self, 'input', signature='io')

    def o_MEMORY(self, itf):
        self.itf_bind('memory', itf, signature='io')
