# SPDX-License-Identifier: Apache-2.0
"""Sustained IO_v2 reads of one endpoint, with the NoC bypassed."""
import importlib

import gvsoc.systree
import vp.clock_domain
from gvrun.parameter import TargetParameter
from gvsoc.signature import IoV2Beat


class Chip(gvsoc.systree.Component):
    def __init__(self, parent, name=None):
        super().__init__(parent, name)
        defaults = dict(hbm=1, count=4096, window=8, burst=16, readslots=4,
                        stress=0, check_interleaving=0)
        p = {k: TargetParameter(self, name=k, value=v, cast=int,
                               description=k).get_value() for k, v in defaults.items()}
        clock = vp.clock_domain.Clock_domain(self, 'clock', frequency=1_000_000_000)
        simple = importlib.import_module('pulp.3d_network.memory_endpoint').MemoryEndpoint
        if p['hbm']:
            dram = importlib.import_module('pulp.3d_network.dramsys_endpoint').DramsysEndpoint
            memory = dram(self, 'memory', data_width=512,
                          dram_type=TargetParameter(self, name='dram_type',
                              value='hbm4-emu-fast.json', cast=str,
                              description='DRAMSys simulation JSON').get_value(),
                          benchmark_init=True, init_size=32768)
            # Include both implementations so hbm=0 works after installation.
            unused = simple(self, 'simple_build', size=1)
            clock.o_CLOCK(unused.i_CLOCK())
        else:
            memory = simple(self, 'memory', data_width=512, size=32768,
                            read_slots=p['readslots'], benchmark_init=True)
        driver = gvsoc.systree.Component(self, 'driver')
        driver.add_properties(p)
        driver.add_sources(['pulp/3d_network/benchmarks/endpoint.cpp'])
        driver.itf_bind('output', memory.i_INPUT(), signature=IoV2Beat(64))
        clock.o_CLOCK(memory.i_CLOCK())
        clock.o_CLOCK(driver.i_CLOCK())
