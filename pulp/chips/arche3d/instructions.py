# SPDX-License-Identifier: Apache-2.0
"""Shared instruction image, Snitch cache and the arche3d I3D source adapter."""
from pathlib import Path
from elftools.elf.elffile import ELFFile
import gvsoc.systree as st
from gvsoc.signature import IoV2SingleReq
from interco.router import Router
from utils.loader.loader import ElfLoader


def instruction_window(arch):
    """Reserve the first interleaving stripe of every channel for one image."""
    return arch.num_cluster_x * arch.num_cluster_y * arch.dram3d_node_interleave


class ProgramImage:
    """Read ELF metadata once, without creating a loader in every tile."""
    def __init__(self, arch, binary, initial_pattern=False):
        self.binary = str(Path(binary).resolve()) if binary else None
        self.entry = arch.instruction_base
        self.size = 0
        segments = []
        if binary:
            with open(self.binary, 'rb') as stream:
                elf = ELFFile(stream)
                self.entry = elf.header['e_entry']
                executable = []
                for seg in elf.iter_segments():
                    if seg['p_type'] != 'PT_LOAD' or seg['p_memsz'] == 0:
                        continue
                    addr, size = seg['p_paddr'], seg['p_memsz']
                    offset = addr - arch.instruction_base
                    if offset < 0 or offset + size > instruction_window(arch):
                        raise ValueError('ELF load segments must be in the shared DRAM program '
                                         'alias; rebuild this binary with arche3d-sw')
                    self.size = max(self.size, offset + size)
                    segments.append((addr, seg['p_offset'], seg['p_filesz'], size))
                    if seg['p_flags'] & 1:
                        executable.append((seg['p_vaddr'], seg['p_vaddr'] + seg['p_filesz']))
                if not any(start <= self.entry < end for start, end in executable):
                    raise ValueError('ELF entry is outside its executable load segments')
        line = arch.icache_line_size
        # Warm the entry-containing cache-sized window. Larger programs continue
        # with demand refills; small images are fully resident before startup.
        self.preheat_base = self.entry & ~(line - 1)
        end = min(arch.instruction_base + self.size,
                  self.preheat_base + arch.icache_size)
        self.preheat_size = max(0, (end - self.preheat_base + line - 1) // line * line)
        # Build one host-side snapshot of the entry window, then broadcast it to
        # the caches at reset release. Preserve ELF zero-fill and initial DRAM
        # bytes in gaps/padding, including the optional benchmark pattern.
        self.preheat_data = bytearray(self.preheat_size)
        if initial_pattern:
            stride = arch.dram3d_node_interleave
            for i in range(self.preheat_size):
                offset = self.preheat_base + i - arch.instruction_base
                endpoint, local = divmod(offset, stride)
                self.preheat_data[i] = (17 * endpoint + 13 * local + (local >> 8)) & 255
        if binary:
            with open(self.binary, 'rb') as stream:
                for addr, file_offset, file_size, memory_size in segments:
                    start = max(addr, self.preheat_base)
                    stop = min(addr + memory_size, self.preheat_base + self.preheat_size)
                    if start >= stop:
                        continue
                    self.preheat_data[start - self.preheat_base:stop - self.preheat_base] = bytes(stop - start)
                    file_stop = min(stop, addr + file_size)
                    if start < file_stop:
                        stream.seek(file_offset + start - addr)
                        data = stream.read(file_stop - start)
                        if len(data) != file_stop - start:
                            raise ValueError('Truncated ELF preload segment')
                        self.preheat_data[start - self.preheat_base:file_stop - self.preheat_base] = data


class IoBridge(st.Component):
    """IO v1 ISS/loader to IO_v2 single-request cache/fabric adapter."""
    def __init__(self, parent, name, arch, read_only=False):
        super().__init__(parent, name)
        front = st.Component(self, 'v1')
        front.add_sources(['pulp/chips/arche3d/io_bridge.cpp'])
        front.add_properties(dict(base=arch.instruction_base, size=instruction_window(arch),
                                  read_only=read_only))
        back = st.Component(self, 'v2')
        back.add_sources(['pulp/chips/arche3d/io_bridge_v2.cpp'])
        self.bind(front, 'request', back, 'request')
        self.bind(back, 'done', front, 'done')
        self.itf_bind('input', st.SlaveItf(front, 'input', signature='io'),
                      signature='io', composite_bind=True)
        back.itf_bind('output', st.SlaveItf(self, 'output', signature=IoV2SingleReq()),
                     signature=IoV2SingleReq())

    def i_INPUT(self):
        return st.SlaveItf(self, 'input', signature='io')

    def o_OUTPUT(self, itf):
        self.itf_bind('output', itf, signature=IoV2SingleReq())


class I3dPort(st.Component):
    def __init__(self, parent, name, arch):
        super().__init__(parent, name)
        self.add_sources(['pulp/chips/arche3d/i3d_port.cpp'])
        self.add_properties(dict(memory_base=arch.dram3d_start_base,
            alias_base=arch.instruction_base, image_size=instruction_window(arch),
            interleave=arch.dram3d_node_interleave, id_width=arch.i3d_axi_id_width,
            max_bytes=arch.i3d_max_burst_beats * arch.i3d_axi_data_width // 8))

    def i_INPUT(self, port):
        return st.SlaveItf(self, f'input_{port}', signature=IoV2SingleReq())

    def o_OUTPUT(self, itf):
        self.itf_bind('output', itf, signature=IoV2SingleReq())

    def o_CACHE_REFILLS(self, itf):
        self.itf_bind('cache_refills', itf, signature='wire<uint64_t>')


class InstructionCache(st.Component):
    def __init__(self, parent, name, arch):
        super().__init__(parent, name)
        # Reuse PULP's IO_v2 Snitch cache. Direct mapping guarantees that a
        # contiguous 32-KiB preheat occupies every line without random eviction.
        cache = st.Component(self, 'shared')
        cache.add_sources(['pulp/chips/arche3d/logic/icache.cpp'])
        cache.add_properties(dict(size=arch.icache_size, line_size=arch.icache_line_size))
        bridge = IoBridge(self, 'core_bridge', arch, read_only=True)
        bridge.o_OUTPUT(st.SlaveItf(cache, 'input', signature=IoV2SingleReq()))
        for core in range(arch.num_core_per_cluster):
            port = Router(self, f'fetch_{core}', bandwidth=arch.icache_core_width // 8)
            port.o_MAP(bridge.i_INPUT())
            self.itf_bind(f'fetch_{core}', port.i_INPUT(), signature='io', composite_bind=True)
        self.itf_bind('data', bridge.i_INPUT(), signature='io', composite_bind=True)
        self.itf_bind('preload', st.SlaveItf(cache, 'preload', signature='wire<Arche3dIcachePreload>'),
                      signature='wire<Arche3dIcachePreload>', composite_bind=True)
        cache.itf_bind('refill', st.SlaveItf(self, 'refill', signature=IoV2SingleReq()),
                       signature=IoV2SingleReq())
        self.itf_bind('flush', st.SlaveItf(cache, 'flush', signature='wire<bool>'),
                      signature='wire<bool>', composite_bind=True)
        cache.itf_bind('flush_ack', st.SlaveItf(self, 'flush_ack', signature='wire<bool>'),
                       signature='wire<bool>')

    def i_FETCH(self, core):
        return st.SlaveItf(self, f'fetch_{core}', signature='io')

    def i_DATA(self):
        return st.SlaveItf(self, 'data', signature='io')

    def o_REFILL(self, itf):
        self.itf_bind('refill', itf, signature=IoV2SingleReq())

    def i_PRELOAD(self):
        return st.SlaveItf(self, 'preload', signature='wire<Arche3dIcachePreload>')

    def i_FLUSH(self):
        return st.SlaveItf(self, 'flush', signature='wire<bool>')

    def o_FLUSH_ACK(self, itf):
        self.itf_bind('flush_ack', itf, signature='wire<bool>')


def system_loader(parent, arch, image, control, cluster):
    """Exactly one ELF loader, connected through cluster zero's I3D source."""
    loader = ElfLoader(parent, 'loader', binary=image.binary)
    bridge = IoBridge(parent, 'loader_bridge', arch)
    loader.o_OUT(bridge.i_INPUT())
    bridge.o_OUTPUT(cluster.i_PROGRAM_LOAD())
    loader.o_START(control.i_IMAGE_LOADED())
