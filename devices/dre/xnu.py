"""DRE single-core XNU bring-up on a 512 MiB window in the upper RAM bank."""
from pathlib import Path
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


def tree(identity=None):
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
    chosen = child(root, "chosen")
    # 0027 predates the SHA-1 provider. Publishing these optional identity
    # values invokes SHA1Init in IOPlatformExpertDevice::generatePlatformUUID.
    chosen.props = [p for p in chosen.props if p[0] not in ("unique-chip-id", "chip-id")]
    if identity:
        serial, imei = identity["serial"], identity["imei"]
        if not re.fullmatch(r"[A-Za-z0-9-]{1,63}", serial):
            raise ValueError("invalid hardware serial")
        if not re.fullmatch(r"[0-9]{15}", imei):
            raise ValueError("expected the phone's 15-digit IMEI")
        digits = [int(ch) * (1 + i % 2) for i, ch in enumerate(imei)]
        if sum(d // 10 + d % 10 for d in digits) % 10 or len(set(imei)) == 1:
            raise ValueError("invalid IMEI checksum")
        serial_bytes = mkadt.cstr(serial)
        root.props.append(("serial-number", 0, serial_bytes))
        hardware.props += [("serial-number", 0, serial_bytes), ("imei", 0, mkadt.cstr(imei))]
        host_id = uuid.uuid5(uuid.NAMESPACE_URL, "urn:oneplus:dre:" + serial)
        chosen.props.append(("dre,host-uuid", 0, host_id.bytes))
    usable = SIZE - mkadt.PANIC_LOG_SIZE
    put(chosen, "dram-base", mkadt.u64(BASE))
    put(chosen, "dram-size", mkadt.u64(usable))
    put(chosen, "firmware-version", b"dre-shim\0")
    put(chosen, "system-firmware-version", b"dre-shim\0")
    put(child(root, "pram"), "reg", mkadt.u64(BASE + usable, mkadt.PANIC_LOG_SIZE))
    cpu = child(child(root, "cpus"), "cpu0")
    put(cpu, "compatible", b"qcom,kryo460\0ARM,v8\0")
    for key in ("timebase-frequency", "peripheral-frequency", "fixed-frequency"):
        put(cpu, key, mkadt.u32(19200000))
    put(cpu, "clock-frequency", mkadt.u32(1804800000))
    arm_io = child(root, "arm-io")
    arm_io.children = [child(arm_io, "gic")]
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


def embed(kernel, out, init_obj, donor=False, identity_path=None):
    data = kernel.read_bytes()
    if donor:
        data = retarget(data)
        kernel = out / "kernel.development.dre"
        kernel.write_bytes(data)
    if b"oneplus,dre\0" not in data and b"oneplus,dre'" not in data:
        raise ValueError("expected a DRE XNU build (missing DRE platform match)")
    adt_path = out / "dre.adt"
    identity = json.loads(identity_path.read_text()) if identity_path else None
    adt_path.write_bytes(tree(identity))
    ramdisk = out / "dre-init.macho"
    ramdisk.write_bytes(mkinit.build(init_obj.read_bytes()))
    data, _ = mkpayload.build(str(kernel), str(adt_path),
                              "debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=md0",
                              str(ramdisk))
    (out / "payload.bin").write_bytes(data)
