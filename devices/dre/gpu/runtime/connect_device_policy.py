#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Opt-in selected306 feature overlay; runtime/publication remain unqualified.

Works on a new pinned prepared frontend/Rust bridge copy, preserving previously
tested sources. It does not run SDK/Rust compilers, a recipe, service or device.
"""
import hashlib
from pathlib import Path
import shutil

HERE = Path(__file__).resolve().parent


def once(text: str, old: str, new: str) -> str:
    if text.count(old) != 1:
        raise ValueError("selected device-policy source context changed: " + repr(old[:100]))
    return text.replace(old, new, 1)


def connect(staged: Path) -> dict:
    frontend = staged / "NativeBridgeMetalFrontend.mm"
    original = frontend.read_text(encoding="utf-8")
    if "dre_metal306_install_device_policy" in original:
        raise ValueError("device policy already connected; require fresh source")
    text = once(original, '#include "compiler_service.h"',
        '#include "compiler_service.h"\n#include "device_policy.h"')
    begin = "// The device queries the runtime answers (nmb_device_answer_selector),"
    end = 'extern "C" id native_bridge_metal_open('
    if text.count(begin) != 1 or text.count(end) != 1:
        raise ValueError("selected dynamic answer-table boundaries changed")
    start, stop = text.index(begin), text.index(end)
    if "static void installAnswers(Class cls)" not in text[start:stop]:
        raise ValueError("selected generic runtime answer installer missing")
    text = text[:start] + "// UNVALIDATED: explicit device_policy replaces generic inherited/family answers.\n" + text[stop:]
    text = once(text, 'free(methods);installAnswers(cls);class_addProtocol(cls,@protocol(MTLDevice));',
        '''free(methods);
 if(!dre_metal306_install_device_policy(cls,gDevice)) {
  NBLOG(@"DRE Metal306: actual native facts or exact device-query ABI unavailable");
  objc_disposeClassPair(cls);return nil;
 }
 class_addProtocol(cls,@protocol(MTLDevice));''')
    text = once(text, '- (BOOL)respondsToSelector:(SEL)selector {\n if(selector==sel_registerName("targetDeviceInfo"))',
        '- (BOOL)respondsToSelector:(SEL)selector {\n if(dre_metal306_selector_refused(object_getClass(self),selector))return NO;\n if(selector==sel_registerName("targetDeviceInfo"))')
    # No Apple-family inference from feature-set numbering. The runtime policy
    # also replaces this exact method with the same verified ABI before use.
    start_marker = '- (BOOL)supportsFeatureSet:(MTLFeatureSet)set {'
    next_marker = '// The original discovery (_MTLAddDevice) runs these four'
    if text.count(start_marker) != 1 or text.count(next_marker) != 1:
        raise ValueError("selected feature-set context changed")
    comment = '// supportsFamily: and the other queries the runtime answers are installed from it'
    if text.count(comment) != 1:
        raise ValueError("selected family inference comment changed")
    start, stop = text.index(comment), text.index(next_marker)
    text = text[:start] + '// No Apple/common family or legacy feature-set equivalence is qualified.\n' + \
        '- (BOOL)supportsFeatureSet:(MTLFeatureSet)set {(void)set;return NO;}\n' + text[stop:]
    reg_start = '// The original discovery (_MTLAddDevice) runs these four'
    reg_end = '- (void)initLimits {}'
    start, stop = text.index(reg_start), text.index(reg_end)
    text = text[:start] + '// Device policy replaces these registration methods before class publication; private base tables remain unqualified.\n' + text[stop:]
    text = once(text, '- (BOOL)hasUnifiedMemory {return YES;}',
        '- (BOOL)hasUnifiedMemory { [self doesNotRecognizeSelector:_cmd];__builtin_trap(); }')
    text = once(text, '- (uint64_t)sharedMemorySize {return [NSProcessInfo processInfo].physicalMemory;}',
        '- (uint64_t)sharedMemorySize { [self doesNotRecognizeSelector:_cmd];__builtin_trap(); }')
    rec = '- (NSUInteger)recommendedMaxWorkingSetSize {'
    following = '- (id<MTLCommandQueue>)newCommandQueue {'
    if text.count(rec) != 1 or text.count(following) != 1:
        raise ValueError("selected working-set context changed")
    start, stop = text.index(rec), text.index(following)
    text = text[:start] + '- (NSUInteger)recommendedMaxWorkingSetSize { [self doesNotRecognizeSelector:_cmd];__builtin_trap(); }\n' + text[stop:]
    waiver = " // The target families supportsFamily: answers true,"
    base = ' Class base=objc_getClass("_MTLDevice");'
    if text.count(waiver) != 1 or text.count(base) != 1:
        raise ValueError("selected old family-waiver journal context changed")
    start, stop = text.index(waiver), text.index(base)
    text = text[:start] + text[stop:]
    # Original functions remain source-compatible for the exact SDK ABI; their
    # implementations are replaced by typed runtime-owned getters before the
    # class is registered. No unregistered partial object is returned.
    bridge = staged / "bridge/NativeMetalObjects.rs"
    rust = bridge.read_text(encoding="utf-8")
    if 'include!("dre_device_facts.rs.inc")' in rust:
        raise ValueError("matching bridge already contains device-facts overlay")
    if 'pub struct NmbDevice(Device,' not in rust or 'pub unsafe extern "C" fn nmb_device_answer(' not in rust:
        raise ValueError("matching bridge device/query ABI changed")
    rust += '\n// UNVALIDATED selected DRE Vulkan-backed facts.\ninclude!("dre_device_facts.rs.inc");\n'
    files = {}
    for name, relative in (("device_policy.h", "device_policy.h"), ("device_policy.mm", "device_policy.mm"),
                           ("dre_device_facts.rs.inc", "bridge/dre_device_facts.rs.inc"),
                           ("base_init/base_metadata.h", "base_init/base_metadata.h"),
                           ("base_init/base_metadata.mm", "base_init/base_metadata.mm"),
                           ("base_init/base_environment.h", "base_init/base_environment.h")):
        path = HERE / name
        destination = staged / relative
        if path.is_symlink() or not path.is_file() or destination.exists():
            raise ValueError("fresh ordinary device-policy source required: " + relative)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, destination)
        files[relative] = hashlib.sha256(destination.read_bytes()).hexdigest()
    frontend.write_text(text, encoding="utf-8", newline="\n")
    bridge.write_text(rust, encoding="utf-8", newline="\n")
    return {"status": "UNVALIDATED_DEVICE_POLICY_SOURCE_CONNECTED", "source_recipe_qualified": False,
        "original_frontend_sha256": hashlib.sha256(original.encode()).hexdigest(),
        "connected_frontend_sha256": hashlib.sha256(text.encode()).hexdigest(),
        "connected_bridge_sha256": hashlib.sha256(rust.encode()).hexdigest(), "sources_sha256": files,
        "device_facts_abi": {"version": 1, "bytes": 72, "export": "nmb_dre_device_facts"},
        "inherited_device_defaults_blocked": True, "apple_family_equivalence_claimed": False,
        "native_fact_queries_exercised": False, "factory_exercised": False,
        "base_metadata_guard_connected": True,
        "inherited_serialization_helper_preserved": True,
        "private_base_tables_initialized": False,
        "publication_ready": False, "metal_acceleration_pass": False}
