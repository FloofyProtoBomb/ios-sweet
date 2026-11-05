#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""UNVALIDATED opt-in source staging recipe; never run during source-only night.

Called only by stage_xnu.py with explicit native-draft selections. This adds
source and build-list entries, not a boot argument, provider or ready device.
It neither applies the generated patches nor starts a build or phone operation.
"""
from pathlib import Path
import difflib
import hashlib
import importlib.util

TARGET = 'iokit/Kernel/arm/dre'
LIBFDT_HEADERS = ('libfdt.h', 'fdt.h', 'libfdt_env.h', 'libfdt_internal.h')
GPU_CORE = (
    'a619_backend', 'a619_holi_power', 'a619_zap', 'a619_pas13',
    'a619_smc64', 'a619_smmu', 'a619_vm', 'a619_cp', 'a619_rbbm',
    'a619_startup',
)
GPU_PLATFORM = ('rpm_smd', 'rpm_glink', 'rpm_mmio', 'holi_power', 'boot_memory',
                'profile', 'io_router', 'gcc_hold', 'rpm_layout')
TOUCH_NATIVE = ('profile', 'gpi_spi', 'nt36672c', 'input', 'smmu', 'qup', 'tlmm')
TOUCH_COMMON = ('firmware', 'report', 'spi_plan')
IRQ_ASSETS = ('DRENativeGIC.cpp', 'dre_spi.h', 'spi_low.h', 'platform_namespace.h',
              'pe_spi.inc', 'sleh_spi.inc', 'platform_namespace.inc', 'service_source_cache.inc')


def read_source(here, name):
    source = here / name
    if source.is_symlink() or not source.is_file():
        raise ValueError('missing ordinary draft source: ' + name)
    return source.read_bytes().decode('utf-8', errors='surrogateescape')


def kernel_text(name, text, gpu_nc_proof=False, firmware_nc_proof=False):
    text = text.replace('#include <string.h>', '#include <libkern/libkern.h>')
    if name.startswith('libfdt/'):
        text = text.replace('<libfdt.h>', '"libfdt.h"').replace('<fdt.h>', '"fdt.h"')
        text = text.replace('<libfdt_env.h>', '"libfdt_env.h"')
        if name.endswith('libfdt_env.h'):
            text = text.replace('#include <stdlib.h>', '#include <libkern/libkern.h>')
            text = text.replace('#include <limits.h>', '#include <machine/limits.h>')
    if name.startswith('touch_native/'):
        text = text.replace('#include <libfdt.h>', '#include "../libfdt/libfdt.h"')
        if name.endswith('xnu_hid_frame.inc'):
            text = text.replace('#include "dre_touch/input.h"', '#include "input.h"')
    if name.startswith(('gpu/platform/', 'gpu/task_vm/')):
        text = text.replace('#include <libfdt.h>', '#include "../../libfdt/libfdt.h"')
    if name.startswith('irq/'):
        text = text.replace('#include <libfdt.h>', '#include "../libfdt/libfdt.h"')
    if name.endswith(('.c', '.cpp')):
        proof = ('#define DRE_GPU_NATIVE_PMAP_PROOF 1\n'
                 if gpu_nc_proof and name == 'gpu/kernel/DREAdrenoGPU.cpp' else '')
        if firmware_nc_proof and name == 'gpu/firmware/native.cpp':
            proof += '#define DRE_GPU_FW_BOOT_NC_PROOF 1\n'
        text = ('/* UNVALIDATED GENERATED NATIVE DRAFT; explicit opt-in only. */\n'
                '#include <pexpert/arm64/board_config.h>\n'
                '#if defined(ARM64_BOARD_CONFIG_DRE)\n' + proof + text.rstrip('\r\n') +
                '\n#endif /* ARM64_BOARD_CONFIG_DRE */\n')
    return text


def build_edit_script(units, touch, gpu_nc_proof=False, firmware_nc_proof=False,
                      surface_bridge=False, irq_hook_source=''):
    """Late edit follows existing UI/HID patches; unknown layouts refuse.

    No script is executed by this function. The future build runner must
    separately apply/review the generated source edits before compilation.
    """
    program = '''from pathlib import Path
import re
units = UNITS
touch = TOUCH
gpu_nc_proof = GPU_NC_PROOF
firmware_nc_proof = FIRMWARE_NC_PROOF
surface_bridge = SURFACE_BRIDGE
irq_hook_source = IRQ_HOOK_SOURCE
file_list = Path("iokit/conf/files.arm64")
raw = file_list.read_bytes()
text = raw.decode("utf-8", errors="surrogateescape")
if any(path in text for path in units):
    raise SystemExit("native draft source list already present; fresh tree required")
pmap_path = None
pmap_prepared = None
hid_path = None
hid_prepared = None
surface_path = None
surface_prepared = None
if gpu_nc_proof or firmware_nc_proof:
    version_file = Path("config/MasterVersion")
    # MasterVersion records the Darwin release, not the XNU source tag.
    # The selected xnu-8796.141.3 source declares Darwin22.6.0 here.
    if not version_file.is_file() or version_file.read_text().splitlines()[:1] != ["22.6.0"]:
        raise SystemExit("native pmap draft requires selected Darwin22.6.0 sources and exact XNU8796 hooks")
    pmap_path = Path("osfmk/arm/pmap/pmap.c")
    pmap_prepared = pmap_path.read_bytes().decode("utf-8", errors="surrogateescape")
    additions = []
    if gpu_nc_proof:
        additions.append("dre_gpu_nc_pmap.inc")
    if firmware_nc_proof:
        additions.append("dre_gpu_fw_boot_nc_pmap.inc")
    for name in additions:
        if name in pmap_prepared:
            raise SystemExit("native pmap proof already included; fresh tree required")
    pmap_prepared += "\\n#if defined(ARM64_BOARD_CONFIG_DRE)\\n"
    pmap_prepared += "".join('#include "' + name + '"\\n' for name in additions)
    pmap_prepared += "#endif /* UNVALIDATED native NC alias observation */\\n"
if touch:
    # The DRE UI selection omits the old Redmi hardware driver patches. Do
    # not tolerate one being reintroduced into an explicitly native build.
    if any(name in text for name in ("SweetTouch.cpp", "SweetTouch.c", "SweetKeysTouch.cpp")):
        raise SystemExit("legacy touch hardware owner must be removed explicitly")
    hid = Path("iokit/Kernel/arm/hid/SweetHID.cpp")
    original = hid.read_bytes().decode("utf-8", errors="surrogateescape")
    if "dreNativeFrame" in original or "xnu_hid_frame.inc" in original:
        raise SystemExit("native HID hook already present; fresh tree required")
    if "bool fEdge[DRE_TOUCH_FINGERS];" not in original or "#define DRE_TOUCH_EDGE_PX 48" not in original:
        raise SystemExit("native HID hook requires the selected0071 edge-contact contract")
    marker = "class SweetHIDTouch : public IOHIDEventService {"
    if original.count(marker) != 1:
        raise SystemExit("selected SweetHIDTouch declaration changed")
    prepared = original.replace(marker, "struct dre_tn_hid_frame;\\n" + marker, 1)
    expression = r"(class SweetHIDTouch\\s*:\\s*public IOHIDEventService\\s*\\{[\\s\\S]*?\\npublic:\\n)"
    prepared, count = re.subn(expression,
        r"\\1\\tbool dreNativeFrame(const struct dre_tn_hid_frame *);\\n", prepared, count=1)
    if count != 1:
        raise SystemExit("selected SweetHIDTouch public section changed")
    prepared += ("\\n#if defined(ARM64_BOARD_CONFIG_DRE)\\n"
        "#include \\\"../dre/touch_native/xnu_hid_frame.inc\\\"\\n"
        "#endif /* native DRE touch frame bridge */\\n")
    hid_path = hid
    hid_prepared = prepared
if surface_bridge:
    # Same narrow hooks as the separately authored surface-overlay recipe.
    # This is retained backing inspection, never a GPU import/cache promise.
    surface_path = Path("iokit/Kernel/arm/SweetSurface.cpp")
    surface_prepared = surface_path.read_bytes().decode("utf-8", errors="surrogateescape")
    if "dre_surface_backing.h" in surface_prepared or "dre_sweet_surface_bridge.inc" in surface_prepared:
        raise SystemExit("native surface bridge already present; fresh tree required")
    for marker in ("#if defined(DRE)", "IOCoreSurfaceRoot::start(IOService *provider)",
        "sweet_surface_get(SweetSurface *s)", "sweet_surface_put(SweetSurface *s)",
        "copyObjectForPortNameInTask", "IOSurfaceRootUserClient::trapIncrementUse",
        "ref->useCount[category]", "s->mapCache = kIOMapDefaultCache;"):
        if marker not in surface_prepared:
            raise SystemExit("selected retained SweetSurface layout changed: " + marker)
    include = '#include <IOKit/IOService.h>\\n'
    startup = '\\tsweet_surface_init_lock();\\n\\tsetProperty(kIOUserClientClassKey, "IOSurfaceRootUserClient");'
    if surface_prepared.count(include) != 1 or surface_prepared.count(startup) != 1:
        raise SystemExit("selected SweetSurface source hook context changed")
    surface_prepared = surface_prepared.replace(include, include +
        '#include "dre/gpu/kernel/dre_surface_backing.h"\\n'
        'static int dre_sweet_surface_bridge_install(IOService *);\\n', 1)
    surface_prepared = surface_prepared.replace(startup,
        '\\tsweet_surface_init_lock();\\n'
        '\\tint backingStatus = dre_sweet_surface_bridge_install(this);\\n'
        '\\tif (backingStatus != DRE_GPU_OK) {\\n'
        '\\t\\tIOLog("DRE SURF: retained-backing bridge unavailable: %d\\\\n", backingStatus);\\n'
        '\\t}\\n'
        '\\tsetProperty(kIOUserClientClassKey, "IOSurfaceRootUserClient");', 1)
    surface_prepared += ('\\n#if defined(ARM64_BOARD_CONFIG_DRE)\\n'
        '#include "dre/gpu/kernel/dre_sweet_surface_bridge.inc"\\n'
        '#endif /* retained backing only; GPU import remains unsupported */\\n')
# Prepare every source edit before replacing existing source bytes. This is
# not an atomic filesystem transaction; the future runner uses a fresh tree.
if irq_hook_source:
    # The agent-authored exact selected-source hook also prepares its five
    # target edits before writing any. It runs only on future explicit late
    # script execution, in a separate namespace to preserve these variables.
    exec(compile(irq_hook_source, "<UNVALIDATED DRE IRQ source hook>", "exec"),
         {"__name__": "dre_deferred_native_irq_hook"})
if hid_path is not None:
    hid_path.write_bytes(hid_prepared.encode("utf-8", errors="surrogateescape"))
if surface_path is not None:
    surface_path.write_bytes(surface_prepared.encode("utf-8", errors="surrogateescape"))
if pmap_path is not None:
    pmap_path.write_bytes(pmap_prepared.encode("utf-8", errors="surrogateescape"))
text = text.rstrip("\\r\\n") + "\\n\\n# UNVALIDATED opt-in DRE native source\\n"
text += "".join(path + "\\toptional iokitcpp\\n" for path in units)
file_list.write_bytes(text.encode("utf-8", errors="surrogateescape"))
print("Added UNVALIDATED native source/HID integration; no hardware readiness established")
'''
    program = program.replace('units = UNITS', 'units = ' + repr(units))
    program = program.replace('touch = TOUCH', 'touch = ' + repr(touch))
    program = program.replace('gpu_nc_proof = GPU_NC_PROOF', 'gpu_nc_proof = ' + repr(gpu_nc_proof))
    program = program.replace('firmware_nc_proof = FIRMWARE_NC_PROOF',
                              'firmware_nc_proof = ' + repr(firmware_nc_proof))
    program = program.replace('surface_bridge = SURFACE_BRIDGE',
                              'surface_bridge = ' + repr(surface_bridge))
    program = program.replace('irq_hook_source = IRQ_HOOK_SOURCE',
                              'irq_hook_source = ' + repr(irq_hook_source))
    return '#!/bin/sh\nset -eu\npython3 - <<\'DRE_NATIVE_SOURCE\'\n' + program + 'DRE_NATIVE_SOURCE\n'


def stage_native_drafts(here, out, features, gpu_nc_proof=False,
                        gpu_firmware=False, firmware_nc_proof=False):
    features = tuple(dict.fromkeys(features))
    if not features or any(feature not in ('gpu', 'touch') for feature in features):
        raise ValueError('explicit gpu/touch native draft selections required')
    if gpu_nc_proof and 'gpu' not in features:
        raise ValueError('GPU pmap observation requires the explicit GPU draft')
    if gpu_firmware and 'gpu' not in features:
        raise ValueError('firmware source requires the explicit GPU draft')
    if firmware_nc_proof and not gpu_firmware:
        raise ValueError('boot-owned NC observation requires the explicit firmware draft')
    assets = {f'libfdt/{name}': f'libfdt/{name}' for name in LIBFDT_HEADERS}
    units = []

    def add(name, target=None):
        target = target or name
        if target in assets.values():
            raise ValueError('duplicate draft destination: ' + target)
        assets[name] = target
        if target.endswith(('.c', '.cpp')):
            units.append(TARGET + '/' + target)

    add('native_board.h')
    add('native_board.c', 'dre_native_board.c')
    add('storage_abi.h')
    add('storage_live.h')
    add('dma_memory.h')
    # Common actual controller/dispatcher source for both native owners.
    # Publication still requires a real external broker and qualified route.
    for name in IRQ_ASSETS:
        add('irq/' + name)
    if 'gpu' in features:
        add('gpu/a619_packet.h')
        for name in GPU_CORE:
            add('gpu/' + name + '.c')
            header = 'gpu/' + name + '.h'
            # The SMC transport is declared in the PAS header, not a separate
            # header named after its implementation translation unit.
            if name != 'a619_smc64':
                add(header)
        for name in GPU_PLATFORM:
            target = 'gpu/platform/dre_gpu_power_profile.c' if name == 'profile' else None
            add('gpu/platform/' + name + '.c', target)
        for name in ('rpm_smd', 'rpm_mmio', 'holi_power', 'boot_memory', 'profile', 'io_router', 'gcc_hold', 'rpm_layout'):
            add('gpu/platform/' + name + '.h')
        add('gpu/platform/native_owner.h')
        add('gpu/platform/native_owner.cpp', 'gpu/platform/DRENativeGPUOwner.cpp')
        add('gpu/platform/rpm_native.h')
        add('gpu/platform/rpm_native.cpp', 'gpu/platform/DRENativeRPM.cpp')
        add('gpu/platform/gic_route.h')
        add('gpu/platform/gic_route.cpp', 'gpu/platform/DRENativeGPUSPI.cpp')
        for name in ('dre_gpu_uapi.h', 'dre_gpu_provider.h',
                     'allocation_catalog.h',
                     'dre_gpu_vm_binding.h', 'dre_gpu_vm_binding.c',
                     'dre_gpu_access_epoch.h', 'dre_gpu_access_epoch.c',
                     'dre_gpu_pmap_proof.h', 'trusted_queue.c', 'trusted_queue.h',
                     'dre_surface_backing.cpp', 'dre_surface_backing.h',
                     'dre_sweet_surface_bridge.inc', 'DREAdrenoGPU.cpp'):
            add('gpu/kernel/' + name)
        add('gpu/kernel/allocation_catalog.c', 'gpu/kernel/dre_gpu_allocation_catalog.c')
        # This intake proves only its narrow structural subset. Merely linking
        # it must not enable RAW or turn a provider into a complete validator.
        for name in ('dre_pm4_intake.c', 'dre_pm4_intake.h', 'a619_pm4_policy.h'):
            add('gpu/vulkan/pm4/' + name)
        # Private planning only: no executor, observed retirement or new cap.
        add('gpu/cache/a619_cache.c')
        add('gpu/cache/a619_cache.h')
        # Non-installable private AP/purpose proposals. These confer no task
        # isolation and cannot change the active VM or enable raw submission.
        for name in ('permissions', 'plan', 'registry_binding'):
            add('gpu/task_vm/' + name + '.h')
            add('gpu/task_vm/' + name + '.c', 'gpu/task_vm/dre_gpu_taskvm_' + name + '.c')
        if gpu_firmware:
            for name in ('package.c', 'package.h', 'region.c', 'region.h',
                         'trust_anchor.c', 'native.cpp', 'native.h'):
                target = ('DRENativeGPUFirmware.cpp' if name == 'native.cpp' else
                          'dre_gpu_firmware_' + name if name.endswith('.c') else name)
                add('gpu/firmware/' + name, 'gpu/firmware/' + target)
            # Lifecycle/boot callback composition calls the actual firmware
            # module. Never link it into a source selection lacking that code.
            for name in ('boot_binding.c', 'boot_binding.h', 'native_provider.h'):
                add('gpu/platform/' + name)
            add('gpu/platform/native_provider.cpp', 'gpu/platform/DRENativeGPUPlatform.cpp')
    if 'touch' in features:
        for name in TOUCH_NATIVE:
            add('touch_native/' + name + '.c', 'touch_native/dre_touch_' + name + '.c')
        for name in ('native.h', 'gpi_spi.h', 'nt36672c.h', 'input.h', 'smmu.h', 'qup.h',
                     'tlmm.h', 'xnu_tlmm.h', 'xnu_hid_frame.inc'):
            add('touch_native/' + name)
        add('touch_native/xnu_provider.h')
        add('touch_native/DRENativeTouch.cpp')
        add('touch_native/DRETouchTLMM.cpp')
        add('touch_native/xnu_irq_route.h')
        add('touch_native/xnu_irq_route.cpp', 'touch_native/dre_touch_xnu_irq_route.cpp')
        for name in TOUCH_COMMON:
            add('touch/' + name + '.h')
            add('touch/' + name + '.c', 'touch/dre_touch_' + name + '.c')

    # Prepare all contents first. Do not write a partial additions patch if
    # an expected owned source file has not yet been authored/committed.
    sections = []
    source_hashes = {}
    for name, target in assets.items():
        original = read_source(here, name)
        encoded = original.encode('utf-8', errors='surrogateescape')
        source_hashes[name] = hashlib.sha256(encoded).hexdigest()
        content = kernel_text(name, original, gpu_nc_proof, firmware_nc_proof)
        sections.extend(difflib.unified_diff([], content.splitlines(True),
            fromfile='/dev/null', tofile='b/' + TARGET + '/' + target))
    # Do not call this while authoring the overnight recipe. A future explicit
    # stage invocation loads only the authored program-text helper, not its
    # edit_selected_tree operation; the latter stays inside the late script.
    irq_helper = here / 'irq/source_hooks.py'
    irq_original = read_source(here, 'irq/source_hooks.py')
    source_hashes['irq/source_hooks.py'] = hashlib.sha256(
        irq_original.encode('utf-8', errors='surrogateescape')).hexdigest()
    irq_spec = importlib.util.spec_from_file_location('dre_selected_irq_source_recipe', irq_helper)
    irq_module = importlib.util.module_from_spec(irq_spec)
    irq_spec.loader.exec_module(irq_module)
    irq_program = irq_module.source_hooks_program()
    if not isinstance(irq_program, str) or not irq_program:
        raise ValueError('missing explicit deferred IRQ source-hook program')
    pmap_assets = []
    if gpu_nc_proof:
        pmap_assets.append(('gpu/kernel/dre_gpu_nc_pmap.inc', 'dre_gpu_nc_pmap.inc'))
    if firmware_nc_proof:
        pmap_assets.append(('gpu/firmware/boot_nc_pmap.inc', 'dre_gpu_fw_boot_nc_pmap.inc'))
    for name, target in pmap_assets:
        original = read_source(here, name)
        source_hashes[name] = hashlib.sha256(original.encode('utf-8', errors='surrogateescape')).hexdigest()
        sections.extend(difflib.unified_diff([], original.splitlines(True),
            fromfile='/dev/null', tofile='b/osfmk/arm/pmap/' + target))
    patch_name = '0105-dre-native-draft-assets.patch'
    script_name = '0106-dre-native-draft-bindings.sh'
    patch = ''.join(sections).encode('utf-8', errors='surrogateescape')
    script = build_edit_script(units, 'touch' in features, gpu_nc_proof,
                               firmware_nc_proof, 'gpu' in features, irq_program).encode('utf-8')
    for name in (patch_name, script_name):
        if (out / name).exists():
            raise ValueError('native draft output already exists: ' + name)
    (out / patch_name).write_bytes(patch)
    (out / script_name).write_bytes(script)
    return {'status': 'UNVALIDATED_SOURCE_RECIPE', 'features': list(features),
        'gpu_pmap_observation_requested': bool(gpu_nc_proof),
        'private_firmware_source_requested': bool(gpu_firmware),
        'boot_owned_nc_observation_requested': bool(firmware_nc_proof),
        'private_trust_anchor_provisioned_by_recipe': False,
        'retained_surface_bridge_requested': 'gpu' in features,
        'surface_gpu_import_supported': False,
        'allocation_catalog_source_requested': 'gpu' in features,
        'task_permission_proposal_source_requested': 'gpu' in features,
        'task_permission_proposal_installs_ptes': False,
        'task_isolation_or_raw_submission_qualified': False,
        'selected_irq_source_hook_requested': True,
        'actual_irq_delivery_proven': False,
        'source_sha256': source_hashes,
        'recipe_sha256': {patch_name: hashlib.sha256(patch).hexdigest(),
                          script_name: hashlib.sha256(script).hexdigest()},
        'translation_units': units, 'executed_or_compiled': False,
        'native_provider_installed': False, 'metal_or_touch_ready': False}
