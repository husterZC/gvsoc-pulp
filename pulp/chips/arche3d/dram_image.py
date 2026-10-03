# SPDX-License-Identifier: Apache-2.0
"""ELF-embedded data for the 64-bit, channel-interleaved DRAM address space.

RV32 PT_LOAD headers cannot name DRAM addresses above 4 GiB. The optional,
non-allocated .arche3d.dram section carries a little-endian header <8sII>
(magic, version, record count), then <QQQQ> records (physical destination,
section-relative data offset, file bytes, memory bytes), followed by payloads.
Memory bytes beyond file bytes are zero-filled, like an ELF load segment.
"""
import struct

SECTION = '.arche3d.dram'
MAGIC = b'A3DRAM\0\0'
VERSION = 1
HEADER = struct.Struct('<8sII')
RECORD = struct.Struct('<QQQQ')


def data_segments(elf, stream, memory_base, memory_size, reserved_size):
    section = elf.get_section_by_name(SECTION)
    if section is None:
        return []
    offset, size = section['sh_offset'], section['sh_size']
    stream.seek(0, 2)
    file_size = stream.tell()
    if (section['sh_type'] != 'SHT_PROGBITS' or section['sh_flags'] & 2 or
            size < HEADER.size or offset + size > file_size):
        raise ValueError('Invalid .arche3d.dram section')
    stream.seek(offset)
    magic, version, count = HEADER.unpack(stream.read(HEADER.size))
    table_end = HEADER.size + count * RECORD.size
    if magic != MAGIC or version != VERSION or table_end > size:
        raise ValueError('Invalid .arche3d.dram header')
    result = []
    for _ in range(count):
        address, source, file_bytes, memory_bytes = RECORD.unpack(stream.read(RECORD.size))
        if (not memory_bytes or file_bytes > memory_bytes or
                address < memory_base + reserved_size or
                address + memory_bytes > memory_base + memory_size or
                (file_bytes and (source < table_end or source + file_bytes > size))):
            raise ValueError('Invalid .arche3d.dram data record')
        result.append((address, offset + source if file_bytes else 0, file_bytes, memory_bytes))
    ordered = sorted(result)
    if any(a[0] + a[3] > b[0] for a, b in zip(ordered, ordered[1:])):
        raise ValueError('Overlapping .arche3d.dram destinations')
    return result


def channel_segments(segments, memory_base, channels, interleave, channel_size):
    """Split global byte ranges into channel-local file/zero-fill fragments."""
    result = [[] for _ in range(channels)]
    for address, source, file_bytes, memory_bytes in segments:
        offset = address - memory_base
        if offset < 0 or offset + memory_bytes > channels * channel_size:
            raise ValueError('ELF preload exceeds the configured DRAM capacity')
        consumed = 0
        while consumed < memory_bytes:
            stripe, within = divmod(offset + consumed, interleave)
            channel = stripe % channels
            local = (stripe // channels) * interleave + within
            length = min(memory_bytes - consumed, interleave - within)
            copied = max(0, min(length, file_bytes - consumed))
            if local + length > channel_size:
                raise ValueError('ELF preload exceeds channel capacity')
            result[channel].append([local, source + consumed if copied else 0, copied, length])
            consumed += length
    return result
