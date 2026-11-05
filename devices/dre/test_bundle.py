#!/usr/bin/env python3
"""Exercise the exact bundle Image with constrained MMIO and a synthetic disk.

No device is accessed. The private FDT is an input, never included or printed.
The UFS model inspects the actual descriptors and rejects every non-read opcode.
Fault cases inject failure returns at the guarded-MMIO helper; this does not
model Qualcomm bus/XPU timing, SError or the real exception handler.
"""
import argparse
import json
from pathlib import Path
import re
import struct
import sys
import uuid
import zlib

from unicorn import (Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_INSN,
                     UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE)
from unicorn.arm64_const import (UC_ARM64_INS_MRS, UC_ARM64_REG_PC, UC_ARM64_REG_X0,
                                UC_ARM64_REG_X1, UC_ARM64_REG_X30)

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import adt

OFF = 1 << 31
SMMU = 0x0c600000
UFS = 0x04804000
VOTE = 0x0147900c
MASK = 0x4c0
LOAD = 0xa8000000
LAST_LBA = 0x1000000
BLOCK_SIZE = 4096
CLOCKS = (0x01445098, 0x01445014, 0x01445010, 0x01445040)


def symbols(path, image):
    p = path.read_bytes()
    shoff, = struct.unpack_from('<Q', p, 40)
    entsize, count = struct.unpack_from('<HH', p, 58)
    sections = [struct.unpack_from('<IIQQQQIIQQ', p, shoff + i * entsize) for i in range(count)]
    for section in sections:
        if section[1] == 1 and section[2] & 2:
            assert image[section[3]:section[3] + section[5]] == p[section[4]:section[4] + section[5]], 'ELF/Image mismatch'
    result = {}
    for section in sections:
        if section[1] != 2:
            continue
        strings = sections[section[6]]
        names = p[strings[4]:strings[4] + strings[5]]
        for off in range(section[4], section[4] + section[5], section[9]):
            name, _, _, _, value, size = struct.unpack_from('<IBBHQQ', p, off)
            name = names[name:names.index(0, name)].decode()
            if name:
                result[name] = LOAD + value
                if name == 'report': result['report_size'] = size
    return result


def change_property(p, target_path, target_name, change):
    """Replace an existing property, updating FDT sizes/offsets when it grows."""
    header = list(struct.unpack_from('>10I', p))
    off, end = header[2], header[2] + header[9]
    names = p[header[3]:header[3] + header[8]]
    stack = []
    while off < end:
        start = off
        token, = struct.unpack_from('>I', p, off); off += 4
        if token == 1:
            last = p.index(0, off); stack.append(p[off:last].decode()); off = (last + 4) & -4
        elif token == 2:
            stack.pop()
        elif token == 3:
            n, key = struct.unpack_from('>II', p, off); off += 8
            name = names[key:names.index(0, key)].decode()
            stop = (off + n + 3) & -4
            if '/'.join(stack) == target_path and name == target_name:
                value = change(p[off:off + n])
                prop = struct.pack('>3I', 3, len(value), key) + value + bytes(-len(value) % 4)
                delta = len(prop) - (stop - start)
                p = p[:start] + prop + p[stop:]
                header[1] += delta; header[3] += delta; header[9] += delta
                return struct.pack('>10I', *header) + p[40:]
            off = stop
        elif token == 9:
            break
        else:
            assert token == 4
    raise AssertionError((target_path, target_name))


def disk(case):
    entries = bytearray(16384)
    entry = 40 * 128  # Exercise an entry beyond the first array block.
    entries[entry:entry + 16] = bytes(range(1, 17))
    struct.pack_into('<QQ', entries, entry + 32, 4096, LAST_LBA - 6)
    entries[entry + 56:entry + 74] = 'userdata\0'.encode('utf-16-le')
    if case == 'duplicate_userdata':
        entries[entry + 128:entry + 256] = entries[entry:entry + 128]
    header = bytearray(4096)
    header[:8] = b'EFI PART'
    struct.pack_into('<III', header, 8, 0x10000, 92, 0)
    struct.pack_into('<4Q', header, 24, 1, LAST_LBA, 6, LAST_LBA - 6)
    struct.pack_into('<QIII', header, 72, 2, 128, 128, zlib.crc32(entries))
    if case == 'gpt_overflow':
        struct.pack_into('<Q', header, 72, 0xffffffff)
        struct.pack_into('<Q', header, 40, 0x100000010)
        struct.pack_into('<Q', header, 48, 0x100000020)
        struct.pack_into('<Q', header, 32, 0x100000100)
    struct.pack_into('<I', header, 16, zlib.crc32(header[:92]))
    if case == 'header_crc':
        header[56] ^= 1
    if case == 'array_crc':
        entries[0] ^= 1
    return {1: bytes(header), **{2 + i: bytes(entries[i * 4096:(i + 1) * 4096]) for i in range(4)}}


