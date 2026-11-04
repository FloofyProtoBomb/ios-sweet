"""Reproduce the RAM-only DRE kernel from the exact upstream 0027 CI binary.

0027 is the last upstream build before the UFS driver. Its normal console,
GIC and timer use the ADT. Only the pre-ADT debug framebuffer is baked into
start.s, sweet_early.c and arm_vm_init.c (patches 0010 and 0012).
"""
import hashlib
import struct

UPSTREAM_SHA256 = "a864388697a9fe3b24fe31654d5bed24a1171a3a1814d17288a8d6f152e95f6a"


def retarget(source):
    if hashlib.sha256(source).hexdigest() != UPSTREAM_SHA256:
        raise ValueError("expected upstream kernel.development.sweet from run 38779441895")
    kernel = bytearray(source)

    def replace(offset, old, new):
        if len(old) != len(new) or kernel[offset:offset + len(old)] != old:
            raise ValueError(f"unexpected 0027 data at {offset:#x}")
        kernel[offset:offset + len(old)] = new

    def movk(offset, register, old, new):
        # A64 MOVK Xd, #imm16, LSL #16. Verify opcode, shift and register too.
        def encoding(value):
            return struct.pack("<I", 0xf2a00000 | (value << 5) | register)
        replace(offset, encoding(old), encoding(new))

    def branch(at, to):
        struct.pack_into("<I", kernel, at, 0x14000000 | (((to - at) // 4) & 0x03ffffff))

    # First CPU marker, framebuffer + 2368 rows * 4320 bytes.
    movk(0x479074, 0, 0x9c9c, 0x85bc)
    # Both bootstrap and final mappings are aligned 32 MiB L2 blocks.
    movk(0x4792b0, 10, 0x9c00, 0x8400)
    movk(0x46f3ac, 8, 0x9c00, 0x8400)
    # Pass the sub-block offset to sweet_early_relocate after TTBR1 switches.
    movk(0x46f4e8, 0, 0x0200, 0x0320)
    replace(0xb316d8, struct.pack("<Q", 0x9c000000), struct.pack("<Q", 0x85200000))
    # Keep the IOCatalogue text and binary sections exactly the same length.
    replace(0x134038, b"xiaomi,sweet';", b"oneplus,dre' ;")
    replace(0x1340d7, b"qcom,kryo470", b"qcom,kryo460")
    replace(0x139a17, b"Kryo 470", b"Kryo 460")
    replace(0x139990, b"Redmi Note 10 Pro\0", b"OnePlus Nord N200\0")
    # Delegate the board overrides to IODTPlatformExpert. The guest product
    # and board come from the ADT, independently of the physical phone.
    branch(0xa47700, 0x9e1388)  # getTargetName
    branch(0xa47750, 0x9e1394)  # getProductName
    # kern_mib.c: HW_MACHINE/HW_MODEL use the corresponding PRODUCT/TARGET
    # switch cases, instead of the macOS iPad-on-Mac compatibility answers.
    table = 0x1f970
    for legacy, actual in ((1, 27), (2, 26)):
        pos = table + 2 * (actual - 1)
        kernel[table + 2 * (legacy - 1):table + 2 * legacy] = kernel[pos:pos + 2]
    # Provide the regular NUL-terminated serial to OSString::withCString.
    # x0 already contains getBytesNoCopy() and the length check has passed.
    branch(0x9e15fc, 0x9e1674)
    # generatePlatformUUID: fetch a prepared 16-byte UUID from /chosen.
    # Put the property name in the bypassed SHA-1 block. Reuse the existing
    # OSData type check, uuid_unparse, property publication and cleanup.
    offset = 0x9e0724 - 0x9e06a8
    adr_x1 = 0x10000001 | ((offset & 3) << 29) | ((offset >> 2) << 5)
    struct.pack_into("<II", kernel, 0x9e06a8, adr_x1, 0xd503201f)
    struct.pack_into("<I", kernel, 0x9e06e0, 0x7100401f)  # cmp w0, #16
    branch(0x9e06e8, 0x9e075c)  # data1->getBytesNoCopy()
    branch(0x9e076c, 0x9e07f4)  # uuid_unparse(bytes, uuid)
    kernel[0x9e0724:0x9e0732] = b"dre,host-uuid\0"
    return kernel
