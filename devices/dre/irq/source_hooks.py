# SPDX-License-Identifier: MIT
"""UNVALIDATED opt-in source recipe. Authored only; NOT RUN tonight.

Root's stager owns copying/allowlisting the source and adding the single C++
translation unit. This narrowly edits the selected XNU8796 source tree only
when explicitly invoked later. It starts no compiler/CI/emulator/phone action.
There is intentionally no __main__, import side effect or automatic recipe.
"""


def edit_selected_tree(root):
    """Requires an isolated selected tree with irq/** already copied into it.

    All exact anchors are qualified before the first source write. Existing
    hooks/refactors are refused: this is not a generic global-handler patch.
    """
    from pathlib import Path

    root = Path(root).resolve(strict=True)
    required = (
        "iokit/Kernel/arm/dre/irq/DRENativeGIC.cpp",
        "iokit/Kernel/arm/dre/irq/dre_spi.h",
        "iokit/Kernel/arm/dre/irq/spi_low.h",
        "iokit/Kernel/arm/dre/irq/platform_namespace.h",
        "iokit/Kernel/arm/dre/irq/platform_namespace.inc",
        "iokit/Kernel/arm/dre/irq/service_source_cache.inc",
        "iokit/Kernel/arm/dre/irq/sleh_spi.inc",
        "iokit/Kernel/arm/dre/irq/pe_spi.inc",
        "iokit/Kernel/arm/dre/native_board.h",
        "iokit/Kernel/arm/dre/libfdt/libfdt.h",
    )
    for relative in required:
        path = root / relative
        if path.is_symlink() or not path.is_file() or not path.resolve().is_relative_to(root):
            raise ValueError("missing ordinary selected IRQ prerequisite: " + relative)
    version = root / "config/MasterVersion"
    # MasterVersion records the Darwin release, not the XNU source tag. The
    # selected 8796.141.3 sources say 22.6.0; exact anchors below qualify the
    # actual source layout before any write.
    if not version.is_file() or version.read_text().splitlines()[:1] != ["22.6.0"]:
        raise ValueError("IRQ draft requires selected Darwin 22.6.0 source anchors")
    names = (
        "pexpert/arm/pe_fiq.c", "osfmk/arm64/sleh.c",
        "iokit/Kernel/IOPlatformExpert.cpp", "iokit/Kernel/IOService.cpp",
        "iokit/IOKit/IOService.h",
    )
    original = {}
    for relative in names:
        path = root / relative
        if path.is_symlink() or not path.is_file() or not path.resolve().is_relative_to(root):
            raise ValueError("missing ordinary selected IRQ target: " + relative)
        original[relative] = path.read_bytes().decode("utf-8", "surrogateescape").replace("\r\n", "\n")
    if any(marker in text for text in original.values() for marker in (
        "dre_spi_summary_dispatch", "pe_spi.inc", "sleh_spi.inc",
        "platform_namespace.inc", "service_source_cache.inc", "DREIRQSourceCacheAccess",
    )):
        raise ValueError("IRQ hook already present; fresh selected tree required")
    prepared = dict(original)

    def replace_one(relative, before, after):
        if prepared[relative].count(before) != 1:
            raise ValueError("selected IRQ source anchor changed: " + relative)
        prepared[relative] = prepared[relative].replace(before, after, 1)

    pe = "pexpert/arm/pe_fiq.c"
    include = "#include <pexpert/arm64/board_config.h>\n"
    replace_one(pe, include, include +
        "#if defined(DRE) && HAS_GIC_V3\n"
        '#include "../../iokit/Kernel/arm/dre/irq/spi_low.h"\n'
        "#endif /* UNVALIDATED owned SPI source */\n")
    mapped = "\t\tgicr_size = gicr_size_prop;\n"
    replace_one(pe, mapped, mapped +
        "#if defined(DRE)\n"
        "\t\tdre_spi_pe_bind(soc_base_phys + gicd_base_prop, gicd_size_prop,\n"
        "\t\t    soc_base_phys + gicr_base_prop, gicr_size_prop);\n"
        "#endif /* observe existing real mapping; no GIC reset */\n")
    prepared[pe] += ('\n#if defined(DRE) && HAS_GIC_V3\n'
        '#include "../../iokit/Kernel/arm/dre/irq/pe_spi.inc"\n#endif\n')

    sleh = "osfmk/arm64/sleh.c"
    declaration = "void sleh_irq(arm_saved_state_t *);\n"
    replace_one(sleh, declaration,
        '#if defined(DRE) && HAS_GIC_V3\n'
        '#include "../../iokit/Kernel/arm/dre/irq/sleh_spi.inc"\n'
        '#endif /* real per-SPI summary registry, UNVALIDATED */\n' + declaration)
    other = (
        "\t} else {\n"
        "\t\tsleh_interrupt_handler_prologue(state, DBG_INTR_TYPE_OTHER);\n"
        "\t\tif (cdp->interrupt_handler) {\n"
        "\t\t\tcdp->interrupt_handler(cdp->interrupt_target,\n"
        "\t\t\t    cdp->interrupt_refCon,\n"
        "\t\t\t    cdp->interrupt_nub,\n"
        "\t\t\t    intid);\n"
        "\t\t}\n"
        '\t\t__builtin_arm_wsr64("ICC_EOIR1_EL1", iar);\n'
        "\t}\n")
    replace_one(sleh, other, other.replace(
        "if (cdp->interrupt_handler)",
        "if (!dre_spi_summary_dispatch(intid) && cdp->interrupt_handler)", 1))
    # Timer/SGI/PPI branches, IAR read, sole existing EOIR write, entropy and
    # prologue/epilogue stay byte-for-byte outside this one condition.

    service_h = "iokit/IOKit/IOService.h"
    controller_comment = "/* for IOInterruptController implementors */\n"
    replace_one(service_h, controller_comment, controller_comment +
        "#if defined(ARM64_BOARD_CONFIG_DRE)\n"
        "\tfriend class DREIRQSourceCacheAccess;\n"
        "#endif /* no layout/vtable change; actual private allocation bridge */\n")
    prepared["iokit/Kernel/IOService.cpp"] += (
        '\n#if defined(ARM64_BOARD_CONFIG_DRE)\n'
        '#include "arm/dre/irq/service_source_cache.inc"\n#endif\n')
    prepared["iokit/Kernel/IOPlatformExpert.cpp"] += (
        '\n#if defined(ARM64_BOARD_CONFIG_DRE)\n'
        '#include "arm/dre/irq/platform_namespace.inc"\n#endif\n')

    # No code, recipe, test or build is executed by authoring this function.
    # If explicitly called later, all exact-context edits above are already
    # prepared before this first mutation of the isolated source tree.
    for relative in names:
        (root / relative).write_bytes(prepared[relative].encode("utf-8", "surrogateescape"))
    return {
        "unvalidated": True,
        "translation_units": ["iokit/Kernel/arm/dre/irq/DRENativeGIC.cpp"],
        "edited_sources": list(names),
        "requires_existing_native_board_and_libfdt": True,
        "policy_provider_supplied": False,
        "hardware_delivery_proven": False,
    }


def source_hooks_program():
    """Return standalone text for root's late edit script; do not execute it.

    Staging callers may embed this code after the existing UI/native edits.
    Explicit execution tomorrow still requires a fresh isolated source tree.
    """
    import inspect
    return (inspect.getsource(edit_selected_tree) +
            '\n_dre_irq_source_result = edit_selected_tree(".")\n')
