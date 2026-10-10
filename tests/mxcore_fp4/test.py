# SPDX-License-Identifier: Apache-2.0
import os
import gvsoc.systree as st
import gvsoc.runner
import vp.clock_domain
from gvrun.parameter import TargetParameter
from pulp.mxcore_fp4 import MXCoreFP4


class Driver(st.Component):
    def __init__(self, parent, name, vectors, mode):
        super().__init__(parent, name)
        self.add_sources(['test.cpp'])
        self.add_properties({'vectors': vectors, 'mode': mode})


class Chip(st.Component):
    def __init__(self, parent, name=None):
        super().__init__(parent, name)
        vectors=TargetParameter(self,name='vectors',value=os.path.join(os.path.dirname(__file__),'build/calibration'),
                                description='Verified RTL fixtures',cast=str).get_value()
        mode=TargetParameter(self,name='mode',value='sync',description='Memory response mode',cast=str).get_value()
        clock=vp.clock_domain.Clock_domain(self,'clock',frequency=100_000_000)
        dut=MXCoreFP4(self,'dut')
        driver=Driver(self,'driver',vectors,mode)
        clock.o_CLOCK(dut.i_CLOCK()); clock.o_CLOCK(driver.i_CLOCK())
        driver.itf_bind('mmio',dut.i_INPUT(),signature='io_v2')
        dut.o_OUT(st.SlaveItf(driver,'memory',signature='io_v2'))
        dut.o_IRQ(st.SlaveItf(driver,'irq',signature='wire<bool>'))


class Target(gvsoc.runner.Target):
    gapy_description='MXCoreFP4 RTL calibration regression'
    model=Chip
    name='test'
