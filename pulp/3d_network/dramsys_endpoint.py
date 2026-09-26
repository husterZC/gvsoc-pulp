# SPDX-License-Identifier: Apache-2.0
"""One DRAMSys channel connected directly to an IO_v2 AXI beat port."""
import json
import os
from pathlib import Path
import re

import gvsoc.systree
from gvsoc.signature import IoV2Beat


class DramsysEndpoint(gvsoc.systree.Component):
    def __init__(self, parent, name, *, dram_type='hbm4-emu-example.json', config=None,
                 library='libDRAMSys_Simulator.so',
                 data_width=512, benchmark_init=False, endpoint_id=0, init_size=4096):
        super().__init__(parent, name)
        if data_width < 8 or data_width > 1024 or data_width & (data_width - 1):
            raise ValueError('data_width must be a power of two from 8 to 1024 bits')
        # Match memory.dramsys: DRAMSYS_PATH is the parent of dramsys_configs,
        # and dram_type selects a simulation JSON within that directory.
        # get_file_path also works with GVSoC's source and installed module paths.
        if config is None:
            if 'DRAMSYS_PATH' in os.environ:
                config = Path(os.environ['DRAMSYS_PATH']) / 'dramsys_configs' / dram_type
            else:
                config = self.get_file_path(f'memory/dramsys_configs/{dram_type}')
                if config is None:
                    raise FileNotFoundError(
                        f'DRAMSys configuration {dram_type!r} not found. Run make build-configs '
                        'or set DRAMSYS_PATH to the parent of dramsys_configs.')
        config = Path(config).resolve()

        def read_json(path):
            # DRAMSys configuration files may contain C/C++ comments.
            text = re.sub(r'("(?:\\.|[^"\\])*"|//[^\n]*|/\*[\s\S]*?\*/)',
                          lambda m: m[0] if m[0].startswith('"') else '', path.read_text())
            return json.loads(text)

        top = read_json(config)['simulation']
        reference = Path(top['memspec'])
        spec_path = (config.parent / reference if reference.parent != Path('.')
                     else config.parent / 'memspec' / reference)
        spec = read_json(spec_path)['memspec']
        arch = spec['memarchitecturespec']
        if arch['nbrOfChannels'] != 1:
            raise ValueError('Each endpoint must describe exactly one DRAM channel')
        native_bytes = arch['width'] * arch.get('nbrOfDevices', 1) // 8
        burst_bytes = native_bytes * arch.get('maxBurstLength', arch['burstLength'])
        size = (native_bytes * arch['nbrOfColumns'] * arch['nbrOfRows'] *
                arch['nbrOfBanks'] * arch.get('nbrOfPseudoChannels', 1) * arch.get('nbrOfStacks', 1))
        self.data_width = data_width
        self.add_properties(dict(require_systemc=True, config=str(config), library=library,
                                 resources=str(config.parent), data_width=data_width,
                                 burst_bytes=burst_bytes, size=size, benchmark_init=benchmark_init,
                                 endpoint_id=endpoint_id, init_size=init_size))
        self.add_properties({'dram-type': dram_type})
        self.add_sources(['pulp/3d_network/dramsys_endpoint.cpp'])

    def i_INPUT(self):
        """Local byte addresses; DRAMSys supplies capacity and memory timing.

        Read requests carry the whole burst; writes and read responses use
        data_width-bit beats. Ready read responses are interleaved round robin,
        with beat order preserved within each transaction. No
        MemoryEndpoint/read_slots stage is needed.
        """
        return gvsoc.systree.SlaveItf(self, 'input', signature=IoV2Beat(self.data_width // 8))
