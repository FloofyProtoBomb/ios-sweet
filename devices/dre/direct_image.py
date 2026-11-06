"""Prepare an ABL-loadable XNU memory image with build-time placement."""

import struct

PAGE = 0x4000
HEADER = 0x2a8
BOOT_LINE = 608
PHYS_BASE = 0x100000000
RAM_SIZE = (2048 - 1) << 20
SPLASH = 0x85200000


def align(value):
    return (value + PAGE - 1) & -PAGE


def flatten(payload):
    """Return initialized kernel/ADT/boot_args bytes and their physical layout."""
    if len(payload) < HEADER:
        raise ValueError('truncated XNU payload')
    magic, version, count = struct.unpack_from('<8sII', payload)
    virt, entry, adt_off, adt_size, end, base, ram = struct.unpack_from('<7Q', payload, 16)
    table_end = HEADER + 32 * count
    if (magic != b'SWEETXNU' or version != 1 or not 0 < count <= 256 or
            not table_end <= adt_off < len(payload) or not 0 < adt_size <= len(payload) - adt_off):
        raise ValueError('invalid XNU segment table or ADT')
    if ((base, ram) != (PHYS_BASE, RAM_SIZE) or virt % PAGE or end % PAGE or
            not virt <= entry < end or entry % 4):
        raise ValueError('unexpected DE2118 kernel address layout')
    bootargs = end + align(adt_size)
    top = bootargs + PAGE
    if top - virt > ram or top > (1 << 64) - 1:
        raise ValueError('XNU, ADT and boot arguments exceed owned RAM')

    cmdline = payload[0x48:HEADER]
    if b'\0' not in cmdline:
        raise ValueError('unterminated XNU command line')
    segments = sorted(struct.unpack_from('<4Q', payload, HEADER + i * 32) for i in range(count))
    first = segments[0][0]
    previous_end = virt
    entry_backed = False
    for address, extent, offset, size in segments:
        if (address % PAGE or extent % PAGE or not extent or
                not previous_end <= address < end or extent > end - address or
                not table_end <= offset <= adt_off or size > min(extent, adt_off - offset)):
            raise ValueError('invalid or overlapping XNU segment')
        previous_end = address + extent
        entry_backed |= address <= entry and entry + 4 <= address + size
    if not entry_backed or top - first > 95 * 1024**2:
        raise ValueError('entry is outside kernel bytes or flat image exceeds boot capacity')

    image = bytearray(top - first)
    for address, _, offset, size in segments:
        image[address - first:address - first + size] = payload[offset:offset + size]
    image[end - first:end - first + adt_size] = payload[adt_off:adt_off + adt_size]
    # XNU arm64 boot_args v2. The device tree pointer is a kernel VA.
    bootargs_offset = bootargs - first
    struct.pack_into('<HH4x4Q6QI4xQI', image, bootargs_offset,
                     2, 2, virt, base, ram, base + top - virt,
                     SPLASH, 1, 1080 * 4, 1080, 2400, 32, 0, end, adt_size)
    image[bootargs_offset + 108:bootargs_offset + 108 + BOOT_LINE] = cmdline
    struct.pack_into('<2Q', image, bootargs_offset + 720, 0, ram)
    layout = {'BASE': base, 'RAM': ram, 'DST': base + first - virt,
              'ENTRY': base + entry - virt, 'ADT': base + end - virt, 'ADT_SIZE': adt_size,
              'ARGS': base + bootargs - virt, 'BYTES': len(image), 'SPLASH': SPLASH}
    return bytes(image), layout
