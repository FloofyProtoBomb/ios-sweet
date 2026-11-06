#!/usr/bin/env python3
"""Build the DE2118 temporary boot image. Requires Zig 0.14.1."""
import argparse
import gzip
import hashlib
import json
import os
import re
from pathlib import Path
import struct
import subprocess
import uuid

HERE = Path(__file__).resolve().parent
SHIM = HERE.parent.parent / "shim"
PAGE = 4096


def pad(data):
    return data + bytes(-len(data) % PAGE)


def boot_v3(kernel):
    # A valid empty gzip/newc generic ramdisk. ABL supplies the active
    # slot's vendor ramdisk, DTB, and overlay separately for header v3.
    trailer = b"TRAILER!!!\0"
    fields = [0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, len(trailer), 0]
    entry = b"070701" + b"".join(b"%08x" % v for v in fields) + trailer
    ramdisk = gzip.compress(entry.ljust(512, b"\0"), mtime=0)
    # The connected DE2118's LineageOS 22.2 boot_b header, Android 15.0.0,
    # July 2025 patch level, empty generic cmdline. Keep vendor_boot in place.
    os_version = (15 << 25) | (25 << 4) | 7
    header = struct.pack("<8s9I1536s", b"ANDROID!", len(kernel),
                         len(ramdisk), os_version, 1580, 0, 0, 0, 0, 3, b"")
    return pad(header) + pad(kernel) + pad(ramdisk)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--zig", default="zig")
    ap.add_argument("--out", type=Path, default=HERE / "out")
    kernels = ap.add_mutually_exclusive_group()
    kernels.add_argument("--xnu", type=Path, help="embed a kernel built with MACHINE_CONFIG=DRE")
    kernels.add_argument("--upstream-0027", type=Path, help="retarget the pinned upstream CI kernel")
    ap.add_argument("--identity", type=Path, help="optional private JSON with a serial expectation and/or IMEI")
    ap.add_argument("--board-tests", action="store_true", help="survey UFS power/link and live DT at handoff")
    ap.add_argument("--scm-info-probe", action="store_true",
                    help="explicit early read-only SCM INFO availability queries; requires --board-tests --xnu")
    ap.add_argument("--ufs-clock-enable", action="store_true",
                    help="enable UFS GCC branches with a bounded wait and rollback")
    ap.add_argument("--dma-probe", action="store_true", help="read UFS/USB SMMU stream routes")
    ap.add_argument("--dma-context", action="store_true", help="read translated UFS/USB context metadata; no SMMU writes")
    ap.add_argument("--driver-prep", action="store_true", help="inspect bounded DMA tables, GPI channels and unowned SPI/USB descriptor plans")
    ap.add_argument("--ufs-handoff-read", action="store_true", help="opt-in read-only UFS test in stable stage-1-disabled/stage-2-bypass handoff mode")
    ap.add_argument("--native-ufs", action="store_true", help="transfer owned DMA memory to the opt-in native read-only XNU driver")
    ap.add_argument("--install-root", action="store_true",
                    help="boot writable iOS from userdata; no diagnostic RAM root (requires private product ADT)")
    ap.add_argument("--quiet-boot", action="store_true",
                    help="hide normal XNU console output on install boots while retaining panics")
    ap.add_argument("--boot-progress", action="store_true",
                    help="show one-shot root and display checkpoints during a quiet install boot")
    ap.add_argument("--panel-hz", type=int, choices=(45, 60, 90),
                    help="target a known panel mode with software vsync; does not set the panel mode")
    ap.add_argument("--ufs-read-test", action="store_true",
                    help="read capacity/GPT on LUN 0 only if physical DMA is verified")
    ap.add_argument("--touch-preflight", action="store_true",
                    help="temporarily vote touch clocks, inspect GENI, select live-DT firmware")
    ap.add_argument("--ui-services", action="store_true", help="publish display/watchdog nodes for the UI/SMP kernel")
    ap.add_argument("--cpus", type=int, choices=range(1, 9), default=1, help="selected CPU count; >1 requires --ui-services")
    ap.add_argument("--ram-mib", type=int, choices=(512, 1024, 2048), default=512,
                    help="upper-RAM window; checked against live RAM and every reservation before XNU")
    ap.add_argument("--product-adt", type=Path, help="optional private D22AP Apple ADT for /product metadata")
    ap.add_argument("--gpu-firmware-provisioning", type=Path,
                    help="UNVALIDATED opt-in private SQE/ZAP provisioning directory; matching kernel anchor and fresh external --out required")
    ap.add_argument("--gpu-firmware-relocated", action="store_true",
                    help="opt-in v2 boot-held aligned PAS13 load arena; requires private firmware provisioning and real relocated-PAS policy")
    args = ap.parse_args()
    if args.quiet_boot and not args.install_root:
        ap.error("--quiet-boot requires --install-root")
    if args.boot_progress and not args.quiet_boot:
        ap.error("--boot-progress requires --quiet-boot")
    if args.install_root:
        if not args.xnu or not args.product_adt:
            ap.error('--install-root requires --xnu and --product-adt')
        # One coherent installation profile, retaining the validated RAM/CPU
        # topology and early handoff guards instead of independent toggles.
        for key in ('native_ufs','ui_services','board_tests','ufs_clock_enable',
                    'dma_probe','dma_context','ufs_read_test','touch_preflight',
                    'driver_prep','ufs_handoff_read'):
            setattr(args,key,True)
        args.cpus=8; args.ram_mib=2048
    if (args.identity or args.board_tests) and not (args.xnu or args.upstream_0027):
        ap.error("--identity and --board-tests need an XNU kernel")
    if args.scm_info_probe and not (args.board_tests and args.xnu):
        ap.error("--scm-info-probe requires --board-tests and --xnu")
    if args.ufs_clock_enable and not args.board_tests:
        ap.error("--ufs-clock-enable requires --board-tests")
    if (args.dma_probe or args.touch_preflight) and not args.board_tests:
        ap.error("--dma-probe and --touch-preflight require --board-tests")
    if args.ufs_read_test and not args.dma_probe:
        ap.error("--ufs-read-test requires --dma-probe")
    if args.dma_context and not args.dma_probe:
        ap.error("--dma-context requires --dma-probe")
    if args.driver_prep and not (args.dma_context and args.ufs_read_test and args.touch_preflight):
        ap.error("--driver-prep requires --dma-context --ufs-read-test --touch-preflight")
    if args.ufs_handoff_read and not args.driver_prep:
        ap.error("--ufs-handoff-read requires --driver-prep")
    if args.native_ufs and not (args.ufs_handoff_read and args.ui_services):
        ap.error("--native-ufs requires --ufs-handoff-read and --ui-services")
    if (args.ui_services or args.product_adt) and not args.xnu:
        ap.error("--ui-services/--product-adt require a native UI/SMP kernel via --xnu")
    if args.panel_hz is not None and not args.ui_services:
        ap.error("--panel-hz requires --ui-services")
    if (args.cpus != 1 or args.product_adt) and not args.ui_services:
        ap.error("--cpus >1 and --product-adt require --ui-services")
    if args.ram_mib != 512 and not args.ui_services:
        ap.error("expanded RAM requires --ui-services")
    out = args.out.resolve()
    gpu_firmware = None
    if args.gpu_firmware_relocated and not args.gpu_firmware_provisioning:
        ap.error("--gpu-firmware-relocated requires --gpu-firmware-provisioning")
    if args.gpu_firmware_provisioning:
        if not args.xnu or not args.native_ufs or not args.ui_services:
            ap.error("--gpu-firmware-provisioning requires --xnu --native-ufs --ui-services")
        if out.exists():
            ap.error("GPU firmware provisioning requires a fresh private --out, preserving prepared images")
        from gpu.firmware.provision import load_for_build
        gpu_firmware = load_for_build(args.gpu_firmware_provisioning.resolve(), args.xnu, out)
    out.mkdir(parents=True, exist_ok=True)
    zig = os.path.abspath(args.zig) if Path(args.zig).is_file() else args.zig
    cc = [zig, "cc", "-target", "aarch64-linux-musl", "-mcpu=generic",
          "-O2", "-ffreestanding", "-fno-pic", "-fno-pie", "-fno-builtin",
          "-fno-stack-protector", "-mgeneral-regs-only", "-mstrict-align",
          "-ffunction-sections", "-fdata-sections", "-fvisibility=hidden",
          "-DFB_BASE=0", "-I" + str(HERE / "libfdt"), "-I" + str(SHIM)]
    sources = [HERE / name for name in ("start.S", "dre.c", "string.c")]
    sources += [SHIM / "console.c"]
    sources += [HERE / "libfdt" / name for name in
                ("fdt.c", "fdt_ro.c", "fdt_check.c", "fdt_addresses.c")]
    kernel = args.xnu or args.upstream_0027
    if kernel:
        from xnu import embed
        # mockfs exposes this project-built Mach-O as /sbin/launchd on md0.
        # No phone storage controller is linked into the 0027 kernel.
        init_obj = out / "init.o"
        init_flags = ["-DDRE_CHECK_IDENTIFIERS"]
        if args.native_ufs:
            init_flags += ["-DDRE_CHECK_NATIVE_UFS"]
            cc += ["-DDRE_NATIVE_UFS"]
            sources += [HERE / "storage_handoff.c", HERE / "storage_abi.c"]
        if args.install_root: cc += ['-DDRE_INSTALL_ROOT']
        cc += [f"-DDRE_RAM_SIZE=0x{args.ram_mib << 20:x}ULL"]
        if args.ram_mib != 512:
            init_flags += [f"-DDRE_EXPECT_RAM_MIB={args.ram_mib - 1}"]
        if args.ui_services:
            init_flags += [f"-DDRE_EXPECT_CPUS={args.cpus}", "-DDRE_CHECK_UI_SERVICES"]
            cc += [f"-DDRE_SMP_COUNT={args.cpus}"]
            sources += [HERE / "cpu_topology.c"]
        if args.board_tests:
            cc += ["-DDRE_BOARD_TESTS"]
            init_flags += ["-DDRE_BOARD_TESTS"]
            sources += [HERE / name for name in ("board_tests.c", "guarded_read.S",
                                                "ufs_clock.c", "peripheral_state.c")]
            if args.scm_info_probe:
                cc += ["-DDRE_SCM_INFO_PROBE"]
                sources += [HERE / name for name in ("gpu/platform/scm_probe.c",
                            "gpu/platform/scm_native.c", "gpu/a619_smc64.c")]
            if args.ufs_clock_enable:
                cc += ["-DDRE_UFS_CLOCK_ENABLE"]
            if args.dma_probe or args.touch_preflight:
                cc += ["-DDRE_COMPACT_REPORT"]
            if args.dma_probe:
                cc += ["-DDRE_DMA_PROBE"]
                sources += [HERE / "dma_route.c"]
            if args.dma_context:
                cc += ["-DDRE_DMA_CONTEXT"]
                sources += [HERE / "dma_context.c"]
            if args.ufs_read_test:
                cc += ["-DDRE_UFS_READ_TEST"]
                sources += [HERE / "ufs_read.c"]
            if args.touch_preflight:
                cc += ["-DDRE_TOUCH_PREFLIGHT"]
                sources += [HERE / "touch_preflight.c", HERE / "touch/firmware.c"]
            if args.driver_prep:
                cc += ["-DDRE_DRIVER_PREP"]
                sources += [HERE / name for name in ("dma_walk.c", "dma_memory.c", "dma_proof.c", "gpi_preflight.c",
                                                      "touch/spi_plan.c", "usb/transport.c", "usb/prepare.c")]
            if args.ufs_handoff_read:
                cc += ["-DDRE_UFS_HANDOFF_READ"]
                sources += [HERE / "ufs_handoff_dma.c"]
        serial = json.loads(args.identity.read_text()).get("serial") if args.identity else None
        if serial is not None:
            if not re.fullmatch(r"[A-Za-z0-9-]{1,63}", serial):
                raise ValueError("invalid hardware serial")
            host_id = uuid.uuid5(uuid.NAMESPACE_URL, "urn:oneplus:dre:" + serial)
            init_flags.append("-DDRE_EXPECT_UUID=" + ",".join(map(str, host_id.bytes)))
            if args.board_tests:
                header = out / "dre_serial.h"
                header.write_text('#define DRE_EXPECT_SERIAL "' + serial + '"\n', encoding="ascii")
                cc += ["-include", str(header)]
        if not args.install_root:
            subprocess.run([zig, "cc", "-target", "aarch64-macos-none", "-g0",
                        '-DINIT_BOARD="DRE"', *init_flags, "-c", str(SHIM.parent / "tools/sweet-init.S"),
                        "-o", str(init_obj)], check=True)
        embed(kernel, out, init_obj, donor=args.upstream_0027 is not None, identity_path=args.identity,
              ui=args.ui_services, cpus=args.cpus, product_path=args.product_adt,
              native_ufs=args.native_ufs, ram_mib=args.ram_mib, install_root=args.install_root,
              gpu_firmware=gpu_firmware is not None,
              gpu_firmware_relocated=args.gpu_firmware_relocated,
              quiet_boot=args.quiet_boot, panel_hz=args.panel_hz, boot_progress=args.boot_progress)
        if gpu_firmware is not None:
            # Private output only. The kernel anchor was matched before any
            # compilation. No default image, source digest or boot argument
            # automatically enables GPU/PAS13 hardware access.
            (out / "gpu-firmware.package.bin").write_bytes(gpu_firmware)
            fw_source = out / "gpu-firmware.S"
            fw_source.write_text(
                f"/* PRIVATE {hashlib.sha256(gpu_firmware).hexdigest()} */\n"
                '.section .dre_gpu_firmware,"a"\n.balign 16384\n'
                '.global _dre_gpu_firmware_start\n_dre_gpu_firmware_start:\n'
                '.incbin "gpu-firmware.package.bin"\n'
                '.global _dre_gpu_firmware_end\n_dre_gpu_firmware_end:\n', encoding="ascii")
            sources += [fw_source, HERE / "gpu/firmware/package.c", HERE / "gpu/firmware/shim_handoff.c"]
            cc += ["-DDRE_GPU_FIRMWARE"]
            if args.gpu_firmware_relocated:
                sources += [HERE / "gpu/firmware/relocation.c", HERE / "gpu/a619_zap.c"]
                cc += ["-DDRE_GPU_FW_RELOCATED=1"]
        source = out / "embedded.S"
        payload = (out / "payload.bin").read_bytes()
        # Zig's assembly cache does not track the file named by .incbin.
        digest = hashlib.sha256(payload).hexdigest()
        source.write_text(f'/* {digest} */\n.section .dre_payload,"a"\n.incbin "payload.bin"\n',
                          encoding="ascii")
        sources += [source, HERE / "handoff.c", HERE / "identity.c", SHIM / "loader.c"]
        cc += ["-DDRE_XNU"]
    objects = []
    for source in sources:
        obj = out / (source.stem + ".o")
        subprocess.run(cc + ["-c", str(source), "-o", str(obj)], cwd=out, check=True)
        objects.append(str(obj))
    elf = out / "dre.elf"
    subprocess.run(cc + ["-static", "-nostdlib", "-Wl,-e,_head", "-Wl,--gc-sections",
                         "-Wl,--build-id=none", "-T", str(HERE / "image.ld"),
                         *objects, "-o", str(elf)], check=True)
    image = out / "Image"
    subprocess.run([zig, "objcopy", "-O", "binary", str(elf), str(image)], check=True)
    code = image.read_bytes()
    if kernel and not code.endswith(payload):
        raise ValueError("linked image does not contain the freshly generated payload")
    image_size, = struct.unpack_from("<Q", code, 16)
    if code[56:60] != b"ARM\x64" or not len(code) <= image_size <= 95 * 1024**2:
        raise ValueError("invalid Linux arm64 Image header")
    code = code.ljust(image_size, b"\0")
    image.write_bytes(code)
    output = out / ("dre-xnu-boot.img" if kernel else "dre-probe-boot.img")
    output.write_bytes(boot_v3(code))
    info = {'device': 'DE2118', 'ui_services': args.ui_services, 'cpus': args.cpus, 'ram_mib': args.ram_mib,
            'board_tests': args.board_tests, 'dma_context': args.dma_context,
            'scm_info_probe': args.scm_info_probe,
            'scm_info_schema': 1 if args.scm_info_probe else None,
            'driver_prep': args.driver_prep,
            'ufs_handoff_read': args.ufs_handoff_read,
            'native_ufs': args.native_ufs,
            'install_root': args.install_root, 'diagnostic_ram_root': not args.install_root,
            'storage_mode': 'userdata-rw' if args.install_root else 'read-only',
            'private_identity_provisioned': args.identity is not None,
            'product_metadata_imported': args.product_adt is not None,
            'image_sha256': hashlib.sha256(output.read_bytes()).hexdigest(),
            'kernel_sha256': hashlib.sha256(kernel.read_bytes()).hexdigest() if kernel else None}
    if args.quiet_boot:
        info['quiet_boot'] = True
    if args.boot_progress:
        info['boot_progress'] = True
    if args.panel_hz is not None:
        info['software_vsync_hz'] = args.panel_hz
    if gpu_firmware is not None:
        info.update(private_gpu_firmware_provisioned=True, gpu_firmware_source="UNVALIDATED_OPT_IN",
                    gpu_firmware_handoff_version=2 if args.gpu_firmware_relocated else 1,
                    gpu_firmware_relocated_source=args.gpu_firmware_relocated,
                    native_gpu_validated=False, metal_ready=False)
    (out / 'build-info.json').write_text(json.dumps(info, indent=2) + '\n')
    print(output)


if __name__ == "__main__":
    main()
