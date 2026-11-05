"""DRE single-core XNU bring-up on a 512 MiB window in the upper RAM bank."""
from pathlib import Path
import copy
import json
import re
import sys
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import mkadt
import mkinit
import mkpayload
from donor import retarget

BASE = 0x100000000
SIZE = 0x20000000
SPLASH = 0x85200000


def tree(identity=None, ui=False, cpus=1, product_path=None, native_ufs=False, ram_mib=512):
    if not 1 <= cpus <= 8 or (cpus != 1 and not ui):
        raise ValueError("secondary CPUs require the UI/SMP kernel profile")
    if ram_mib not in (512, 1024, 2048) or (ram_mib != 512 and not ui):
        raise ValueError("unsupported DRE RAM window")
    # Reuse the common Apple boot properties and node encoding. Only the boot
    # CPU, interrupt controller, framebuffer, RAM and panic buffer are exposed.
    root = mkadt.build(qemu=True, include_product=False)
    def put(node, key, val):
        node.props = [(k, flags, val if k == key else old)
                      for k, flags, old in node.props]
    def child(node, name):
        return next(c for c in node.children if c.name == name)

    put(root, "compatible", b"D22AP\0iPhone10,3\0oneplus,dre\0qcom,sm4350\0AppleARM\0")
    put(root, "model", b"iPhone10,3\0")
    hardware = mkadt.node("oneplus-hardware", [
        ("model", b"DE2118\0"), ("board", b"dre\0"), ("soc", b"SM4350\0")])
    root.children.append(hardware)
    if native_ufs:
        root.children.append(mkadt.node("dre-ufs", [
            ("compatible", b"oneplus,dre-ufs\0"), ("dre,handoff", bytes(64))]))
    chosen = child(root, "chosen")
    # 0027 predates the SHA-1 provider. Publishing these optional identity
    # values invokes SHA1Init in IOPlatformExpertDevice::generatePlatformUUID.
    chosen.props = [p for p in chosen.props if p[0] not in ("unique-chip-id", "chip-id")]
    # Fixed-size slots are populated from ABL's live FDT by the phone shim.
    # Explicit identities also support virtual tests and optional IMEI staging.
    serial_bytes, host_bytes = bytes(64), bytes(16)
    if identity:
        serial, imei = identity.get("serial"), identity.get("imei")
        if serial is not None and not re.fullmatch(r"[A-Za-z0-9-]{1,63}", serial):
            raise ValueError("invalid hardware serial")
        if imei is not None and not re.fullmatch(r"[0-9]{15}", imei):
            raise ValueError("expected the phone's 15-digit IMEI")
        if imei is not None:
            digits = [int(ch) * (1 + i % 2) for i, ch in enumerate(imei)]
            if sum(d // 10 + d % 10 for d in digits) % 10 or len(set(imei)) == 1:
                raise ValueError("invalid IMEI checksum")
            hardware.props.append(("imei", 0, mkadt.cstr(imei)))
        if serial is not None:
            serial_bytes = mkadt.cstr(serial).ljust(64, b"\0")
            host_bytes = uuid.uuid5(uuid.NAMESPACE_URL, "urn:oneplus:dre:" + serial).bytes
    root.props.append(("serial-number", 0, serial_bytes))
    hardware.props.append(("serial-number", 0, serial_bytes))
    chosen.props.append(("dre,host-uuid", 0, host_bytes))
    usable = (ram_mib << 20) - mkadt.PANIC_LOG_SIZE
    put(chosen, "dram-base", mkadt.u64(BASE))
    put(chosen, "dram-size", mkadt.u64(usable))
    put(chosen, "firmware-version", b"dre-shim\0")
    put(chosen, "system-firmware-version", b"dre-shim\0")
    put(child(root, "pram"), "reg", mkadt.u64(BASE + usable, mkadt.PANIC_LOG_SIZE))
    cpu_parent = child(root, "cpus")
    template = child(cpu_parent, "cpu0")
    def handles(node):
        for key, _, value in node.props:
            if key == "AAPL,phandle" and len(value) == 4:
                yield int.from_bytes(value, "little")
        for node_child in node.children:
            yield from handles(node_child)
    last_handle = max(handles(root))
    cpu_parent.children = []
    for core in range(cpus):
        cpu = copy.deepcopy(template)
        put(cpu, "name", mkadt.cstr(f"cpu{core}"))
        put(cpu, "compatible", b"qcom,kryo460\0ARM,v8\0")
        for key, value in (("reg", core << 8), ("cpu-id", core),
                           ("cluster-core-id", core), ("l2-cache-id", core)):
            put(cpu, key, mkadt.u32(value))
        if core:
            put(cpu, "AAPL,phandle", mkadt.u32(last_handle + core))
            # Frequency control is not implemented. Do not copy CPU0's
            # nominal clock into the heterogeneous secondary-core entries.
            cpu.props = [p for p in cpu.props if p[0] != "clock-frequency"]
        else:
            put(cpu, "clock-frequency", mkadt.u32(1804800000))
        put(cpu, "state", mkadt.cstr("running" if not core else "stopped"))
        for key in ("timebase-frequency", "peripheral-frequency", "fixed-frequency"):
            put(cpu, key, mkadt.u32(19200000))
        cpu_parent.children.append(cpu)
    arm_io = child(root, "arm-io")
    arm_io.children = [c for c in arm_io.children if c.name in
                       (("gic", "wdt", "disp0") if ui else ("gic",))]
    if ui:
        put(child(arm_io, "disp0"), "compatible", b"dre,display\0")
        if product_path:
            product = mkadt.d22ap_product(str(product_path))
            if not product:
                raise ValueError("D22AP product metadata unavailable")
            root.children.append(mkadt.node("product", product))
    gic = child(arm_io, "gic")
    put(gic, "reg", mkadt.u64(0x0f200000 - mkadt.SOC_BASE, 0x10000,
                               0x0f240000 - mkadt.SOC_BASE, 0x100000))
    put(gic, "virtual-timer-intid", mkadt.u32(19))
    # Publish the A619 resources for driver bring-up. No Apple GPU personality
    # matches this node. The physical interrupt is GIC SPI 177 (INTID 209).
    arm_io.children.append(mkadt.node("gpu", [
        ("compatible", b"qcom,adreno-gpu-a619-holi\0"),
        ("qcom,chipid", mkadt.u32(0x06010900)),
        ("reg", mkadt.u64(0x05900000 - mkadt.SOC_BASE, 0x40000,
                          0x05961000 - mkadt.SOC_BASE, 0x800,
                          0x0596a000 - mkadt.SOC_BASE, 0x30000,
                          0x0599e000 - mkadt.SOC_BASE, 0x1000)),
        ("qcom,smmu-reg", mkadt.u64(0x05940000, 0x10000)),
        ("qcom,irq-intid", mkadt.u32(209)),
        ("qcom,zap-firmware", b"a615_zap\0"),
    ]))
    fb = child(root, "sweet-fbcon")
    put(fb, "reg", mkadt.u64(SPLASH, 0xc00000))
    return mkadt.adt.serialize(root)


def embed(kernel, out, init_obj, donor=False, identity_path=None,
          ui=False, cpus=1, product_path=None, native_ufs=False, ram_mib=512, install_root=False):
    data = kernel.read_bytes()
    if donor:
        data = retarget(data)
        kernel = out / "kernel.development.dre"
        kernel.write_bytes(data)
    if b"oneplus,dre\0" not in data and b"oneplus,dre'" not in data:
        raise ValueError("expected a DRE XNU build (missing DRE platform match)")
    if ui and not all(marker in data for marker in
                      (b"IOCoreSurfaceRoot", b"AppleCLCD", b"sweet_secondary_entry")):
        raise ValueError("--ui-services requires a kernel built with the DRE ui profile")
    if native_ufs and not all(marker in data for marker in
        (b"_dre_ufs_prepare_device\0", b"_dre_dma_boot_owned_ram_allowed\0",
         b"DRE UFS: transient FDT mapping lifetime PASS", b"dre_ufs_state\0",
         b"DRE UFS: owned Normal-NC DMA mapping PASS")):
        raise ValueError("--native-ufs requires the native driver with owned Normal-NC DMA and stage reporting")
    if install_root and (not native_ufs or not ui or not product_path or
        b'DRE UFS: userdata read-write enabled' not in data):
        raise ValueError('--install-root requires UI, product metadata and a userdata-RW native kernel')
    adt_path = out / "dre.adt"
    identity = json.loads(identity_path.read_text()) if identity_path else None
    adt_path.write_bytes(tree(identity, ui=ui, cpus=cpus, product_path=product_path, native_ufs=native_ufs, ram_mib=ram_mib))
    ramdisk = None
    if not install_root:
        ramdisk = out / "dre-init.macho"
        ramdisk.write_bytes(mkinit.build(init_obj.read_bytes()))
    root_device = 'ufsroot0' if install_root else 'md0'
    cmdline = f"debug=0xc serial=3 cpus={cpus} keepsyms=1 -enable_kprintf_spam -noprogress rd={root_device}"
    if ui:
        cmdline += " msgbuf=65536"  # Retain driver-start records until diagnostic PID 1.
    if native_ufs:
        cmdline += " dre_ufs=1"
    if install_root:
        # mkiosroot intentionally edits launchd.plist for this local port.
        # The stock loader otherwise insists on its signed AMFI-backed cache.
        cmdline += (' dre_ufs_rw=1 rootrw=1 unentitled_ios_sim_launch=1 ignition_level=1'
                    ' launchd_unsecure_cache=1'
                    ' sweetfb_noconsole=1 sweetfb_hz=30')
    if len(cmdline.encode()) >= 608: raise ValueError('boot arguments exceed shim limit')
    data, _ = mkpayload.build(str(kernel), str(adt_path), cmdline, str(ramdisk) if ramdisk else None)
    (out / "payload.bin").write_bytes(data)
