# SPDX-License-Identifier: Apache-2.0
import importlib
import gvsoc.systree
import vp.clock_domain
from gvsoc.signature import IoV2SingleReq, IoV2Beat
from gvrun.parameter import TargetParameter

models = importlib.import_module('pulp.3d_network.interconnect')
MemoryEndpoint = importlib.import_module('pulp.3d_network.memory_endpoint').MemoryEndpoint


class Chip(gvsoc.systree.Component):
    def __init__(self, parent, name=None, overrides=None):
        super().__init__(parent, name)
        defaults = dict(i3d=0, fabric=0, mode=1, nx=32, ny=32, groups=16,
                        repeats=1, seed=1, sparse=1, burst=1, sc=4, mc=4,
                        readslots=4, offset=0, datawidth=64, stress=0, spill=2,
                        functional=0, backing=1, endpoint=1, addrwidth=32,
                        frequency=100_000_000, interleave_bytes=4096,
                        memory_bytes=4096, progress_cycles=0)
        defaults.update(overrides or {})
        p = {k: TargetParameter(self, name=k, value=v, cast=int,
                               description=k).get_value() for k, v in defaults.items()}
        if p['fabric'] not in (0, 1, 2):
            raise ValueError('fabric must be 0 (fat tree), 1 (mesh), or 2 (crossbar)')
        if p['i3d'] and p['endpoint'] == 3:
            dram_type = TargetParameter(self, name='dram_type', value='hbm4-emu-fast.json',
                                       cast=str, description='DRAMSys simulation JSON').get_value()
        clock = vp.clock_domain.Clock_domain(self, 'clock', frequency=p['frequency'])
        # Include the RAM endpoint when building the default native target so
        # the installed benchmark can also run with i3d=1.
        if not p['i3d']:
            build_endpoint = MemoryEndpoint(self, 'endpoint_build', size=1)
            clock.o_CLOCK(build_endpoint.i_CLOCK())
        if p['i3d']:
            network = models.I3dInterconnect(self, 'network', fabric=p['fabric'],
                num_x=p['nx'], num_y=p['ny'], routing_mode=p['mode'],
                source_contexts=p['sc'], memory_contexts=p['mc'],
                axi_data_width=p['datawidth'], axi_addr_width=p['addrwidth'],
                io_spill=p['spill'], interleave_bytes=p['interleave_bytes'],
                memory_bytes=p['memory_bytes'])
        elif p['fabric'] == 1:
            network = models.MeshInterconnect(self, 'network', num_x=p['nx'],
                num_y=p['ny'], io_spill=p['spill'], data_width=p['datawidth'])
        elif p['fabric'] == 2:
            network = models.XbarInterconnect(self, 'network', num_x=p['nx'],
                num_y=p['ny'], io_spill=p['spill'], data_width=p['datawidth'])
        else:
            network = models.FatTreeInterconnect(self, 'network',
                routing_mode=p['mode'], data_width=p['datawidth'])
        driver = gvsoc.systree.Component(self, 'driver')
        driver.add_properties(p)
        driver.add_sources(['pulp/3d_network/benchmarks/traffic.cpp'])
        for i in range(p['nx'] * p['ny']):
            driver.itf_bind(f'output_{i}', network.i_INPUT(i), signature=IoV2SingleReq())
            backing = gvsoc.systree.SlaveItf(driver, f'input_{i}', signature=IoV2SingleReq())
            if p['i3d']:
                if p['endpoint'] == 0:
                    # Protocol probe: an immediate whole-read response on a
                    # beat port. This target has no memory-service component.
                    network.o_OUTPUT(i, gvsoc.systree.SlaveItf(driver, f'input_{i}',
                                                            signature=IoV2Beat(p['datawidth'] // 8)))
                    continue
                if p['endpoint'] == 2:
                    continue  # An unbound memory port must report decode errors.
                if p['endpoint'] == 3:
                    Dram = importlib.import_module('pulp.3d_network.dramsys_endpoint').DramsysEndpoint
                    memory = Dram(self, f'memory_{i}', data_width=p['datawidth'],
                        dram_type=dram_type,
                        benchmark_init=True, endpoint_id=i, init_size=p['memory_bytes'])
                    network.o_OUTPUT(i, memory.i_INPUT())
                    clock.o_CLOCK(memory.i_CLOCK())
                    continue
                memory = MemoryEndpoint(self, f'memory_{i}', data_width=p['datawidth'],
                                        size=p['memory_bytes'], read_slots=p['readslots'],
                                        benchmark_init=True, endpoint_id=i)
                network.o_OUTPUT(i, memory.i_INPUT())
                if p['backing']:
                    memory.o_OUTPUT(backing)
                clock.o_CLOCK(memory.i_CLOCK())
            else:
                network.o_OUTPUT(i, backing)
        clock.o_CLOCK(driver.i_CLOCK())
        clock.o_CLOCK(network.i_CLOCK())
