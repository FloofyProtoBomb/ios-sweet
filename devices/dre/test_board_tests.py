#!/usr/bin/env python3
"""Run the exact combined Image against constrained MMIO models in Unicorn 2.

The live DTB and personalized image remain outside the repository. This checks
handoff, clock-write limits/rollback and peripheral access gating, not hardware.
"""
import argparse
from pathlib import Path
import struct
import re
import sys
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import adt

from unicorn import (Uc, UC_ARCH_ARM64, UC_HOOK_INSN, UC_HOOK_MEM_READ,
                     UC_HOOK_MEM_WRITE, UC_MODE_ARM)
from unicorn.arm64_const import UC_ARM64_INS_MRS, UC_ARM64_REG_PC, UC_ARM64_REG_X0

OFF = 1 << 31
CLOCKS = (0x01445098, 0x01445014, 0x01445010, 0x01445040)


def run(image, dtb, case, enable):
    if case == "dt_mismatch":
        dtb = dtb.replace(b"qcom,holi-gcc\0", b"qcom,xoli-gcc\0")
    kernel = image[4096:4096 + struct.unpack_from("<I", image, 8)[0]]
    payload = kernel.index(b"SWEETXNU\x01\0\0\0")
    va, entry, _, _, end, base, _ = struct.unpack_from("<7Q", kernel, payload + 16)
    cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    load, tree = 0xa8000000, 0x82000000
    for addr, data in ((load, kernel), (tree, dtb)):
        cpu.mem_map(addr, (len(data) + 4095) & -4096)
        cpu.mem_write(addr, data)
    cpu.mem_map(0x85200000, 0xc00000)
    cpu.mem_map(base, (end - va + 0x20000 + 4095) & -4096)

    regs = {0x01445004: OFF, 0x01445020: 0x100, 0x01445024: 0x201,
            0x01445060: 0x100, 0x01445064: 0x401,
            0x04804008: 0x210, 0x04804034: 1, 0x04804030: 3, 0x04804058: 0,
            0x0141a004: OFF, 0x0141a010: 1, 0x0141a084: 1,
            0x04e0c120: 0x5533320a, 0x04e0c110: 0x00112000,
            0x0147900c: 0x4c0, 0x0141f004: 0, 0x0141f008: 0, 0x0141f144: 0,
            0x04a80068: 0x142, 0x04a80040: 0,
            0x01436004: 1, 0x0599106c: OFF, 0x0599100c: OFF,
            0x05991054: 1, 0x05900840: 0}
    initial = [1, 1, 1, 1]
    stuck = set()
    expected_ufs, expected_clock = "LINK_READY", "READY"
    expected_usb, expected_touch, expected_gpu = "DWC3_DEVICE", "SPI_FW_READY", "RBBM_ACCESSIBLE"
    if case == "power_off":
        regs[0x01445004] = 0
        expected_ufs = "POWER_OFF"
    elif case == "enable_all":
        initial = [OFF | 0x100, OFF | 0x102, OFF | 0x102, OFF | 0x102]
        expected_clock = "ENABLED" if enable else "DISABLED"
    elif case == "hwcg_idle":
        initial = [1, OFF | 3, OFF | 3, OFF | 3]
    elif case == "timeout_rollback":
        initial = [OFF | 0x100, OFF | 0x100, OFF | 0x100, OFF | 0x100]
        stuck = {CLOCKS[2]}
        expected_clock = "TIMEOUT" if enable else "DISABLED"
    elif case == "preenabled_halted":
        initial = [1, OFF | 1, 1, 1]
        stuck = {CLOCKS[1]}
        expected_clock = "TIMEOUT" if enable else "HALTED"
    elif case == "host_disabled":
        regs[0x04804034] = 0
        expected_ufs = "HOST_DISABLED"
    elif case == "no_device":
        regs[0x04804030] = 0
        expected_ufs = "NO_DEVICE"
    elif case == "list_not_ready":
        regs[0x04804030] = 1
        expected_ufs = "LIST_NOT_READY"
    elif case == "link_busy":
        regs[0x04804058] = 1
        expected_ufs = "LINK_BUSY"
    elif case == "peripheral_power_off":
        regs[0x0141a004] = regs[0x0599106c] = regs[0x0599100c] = 0
        regs[0x0147900c] = 0
        expected_usb, expected_touch, expected_gpu = "POWER_OFF", "CLOCK_OFF", "POWER_OFF"
    elif case == "peripheral_clock_off":
        regs[0x0141a010] = regs[0x01436004] = 0
        regs[0x0141f144] = OFF
        expected_usb, expected_touch, expected_gpu = "CLOCK_OFF", "CLOCK_OFF", "CONFIG_CLOCK_OFF"
    elif case == "peripheral_no_fw":
        regs[0x04e0c120] = regs[0x04a80068] = regs[0x05991054] = 0
        expected_usb, expected_touch, expected_gpu = "NO_DWC3", "SPI_FW_MISSING", "GFX_CLOCK_OFF"
    elif case == "dt_mismatch":
        expected_ufs = expected_usb = expected_touch = expected_gpu = "DT_MISMATCH"
    elif case != "ready":
        raise ValueError(case)
    regs.update(zip(CLOCKS, initial))
    pages = {addr & -4096 for addr in regs}
    for page in pages:
        cpu.mem_map(page, 4096)
    for addr, value in regs.items():
        cpu.mem_write(addr, struct.pack("<I", value))
    reads, writes = set(), []

    def mmio_read(uc, access, addr, size, value, _):
        assert size == 4 and addr in regs, f"unexpected MMIO read {addr:#x}/{size}"
        reads.add(addr)
        if addr in CLOCKS and regs[addr] & 1 and addr not in stuck:
            # An enabled non-HWCG branch starts running. HWCG may stay idle.
            if addr == CLOCKS[0] or not (regs[addr] & 2):
                regs[addr] &= ~OFF
        uc.mem_write(addr, struct.pack("<I", regs[addr]))

    def mmio_write(uc, access, addr, size, value, _):
        assert enable and case != "power_off", "unexpected clock mutation"
        assert size == 4 and addr in CLOCKS, f"unexpected MMIO write {addr:#x}/{size}"
        assert (value ^ regs[addr]) & ~1 == 0, "changed a clock bit other than enable"
        writes.append((addr, value))
        regs[addr] = value

    for page in pages:
        cpu.hook_add(UC_HOOK_MEM_READ, mmio_read, begin=page, end=page + 4095)
        cpu.hook_add(UC_HOOK_MEM_WRITE, mmio_write, begin=page, end=page + 4095)
    values = {(3, 0, 4, 2, 2): 4, (3, 0, 0, 7, 0): 0x101124,
              (3, 3, 14, 0, 0): 19200000, (3, 0, 0, 0, 5): 0x80000000}
    counter = 0

    def mrs(uc, register, cp, _):
        nonlocal counter
        key = cp.op0, cp.op1, cp.crn, cp.crm, cp.op2
        if key == (3, 3, 14, 0, 2):
            counter += 1000
            uc.reg_write(register, counter)
            return 1
        if key in values:
            uc.reg_write(register, values[key])
            return 1
        return 0

    cpu.hook_add(UC_HOOK_INSN, mrs, None, 1, 0, UC_ARM64_INS_MRS)
    cpu.reg_write(UC_ARM64_REG_X0, tree)
    cpu.emu_start(load, base + entry - va, count=250000000)
    assert cpu.reg_read(UC_ARM64_REG_PC) == base + entry - va, "did not reach XNU"
    ba = cpu.reg_read(UC_ARM64_REG_X0)
    raw = bytes(cpu.mem_read(ba + 108, 608))
    assert b"\0" in raw, "unterminated boot arguments"
    args = raw.split(b"\0")[0].decode()
    fields = args.split(" dre_report=")[1].split("|")
    adt_va, adt_size = struct.unpack("<QI", bytes(cpu.mem_read(ba + 96, 12)))
    root, consumed = adt.parse(bytes(cpu.mem_read(adt_va + base - va, adt_size)))
    expected_serial = re.search(rb"(?:^|[ \0])androidboot.serialno=([A-Za-z0-9-]{1,63})(?: |\0)", dtb)[1]
    hardware = next(n for n in root.children if n.name == "oneplus-hardware")
    chosen = next(n for n in root.children if n.name == "chosen")
    assert consumed == adt_size
    assert root.prop("serial-number").split(b"\0")[0] == expected_serial
    assert hardware.prop("serial-number") == root.prop("serial-number")
    assert chosen.prop("dre,host-uuid") == uuid.uuid5(uuid.NAMESPACE_URL, "urn:oneplus:dre:" + expected_serial.decode()).bytes
    if expected_clock not in ("READY", "ENABLED") and case != "power_off":
        expected_ufs = "CLOCK_" + expected_clock
    assert fields[0] == "UFS=" + expected_ufs, fields
    if case not in ("power_off", "dt_mismatch"):
        assert "UCLK=" + expected_clock in fields, fields
    assert {"GPU_DT=A619v1", "TOUCH_DT=NT36672C_SPI0",
            "PANEL_SEL=TIANMA_NT36672C"} <= set(fields), fields
    assert {"SERIAL=ABL_MATCH", "SERIAL=ABL_DETECTED"} & set(fields), fields
    for prefix, expected in (("USB", expected_usb), ("TOUCH_SPI", expected_touch), ("GPU_HW", expected_gpu)):
        assert any(s == prefix + "=" + expected or s.startswith(prefix + "=" + expected + "_") for s in fields), fields
    hci = {0x04804008, 0x04804034, 0x04804030, 0x04804058}
    assert bool(hci & reads) == (expected_clock in ("READY", "ENABLED") and case not in ("power_off", "dt_mismatch"))
    if case == "dt_mismatch":
        assert not reads and not writes
    if case.startswith("peripheral_"):
        assert 0x05900840 not in reads
        if case != "peripheral_no_fw":
            assert not ({0x04e0c120, 0x04e0c110, 0x04a80068, 0x04a80040} & reads)
    if case in ("timeout_rollback", "preenabled_halted"):
        assert [regs[a] & 1 for a in CLOCKS] == [v & 1 for v in initial], "rollback lost original enables"
    elif case == "enable_all" and enable:
        assert len(writes) == 4 and [a for a, _ in writes] == list(CLOCKS)
    else:
        assert not writes
    print(f"PASS {case}: {len(args)}/607 argument bytes, {len(writes)} MMIO writes", flush=True)
    return args


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("private_image", type=Path)
    ap.add_argument("device_dtb", type=Path)
    ap.add_argument("--ufs-clock-enable", action="store_true")
    ap.add_argument("--case", action="append")
    args = ap.parse_args()
    image, dtb = args.private_image.read_bytes(), args.device_dtb.read_bytes()
    cases = args.case or ("ready", "power_off", "enable_all", "hwcg_idle", "timeout_rollback",
                          "preenabled_halted", "host_disabled", "no_device", "list_not_ready",
                          "link_busy", "peripheral_power_off", "peripheral_clock_off", "peripheral_no_fw", "dt_mismatch")
    for case in cases:
        run(image, dtb, case, args.ufs_clock_enable)