def run(image, dtb, syms, case, context_probe=False, driver_prep=False, handoff_read=False, native_ufs=False):
    if case == 'dt_mismatch':
        dtb = dtb.replace(b'qcom,holi-gcc\0', b'qcom,xoli-gcc\0')
    elif case == 'touch_dt_mismatch':
        dtb = dtb.replace(b'qcom,spi-geni\0', b'qcom,xpi-geni\0')
    elif case == 'smmu_dt_mismatch':
        dtb = dtb.replace(b'qcom,qsmmu-v500\0', b'qcom,xsmmu-v500\0')
    elif case in ('panel_unknown', 'panel_ambiguous', 'panel_dsjm'):
        def panel(args):
            if case == 'panel_unknown':
                return args.replace(b'tp_nt36672c_tm', b'xp_nt36672c_tm')
            if case == 'panel_dsjm':
                return args.replace(b'tp_nt36672c_tm', b'tp_nt36672c_dsjm')
            return args.rstrip(b'\0') + b' tp_nt36672c_dsjm\0'
        dtb = change_property(dtb, '/chosen', 'bootargs', panel)
    elif case == 'firmware_bad':
        dtb = change_property(dtb, '/soc/spi@4a80000/novatek@0', 'firmware-data-0', lambda p: bytes(len(p)))
    elif case == 'gpi_ee_mismatch':
        dtb = change_property(dtb, '/soc/qcom,gpi-dma@4a00000', 'qcom,gpi-ee-offset', lambda p: struct.pack('>I', 0x20000))
    elif case == 'gpi_dma_mismatch':
        dtb = change_property(dtb, '/soc/qcom,gpi-dma@4a00000', '#dma-cells', lambda p: struct.pack('>I', 4))
    kernel = image[4096:4096 + struct.unpack_from('<I', image, 8)[0]]
    payload = kernel.index(b'SWEETXNU\x01\0\0\0')
    va, entry, _, _, end, base, _ = struct.unpack_from('<7Q', kernel, payload + 16)
    cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    # The old synthetic FDT address was inside the live secure CDSP carve-out.
    # Runtime ownership checks require an unreserved RAM address.
    tree = 0xa0000000 if native_ufs else 0x82000000
    for address, contents in ((LOAD, kernel), (tree, dtb)):
        cpu.mem_map(address, (len(contents) + 4095) & -4096); cpu.mem_write(address, contents)
    cpu.mem_map(0x85200000, 0xc00000)
    cpu.mem_map(base, (end - va + 0x20000 + 4095) & -4096)
    regs = {0x01445004: OFF, 0x01445020: 0, 0x01445024: 0x100401,
            0x01445060: 0, 0x01445064: 0x100203,
            UFS: 1 << 24, UFS + 8: 0x300, UFS + 0x34: 1, UFS + 0x30: 0x10f,
            UFS + 0x58: 0, UFS + 0x78: 0, UFS + 0x50: 0x90000000,
            UFS + 0x54: 0, UFS + 0x60: 1, UFS + 0x24: 0xff, UFS + 0x5c: 0, UFS + 0x20: 0,
            0x0141a004: OFF, 0x0141a010: 0x4221, 0x0141a084: 1,
            0x04e0c120: 0x5533330a, 0x04e0c110: 0x102000,
            0x04e0c700: 0, 0x04e0c704: 0, 0x04e0c70c: 1 << 22, 0x04e0c720: 3, 0x04e0c408: 4096,
            VOTE: 0x09000020, 0x0141f004: OFF | 2, 0x0141f008: OFF | 2, 0x0141f144: OFF,
            0x04a80068: 0x142, 0x04a80040: 0, 0x04a80064: 0,
            0x04a80e24: 0x20100000, 0x04a80e28: 0x20100000,
            0x00500000: 8, 0x00501000: 8, 0x00502000: 4, 0x00503000: 4, 0x00516004: 1,
            0x01436004: 1, 0x0599106c: 0x222001, 0x0599100c: 0x222001,
            SMMU: 1 << 10, SMMU + 0x20: (1 << 30) | (1 << 29) | (1 << 27) | (8 << 9) | 8,
            SMMU + 0x24: (6 << 28) | 32}
    regs.update({a: v for a, v in zip(CLOCKS, (1, OFF | 0xb, 0x229, 0x229))})
    for i in range(8):
        regs[SMMU + 0x800 + i * 4] = 0
        regs[SMMU + 0xc00 + i * 4] = 0
    regs[SMMU + 0x800] = OFF | 0x60; regs[SMMU + 0xc00] = 1 << 16
    regs[SMMU + 0x804] = OFF | 0xe0; regs[SMMU + 0xc04] = 1 << 16
    if driver_prep:
        regs[SMMU + 0x808] = OFF | 0x16; regs[SMMU + 0xc08] = 1 << 16
        regs.update({0x04e0c140: 32 << 24 | 2, 0x04e0c144: 1 << 15,
                     0x04e0c14c: 6 << 12 | 3 << 18, 0x04e0c720: 0, 0x04e0c408: 0,
                     0x04a8007c: 2, 0x04a80048: 4 << 4 | 1})
        for i in range(5):
            for off in (0x20000, 0x20080, 0x21000): regs[0x049f0000 + off + i * 0x4000] = 0
    route, io = 'BYPASS', 'GPT_PASS'
    touch, fw, usb = 'SPI_PIO_READY', 'LAYOUT_OK', 'HALTED'
    unsafe = case in ('restore_failed', 'touch_restore_failed', 'abort_stuck', 'host_lost', 'handoff_guard_mid', 'handoff_restore_failed')
    fault_reads = set()
    if case == 'translated':
        regs[SMMU + 0xc00] = 7; route, io = 'TRANSLATED', 'DMA_BLOCKED'
    elif case == 'route_fault':
        regs[SMMU + 0xc00] = 2 << 16; route, io = 'FAULT', 'DMA_BLOCKED'
    elif case == 'route_reserved':
        regs[SMMU + 0xc00] = 3 << 16; route, io = 'RESERVED', 'DMA_BLOCKED'
    elif case in ('unmatched_fault', 'unmatched_bypass'):
        regs[SMMU + 0x800] = 0
        if case == 'unmatched_bypass': regs[SMMU] = 0
        route = case.upper(); io = 'GPT_PASS' if case.endswith('bypass') else 'DMA_BLOCKED'
    elif case == 'ambiguous':
        regs[SMMU + 0x808] = OFF | 0x60; regs[SMMU + 0xc08] = 1 << 16
        route, io = 'AMBIGUOUS', 'DMA_BLOCKED'
    elif case == 'masked':
        regs[SMMU + 0x800] = OFF | (15 << 16) | 0x60
    elif case == 'extended':
        regs[SMMU + 0x20] |= 1 << 8; regs[SMMU] |= 1 << 3
        regs[SMMU + 0x800] = 0x60; regs[SMMU + 0x804] = 0xe0
        regs[SMMU + 0xc00] |= 1 << 10; regs[SMMU + 0xc04] |= 1 << 10
    elif case == 'bad_smmu_id':
        regs[SMMU + 0x20] = 0; route, io = 'BAD_ID', 'DMA_BLOCKED'
    elif case == 'client_disabled':
        regs[SMMU] |= 1; route, io = 'CLIENT_DISABLED', 'DMA_BLOCKED'
    elif case == 'indexed':
        regs[SMMU + 0x20] &= ~(1 << 27); route, io = 'INDEXED_UNSUPPORTED', 'DMA_BLOCKED'
    elif case in ('smmu_read_fault', 'smmu_late_read_fault'):
        fault_reads.add(SMMU + (0x20 if case == 'smmu_read_fault' else 0x81c))
        route, io = 'READ_FAULT', 'DMA_BLOCKED'
    elif case == 'groups_unsupported':
        regs[SMMU + 0x20] = regs[SMMU + 0x20] & ~255 | 129
        route, io = 'GROUPS_UNSUPPORTED', 'DMA_BLOCKED'
    elif case == 'sid_unsupported':
        regs[SMMU + 0x20] = regs[SMMU + 0x20] & ~(15 << 9) | (6 << 9)
        route, io = 'SID_UNSUPPORTED', 'DMA_BLOCKED'
    elif case == 'host_busy':
        regs[UFS + 0x78] = 1; io = 'HOST_NOT_IDLE'
    elif case == 'ring_overlap':
        regs[UFS + 0x50] = syms['arena']; io = 'RING_OVERLAP'
    elif case == 'hci_read_fault':
        fault_reads.add(UFS); io = 'HCI_READ_FAULT'
    elif case == 'cache_on':
        io = 'CACHE_OR_MMU_ON'
    elif case == 'host_off':
        regs[0x01445004] = 0; route, io = 'NOT_READY', 'NOT_READY'
    elif case == 'ufs_clock_timeout':
        regs[CLOCKS[2]] = OFF; route, io = 'NOT_READY', 'NOT_READY'
    elif case == 'touch_clock_timeout':
        touch = 'CLOCK_TIMEOUT'
    elif case == 'touch_read_fault':
        fault_reads.add(0x04a80068); touch = 'GENI_READ_FAULT'
    elif case == 'touch_no_fw':
        regs[0x04a80068] = 0; touch = 'SPI_FW_MISSING'
    elif case == 'touch_dma_only':
        regs[0x04a80064] = 1; touch = 'SPI_GPI_ONLY'
    elif case == 'touch_fifo_width':
        regs[0x04a80e24] = 0x00100000; touch = 'SPI_FIFO_UNSUPPORTED'
    elif case == 'touch_fifo_depth':
        regs[0x04a80e28] = 0x20000000; touch = 'SPI_FIFO_UNSUPPORTED'
    elif case == 'touch_busy':
        regs[0x04a80040] = 1; touch = 'SPI_BUSY'
    elif case == 'touch_already_voted':
        regs[VOTE] |= MASK
    elif case == 'touch_restore_failed':
        touch = 'RESTORE_FAILED'
    elif case == 'touch_dt_mismatch':
        touch = 'DT_MISMATCH'
    elif case == 'smmu_dt_mismatch':
        route, io = 'DT_MISMATCH', 'DMA_BLOCKED'
    elif case in ('panel_unknown', 'panel_ambiguous'):
        fw = 'PANEL_UNCONFIRMED'
    elif case == 'firmware_bad':
        fw = 'END_MARKER'
    elif case == 'usb_running':
        regs[0x04e0c704] = OFF; regs[0x04e0c70c] = 0; usb = 'RUNNING'
    elif case == 'usb_resetting':
        regs[0x04e0c704] = 1 << 30; usb = 'RESETTING'
    elif case == 'usb_power_off':
        regs[0x0141a004] = 0; usb = 'POWER_OFF'
    elif case == 'usb_translated':
        regs[SMMU + 0xc04] = 3
    elif case.startswith(('context_', 'walk_')) or case == 'both_translated':
        regs[SMMU + 0xc00] = 2; route, io = 'TRANSLATED', 'DMA_BLOCKED'
        if case == 'both_translated' or case.startswith('walk_'): regs[SMMU + 0xc04] = 1
        elif case == 'context_layout': regs[SMMU + 0x24] = 32
        elif case == 'context_64k': regs[SMMU + 0x24] = (1 << 31) | (2 << 28) | 8
        elif case == 'context_handset_span':
            # Android observed 0x80000 hardware bytes in the 0x100000 DT
            # reservation. ID1's page-size/bank-count bits remain modeled.
            regs[SMMU + 0x24] = (5 << 28) | 32
            regs[SMMU + 0xc04] = 1
        elif case == 'context_outside_resource': regs[SMMU + 0x24] = (7 << 28) | 32
        elif case == 'context_banks_outside_span': regs[SMMU + 0x24] = (5 << 28) | 65
        elif case == 'context_bank_out_of_range': regs[SMMU + 0x24] = (5 << 28) | 2
    elif case == 'gpi_busy': regs[0x04a10000] = 2 << 20
    elif case in ('gpi_translated_identity', 'gpi_translated_missing'): regs[SMMU + 0xc08] = 5
    elif case == 'gpi_error': regs[0x04a10000] = 15 << 20
    elif case == 'gpi_fault': fault_reads.add(0x04a10000)
    elif case == 'spi_clock_unset': regs[0x04a80048] = 0
    elif case == 'usb_caps_fault': fault_reads.add(0x04e0c140)
    elif case == 'usb_caps_unsupported': regs[0x04e0c140] = 16 << 24 | 2
    elif case == 'usb_resources_busy': regs[0x04e0c720] = 3
    elif case == 'usb_caps_64':
        regs[0x04e0c140] = 64 << 24 | 2; regs[0x04e0c144] = 4 << 15; regs[0x04e0c14c] = 32 << 12 | 16 << 18
    elif case.startswith('handoff_'):
        regs[SMMU + 0xc00] = 2; route = 'TRANSLATED'
        io = {'handoff_m0':'GPT_PASS', 'handoff_guard_before':'DMA_GUARD_BLOCKED',
              'handoff_guard_mid':'DMA_GUARD_CHANGED', 'handoff_ocs_error':'BAD_RESPONSE',
              'handoff_timeout':'COMMAND_TIMEOUT', 'handoff_restore_failed':'RESTORE_FAILED'}.get(case, 'DMA_BLOCKED')
    elif case == 'dt_mismatch':
        route, io, touch, usb = 'NOT_READY', 'NOT_READY', 'DT_MISMATCH', 'DT_MISMATCH'
    expectations = {'ocs_error': 'BAD_RESPONSE', 'wrong_tag': 'BAD_RESPONSE',
                    'short_transfer': 'SHORT_TRANSFER', 'capacity_512': 'CAPACITY_UNSUPPORTED',
                    'header_crc': 'GPT_HEADER_CRC', 'array_crc': 'GPT_ARRAY_CRC',
                    'duplicate_userdata': 'BAD_USERDATA_RANGE', 'gpt_overflow': 'GPT_LAYOUT_UNSUPPORTED',
                    'command_timeout': 'COMMAND_TIMEOUT', 'attach_failed': 'ATTACH_FAILED',
                    'restore_failed': 'RESTORE_FAILED', 'abort_stuck': 'RESTORE_FAILED',
                    'host_lost': 'RESTORE_FAILED'}
    io = expectations.get(case, io)
    table_values = {}
    if context_probe:
        pgshift = 16 if regs[SMMU + 0x24] & OFF else 12
        context_span = 1 << (((regs[SMMU + 0x24] >> 28) & 7) + 1 + pgshift)
        context_registers = set()
        for bank in range(32 if pgshift == 12 else 8):
            global_base = SMMU + (1 << pgshift)
            context_base = SMMU + context_span + (bank << pgshift)
            regs[global_base + bank * 4] = 1 << 16
            regs[global_base + 0x800 + bank * 4] = 1
            context_registers.update((global_base + bank * 4, global_base + 0x800 + bank * 4))
            for off, value in ((0, 1), (0x10, 2), (0x20, 0x96000000), (0x24, 0), (0x30, 32), (0x58, 0)):
                regs[context_base + off] = value
                context_registers.add(context_base + off)
        cb = SMMU + context_span + (2 << pgshift)
        if case == 'context_read_fault': fault_reads.add(cb + 0x30)
        if case == 'context_disabled': regs[cb] = 0
        if case == 'context_a32': regs[SMMU + (1 << pgshift) + 0x808] = 0
        if case == 'context_stage2': regs[SMMU + (1 << pgshift) + 8] = 0
        if case == 'context_combined': regs[SMMU + (1 << pgshift) + 8] = 3 << 16
        if case == 'context_big_endian': regs[cb] |= 16
        if driver_prep:
            for bank in range(32 if pgshift == 12 else 8):
                root = 0x110000000 + bank * 0x10000
                context_base = SMMU + context_span + (bank << pgshift)
                regs[context_base + 0x20] = root & 0xffffffff; regs[context_base + 0x24] = root >> 32
                # Independent 3-level 4K identity maps for both private arenas.
                for buffer in (syms['arena'], syms['usb_arena'], syms['spi_arena']):
                    for offset in (0, 4096):
                        address = buffer + offset
                        l2, l3 = root + 4096, root + 8192
                        table_values[root + (address >> 30 & 511) * 8] = l2 | 3
                        table_values[l2 + (address >> 21 & 511) * 8] = l3 | 3
                        table_values[l3 + (address >> 12 & 511) * 8] = address | 0x443
            root = 0x110020000; leaf = root + 8192 + (syms['arena'] >> 12 & 511) * 8
            if case == 'walk_missing': table_values[leaf] = 0
            if case == 'walk_readonly': table_values[leaf] |= 128
            if case == 'walk_other': table_values[leaf] += 0x100000
            if case == 'walk_reserved': regs[cb + 0x20] = 0x85200000; regs[cb + 0x24] = 0
            if case == 'walk_self_table': regs[cb + 0x20] = syms['arena']; regs[cb + 0x24] = 0
            if case == 'walk_read_fault': fault_reads.add(leaf)
            if case == 'walk_bad_granule': regs[cb + 0x30] |= 1 << 14
            if case == 'gpi_translated_missing':
                table_values[0x110050000 + 8192 + (syms['spi_arena'] >> 12 & 511) * 8] = 0
            if case.startswith('handoff_'):
                regs[SMMU + (1 << pgshift) + 8] = 0x1f000
                regs[cb] = 0
                if case == 'handoff_attrs': regs[SMMU + (1 << pgshift) + 8] |= 0x100
                if case == 'handoff_forced_cache': regs[cb] = 1 << 20
                if case == 'handoff_fault': regs[cb + 0x58] = 2
                if case == 'handoff_fault_read': fault_reads.add(cb + 0x58)
                if case == 'handoff_m_on': regs[cb] = 1
                if case == 'handoff_bad_type': regs[SMMU + (1 << pgshift) + 8] = 3 << 16
    initial = regs.copy()
    pages = {address & -4096 for address in regs}
    for page in pages: cpu.mem_map(page, 4096)
    for address, value in regs.items(): cpu.mem_write(address, struct.pack('<I', value))
    if table_values:
        for page in {a & -4096 for a in table_values}: cpu.mem_map(page, 4096)
        for address, value in table_values.items(): cpu.mem_write(address, struct.pack('<Q', value))
    reads, writes, commands = set(), [], []
    context_high_reads, handoff_checks = 0, 0
    blocks = disk(case)
    entry_count, entry_size = struct.unpack_from('<II', blocks[1], 80)
    expected_success_commands = 4 + (entry_count * entry_size + BLOCK_SIZE - 1) // BLOCK_SIZE
    old_ring = initial[UFS + 0x50]

    def complete():
        ring = regs[UFS + 0x50] | regs[UFS + 0x54] << 32
        assert LOAD <= ring < LOAD + len(kernel) - 8192, 'DMA ring escaped Image'
        assert ring % 4096 == 0 and regs[UFS + 0x60] == 1
        descriptor = struct.unpack('<8I', cpu.mem_read(ring, 32))
        ucd = descriptor[4] | descriptor[5] << 32
        assert ucd == ring + 0x400 and not descriptor[1] and not descriptor[3]
        command = bytes(cpu.mem_read(ucd, 32))
        assert command[2] == 0, 'nonzero LUN (including RPMB)'
        op = command[16] if command[0] == 1 else 0xff
        assert op in (0xff, 0, 0x9e, 0x28), f'forbidden UFS opcode {op:#x}'
        length = int.from_bytes(command[12:16], 'big')
        assert length == {0xff: 0, 0: 0, 0x9e: 32, 0x28: 4096}[op]
        assert descriptor[0] == (1 << 28) | (1 << 24) | ((2 << 25) if length else 0)
        assert command[1] == (0x40 if length else 0), 'write/data-out transfer'
        assert descriptor[2] == 15 and descriptor[6] == (128 << 16) | 128
        response = bytearray(512)
        response[0] = 0x20 if op == 0xff else 0x21; response[3] = command[3]
        block = None
        if op == 0x9e:
            assert command[17] == 0x10 and command[26:30] == b'\0\0\0 '
            last = 0x100000100 if case == 'gpt_overflow' else LAST_LBA
            block = struct.pack('>QI', last, 512 if case == 'capacity_512' else 4096) + bytes(20)
        elif op == 0x28:
            assert command[23:25] == b'\0\1', 'READ(10) spans more than one block'
            lba = int.from_bytes(command[18:22], 'big')
            assert lba in blocks, f'unexpected sector {lba}'
            block = blocks[lba]
        commands.append(op)
        if op == 0 and case == 'unit_attention' and commands.count(0) < 3:
            response[7] = 2
        if block is not None:
            prdt = struct.unpack('<4I', cpu.mem_read(ucd + 1024, 16))
            assert descriptor[7] == (256 << 16) | 1
            assert prdt == (ring + 4096, 0, 0, length - 1)
            assert ring + 8192 <= LOAD + len(kernel)
            cpu.mem_write(prdt[0], block)
        else:
            assert descriptor[7] == 0
        ocs = 6 if case in ('ocs_error', 'handoff_ocs_error') else 0
        if case == 'wrong_tag': response[3] ^= 1
        if case == 'short_transfer' and length:
            response[1] = 0x20; response[12:16] = struct.pack('>I', 1)
        cpu.mem_write(ucd + 512, bytes(response)); cpu.mem_write(ring + 8, struct.pack('<I', ocs))
        if case not in ('command_timeout', 'abort_stuck', 'handoff_timeout'): regs[UFS + 0x58] = 0

    def mmio_read(uc, access, address, size, value, _):
        nonlocal context_high_reads, handoff_checks
        assert size == 4 and address in regs, f'unexpected MMIO read {address:#x}/{size}'
        reads.add(address)
        if case == 'context_changed' and address == cb + 0x24:
            context_high_reads += 1
            regs[address] = context_high_reads
        if handoff_read and case in ('handoff_guard_before', 'handoff_guard_mid') and address == cb + 0x58:
            handoff_checks += 1
            if handoff_checks >= (2 if case == 'handoff_guard_before' else 4): regs[address] = 2
        if address == 0x0141f144:
            regs[address] = 0 if regs[VOTE] & MASK == MASK and case != 'touch_clock_timeout' else OFF
        if address in CLOCKS and regs[address] & 1 and case != 'ufs_clock_timeout':
            if address == CLOCKS[0] or not regs[address] & 2: regs[address] &= ~OFF
        uc.mem_write(address, struct.pack('<I', regs[address]))

    def mmio_write(uc, access, address, size, value, _):
        assert size == 4 and address in regs, f'unexpected MMIO write {address:#x}/{size}'
        assert not SMMU <= address < SMMU + 0x100000, 'SMMU configuration changed'
        writes.append((address, value))
        if address == VOTE:
            assert not ((value ^ regs[address]) & ~MASK), 'changed unrelated clock votes'
            regs[address] = value
        elif address in CLOCKS:
            assert not ((value ^ regs[address]) & ~1), 'changed UFS rate/reset/gating'
            regs[address] = value
        else:
            assert address in {UFS + v for v in (0x24, 0x50, 0x54, 0x58, 0x5c, 0x60, 0x20)}
            if address == UFS + 0x50 and ((case == 'attach_failed' and value != old_ring) or
                                        (case in ('restore_failed', 'handoff_restore_failed') and value == old_ring)):
                return
            if address == UFS + 0x58:
                assert value == 1 and (route in ('BYPASS', 'UNMATCHED_BYPASS') or
                    (handoff_read and case in ('handoff_m0', 'handoff_guard_mid', 'handoff_ocs_error', 'handoff_timeout', 'handoff_restore_failed')))
                regs[address] = value; complete()
            elif address == UFS + 0x5c:
                assert value == 0xfffffffe
                if case != 'abort_stuck': regs[UFS + 0x58] = 0
                if case == 'host_lost': regs[UFS + 0x30] = 0
            elif address == UFS + 0x20:
                assert value == 1
            else:
                regs[address] = value

    for page in pages:
        cpu.hook_add(UC_HOOK_MEM_READ, mmio_read, begin=page, end=page + 4095)
        cpu.hook_add(UC_HOOK_MEM_WRITE, mmio_write, begin=page, end=page + 4095)
    counter, board_return, stopped_error = 0, None, False
    special = {(3, 0, 4, 2, 2): 4, (3, 0, 0, 7, 0): 0x101124,
               (3, 3, 14, 0, 0): 19200000, (3, 0, 0, 0, 5): 0x80000000,
               (3, 0, 1, 0, 0): 4 if case == 'cache_on' else 0}

    def mrs(uc, register, cp, _):
        nonlocal counter
        key = cp.op0, cp.op1, cp.crn, cp.crm, cp.op2
        if key == (3, 3, 14, 0, 2):
            counter += 19200; uc.reg_write(register, counter); return 1
        if key in special:
            uc.reg_write(register, special[key]); return 1
        return 0

    table_reads, guarded_reads = set(), []
    leaf_reads = 0
    def code(uc, pc, size, _):
        nonlocal board_return, stopped_error, leaf_reads
        if pc == syms['dre_read32']:
            address = uc.reg_read(UC_ARM64_REG_X0)
            guarded_reads.append(address)
            if address & ~7 in table_values: table_reads.add(address)
            if driver_prep and case in ('walk_route_changed', 'walk_context_changed', 'walk_torn', 'walk_table_changed'):
                if address == leaf:
                    leaf_reads += 1
                    if case == 'walk_route_changed': regs[SMMU + 0xc00] = 3
                    if case == 'walk_context_changed': regs[cb + 0x30] = 33
                    if case == 'walk_torn' and leaf_reads == 2:
                        uc.mem_write(leaf, struct.pack('<Q', table_values[leaf] ^ 4096))
                if case == 'walk_table_changed' and address == leaf + 4 and leaf_reads == 2:
                    uc.mem_write(leaf, struct.pack('<Q', table_values[leaf] ^ 4096))
        if pc == syms['dre_read32'] and uc.reg_read(UC_ARM64_REG_X0) in fault_reads:
            uc.reg_write(UC_ARM64_REG_X0, 0); uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_X30))
        if pc == syms['dre_write32'] and case == 'touch_restore_failed':
            if uc.reg_read(UC_ARM64_REG_X0) == VOTE and uc.reg_read(UC_ARM64_REG_X1) == initial[VOTE]:
                uc.reg_write(UC_ARM64_REG_X0, 0); uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_X30))
        if pc == syms['dre_board_error']:
            board_return = uc.reg_read(UC_ARM64_REG_X30)
        elif board_return is not None and pc == board_return:
            if uc.reg_read(UC_ARM64_REG_X0):
                stopped_error = True; uc.emu_stop()

    # Hook only the relevant addresses, rather than slowing every copied byte.
    for address in (syms['dre_read32'], syms['dre_write32'], syms['dre_board_error']):
        cpu.hook_add(UC_HOOK_CODE, code, begin=address, end=address)
    # The return address is stable at this call site; inspect it through a
    # narrow handoff-function hook, not through the large kernel copy loop.
    cpu.hook_add(UC_HOOK_CODE, code, begin=syms['dre_handoff'], end=syms['dre_handoff'] + 4096)
    cpu.hook_add(UC_HOOK_INSN, mrs, None, 1, 0, UC_ARM64_INS_MRS)
    cpu.reg_write(UC_ARM64_REG_X0, tree)
    cpu.emu_start(LOAD, base + entry - va, count=1000000000 if handoff_read else 250000000)
    assert stopped_error == unsafe, ('handoff safety gate', case, stopped_error)
    if not unsafe:
        assert cpu.reg_read(UC_ARM64_REG_PC) == base + entry - va, 'instruction budget ended before XNU handoff'
    fields = bytes(cpu.mem_read(syms['report'], syms['report_size'])).split(b'\0')[0].decode().removeprefix('dre_report=').split('|')
    assert any(p.startswith('DMA_UFS=' + route + '/') for p in fields), fields
    assert any(p.startswith('UFS_IO=' + io + '/') for p in fields), fields
    assert any(p == 'TOUCH=' + touch or p.startswith('TOUCH=' + touch + '/') for p in fields), fields
    assert any(p.startswith('TOUCH_FW=' + fw + '/') for p in fields), fields
    assert any(p == 'USB_STATE=' + usb or p.startswith('USB_STATE=' + usb + '/') for p in fields), fields
    assert {'SERIAL=ABL_DETECTED', 'SERIAL=ABL_MATCH'} & set(fields), fields
    if case == 'usb_translated': assert ('DMA_USB=TRANSLATED/3' if driver_prep else 'DMA_USB=TRANSLATED/00000003') in fields
    if case == 'panel_dsjm': assert any(p.startswith('TOUCH_FW=LAYOUT_OK/' + ('1/' if driver_prep else '00000001/')) for p in fields)
    if driver_prep:
        expected_walk = {'walk_missing': 'UNMAPPED', 'walk_readonly': 'LEAF_PERMISSION',
            'walk_other': 'MAPPED_OTHER', 'walk_reserved': 'TABLE_RAM_BLOCKED',
            'walk_self_table': 'TABLE_RAM_BLOCKED', 'walk_read_fault': 'TABLE_READ_FAULT',
            'walk_bad_granule': 'TCR_UNSUPPORTED', 'walk_route_changed': 'ROUTE_CHANGED',
            'walk_context_changed': 'CONTEXT_CHANGED', 'walk_torn': 'TABLE_TORN', 'walk_table_changed': 'TABLE_CHANGED'}
        if case.startswith('walk_'):
            wanted = expected_walk.get(case, 'IDENTITY_RW')
            assert any(p.startswith('UMAP=' + wanted + '/') for p in fields), fields
            assert 'XMAP=IDENTITY_RW/2' in fields, fields
            if case == 'walk_reserved': assert not any(0x85200000 <= a < 0x85201000 for a in guarded_reads)
            if case == 'walk_self_table': assert not any(syms['arena'] <= a < syms['arena'] + 4096 for a in guarded_reads)
        if case == 'ready':
            assert 'USB_PLAN=TRB_PLAN_OK/20/6/1' in fields and 'GPI=IDLE/1f/0' in fields
            assert 'SPI_PLAN=SPI_PLAN_OK' in fields
        wanted = {'gpi_error':'CHANNEL_ERROR', 'gpi_fault':'READ_FAULT'}.get(case)
        if wanted: assert any(p.startswith('GPI=' + wanted + '/') for p in fields), fields
        if case == 'gpi_busy': assert 'GPI=IDLE/1e/2' in fields
        if case in ('gpi_translated_identity', 'gpi_translated_missing'):
            assert 'GDMA=TRANSLATED/5' in fields
            assert 'GMAP=' + ('IDENTITY_RW' if case.endswith('identity') else 'UNMAPPED') in fields
        if case in ('gpi_ee_mismatch', 'gpi_dma_mismatch'):
            assert 'GPI=DT_MISMATCH/0/0' in fields
            assert not any(0x04a00000 <= a < 0x04a60000 for a in guarded_reads), 'GPI read with unchecked metadata'
        if case == 'spi_clock_unset': assert 'SPI_PLAN=SPI_CLOCK_UNSET' in fields
        wanted_usb = {'usb_caps_fault':'CAPS_READ_FAULT', 'usb_caps_unsupported':'CAPS_UNSUPPORTED',
                      'usb_resources_busy':'RESOURCES_IN_USE'}.get(case)
        if wanted_usb: assert any(p.startswith('USB_PLAN=' + wanted_usb + '/') for p in fields), fields
        if case == 'usb_caps_64': assert 'USB_PLAN=TRB_PLAN_OK/40/20/4' in fields
    if handoff_read and case.startswith('handoff_'):
        expected_admit = 'HANDOFF_BYPASS' if case in ('handoff_m0', 'handoff_guard_before', 'handoff_guard_mid', 'handoff_ocs_error', 'handoff_timeout', 'handoff_restore_failed') else {
            'handoff_attrs':'ATTRIBUTES_UNSUPPORTED', 'handoff_forced_cache':'ATTRIBUTES_UNSUPPORTED',
            'handoff_fault':'STATE_OR_FAULT_BLOCKED', 'handoff_fault_read':'STATE_OR_FAULT_BLOCKED',
            'handoff_m_on':'S1_64', 'handoff_bad_type':'COMBINED'}[case]
        assert 'UFS_ADMIT=' + expected_admit in fields, fields
        assert len(commands) == (expected_success_commands if case in ('handoff_m0', 'handoff_restore_failed') else
                                 1 if case in ('handoff_guard_mid', 'handoff_ocs_error', 'handoff_timeout') else 0)
    if context_probe:
        expected_context = {'context_layout': 'LAYOUT', 'context_outside_resource': 'LAYOUT',
            'context_banks_outside_span': 'LAYOUT', 'context_bank_out_of_range': 'LAYOUT', 'context_read_fault': 'FAULT',
            'context_disabled': 'M_OFF', 'context_a32': 'S1_32', 'context_stage2': 'S2',
            'context_combined': 'COMBINED', 'context_big_endian': 'BIG_ENDIAN', 'context_changed': 'CHANGED'}
        if case in expected_context or case in ('context_64k', 'both_translated', 'translated', 'context_handset_span'):
            state = expected_context.get(case, 'S1_64')
            assert any(p == 'UCTX=' + state or p.startswith('UCTX=' + state + '/') for p in fields), fields
        if case in ('both_translated', 'context_handset_span'):
            assert any(p.startswith('XCTX=S1_64/') for p in fields), fields
        if expected_context.get(case) == 'LAYOUT':
            assert not reads & context_registers, 'bank registers read with an invalid layout'
            assert any(p.startswith('UCTX=LAYOUT/') for p in fields), 'missing rejected ID1 value'
        if case == 'context_handset_span':
            assert {SMMU + 0x42000, SMMU + 0x41000} <= reads, 'wrong handset bank addresses'
            assert not any(SMMU + 0x80000 <= a < SMMU + 0x100000 for a in reads)
        if route not in ('TRANSLATED',): assert 'UCTX=SKIP' in fields, fields
    if case in ('touch_clock_timeout', 'touch_dt_mismatch', 'dt_mismatch'):
        assert not any(0x04a80000 <= a < 0x04a84000 for a in reads), 'GENI read without checked clocks/resources'
    if case in ('usb_power_off', 'dt_mismatch'):
        assert not any(0x04e00000 <= a < 0x04e0cd00 for a in reads), 'USB read while unavailable'
    assert not any(0x05900000 <= a < 0x05990000 for a in reads), 'GPU register read with power off'
    if io in ('DMA_BLOCKED', 'DMA_GUARD_BLOCKED', 'NOT_READY', 'HOST_NOT_IDLE', 'RING_OVERLAP', 'HCI_READ_FAULT', 'CACHE_OR_MMU_ON'):
        assert not commands and not any(UFS <= a < UFS + 0x3000 for a, _ in writes)
    if not unsafe:
        assert cpu.reg_read(UC_ARM64_REG_PC) == base + entry - va, 'did not reach XNU'
        assert [regs[UFS + off] for off in (0x50, 0x54, 0x60, 0x24)] == [initial[UFS + off] for off in (0x50, 0x54, 0x60, 0x24)]
        assert regs[VOTE] == initial[VOTE], 'touch clock votes not restored'
        ba = cpu.reg_read(UC_ARM64_REG_X0)
        raw = bytes(cpu.mem_read(ba + 108, 608))
        assert b'\0' in raw
        args = raw.split(b'\0')[0].decode()
        assert args.split(' dre_report=')[1].split('|') == fields
        adt_va, adt_size = struct.unpack('<QI', bytes(cpu.mem_read(ba + 96, 12)))
        root, consumed = adt.parse(bytes(cpu.mem_read(adt_va + base - va, adt_size)))
        expected_serial = re.search(rb'(?:^|[ \0])androidboot.serialno=([A-Za-z0-9-]{1,63})(?: |\0)', dtb)[1]
        chosen = next(n for n in root.children if n.name == 'chosen')
        assert consumed == adt_size and root.prop('serial-number').split(b'\0')[0] == expected_serial
        assert chosen.prop('dre,host-uuid') == uuid.uuid5(uuid.NAMESPACE_URL, 'urn:oneplus:dre:' + expected_serial.decode()).bytes
        if native_ufs:
            node = next(n for n in root.children if n.name == 'dre-ufs')
            raw_handoff = node.prop('dre,handoff')
            w = struct.unpack('<8Q', raw_handoff)
            assert node.prop('compatible') == b'oneplus,dre-ufs\0'
            assert w[0] == 0x0031534655455244 and w[7] == zlib.crc32(raw_handoff[:56])
            assert w[1:3] == (syms['arena'], 16384) and not w[1] % 16384
            assert w[5:7] == (syms['_head'], syms['_end'] - syms['_head'])
            assert w[4] == len(dtb) and bytes(cpu.mem_read(w[3], w[4])) == dtb
            assert 'dre_ufs=1' in args
    else:
        args = ' '.join(fields)
    if case == 'dt_mismatch': assert not reads and not writes
    if io == 'GPT_PASS': assert len(commands) == expected_success_commands + (2 if case == 'unit_attention' else 0)
    print(f'PASS {case}: {len(args)}/607 argument bytes, {len(commands)} read-only UFS commands, '
          f'{len(writes)} MMIO writes, XNU {"blocked" if unsafe else "handoff"}', flush=True)
    return fields


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('image', type=Path)
    ap.add_argument('device_dtb', type=Path)
    ap.add_argument('--elf', type=Path, required=True)
    ap.add_argument('--case', action='append')
    ap.add_argument('--report-json', type=Path, help='save synthetic model reports (no device identifiers)')
    ap.add_argument('--dma-context', action='store_true')
    ap.add_argument('--driver-prep', action='store_true')
    ap.add_argument('--ufs-handoff-read', action='store_true')
    ap.add_argument('--native-ufs', action='store_true')
    args = ap.parse_args()
    cases = args.case or ('ready', 'translated', 'route_fault', 'route_reserved', 'unmatched_fault',
        'unmatched_bypass', 'ambiguous', 'masked', 'extended', 'bad_smmu_id', 'client_disabled',
        'indexed', 'smmu_read_fault', 'smmu_late_read_fault', 'groups_unsupported', 'sid_unsupported',
        'smmu_dt_mismatch', 'host_busy', 'host_off', 'ufs_clock_timeout', 'ring_overlap', 'hci_read_fault', 'cache_on',
        'unit_attention', 'ocs_error', 'wrong_tag', 'short_transfer', 'capacity_512', 'header_crc',
        'array_crc', 'duplicate_userdata', 'gpt_overflow', 'command_timeout', 'attach_failed',
        'restore_failed', 'abort_stuck', 'host_lost', 'touch_clock_timeout', 'touch_read_fault', 'touch_no_fw',
        'touch_dma_only', 'touch_fifo_width', 'touch_fifo_depth',
        'touch_busy', 'touch_already_voted', 'touch_restore_failed', 'touch_dt_mismatch',
        'panel_unknown', 'panel_ambiguous', 'panel_dsjm', 'firmware_bad', 'usb_running',
        'usb_resetting', 'usb_power_off', 'usb_translated', 'dt_mismatch')
    if args.dma_context and not args.case:
        cases += ('both_translated', 'context_layout', 'context_64k', 'context_read_fault',
                  'context_disabled', 'context_a32', 'context_stage2', 'context_combined',
                  'context_big_endian', 'context_changed', 'context_handset_span',
                  'context_outside_resource', 'context_banks_outside_span', 'context_bank_out_of_range')
    if args.driver_prep and not args.case:
        cases += ('walk_identity', 'walk_missing', 'walk_readonly', 'walk_other', 'walk_reserved',
                  'walk_read_fault', 'walk_bad_granule', 'gpi_busy', 'gpi_error', 'gpi_fault',
                  'spi_clock_unset', 'usb_caps_fault', 'usb_caps_unsupported', 'usb_resources_busy',
                  'walk_self_table', 'walk_route_changed', 'walk_context_changed', 'walk_torn', 'walk_table_changed',
                  'gpi_ee_mismatch', 'gpi_dma_mismatch', 'gpi_translated_identity', 'gpi_translated_missing')
    if args.ufs_handoff_read and not args.case:
        cases += ('handoff_m0', 'handoff_attrs', 'handoff_forced_cache', 'handoff_fault', 'handoff_fault_read',
                  'handoff_m_on', 'handoff_bad_type', 'handoff_guard_before', 'handoff_guard_mid', 'usb_caps_64',
                  'handoff_ocs_error', 'handoff_timeout', 'handoff_restore_failed')
    image, dtb = args.image.read_bytes(), args.device_dtb.read_bytes()
    kernel = image[4096:4096 + struct.unpack_from('<I', image, 8)[0]]
    syms = symbols(args.elf, kernel)
    reports = {case: run(image, dtb, syms, case, args.dma_context, args.driver_prep, args.ufs_handoff_read, args.native_ufs) for case in cases}
    if args.report_json:
        args.report_json.write_text(json.dumps(reports, indent=2) + '\n')
