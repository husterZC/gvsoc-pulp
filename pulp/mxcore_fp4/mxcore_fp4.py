# SPDX-License-Identifier: Apache-2.0
"""MXFP4-only MXCore, calibrated for the 54 shapes documented in README.md."""
import gvsoc.systree as st


class MXCoreFP4(st.Component):
    """16 packed lanes (32 FP4 MACs) x 32 PEs, Reuse=32, 256-bit TCDM.

    INPUT and OUT use the io_v2 single-request protocol. For a beat-based
    interconnect, bind OUT through an io_v2 single-request-to-beat adapter.
    IRQ remains asserted until software clears completion (write FINISHED=1).
    """

    def __init__(self, parent, name):
        super().__init__(parent, name)
        self.set_component('pulp.mxcore_fp4.mxcore_fp4')

    def i_INPUT(self):
        return st.SlaveItf(self, 'input', signature='io_v2')

    def o_OUT(self, itf):
        self.itf_bind('out', itf, signature='io_v2')

    def o_IRQ(self, itf):
        self.itf_bind('irq', itf, signature='wire<bool>')
